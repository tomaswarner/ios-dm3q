# /dev/kgsl-3d0 on our XNU: mmap of GPU buffers, memory, kernel structure

Status: DESIGN ONLY (2026-10-21). No code was edited, built, committed or flashed. Written for the Turnip (Mesa KGSL backend)
cross-build described in `TURNIP-IOS.md` (section 4 = ioctl list, section 2.3 item 4 = the mmap blocker).

Conventions
- `[xnu]` = the source tree at `port/research/bt-002/xnu` (xnu-8796 family; I did not run `git describe` on it, so the exact tag is
  UNVERIFIED, but every function cited below was read in that tree). Paths are relative to it, line numbers are from that tree.
- `[mesa]` = `~/hios/third_party/mesa/src/freedreno/vulkan/` (our patched Mesa), `[patch NNNN]` = `port/xnu/patches/`.
- Anything I could not confirm in source is marked **UNVERIFIED**. Code blocks are sketches, never compiled.

---------------------------------------------------------------------------------------------------------------------------------

## 0. Summary and recommendation

1. **Why mmap fails.** `mmap()` on a character-device vnode returns `ENODEV` unconditionally, before any driver is consulted
   (`bsd/kern/kern_mman.c` `mmap()` l.546-549, the "/dev/zero hack" branch). The `d_mmap` member of `struct cdevsw` exists but is a dead
   field: type `int (void)` (`bsd/sys/conf.h` l.113, l.197) and nothing in `bsd/`, `osfmk/`, `iokit/` ever calls it (grep of
   `->d_mmap`/`.d_mmap` finds only initialisers set to `eno_mmap`).
2. **The VM machinery to map arbitrary physical pages into a task already exists and is used by IOKit**: the *device pager*
   (`osfmk/vm/device_vm.c`) and, equivalently, Mach *named entries* (`mach_make_memory_entry_64`, `osfmk/vm/vm_user.c`) mapped with
   `vm_map_enter_mem_object` (`osfmk/vm/vm_map.c` l.4856). Cache mode (write-combine etc.) is a property of the VM object / the
   physical page and the arm64 pmap handles aliases of the kernel physical aperture correctly (section 3.4).
3. **Recommended path (two phases, one shared kernel function `kgsl_bo_map_to_task()`):**
   - **Phase 1 (first light, zero core-XNU edits):** add a private ioctl `IOCTL_KGSL_IOS_MAP` to our cdev. The ioctl runs in the
     caller's thread, so `current_map()` is the user map; it calls `vm_map_enter_mem_object(current_map(), ..., entry_port, ...)`
     and returns the user VA. Mesa change: replace the two `mmap(local_fd, id<<14)` calls (`kgsl_bo_map`, `kgsl_bo_user_map` in
     `tu_knl_kgsl.cc`) by that ioctl; `munmap()` stays `munmap()` (vm_map entries are independent of the driver).
   - **Phase 2 (unmodified Turnip mmap path):** a ~60 line patch in `kern_mman.c` + `conf.h` that makes `d_mmap` a real hook which
     returns a named-entry port; `mmap(fd, off=id<<14)` then works for any program. Reuses the same `kgsl_bo_map_to_task()`.
4. **Memory:** BO pages = a kernel-created anonymous Mach VM object (`mach_make_memory_entry_64(MAP_MEM_NAMED_CREATE | MAP_MEM_WCOMB)`),
   wired through a kernel mapping, physical addresses read with `pmap_find_phys()` for the GPU SMMU page tables. Scattered pages are
   fine because the GPU has its own SMMU stage-1; physically contiguous memory (`kmem_alloc_contig`) only for kernel-internal
   firmware/ring/GMU buffers. Details and fallbacks in section 3.
5. **Kernel structure:** a plain-C BSD cdev (`cdevsw_add` + `devfs_make_node_clone`), *not* an IOKit user client. Turnip's
   `open()`+`ioctl()` model is a BSD fd; a user client would require IOKit calls in Mesa and entitlements, and does not give
   `open("/dev/kgsl-3d0")`. The cdev is a thin front end; the actual GPU bring-up (GMU, RPMh, SMMU) lives in the same file set or
   an IOService it calls into. `devfs_make_node_clone` is required to get per-open state (section 4.2).

---------------------------------------------------------------------------------------------------------------------------------

## 1. (a) Device mmap in XNU: history, current state, why ENODEV, minimal change

### 1.1 What `mmap()` does for a character device today

`bsd/kern/kern_mman.c`, function `mmap()`:
- l.293-306: UNIX03 check: `file_pos & offset_alignment_mask` -> `EINVAL`. The mask is `vm_map_page_mask(current_map())` (0x3FFF for the
  16K iOS user map; 0xFFF only for "exotic" maps). This is why Turnip's `id << 12` offsets already had to become `id << 14`.
- l.504-512: `fp_lookup`; `DTYPE_PSXSHM` goes to `pshm_mmap`, `DTYPE_VNODE` continues, anything else `EINVAL`.
- l.524: only `VREG` and `VCHR` vnodes pass.
- l.531-536: updates the access time (harmless on devfs).
- **l.546-549:** `if (vp->v_type == VCHR || vp->v_type == VSTR) { vnode_put(vp); error = ENODEV; goto bad; }` with the comment
  "XXX hack to handle use of /dev/zero to map anon memory (ala SunOS)". No cdevsw lookup. This is the complete answer to "why ENODEV".
- Only the `else` (regular files) continues to `mac_file_check_mmap` (l.612) and `VNOP_MMAP_CHECK` (l.628), then to
  `ubc_getobject(vp)` (l.788) giving a `memory_object_control_t`, then `vm_map_enter_mem_object_control()` (l.879).
- l.783 shows an existing precedent for "VM object that is not the vnode's UBC object": swap files use
  `swapfile_pager_setup(vp)` -> `swapfile_pager_control(pager)` and `memory_object_deallocate(pager)` at the `bad:` label (l.926).
  A device pager can be slotted in exactly the same way.

### 1.2 The dead `d_mmap` hook

`bsd/sys/conf.h`: `typedef int mmap_fcn_t(void);` (l.113); `struct cdevsw { ... mmap_fcn_t *d_mmap; ... }` (l.197);
`#define eno_mmap ((mmap_fcn_t *)(empty_fcn_t*)&enodev)` (l.147). All in-tree cdevs set it to `eno_mmap`
(`bsd/dev/arm64/conf.c` l.108/176-197, `tty_ptmx.c` l.130/148, `bpf.c` l.280, `pf_ioctl.c` l.206, `audit_pipe.c` l.241,
`audit_session.c` l.350, `vfs_fsevents.c` l.2800). Because no caller exists, changing the typedef is source-compatible with every
in-tree initialiser (they only use the `eno_mmap` macro, which casts to `mmap_fcn_t *`). Historical note (BSD heritage: `d_mmap`
returned a page frame for the old BSD `dev_pager`): UNVERIFIED for XNU history, not needed for the design.

### 1.3 The device pager (what a "mapped device" is in XNU)

`osfmk/vm/device_vm.c`:
- `device_pager_setup(device, device_handle, size, flags)` (l.137): allocates a `device_pager` (a `memory_object`), creates a named VM
  object (`memory_object_create_named`), marks it `true_share`, returns the `memory_object_t`. `device_handle` is an opaque
  `uintptr_t` stored in the pager; `flags` = `DEVICE_PAGER_*` (`osfmk/device/device_port.h` l.73-78: GUARDED 0x1, COHERENT 0x2,
  CACHE_INHIB 0x4, WRITE_THROUGH 0x8, **CONTIGUOUS 0x100**, NOPHYSCACHE 0x200) OR'd with a `VM_WIMG_*` value.
- `device_pager_init` (l.246): sets `vm_object->private = TRUE`, `phys_contiguous` if `DEVICE_PAGER_CONTIGUOUS`,
  and **`vm_object->wimg_bits = flags & VM_WIMG_MASK`** (l.~280). This is the cache mode of the whole object.
- `device_pager_populate_object(device, offset, page_num, size)` (l.178): calls `vm_object_populate_with_private()`
  (`osfmk/vm/vm_object.c` l.5283) which, for a non-contiguous object, inserts *private* `vm_page`s that point at the given physical
  page numbers one by one (l.5310-5355: `m->vmp_private = TRUE; VM_PAGE_SET_PHYS_PAGE(m, base_page); vm_page_insert`), or, for a
  contiguous object, just records `vo_shadow_offset = phys_page << PAGE_SHIFT` (l.5366).
  So **scattered physical pages are supported** (call it once per page with a different `page_num`); contiguous is an optimisation
  that makes the fault path map the whole range with one `pmap_map_block_addr` (`osfmk/vm/vm_fault.c` l.6180-6210).
- `device_pager_data_request` / `data_return` (l.335, l.308) call `device_data_action()` which is **defined in IOKit**
  (`iokit/Kernel/IOMemoryDescriptor.cpp` l.215) and cast `device_handle` to an `IOMemoryDescriptorReserved *`. Likewise
  `device_pager_deallocate` calls `device_close(handle)` (`device_vm.c` l.~395, defined at `IOMemoryDescriptor.cpp` l.~236) which
  `IOFreeType`s the handle. **A non-IOKit user of the device pager must therefore make these two entry points dispatch on a flag**
  (section 1.5, variant A).
- `device_pager_map` / `device_pager_last_unmap` (l.451, l.476): the pager keeps one extra reference while *any* mapping exists
  (`is_mapped`), dropped on last unmap. The "named" reference is released via `device_close` -> this is the lifetime hook that tells
  a driver "no user mapping remains".
- In-tree template for using it outside IOKit: `osfmk/vm/vm_tests.c` l.395-435 (`vm_test_device_pager_transpose`: `device_pager_setup(NULL, 0, size, 0)`, then
  `vm_map_enter_mem_object(kernel_map, &addr, size, 0, VM_MAP_KERNEL_FLAGS_DATA_ANYWHERE(), (void *)device_pager, 0, FALSE, ...)`;
  `vm_map_enter_mem_object` accepts a raw `memory_object_t` cast to `ipc_port_t` because of the `IKOT_MEMORY_OBJECT` branch at
  `osfmk/vm/vm_map.c` l.~4617).

### 1.4 How IOKit gets memory into a task (the shipping path)

- Kernel side: `IOMemoryDescriptor::createMappingInTask(task, atAddress, options, offset, length)`
  (`iokit/IOKit/IOMemoryDescriptor.h` l.740-757). Internally `IOGeneralMemoryDescriptor::memoryReferenceCreate`
  (`iokit/Kernel/IOMemoryDescriptor.cpp`, l.~480-800) builds Mach named entries:
  - virtual/kernel memory: `mach_make_memory_entry_internal(map, &actualSize, entryAddr, prot, ...)` (l.~640) with `map =
    get_task_map(_task)` (kernel_map for a kernel buffer) or, for pageable `IOBufferMemoryDescriptor`, `prot |= MAP_MEM_NAMED_CREATE`
    and `map = NULL` (l.~598-621) -> anonymous object, exactly like POSIX shm;
  - physical ranges (`_task == 0`): `device_pager_setup(NULL, (uintptr_t)reserved, size, pagerFlags)` + 
    `mach_memory_object_memory_entry_64(host, false, size, VM_PROT_READ|VM_PROT_WRITE, pager, &entry)` (l.760-775), and later
    `populateDevicePager()` (l.5144-5210) loops `device_pager_populate_object()` per page (`chunk = pagerContig ? round_page(segLen) : page_size`).
  - cache mode: table `iomd_mem_types[]` (l.360-371) maps `kIOWriteCombineCache` -> `{VM_WIMG_WCOMB, MAP_MEM_WCOMB}` and so on;
    `pagerFlagsForCacheMode()` (l.386) feeds the pager flags, `vmProtForCacheMode()` (l.375) feeds the named-entry permission.
  - The entry is then entered into the task map with `vm_map_enter_mem_object`-family calls inside `IOMemoryDescriptor::doMap`.
- User side: `IOConnectMapMemory64` -> `is_io_connect_map_memory_into_task` (`iokit/Kernel/IOUserClient.cpp` l.4666) ->
  `IOUserClient::mapClientMemory64` (l.1937) -> subclass `clientMemoryForType()` -> `createMappingInTask`. The address is returned
  to user space; no file offset is involved. (A user can equally call `mach_vm_map()` with a named-entry port; the port is what
  `createMappingInTask` internally produces.)
- **Hazard that matters for us:** the `IOMemoryMap` returned by `createMappingInTask` *owns* the mapping ("freeing the object destroys the
  mapping", header comment at `IOMemoryDescriptor.h` l.~755). It is meant to be kept by an `IOUserClient` until `IOConnectUnmapMemory`
  or client death. With Turnip's `munmap()`/`MAP_FIXED` replacement semantics, an `IOMemoryMap` can later `vm_deallocate` an address range
  that user space has since reused. So **do not use `IOMemoryMap` for mmap-style mappings**; create the VM entry directly from a
  named entry (variant B below) and let VM refcounting own the object.

### 1.5 Minimal kernel change that makes `mmap(fd, off)` work

There are two correct variants; both replace the `ENODEV` block and both are small. **Recommended: B.**

**Variant A: device pager + `vm_map_enter_mem_object_control` (closest to the vnode flow)**
1. `conf.h`: change `typedef int mmap_fcn_t(void);` to a real signature, e.g.
   `typedef int mmap_fcn_t(dev_t dev, off_t off, user_size_t len, int prot, memory_object_t *pager_out, vm_object_offset_t *obj_off_out);`
   (all existing `eno_mmap` initialisers keep compiling since they cast through `mmap_fcn_t *`).
2. `kern_mman.c` l.546: if `vp->v_type == VCHR` and `cdevsw[major(vp->v_rdev)].d_mmap` is neither NULL nor `eno_mmap`, compute `maxprot` from
   `fp->fp_glob->fg_flag` (as the regular-file branch does), run `mac_file_check_mmap`, call the hook, set `pager`, `control =
   device_pager_control(pager)` (new tiny accessor next to `swapfile_pager_control`), set `file_pos = obj_off`, then fall through to the
   existing `vm_map_enter_mem_object_control()` and the existing `memory_object_deallocate(pager)` at `bad:` (l.926).
3. `device_vm.c`: add `DEVICE_PAGER_BSD 0x400`; in `device_pager_data_request/return` skip `device_data_action` when set
   (all pages are pre-populated, so a fault on a missing page returns `KERN_FAILURE`), and in `device_pager_deallocate` call a registered
   `bsd_device_pager_release(handle)` instead of `device_close`.
4. Cost: touches `kern_mman.c`, `conf.h`, `device_vm.c`, `vm_protos.h`. Needs the driver to keep the backing pages alive until
   `release` is called (extra refcount protocol, section 3.5).

**Variant B: named entry + `vm_map_enter_mem_object` (recommended)**
1. Same `conf.h` change, signature returns an `ipc_port_t` named entry: `int (*d_mmap)(dev_t dev, off_t off, user_size_t len, int prot,
   ipc_port_t *entry, vm_object_offset_t *entry_off, vm_prot_t *maxprot)`.
2. `kern_mman.c` l.546: replace the unconditional `ENODEV` by the hook call (sketch in 4.5). For the VM call use the same machinery as
   `pshm_mmap` (`bsd/kern/posix_shm.c` l.~856-870): `vm_map_enter_mem_object(user_map, &user_addr, user_size, 0, vmk_flags, entry,
   entry_off, FALSE, prot, maxprot, VM_INHERIT_SHARE)` with `vmk_flags` already prepared by `mmap()` for `MAP_FIXED` -> `vmf_overwrite`
   (l.689). Then `mach_memory_entry_port_release(entry)` (the API `posix_shm.c` l.652/1053 uses to drop an entry).
3. No `device_vm.c` change, no pager lifetime protocol: the named entry holds a reference on the `vm_object`; the object lives until
   the last mapping and the last port reference are gone (`vm_map_enter_mem_object` takes `vm_object_reference_locked`, `vm_map.c` l.~4617).
4. Precedent for a kernel-created anonymous entry mapped into a user task: `posix_shm.c` l.613 `mach_make_memory_entry_64(VM_MAP_NULL,
   &mosize, 0, MAP_MEM_NAMED_CREATE | VM_PROT_DEFAULT, &mem_object, 0)`, mapped by `pshm_mmap`.

### 1.6 Facts about the user mapping that both variants inherit
- `MAP_FIXED` (Turnip "placed" maps, `tu_knl_kgsl.cc` l.492-496 and the SVM `client_iova` path l.209) is atomic overwrite via
  `vmf_overwrite` (`kern_mman.c` l.689). With the hook-based path this works for free; in the ioctl path I have to pass the flag myself.
- `VM_INHERIT_SHARE` for `MAP_SHARED` (l.763/884): after `fork()` the child shares the BO pages. Harmless (the child has no KGSL context),
  and it is why BO pages must be object-owned (section 3.5), never freed by the driver while an inherited mapping exists.
- Page size: the kernel `PAGE_SIZE` and the iOS user map are 16K (mask 0x3FFF, `kern_mman.c` l.302). All BO sizes returned in `mmapsize` must be
  multiples of 16K (TURNIP-IOS.md 4.3). GPU pages stay 4K in the SMMU, so one kernel page = 4 SMMU PTEs.

---------------------------------------------------------------------------------------------------------------------------------

## 2. (b) Alternatives that avoid patching `mmap()`

| Option | Mechanism | Mesa change | XNU core change | Effort | Risk |
|---|---|---|---|---|---|
| **B1. Private ioctl "map BO"** (recommended phase 1) | cdev `d_ioctl` runs on the calling thread: `vm_map_enter_mem_object(current_map(), &addr, size, 0, flags, bo->entry, 0, FALSE, prot, prot, VM_INHERIT_SHARE)`; result in the ioctl struct | 2 call sites (`kgsl_bo_map`, `kgsl_bo_user_map`): `mmap` -> `ioctl(IOCTL_KGSL_IOS_MAP)`. Wrapper `kgsl_ios_mmap(fd, id, size, fixed_addr)`. | none (only the new cdev file) | very low; ~80 lines kernel, ~30 lines Mesa | very low. Nothing in VM or VFS changes; failure = ioctl error |
| **B2. `d_mmap` hook** (phase 2) | variant B in 1.5 | none beyond the existing `<<14` | `kern_mman.c`, `conf.h` (~60 lines) | low | low-medium: touches the hot `mmap()` path, but only the previously dead `VCHR` branch; non-device paths unchanged |
| B3. Device pager variant A | 1.5 variant A | none | `kern_mman.c`, `conf.h`, `device_vm.c`, `vm_protos.h` | medium | medium: lifetime protocol between pager and driver; fault on unpopulated page |
| B4. Private syscall (`syscalls.master` + `sys_kgsl_map`) | new BSD syscall returning a VA, called from Mesa with `syscall(N, ...)` | `syscall()` call (libSystem `syscall(2)` is deprecated on iOS; **UNVERIFIED** that it is exported and unrestricted by the dyld shared cache of 16.7) | `bsd/kern/syscalls.master`, generated `init_sysent.c`, `sysproto.h`, the build of syscall tables | medium-high (generated tables in our CI patch flow) | medium; more surface than B1 for no extra capability |
| B5. IOKit style: user client + `IOConnectMapMemory64` / `mach_vm_map` with a port | `IOUserClient::clientMemoryForType` + `mapClientMemory64` (IOUserClient.cpp l.1937) | Mesa would use `IOServiceOpen`, `IOConnectCallMethod` for **all** ioctls, not only mmap | none | high (rewrite all of tu_knl_kgsl.cc's ioctl layer) | medium; IOKit entitlements/sandbox; also mismatches the `open("/dev/kgsl-3d0")` contract in `tu_knl.cc:345` |
| B6. `mmap` of a *different* object: kernel exports each BO as a POSIX-shm-like fd | `DTYPE_PSXSHM` fd, Mesa `mmap(shm_fd)` (`kern_mman.c` l.504 -> `pshm_mmap`) | `ioctl(GET_BO_FD)` then normal `mmap` | needs a way to create a `pshm_info` from a kernel entry: **new fileops** | medium | medium; pshm fds are not wired/WC aware (**UNVERIFIED**: pshm mapping cache mode) |

Recommendation: implement B1 first (it is independent of any change in `kern_mman.c`, so the first CI build that contains the GPU driver can
already run `vkCreateInstance`/`vkMapMemory`), keeping `kgsl_bo_map_to_task()` free of ioctl details so that B2 can call it. B2 is worth doing
because it keeps Turnip identical to upstream except for the shift (fewer Mesa patches to carry, and future Mesa tools such as `tu_dump`
that mmap by offset also work). B3 only if we later need an object that is *not* anonymous RAM (e.g. mapping a fixed physical carve-out such as the
GPU GMEM or a Gunyah-shared region), where `device_pager_populate_object` with arbitrary page numbers is the only supported route.

Why not IOKit's `IOMemoryDescriptor::createMappingInTask` for B1: see the `IOMemoryMap` ownership hazard in 1.4. It would also pull the
whole IOKit descriptor graph into a path that is otherwise plain C.

---------------------------------------------------------------------------------------------------------------------------------

## 3. (c) Memory for GPU buffers: allocation, physical addresses, cache attributes, freeing

### 3.1 Choices

| Need | API | Notes (source) |
|---|---|---|
| **Scattered wired RAM mapped to user as WC or cached (all Vulkan BOs)** | `mach_make_memory_entry_64(VM_MAP_NULL, &size, 0, MAP_MEM_NAMED_CREATE | VM_PROT_DEFAULT | cache-bits, &entry, 0)`, then map once into `kernel_map` and wire | `osfmk/vm/vm_user.c` l.2712-2870: creates `vm_object_allocate(map_size)`, `copy_strategy = MEMORY_OBJECT_COPY_NONE`, `true_share = TRUE` (no shadow/COW), sets `object->wimg_bits` from the permission cache bits (l.2841-2845). `ANON_MAX_SIZE` limit per entry (`osfmk/mach/vm_param.h` l.246, value UNVERIFIED; split BOs above it as `pshm_truncate` does, `posix_shm.c` l.610-618). Used for POSIX shm and for pageable `IOBufferMemoryDescriptor` (`IOMemoryDescriptor.cpp` l.598-621). |
| Wired kernel-only scattered memory | `kernel_memory_allocate(kernel_map, &va, size, mask, KMA_KOBJECT|KMA_ZERO, tag)` | `osfmk/vm/vm_kern.h` l.619; flags `KMA_*` l.560-588. Pages live in the kernel object, only reachable through the kernel VA; to give them to a task you must make a named entry on the kernel range (what IOKit does, `IOMemoryDescriptor.cpp` ~l.640) or use the device pager. Fine for kernel-only structures (context records, ring-less queues). |
| **Physically contiguous** (CP ring, GMU/HFI queues, MMU page tables if the SMMU walker needs one range, zap shader region) | `kmem_alloc_contig(kernel_map, &va, size, mask, max_pnum, pnum_mask, flags, tag)` | `osfmk/vm/vm_kern.h` l.686-707 (inline) -> `kmem_alloc_contig_guard` (`vm_kern.c` l.1036) -> `cpm_allocate` (`osfmk/vm/vm_resident.c` l.6539) -> `vm_page_find_contiguous` (l.5846). Our UFS driver already does this: `[patch 0028]` `kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0, ...)`. `IOBufferMemoryDescriptor.h` l.202 warns it "may fail if memory is fragmented, may cause large amounts of paging activity, and may take a very long time"; allocate such buffers at GPU init, never per-Vulkan-allocation. `max_pnum`/`pnum_mask` restrict the physical range/alignment (e.g. below 4 GiB for 32-bit GMU descriptors; use `atop(limit)` as `IOKernelAllocateWithPhysicalRestrict` does, `iokit/Kernel/IOLib.cpp` l.719-790). |
| IOKit-flavoured equivalent | `IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache, size, mask)` | `iokit/Kernel/IOBufferMemoryDescriptor.cpp` l.255-346, l.531; internally the same `kmem_alloc_contig` (`IOLib.cpp` l.775). Only worthwhile if the GPU driver is an IOService. |
| Raw page allocation | `vm_page_alloc` (`vm_resident.c` l.4029), `vm_page_grab` (l.3132) | Internal VM API, requires owning the vm_object and the queues locking; not recommended. |

Recommendation: **BO = anonymous named entry** (row 1); firmware/ring/GMU/page-table backing = `kmem_alloc_contig`.
The GPU SMMU removes any need for physical contiguity of user BOs, which is exactly why scattered allocation is right (large
Vulkan allocations of hundreds of MiB would otherwise fail on a fragmented system).

### 3.2 Procedure for a BO (sketch)

```
size  = round_page(req.size)                         // 16K multiple; mmapsize = size
entry: mach_make_memory_entry_64(VM_MAP_NULL, &msz, 0,
           MAP_MEM_NAMED_CREATE | VM_PROT_DEFAULT | SET_MAP_MEM(cachemode, 0), &port, 0)
           // cachemode = MAP_MEM_WCOMB for KGSL_CACHEMODE_WRITECOMBINE
           // MAP_MEM_FLAGS_USER must accept the cache bits (vm_user.c l.2563-2575): UNVERIFIED which bits; IOKit passes
           // SET_MAP_MEM(iomd_mem_types[mode].object_type, prot) (IOMemoryDescriptor.cpp l.375-382) so follow that.
kva = 0; vm_map_enter_mem_object(kernel_map, &kva, size, 0, VM_MAP_KERNEL_FLAGS_DATA_ANYWHERE(), port, 0, FALSE,
           VM_PROT_DEFAULT, VM_PROT_DEFAULT, VM_INHERIT_NONE)     // vm_map.c l.4856; kernel_map range id defaults to DATA (vm_map.c l.22863)
vm_map_wire_kernel(kernel_map, kva, kva+size, VM_PROT_DEFAULT, VM_KERN_MEMORY_DEVICE?, FALSE)  // vm_map.c l.7211; tag name UNVERIFIED
for each 16K page i: pn = pmap_find_phys(kernel_pmap, kva + i*PAGE_SIZE); paddr = ptoa_64(pn)   // pmap.h l.494, as sweet_ufs does
```
Notes
- Zero fill: pages materialise on the `vm_map_wire` fault (zero-filled). Whether zero-filling happens before or after the object's WIMG is applied to
  the page is **UNVERIFIED**; it does not matter for correctness because attribute changes on a managed page flush it
  (`pmap_sync_wimg`, `osfmk/arm/pmap/pmap.c` l.10093-10102, from `COPYBACK` to anything else calls `pmap_sync_page_attributes_phys`).
- `vm_map_wire` also guarantees the compressor/pageout will not touch the pages (wired pages are not on the pageable queues). **UNVERIFIED**
  that `vm_map_wire_kernel` on a named-entry mapping in `kernel_map` is accepted without special flags in this tree; the fallback is to hold the
  object locked and `vm_page_wire()` each page after `vm_page_alloc()` (internal API).
- Cap total wired BO memory per open context and globally (suggest 50% of RAM) to avoid wiring-DoS; jetsam does not see kernel-wired memory.

### 3.3 Physical addresses for the GPU SMMU
- `pmap_find_phys(kernel_pmap, va)` returns a `ppnum_t` (`osfmk/arm/pmap/pmap.h` l.494; used in `[patch 0028]` l.524 for the UFS DMA area).
  For a 16K kernel page multiply by `PAGE_SIZE`, then emit four 4K PTEs for the GPU (TURNIP-IOS.md 4.3: GPU pages are 4K).
- Alternative for a contiguous buffer: one `pmap_find_phys` at the base.
- **UNVERIFIED (important):** these are the *VM's* physical addresses (IPAs under Gunyah). The GPU SMMU (MMU-500 `kgsl-smmu` at 0x03da0000, see
  `A740-POWERON.md` section 2) sees whatever the SMMU stage-2/Gunyah configuration provides. Our UFS driver DMA works with the same addresses
  (`[patch 0034]` "ufs-dma-above-4g"), which suggests IPA==PA for the primary VM, but that was UFS (different SID); it must be confirmed for the GPU
  SMMU context bank during probe 2 of A740-POWERON.md.

### 3.4 Cache attributes (WIMG) on arm64 XNU
- Constants: `osfmk/arm/memory_types.h` l.40-46: `VM_WIMG_DEFAULT=COPYBACK=0x2`, `VM_WIMG_INNERWBACK=0x12`, `VM_WIMG_IO=0x7`, `VM_WIMG_WTHRU=0xb`,
  **`VM_WIMG_WCOMB=0x6`**. `MAP_MEM_*` equivalents in `osfmk/mach/memory_object_types.h` l.409-419 (`MAP_MEM_WCOMB`=4, `MAP_MEM_COPYBACK`=1, ...).
- Mapping to PTEs: `wimg_to_pte()` (`osfmk/arm/pmap/pmap.c` l.5647): `VM_WIMG_WCOMB` -> `ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITECOMB)` + `NX|PNX`
  (Normal non-cacheable, i.e. what Linux calls write-combine for GPU memory); `VM_WIMG_COPYBACK` -> `CACHE_ATTRINDX_WRITEBACK`, outer-shareable.
  `VM_WIMG_IO` on DRAM is mapped as Device-GRE (l.5654-5663), so **do not use `MAP_MEM_IO` for GPU BOs**; and `pmap_update_cache_attributes_locked`
  panics for `IO/POSTED*` on managed pages if `pmap_panic_dev_wimg_on_managed` (pmap.c l.~10650).
- Where the attribute lives: per physical page in `pp_attr_table` (`pmap_set_cache_attributes_priv`, pmap.c l.10534-10590), set automatically when a
  page is inserted into an object whose `wimg_bits != VM_WIMG_USE_DEFAULT` (`vm_page_insert_internal`, `osfmk/vm/vm_resident.c` l.1850-1856,
  `PMAP_SET_CACHE_ATTR` in `osfmk/vm/pmap.h` l.513), and reset to default when the page leaves the object (`vm_resident.c` l.2282-2284). `pmap_enter`
  then takes the cacheability from `pmap_cache_attributes(pn)` when the caller passes no WIMG (pmap.c l.6107-6115).
- **Alias safety (the usual WC pitfall):** `pmap_update_cache_attributes_locked` (pmap.c l.10632-10735) rewrites the PTE of the **kernel physical aperture**
  (`#if __ARM_PTE_PHYSMAP__`, defined `osfmk/arm64/proc_reg.h` l.2651) and every existing mapping on the page's PV list (kernel VA, user VAs). So a WC
  BO has no cacheable alias left, and flush-on-change happens in `pmap_sync_wimg`.
- Cached (host-cached, `KGSL_CACHEMODE_WRITEBACK`) BOs: the pages are default COPYBACK, user space needs `DC CVAC/CIVAC` which EL0 may issue because
  `SCTLR_UCI_ENABLED` is set (`osfmk/arm64/proc_reg.h` l.562, l.655). Mesa's `sync_cache` path only works if `has_cached_non_coherent_memory`
  (`tu_device.cc` ~l.3990). The kernel must also guarantee GPU-visible coherence: either IOCOHERENT (UNVERIFIED whether the A740 SMMU contexts
  on this phone are configured as snooping) or clean/invalidate (`flush_dcache64(pa, len, TRUE)`, `osfmk/arm/caches_internal.h` l.43, used
  by `[patch 0028]` l.156). **First version: reject `IOCOHERENT` and `WRITEBACK` in `GPUMEM_ALLOC_ID` (clear the bits in the echoed `flags`)**
  so Turnip sets `has_cached_coherent_memory = has_cached_non_coherent_memory = false` (TURNIP-IOS.md 4.1) and every BO is WC.
- WC semantics: stores are merged/non-cacheable, loads are slow, no ordering to the GPU except by a barrier (`DSB`) before the submit ioctl; Turnip already
  assumes this for WC.

### 3.5 Freeing and lifetime rules
1. User `munmap()` removes only the user VM entry. The object stays alive through the driver's `kva` mapping and the named-entry port.
2. `GPUMEM_FREE_ID`: (a) unmap the GPU VA range from the SMMU and invalidate its TLB, (b) make sure the GPU is not still using the BO (wait for the
   retire timestamp of the last submit referencing it, or reset the context), (c) `vm_map_remove`/`vm_deallocate(kernel_map, kva, size)` (this unwires),
   (d) `mach_memory_entry_port_release(port)`. The pages are freed by the VM only when the last of {user mapping(s), kernel mapping, port} is gone, so
   a program that kept its mapping (or a forked child) can keep reading but the GPU can no longer DMA into recycled pages (the SMMU mapping is gone before
   the pages can be recycled). Never free the physical pages manually in this design; that is the main reason for choosing the VM-object route over
   `kernel_memory_allocate` + device pager.
3. Close (`d_close`) of the last fd of that open context: free all remaining BOs and contexts after quiescing the GPU.
4. Contiguous buffers: `kmem_free(kernel_map, va, size)` (check the exact free entry point for `kmem_alloc_contig` in this tree: **UNVERIFIED**;
   `[patch 0028]` never frees its DMA area).
5. Page attributes of freed WC pages are reset by the VM on page removal (`vm_resident.c` l.2282); for `kmem_alloc_contig` memory that you change to WC yourself
   with `pmap_set_cache_attributes(pn, VM_WIMG_WCOMB)`, set `VM_WIMG_DEFAULT` back before freeing.

### 3.6 `USE_CPU_MAP` / SVM BOs
With `KGSL_MEMFLAGS_USE_CPU_MAP` Turnip expects GPU VA == CPU VA (`tu_knl_kgsl.cc` `kgsl_bo_user_map`, l.208-232): the kernel would have to program the SMMU
at the user's chosen address and keep the mapping fixed. **Reject it initially** (fail the ioctl; Turnip then sets `has_set_iova = false`, TURNIP-IOS.md 4.1).
It becomes possible later because the mapping call knows the user VA (`user_addr` returned by `vm_map_enter_mem_object`).

---------------------------------------------------------------------------------------------------------------------------------

## 4. (d) Kernel structure: BSD cdev vs IOKit user client, skeleton, files

### 4.1 Decision: BSD cdev with cloning, thin front end
- Turnip does `open("/dev/kgsl-3d0", O_RDWR|O_CLOEXEC)`, `ioctl(fd, _IOWR(0x09, nr, struct), &s)` (TURNIP-IOS.md 4), `mmap(fd, ...)`. That is the
  VFS path: `open` -> `spec_open` -> `cdevsw[major].d_open` (`bsd/miscfs/specfs/spec_vnops.c` l.321-353); `ioctl` -> `ioctl()` (`bsd/kern/sys_generic.c` l.845-1015)
  which already does `copyin`/`copyout` of the struct according to the `IOC_IN`/`IOC_OUT` bits and size (l.876-1015, `size > IOCPARM_MAX` rejected) ->
  `spec_ioctl` (`spec_vnops.c` l.782-806) -> `cdevsw[major].d_ioctl(dev, cmd, data, fflag, p)`.
  Nested user pointers (`kgsl_device_getproperty.value`, `kgsl_gpu_command.cmdlist`, `synclist`, `kgsl_gpumem_bind_ranges.ranges`) need explicit
  `copyin`/`copyout` (TURNIP-IOS.md 4 header).
- IOKit user client (`IOServiceOpen` + `IOConnectCallStructMethod` + `IOConnectMapMemory64`) would need IOKit client code in Mesa for every ioctl, entitlements and a
  rewrite of the `kgsl_*` call layer; there is no `open()` at all. Reject.
- Hybrid is natural: the **GPU hardware driver** (GMU bring-up, RPMh, SMMU contexts, interrupts, power) can be an IOService (it needs the registry, DT,
  clock/regulator helpers, `IOMapper`-style services) while `/dev/kgsl-3d0` is a plain C file in `bsd/dev` that calls into it through a small C interface
  (`kgsl_hw_submit()`, `kgsl_hw_map_iova()` ...). That is the same split as `[patch 0028]` (plain C in `bsd/dev/sweet_ufs.c`, hooked from
  `iokit/bsddev/IOKitBSDInit.cpp`) and `[patch 0109]` (`iokit/Kernel/arm/SweetAccelerator.cpp`, the IOService `IOAcceleratorES` that Metal looks for). The
  accelerator nub's `start()` is the natural place to call `dm3q_kgsl_register()` once the GPU is up, so that the node appears only when the hardware works
  (ENOENT => `VK_ERROR_INCOMPATIBLE_DRIVER` in Turnip, `tu_knl.cc:345` per TURNIP-IOS.md).

### 4.2 Per-open state: why `devfs_make_node_clone`
`spec_open` calls `d_open` for every open but `spec_close` calls `d_close` **only when `vcount(vp) == 0`** (`spec_vnops.c` l.2722-2761), i.e. when the last
reference to the *vnode* is gone, across all processes. With a normal `devfs_make_node` all opens of `/dev/kgsl-3d0` share one vnode, hence one
`d_close` at the very end and no way to know which process owns which BO/context when a process dies.
`devfs_make_node_clone(dev, DEVFS_CHAR, uid, gid, perms, clone_fn, "kgsl-3d0")` (`bsd/miscfs/devfs/devfs.h` l.68-92, `devfs_tree.c` l.1255-1335) calls `clone_fn(dev, DEVFS_CLONE_ALLOC)`
on every lookup to pick a **new minor number** and creates a fresh vnode per open ("A cloned vnode is not hooked into the devnode; every lookup gets a new vnode",
`devfs_tree.c` l.~1320); `d_close` is then per open file description (also correct under `dup`/`fork`, since they share the vnode reference). The in-tree example is
`/dev/ptmx` (`bsd/kern/tty_ptmx.c` l.226-228 `devfs_make_node_clone(makedev(ptmx_major, 0), DEVFS_CHAR, UID_ROOT, GID_TTY, 0666, ptmx_clone, PTMX_TEMPLATE)`, clone callback l.471-500,
`cdevsw_add(-15, &ptmx_cdev)` l.205). Use `minor(dev)` as the index into a table of `struct kgsl_open` contexts (allocated in `clone_fn(ALLOC)` or in `d_open`; freed in `d_close`).
Caveat: `clone_fn(DEVFS_CLONE_FREE)` is only called by devfs when vnode creation fails (l.1332), so free state in `d_close`.
Exactly how ptmx guards the "same minor allocated twice concurrently" race (`ptmx_clone` comment l.~490) must be avoided: take a lock in `clone_fn` and mark the slot in use before returning.

### 4.3 Registration code skeleton (new file `bsd/dev/dm3q_kgsl.c`)

```c
#include <sys/conf.h>
#include <sys/ioccom.h>
#include <miscfs/devfs/devfs.h>
#include "msm_kgsl.h"          /* copy of Mesa's src/freedreno/vulkan/msm_kgsl.h ; struct sizes verified in TURNIP-IOS.md */

static int kgsl_open(dev_t, int, int, proc_t);
static int kgsl_close(dev_t, int, int, proc_t);
static int kgsl_ioctl(dev_t, u_long, caddr_t, int, proc_t);
static int kgsl_mmap(dev_t, off_t, user_size_t, int, ipc_port_t *, vm_object_offset_t *, vm_prot_t *);   /* phase 2 */
static int kgsl_clone(dev_t, int);

static const struct cdevsw kgsl_cdevsw = {
	.d_open = kgsl_open, .d_close = kgsl_close, .d_read = eno_rdwrt, .d_write = eno_rdwrt,
	.d_ioctl = kgsl_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_ttys = NULL,
	.d_select = eno_select, .d_mmap = eno_mmap /* -> kgsl_mmap in phase 2 */, .d_strategy = eno_strat,
	.d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = 0,
};   /* field list from bsd/sys/conf.h l.187-202; initialiser style as tty_ptmx.c l.120-150 */

int
dm3q_kgsl_register(void)
{
	int maj = cdevsw_add(-1, &kgsl_cdevsw);              /* bsd/kern/bsd_stubs.c l.229; -1 = first free slot */
	if (maj < 0) return ENXIO;
	devfs_make_node_clone(makedev(maj, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, kgsl_clone, "kgsl-3d0");
	return 0;
}
```
(uid/gid/mode: 0666 so that sandboxed third-party apps can open it; the sandbox profile must also allow `file-read-data`, `file-write-data` and `file-ioctl` on
`/dev/kgsl-3d0` - **UNVERIFIED** how `[patch 0050]` sandbox policy treats unknown device nodes; check before first app test.)

### 4.4 ioctl dispatch skeleton
```c
static int
kgsl_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, proc_t p)
{
	struct kgsl_open *ko = kgsl_open_for_minor(minor(dev));
	switch (cmd) {
	case _IOWR(0x09, 0x02, struct kgsl_device_getproperty):   return kgsl_getproperty(ko, (void *)data);   /* copyout of .value inside */
	case _IOWR(0x09, 0x13, struct kgsl_drawctxt_create):      return kgsl_ctx_create(ko, (void *)data);
	case _IOW (0x09, 0x14, struct kgsl_drawctxt_destroy):     return kgsl_ctx_destroy(ko, (void *)data);
	case _IOWR(0x09, 0x34, struct kgsl_gpumem_alloc_id):      return kgsl_bo_alloc(ko, (void *)data);     /* sets id, mmapsize, gpuaddr; echoes only supported flags */
	case _IOWR(0x09, 0x35, struct kgsl_gpumem_free_id):       return kgsl_bo_free(ko, (void *)data);
	case _IOWR(0x09, 0x4a, struct kgsl_gpu_command):          return kgsl_gpu_command(ko, (void *)data);  /* copyin cmdlist/synclist */
	case _IOWR(0x09, 0x16, struct kgsl_cmdstream_readtimestamp_ctxtid): ...
	case _IOW (0x09, 0x07, struct kgsl_device_waittimestamp_ctxtid):    ...  /* ETIMEDOUT on timeout; never spurious EAGAIN: safe_ioctl retries EAGAIN/EINTR */
	case KGSL_IOS_IOCTL_MAP:                                  return kgsl_bo_map_ioctl(ko, (void *)data);  /* phase 1 */
	default: return ENOTTY;      /* GPUOBJ_*, BIND_RANGES, AUX_COMMAND, PERFCOUNTER: graceful fallback per TURNIP-IOS.md 4.1 */
	}
}
```
Use the Darwin encodings of TURNIP-IOS.md 4.1 (`_IOW` = `IOC_IN` 0x80000000, `bsd/sys/ioccom.h` l.81-97); compile the kernel file with the same
`msm_kgsl.h` so `_IOWR(...)` evaluates to exactly the values the dylib issues (the table's `darwin cmd` column is the check).

### 4.5 Phase 1 and phase 2 mapping code
```c
/* Shared by ioctl (phase 1) and d_mmap (phase 2). `map` is the user map, never kernel. */
static kern_return_t
kgsl_bo_map_to_task(struct kgsl_bo *bo, vm_map_t map, vm_map_offset_t *addr, bool fixed, vm_prot_t prot)
{
	vm_map_kernel_flags_t f = fixed ? VM_MAP_KERNEL_FLAGS_FIXED(.vmf_overwrite = true)   /* as posix_shm.c l.~850 */
	                                : VM_MAP_KERNEL_FLAGS_ANYWHERE();
	return vm_map_enter_mem_object(map, addr, bo->size, 0, f, bo->entry, 0, FALSE,
	    prot, prot, VM_INHERIT_SHARE);                    /* never PROT_EXEC: maxprot = prot */
}

/* phase 1 */
static int
kgsl_bo_map_ioctl(struct kgsl_open *ko, struct kgsl_ios_map *a /* {id, flags, addr in/out, size} */)
{
	struct kgsl_bo *bo = kgsl_bo_get(ko, a->id);   if (!bo) return EINVAL;
	vm_map_offset_t addr = a->addr;
	kern_return_t kr = kgsl_bo_map_to_task(bo, current_map(), &addr, a->flags & FIXED, VM_PROT_READ | VM_PROT_WRITE);
	a->addr = addr;  kgsl_bo_put(bo);
	return kr == KERN_SUCCESS ? 0 : (kr == KERN_NO_SPACE || kr == KERN_INVALID_ADDRESS ? ENOMEM : EINVAL);   /* error map as kern_mman.c l.903-915 */
}

/* phase 2: called from kern_mman.c instead of ENODEV */
static int
kgsl_mmap(dev_t dev, off_t off, user_size_t len, int prot, ipc_port_t *entry, vm_object_offset_t *entry_off, vm_prot_t *maxprot)
{
	struct kgsl_bo *bo = kgsl_bo_get(kgsl_open_for_minor(minor(dev)), (uint32_t)(off >> 14));  /* id = offset >> 14 */
	if (!bo || (off & 0x3FFF) || len > bo->size) return EINVAL;
	if (prot & PROT_EXEC) return EACCES;
	*entry = bo->entry; ipc_port_copy_send(bo->entry) /* or take an extra ref; mmap() releases it */;
	*entry_off = 0;  *maxprot = VM_PROT_READ | VM_PROT_WRITE;
	return 0;
}
```
`kern_mman.c` change (phase 2), replacing l.546-549:
```c
if (vp->v_type == VCHR) {
	mmap_fcn_t *mfn = cdevsw[major(vp->v_rdev)].d_mmap;
	if (mfn == NULL || mfn == eno_mmap) { vnode_put(vp); error = ENODEV; goto bad; }
	maxprot = (fp->fp_glob->fg_flag & FREAD ? VM_PROT_READ : 0) | (fp->fp_glob->fg_flag & FWRITE ? VM_PROT_WRITE : 0);
	/* mac_file_check_mmap(...) here exactly as l.612 */
	error = (*mfn)(vp->v_rdev, file_pos + pageoff, user_size, prot, &cdev_entry, &cdev_off, &maxprot);   /* file_pos already page aligned here, l.318 */
	if (error) { vnode_put(vp); goto bad; }
	cdev_mapping = true;         /* new local; skips the ubc_getobject() branch at l.783-791 */
} else if (vp->v_type == VSTR) { ... ENODEV as before }
```
and in the later `else` branch (l.781) `if (cdev_mapping) { result = vm_map_enter_mem_object(user_map, &user_addr, user_size, 0, vmk_flags, cdev_entry, cdev_off, FALSE, prot, maxprot, VM_INHERIT_SHARE); mach_memory_entry_port_release(cdev_entry); } else { ...existing... }`.
Note `mmap()` already rounds `user_size` and handles `MAP_FIXED` (`vmf_overwrite`, l.689), `MAP_ANON` never reaches here.
`mach_memory_entry_port_release` vs `ipc_port_release_send` for dropping the extra reference: **UNVERIFIED** which one is right for a port the driver keeps; `posix_shm.c` l.652 uses the former for entries it owns. Let the hook return the reference to be dropped explicitly (e.g. via `ipc_port_copy_send`).

### 4.6 Files to touch (all in our patch series, new numbers after 0110)

Phase 1 (cdev + memory, no core VM change):
1. `bsd/dev/dm3q_kgsl.c` (new): cdevsw, clone, ioctl, BO table, `kgsl_bo_map_to_task()`.
2. `bsd/conf/files`: `bsd/dev/dm3q_kgsl.c   standard` (format as `[patch 0028]`, line `bsd/dev/sweet_ufs.c standard`).
3. Hardware side: `iokit/Kernel/arm/SweetAccelerator.cpp` (exists, `[patch 0109]`) or a new `Dm3qAdreno.cpp` + `iokit/conf/files.arm64` entry; calls `dm3q_kgsl_register()` from `start()`; matching table entry in `iokit/KernelConfigTables.cpp` (as 0109 did).
4. Header `bsd/dev/msm_kgsl.h` copy (or `#include` of a staged copy; the Mesa one needs `linux/ioctl.h`/`types.h` shims from `port/tools/turnip-ios/include/linux/`).
5. Mesa patch `port/tools/turnip-ios/mesa-turnip-ios.patch`: new helper `kgsl_ios_mmap()` replacing the 2 `mmap()` sites (`tu_knl_kgsl.cc` l.208 and l.491-496) and defining `IOCTL_KGSL_IOS_MAP`.
Phase 2 additionally:
6. `bsd/sys/conf.h` (`mmap_fcn_t` typedef, l.113), `bsd/kern/kern_mman.c` (l.546 block + the `else` at l.781), `bsd/kern/kern_mman.c` includes. Remove the Mesa helper (keep `<< 14`).
Phase 3 (only if device pager is needed): `osfmk/vm/device_vm.c`, `osfmk/vm/vm_protos.h`, `iokit/Kernel/IOMemoryDescriptor.cpp` (no change if the flag path is used).
Anything touching `tools/mkadt.py` is not needed; the GPU nodes are already in the DT per `A740-POWERON.md`.

### 4.7 Test plan (cheap steps first)
1. Host-side: compile `dm3q_kgsl.c` with the CI toolchain only; no flash.
2. On device (stub hardware): cdev with `DEVICE_GETPROPERTY` (A740 values, TURNIP-IOS.md 4.2), `DRAWCTXT_*` returning ids, `GPUMEM_ALLOC_ID`/`FREE_ID`/map backed by real WC pages but no GPU: a tiny Mach-O test program that opens the node, allocs, maps, writes a pattern, `fork()`s and checks sharing, `munmap`s, frees; verify no panic with `vm_page` accounting (wired count before/after).
3. Then `vkCreateInstance`/`vkEnumeratePhysicalDevices`/`vkAllocateMemory`+`vkMapMemory` from `libvulkan_freedreno.dylib` (no submit).
4. GPU submit only after probe 2/3 of `A740-POWERON.md`.

---------------------------------------------------------------------------------------------------------------------------------

## 5. Risks and open points (consolidated)

1. **Gunyah / SMMU addressing (UNVERIFIED).** Whether VM "physical" addresses equal the addresses the GPU SMMU contexts must translate to, and whether GPU stage-1 context banks can be programmed by us at all, is the big hardware question (A740-POWERON.md). The memory design above is independent of it.
2. **Wired-memory accounting and DoS.** Wired BOs are kernel-wired; cap per open and globally; ensure `d_close` always frees (a crashing app leaves the fd to be closed by process exit, which calls `d_close` for a cloned vnode).
3. **GPU use-after-free.** The only dangerous ordering is "pages recycled while the SMMU still maps them". Rule in 3.5(2): SMMU unmap + TLB invalidate + GPU idle for that BO before the last reference is dropped.
4. **`mach_make_memory_entry_64` cache bits and `vm_map_wire_kernel` acceptance in `kernel_map` (UNVERIFIED in this tree).** The fallback is the device pager variant (3.1/1.5A) with `kmem_alloc_contig`/`kernel_memory_allocate` pages, which `IOMemoryDescriptor` proves works but needs the `device_vm.c` flag patch.
5. **Zero-fill/WIMG ordering** (3.2) and **IOCOHERENT** semantics (3.4) UNVERIFIED; the first version avoids both (WC only, no coherent flag).
6. `ANON_MAX_SIZE` value (UNVERIFIED) bounds the size of one named entry; split large BOs into several entries and map them contiguously with `vmf_overwrite` fixed mappings like `pshm_mmap`.
7. **MAC/sandbox/AMFI**: access to `/dev/kgsl-3d0` from an app (entitlement or sandbox rule) is UNVERIFIED; for the first tests run Turnip from a platform binary/root shell like the existing test tools.
8. `mmap` hook MAC check: `mac_file_check_mmap` must run for the device case as for files (l.612) or the sandbox policy is bypassed.
9. Errors: `safe_ioctl` retries on `EINTR`/`EAGAIN` forever (`tu_knl_kgsl.cc` ~l.57), so never return `EAGAIN` for a permanent failure.
