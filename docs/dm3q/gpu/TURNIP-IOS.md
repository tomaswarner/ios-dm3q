# Turnip (freedreno Vulkan, KGSL backend) for iOS 16 arm64

Status (2026-10-21): **BUILT and import-checked.** Not run on the phone (no device access in this task).

Artifact: `third_party/mesa/build-ios-turnip/src/freedreno/vulkan/libvulkan_freedreno.dylib`
- 15,646,032 bytes, Mach-O 64-bit arm64 dylib, `LC_BUILD_VERSION platform 2 (iOS) minos 16.0 sdk 27.1`
- deps: `/usr/lib/libz.1.dylib`, `/usr/lib/libSystem.B.dylib`, `/usr/lib/libc++.1.dylib` (no LLVM)
- exports `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`
- Mesa main af17d14 (27.0.0-devel). Lavapipe dir `build-ios` untouched.

## 1. Exact build

Everything is scripted in `port/tools/turnip-ios/configure.sh` (run from anywhere; re-runnable, does `--reconfigure`
if the dir exists):

```
PATH=/opt/homebrew/opt/bison/bin:$HOME/hios/third_party/venv/bin:$PATH
cd ~/hios/third_party/mesa           # ~/hios = space-free symlink to "hackios s23"
meson setup build-ios-turnip -Dbuildtype=release -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
  -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dvulkan-layers= -Dtools= -Dzstd=disabled \
  -Dexpat=disabled -Dxmlconfig=disabled -Dcpp_rtti=false -Dzlib=disabled -Dshader-cache=disabled \
  --cross-file=$HOME/hios/port/tools/turnip-ios/mesa-turnip-ios-arm64.cross
ninja -C build-ios-turnip src/freedreno/vulkan/libvulkan_freedreno.dylib      # ~672 steps
```

Notes / gotchas hit:
- **Needs bison >= 3** (`brew install bison`, done): Xcode's bison 2.3 rejects `-Werror -Wcounterexamples` used for `ir3_parser.y`.
  The dir `/opt/homebrew/opt/bison/bin` must precede `/usr/bin` in PATH.
- Build **only the dylib target**. Plain `ninja` also builds the `libxml2`/`libarchive` subprojects (for freedreno rnn/decode
  tools) which fail to link on iOS (they pick MacOSX.sdk `libz.tbd`). They are not needed.
- `-Dzlib=enabled` must NOT be used: meson then adds `-I<MacOSX.sdk>/usr/include` and breaks libc++ header lookup. zlib is only
  needed for `gzopen/gzwrite` (u_trace, freedreno_rd_output); the iOS SDK has `zlib.h`/`libz.tbd`, so `-lz` is added in the Mesa
  patch (see 2.1) and iOS 16 has `/usr/lib/libz.1.dylib`.
- `-Wl,-undefined,dynamic_lookup` must NOT go in the cross file (it makes every meson `has_function` probe succeed, e.g. it
  wrongly "finds" `secure_getenv`). It is applied only to the Turnip dylib link via the patch.
- LLVM not used at all (`-Dllvm=disabled`); ir3 is Turnip's own compiler. `llvm-config` entries in the cross file are inert.
- `port/tools/turnip-ios/mesa-turnip-ios-arm64.cross` = `mesa-ios-arm64.cross` + `-I port/tools/turnip-ios/include` in c/cpp_args
  + `-lz` in link args (same SDK, `-arch arm64 -miphoneos-version-min=16.0`, iPhoneOS27.1.sdk).

## 2. Source changes (all in `port/tools/turnip-ios/mesa-turnip-ios.patch`, a `git diff` of 3 files; not committed)

### 2.1 Mesa patch
| File | Change | Reason |
|---|---|---|
| `src/freedreno/vulkan/meson.build` | on darwin add `-Wl,-undefined,dynamic_lookup -lz` to the dylib link | ld64 rejects the *weak* (optional) `tu_*`, `vk_cmd_enqueue_*`, `vk_common_*`, `wsi_*` entrypoint declarations that are left undefined (ELF tolerates; same approach as `src/kosmickrisp/vulkan/meson.build`). `-lz` for gz*. |
| `tu_device.cc` | `#ifdef __APPLE__` skip `pthread_condattr_setclock(CLOCK_MONOTONIC)` | Darwin has no such call (compile error). Cond var stays CLOCK_REALTIME. |
| `tu_knl_kgsl.cc` | `#ifdef __APPLE__` in `kgsl_syncobj_wait` convert the monotonic absolute deadline to realtime for `pthread_cond_timedwait` | companion of the previous change (otherwise timeouts would be wrong by the clock offset). |
| `tu_knl_kgsl.cc` | `kgsl_mmap_offset()`: mmap offset = `gem_handle << 14` on Apple (was `<< 12`), 64-bit math | **XNU `mmap()` returns EINVAL when file offset is not 16K aligned** (`kern_mman.c` ~l.304, mask 0x3FFF for normal arm64 maps). `id<<12` is only aligned for id%4==0. Emulated /dev/kgsl-3d0 must decode `id = offset >> 14`. |
| `tu_knl_kgsl.cc` | `timestamp_to_fd()` stub on Apple (ENOSYS + one-time `mesa_loge`) | no sync_file on XNU |
| `tu_knl_kgsl.cc` | `kgsl_bo_init` SHAREABLE path: clear error if `kgsl_dma_fd < 0`; `kgsl_bo_init_dmabuf` returns clear error | no dma-heap/ion on iOS |

### 2.2 Compat headers (not Mesa changes): `port/tools/turnip-ios/include/linux/`
- `ioctl.h` -> `<sys/ioctl.h>`; `types.h` -> `__u8..__u64`, `__s*`, `__kernel_size_t` (= unsigned long, LP64 both);
  `dma-heap.h` -> `struct dma_heap_allocation_data` + `DMA_HEAP_IOCTL_ALLOC` (only so `tu_knl_kgsl.cc` compiles).
- `ion/ion.h`, `ion/ion_4.19.h`, `msm_kgsl.h` are already in-tree in `src/freedreno/vulkan/` and compile unchanged with the shims.
- `util/libsync.h` compiles unchanged (poll + `SYNC_IOC_MERGE` ioctl; the ioctl just fails with ENOTTY).

### 2.3 Complete list of runtime stubs / things that cannot work on iOS (fail at runtime, not build time)
1. **sync_file fences** (`KGSL_TIMESTAMP_EVENT_FENCE` fd): `timestamp_to_fd()` returns -1/ENOSYS with log `turnip-ios: sync_file
   fences are not available on iOS`. Affects: cross-queue timestamp merge/wait_any, `vkGetSemaphoreFdKHR`/sync-fd export of a
   submitted timestamp. Normal single-queue timeline/fence/wait (`DEVICE_WAITTIMESTAMP_CTXTID`) is unaffected.
2. **sync_file ops on external fds** (`sync_wait`, `sync_merge` from libsync.h, imported SYNC_FD semaphores): `SYNC_IOC_MERGE`
   -> ENOTTY; `poll()` on non-sync fds is meaningless. Left as-is (unreachable unless an app imports a sync fd).
3. **dma-buf / ion / dma-heap** (`/dev/dma_heap/system`, `/dev/ion`): `open()` fails -> only a `mesa_logw` and `kgsl_dma_fd=-1`;
   exportable memory (VK_KHR_external_memory_fd allocation with SHAREABLE) and dma-buf import return
   `VK_ERROR_OUT_OF_DEVICE_MEMORY` with message `turnip-ios: ... unsupported on iOS`. `bo_init_new_dmaheap/ion/ion_legacy` are
   compiled but dead.
4. **mmap of the device fd**: stock XNU `mmap()` on a VCHR vnode returns **ENODEV** (`kern_mman.c` ~l.546, "/dev/zero hack"
   branch). The dylib still calls `mmap(fd=/dev/kgsl-3d0, off=id<<14)` for every BO (`kgsl_bo_map`, `kgsl_bo_user_map`). The port's
   XNU must add a character-device mmap hook (or Mesa must be changed to a different mapping scheme). **Biggest open
   kernel-side requirement.**
5. `KGSL_PROP`/ioctls not implemented by the emulation fall back gracefully (see section 4 minimal set).
6. `PERFCOUNTER_READ` and `GPU_COMMAND` profiling buffer are only used with `HAVE_PERFETTO`/u_trace; perfetto is off in this build.
7. `VK_KHR_display`, WSI platforms: none built (`-Dplatforms=` ; headless WSI only). A real iOS surface still needs a WSI layer
   (separate work; the m2v/Metal stack path).

## 3. Import check (iOS 16.7 20H392 shared cache)

Method: `nm -m libvulkan_freedreno.dylib | grep '(undefined)'`; exports of every `/usr/lib/system/*`, `libSystem*`, `libc++*`,
`libc++abi*`, `libz.1.dylib`, `libresolv`, `libm` image in `apple/dsc/20H392__iPhone10,3_6/dyld_shared_cache_arm64` collected with
`ipsw dyld macho <dsc> <image> -n` (20,789 external symbols), compared with `comm`.
- 5,822 undefined entries total.
- **5,607 are weak** (`weak external`): 5,601 are "dynamically looked up" optional Vulkan entrypoint declarations
  (`tu_*<chip>`, `vk_cmd_enqueue_*`, `vk_common_*`, `wsi_*`, `tu_rmv_*`) = deliberate weak/unresolved by design (null at runtime, never
  called); 6 are weak libc pthread/mutexattr symbols that resolve from libSystem.
- **215 strong imports** (187 libSystem, 29 libc++ incl. `___cxa_*`, `__Znwm`, `__ZdlPv`, 5 libz `gz*` + `dyld_stub_binder`):
  **all 215 exist in the cache except `_memset_pattern16`**, which `ipsw dyld symaddr` shows as
  `__platform_memset_pattern16 re-exported` in libsystem_c (present; same as lavapipe note).
- 0 strong undefined symbols resolved via flat/dynamic lookup. Nothing from `libLLVM`, libelf, libdrm, libxml2.

## 4. KGSL interface the emulated `/dev/kgsl-3d0` must provide

Device node: `open("/dev/kgsl-3d0", O_RDWR|O_CLOEXEC)` (tu_knl.cc:345). ENOENT => driver reports VK_ERROR_INCOMPATIBLE_DRIVER.
`fstat`/`major/minor` of the node are not used for KGSL. Device is used from one fd for everything (`local_fd == device->fd`).

### 4.1 ioctls actually used (group 0x09; struct sizes verified by compiling msm_kgsl.h; all LP64, same on Darwin/Linux)
`darwin` = the number the iOS dylib issues (Darwin `_IOW` = `IOC_IN` 0x80000000, not Linux's 0x40000000: only the 3 pure-`_IOW`
ones differ). XNU auto copyin/copyout by the direction bits and size; nested pointers need explicit `copyin`.

| ioctl | nr | darwin cmd | linux cmd | struct (size) | purpose / call site |
|---|---|---|---|---|---|
| DEVICE_GETPROPERTY | 0x02 | 0xc0180902 | same | kgsl_device_getproperty (24) {type, void* value, sizebytes} | all property queries, 4.2 |
| DRAWCTXT_CREATE | 0x13 | 0xc0080913 | same | kgsl_drawctxt_create (8) {flags in = SAVE_GMEM\|NO_GMEM_ALLOC\|PREAMBLE (0x13), drawctxt_id out} | per queue (`kgsl_submitqueue_new`) |
| DRAWCTXT_DESTROY | 0x14 | 0x80040914 | 0x40040914 | kgsl_drawctxt_destroy (4) {drawctxt_id} | queue close |
| GPUMEM_ALLOC_ID | 0x34 | 0xc0300934 | same | kgsl_gpumem_alloc_id (48) {id, u32 flags, size, mmapsize, gpuaddr, pad[2]} | every BO; also feature probes (size 0x1000 then FREE_ID) |
| GPUMEM_FREE_ID | 0x35 | 0xc0080935 | same | kgsl_gpumem_free_id (8) {id} | BO destroy, probe cleanup |
| GPUMEM_GET_INFO | 0x36 | 0xc0480936 | same | kgsl_gpumem_get_info (72) {gpuaddr, id, flags, size, mmapsize, useraddr, pad[4]} | only sparse VMA (query iova of VBO); input `id` |
| GPUMEM_BIND_RANGES | 0x56 | 0xc0200956 | same | kgsl_gpumem_bind_ranges (32) {u64 ranges(ptr to kgsl_gpumem_bind_range[32B]), nents, size, id, flags} | sparse binding + `kgsl_is_virtual_bo_supported` probe |
| GPUOBJ_ALLOC | 0x45 | 0xc0300945 | same | kgsl_gpuobj_alloc (48) {u64 size, flags, va_len, mmapsize, u32 id, metadata_len, u64 metadata} | sparse virtual BO (`KGSL_MEMFLAGS_VBO`) |
| GPUOBJ_FREE | 0x46 | 0x80200946 | 0x40200946 | kgsl_gpuobj_free (32) {flags, priv, id, type, len} | free VBO |
| GPUOBJ_IMPORT | 0x48 | 0xc0200948 | same | kgsl_gpuobj_import (32) {priv ptr -> kgsl_gpuobj_import_dma_buf{int fd}, priv_len, flags, type=DMABUF, id} | dma-buf import: **stubbed out on iOS** (never issued) |
| GPUOBJ_INFO | 0x47 | 0xc0300947 | same | kgsl_gpuobj_info (48) | dma-buf import path: **never issued on iOS** |
| GPU_COMMAND | 0x4a | 0xc040094a | same | kgsl_gpu_command (64) | normal submit, see 4.4 |
| GPU_AUX_COMMAND | 0x57 | 0xc0300957 | same | kgsl_gpu_aux_command (48) (+ kgsl_gpu_aux_command_generic 24 -> kgsl_gpu_aux_command_bind 32) | sparse bind submits only |
| TIMESTAMP_EVENT | 0x33 | 0xc0200933 | same | kgsl_timestamp_event (32) type=FENCE(2) | **stubbed on iOS; never issued** |
| CMDSTREAM_READTIMESTAMP_CTXTID | 0x16 | 0xc00c0916 | same | kgsl_cmdstream_readtimestamp_ctxtid (12) {context_id, type=KGSL_TIMESTAMP_RETIRED(2) in, timestamp out} | `kgsl_poll_timestamp` |
| DEVICE_WAITTIMESTAMP_CTXTID | 0x07 | 0x800c0907 | 0x400c0907 | kgsl_device_waittimestamp_ctxtid (12) {context_id, timestamp, timeout ms} | fence wait; returns 0, or errno ETIMEDOUT (timeout), EINTR/EAGAIN (retried), EDEADLK/EPROTO/ENOENT => VK_ERROR_DEVICE_LOST |
| PERFCOUNTER_READ | 0x3b | 0xc018093b | same | kgsl_perfcounter_read (24) | only under HAVE_PERFETTO: **not compiled** |

**Minimal emulation to get a device up and running Vulkan:** GETPROPERTY, DRAWCTXT_CREATE/DESTROY, GPUMEM_ALLOC_ID, GPUMEM_FREE_ID,
GPU_COMMAND, READTIMESTAMP_CTXTID, WAITTIMESTAMP_CTXTID, plus the mmap path. Everything else may return an error:
- `GPUMEM_ALLOC_ID` with `USE_CPU_MAP` (1<<28) failing/not echoed => `has_set_iova=false` (no replayable/fixed-address BOs).
- `GPUMEM_ALLOC_ID` with `IOCOHERENT\|WRITEBACK<<26` failing or not echoing the flags => `has_cached_coherent_memory=false`.
  (The flags field is echoed back and compared: `(req.flags & flags) == flags`.)
- `GPUOBJ_ALLOC`(VBO)/`BIND_RANGES` failing => `has_sparse/has_lazy_bos=false` (no sparse binding, no lazy memory).

ioctl calls loop on EINTR and EAGAIN (`safe_ioctl`).

### 4.2 Device properties (KGSL_PROP_*, `ioctl DEVICE_GETPROPERTY`; `sizebytes` = exact size given)
| prop | id | buffer | required | A740 value |
|---|---|---|---|---|
| DEVICE_INFO | 0x1 | `struct kgsl_devinfo` (40 B: u32 device_id, u32 chip_id, u32 mmu_enabled, u64 gmem_gpubaseaddr, u32 gpu_id, u64 gmem_sizebytes) | yes | `chip_id = 0x43050a01` (core 0x43, major 5, minor 0x0a, patch 01 -> `FD740`/a7xx_gen2 in freedreno_devices.py; gpu_id derived by Turnip = 6760, field `gpu_id` itself ignored), `gmem_sizebytes = 0x300000` (3 MiB, A740 per msm catalog; overridable with env `TU_GMEM`), `mmu_enabled=1`, `device_id=0`, `gmem_gpubaseaddr` ignored (set `0x100000`) |
| UCHE_GMEM_VADDR | 0x13 | u64 | yes | `0x100000`. Turnip adds it to GMEM addresses (`gmem_base`, tu_clear_blit.cc/tu_cmd_buffer.cc), so it must equal what the kernel programs in `UCHE_GMEM_RANGE_MIN` (msm uses 0x100000 for a6xx/a7xx). Keep self-consistent. |
| HIGHEST_BANK_BIT | 0x17 | u32 | yes | `16` (matches `highest_bank_bit = 16` in freedreno_devices.py for 740) |
| UBWC_MODE | 0x1B | u32 | yes | `KGSL_UBWC_4_0 = 4` (SM8550 class; -> 8-channel macrotile, swizzle levels 0x6). Values 1..6 accepted, others fail device init. |
| UCHE_TRAP_BASE | 0x2F | u64 | no | if ioctl fails Turnip uses `0x1fffffffff000`; either return that or fail |
| GPU_RESET_STAT | 0x9 | u32 in/out: in = context id, out = KGSL_CTX_STAT_* | per `vkGetDeviceStatus`-style check | return `0` (NO_ERROR); `2` (INNOCENT_CONTEXT_RESET_EXT) is also tolerated; any other nonzero => VK_ERROR_DEVICE_LOST |
| IS_RAYTRACING_ENABLED | 0x2D | u32 | no | `0` (ignored on A740: fuse not present) |
| GPU_VA64_SIZE | 0x2C | u64 | no | optional; if unsupported va_size stays 0 (heap = os_get_gpu_heap_size). If returned it caps the Vulkan heap size (`MIN2(ram, va_size)`); use `1<<48` or fail |

Any other KGSL_PROP is never queried by Turnip (checked with grep over `src/freedreno/vulkan`). `device_info` failure aborts device
enumeration (`goto fail`). Also derived by Turnip, not from KGSL: `va_start = 0x100000000` (fake, unused).

### 4.3 Memory conventions
- `GPUMEM_ALLOC_ID`: in `size`, `flags` (u32): cache mode in bits 26-27 (`KGSL_CACHEMODE_WRITECOMBINE=0` for non-cached, `WRITEBACK=3` for
  host-cached), `GPUREADONLY` (1<<24), `USE_CPU_MAP` (1<<28), `IOCOHERENT` (1<<31), alignment hint `log2(align) << 16`
  (`KGSL_MEMALIGN_SHIFT`; only 64K/1M used), `FORCE_32BIT` is bit 32 (cannot be in this u32 flags). Out: `id` (non-zero, used
  as `gem_handle`; indexes Turnip's BO table), `mmapsize` (= BO size, page-multiple; Turnip uses it as `bo->size`), `gpuaddr` (GPU VA = `bo->iova`),
  except with USE_CPU_MAP where gpuaddr=0 and the later mmap fixes it.
- **CPU mapping of a BO: `mmap(placed_addr|NULL, bo->size, PROT_READ|PROT_WRITE, MAP_SHARED [|MAP_FIXED], fd_of_/dev/kgsl-3d0, offset = id << PAGE_SHIFT)`.**
  Linux uses PAGE_SHIFT 12; this build uses **14** on Apple (16K pages, see 2.1). Mapped lazily on `vkMapMemory` (`kgsl_bo_map`) and unmapped at BO destroy;
  `REPLAYABLE` BOs are mapped at once at alloc, CPU address == GPU address (SVM) with `never_unmap`.
  Cache attrs: WC for non-cached memory; Turnip does explicit cache maintenance only for cached types (it assumes `DC CVAC`-style user ops).
- GPU VA of all BOs and every IB: Turnip submits **GPU addresses** (`gpuaddr = bo->iova + offset`) together with the gem id.
- Sparse (optional): VBO = `GPUOBJ_ALLOC(flags=VBO[|NO_MAP_ZERO][|USE_CPU_MAP])`; binds via `GPUMEM_BIND_RANGES` ioctl or GPU_AUX_COMMAND_BIND.
- Page-size caveat: GPU pages are 4K but the CPU is 16K; all KGSL sizes returned in `mmapsize` should be multiples of 16K to be mappable.

### 4.4 Command submission (`GPU_COMMAND`, tu_knl_kgsl.cc `kgsl_queue_submit`)
`flags = KGSL_CMDBATCH_SUBMIT_IB_LIST (4)`, `cmdlist` = array of `kgsl_command_object{gpuaddr, size, flags=KGSL_CMDLIST_IB, id}` (32 B each, `cmdsize=32`),
optional single `synclist` entry (`kgsl_command_syncpoint{type, priv ptr, size}`): `TYPE_TIMESTAMP` (`kgsl_cmd_syncpoint_timestamp{context_id, timestamp}`,
wait for an earlier timestamp of a context) or `TYPE_FENCE` (sync_file fd: not reachable on iOS). `context_id` = drawctxt id.
Out: `timestamp` (per-context monotonically increasing u32, wrap-safe compare used; Turnip stores it as `queue->fence`).
Failure => `VK_ERROR_DEVICE_LOST`. Empty submits are handled in userspace (never sent). The kernel must schedule the IBs through the
Adreno CP (A7xx: SQE/GMU path) and bump the retired timestamp that READTIMESTAMP/WAITTIMESTAMP report.
`GPU_AUX_COMMAND` only for sparse binds (`kgsl_gpu_aux_command{flags=BIND[|SYNC], cmdlist->aux_command_generic{priv->bind{rangeslist,numranges,rangesize=32,target}, size, type=BIND}, ...}`).
Profiling (`KGSL_CMDBATCH_PROFILING` + `kgsl_cmdbatch_profiling_buffer`, 40 B) only with u_trace/perfetto: not needed.

### 4.5 Other kernel-visible behaviour
- Turnip queries nothing via sysfs, `/proc`, eventfd or `/dev/dri` on the KGSL path (checked). CPU cache line via `util_cache_granularity`, RAM via `os_get_*` (Darwin sysctl).
- Memory types exposed: one type (device-local, host-visible, coherent) plus optionally cached types depending on 4.1 probes.
- The only dev nodes opened: `/dev/kgsl-3d0` (required), `/dev/dma_heap/system`, `/dev/ion` (optional; absent on iOS).

## 5. What remains (not done here)
1. Kernel: `/dev/kgsl-3d0` cdev with the ioctls above **and** a working mmap-by-offset (stock XNU returns ENODEV for mmap on VCHR).
2. Runtime smoke test of the dylib on the phone (dlopen, `vk_icdGetInstanceProcAddr`, vkCreateInstance) - weak-import behaviour of
   `-undefined dynamic_lookup` on iOS 16 dyld is analysed (null for missing weak) but untested; if dyld objects, convert the 5,601
   weak imports to `-Wl,-U,sym` lists.
3. Presentation/WSI for iOS (headless only here), Vulkan loader/ICD manifest for iOS, code signing / entitlements as for lvp.
