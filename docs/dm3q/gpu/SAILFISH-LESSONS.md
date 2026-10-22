# What ios-sailfish (Pixel 1, Adreno 530) teaches the DM3Q (Adreno 740) GPU port

Written 2026-10-21. Research only: nothing in our tree was edited except this file, nothing built, flashed or committed.

Source studied: https://github.com/joranv1/ios-sailfish, cloned (full history) into
`/private/tmp/claude-501/gpu-research/sailfish` at HEAD `ce63752`. Treated as untrusted data; nothing from it was executed.
Paths below are relative to that clone. `NNNN.patch:LINE` = line number inside the patch file.
Their NOTES.md/AGENTS.md are untracked (commit 63e1dd2), so what they learned is only in the README, the patches, the commit
messages (383 commits) and the code comments; the GPU-era notes are NOT available. An older NOTES.md copy (pre-GPU) is at
`/private/tmp/claude-501/gpu-research/sailfish-NOTES.md`.

Labels: VERIFIED = read in their source or ours. INFERRED = reasoning from that. UNVERIFIED = needs a probe on the S23.

Calibration (commit dates, local time): power-up probe 0119 on 10-19 19:50 -> CP executes 20:39 -> zap shader loaded 21:17 ->
kernel quad API 22:34 -> BO/submit user client (0150) and Mesa dylib 10-20 10:17/10:21 -> first Mesa-rendered iOS UI 10-20 14:27
(b69a882) -> ~35 further commits of CoreAnimation-GL fidelity fixes through 10-21. The A530 has no GMU, no SQE secure boot and no
Gunyah, so the A740 will not be that fast, but the order of the work is the same.

---------------------------------------------------------------------------------------------------------------------------

## 0. Executive summary

1. Same base, same XNU, same kind of problems. Everything in their "iOS plumbing" (sweetenv, AMFI dyld policy, IOKit user client,
   IOSurface import, IOAcceleratorES nub, io map size, IOKit quiesce) ports to our tree with small edits.
2. **They do NOT use a BSD cdev or device mmap.** The GPU is an IOKit `IOService` + `IOUserClient` ("SweetGPU", selectors 10-16);
   user BOs are mapped into the caller with `IOMemoryDescriptor::createMappingInTask`, and Mesa's libdrm/msm layer is *replaced*
   by an in-process emulation that calls `IOConnectCall*`. This removes the blocker in our `KGSL-XNU-DESIGN.md` (XNU `mmap()` on a
   VCHR returns ENODEV) without touching `kern_mman.c`. It is the fastest route and I recommend it for Turnip too (section 3.4).
3. Their GPU render path is **GL, not Metal**: Mesa freedreno Gallium (GLES 3.1 context) behind an `EAGLContext` method-swizzling shim
   inside backboardd, which makes CoreAnimation use its own `CA::OGL::GLESContext` renderer. CA picks GL simply because there is no
   Metal device (their `IOAcceleratorES` nub has no `MetalPluginName`) and `initWithAPI:3` succeeds.
4. Mesa Gallium freedreno supports a7xx today (templated `A7XX` code in fd6_*.cc; chip id 0x43050a01 is in
   `freedreno_devices.py` as `FD740`; Mesa CI runs GLES 3.2 + GL 4.6 CTS on `FD750`). So the GL route is available for the A740.
5. Their zap-shader-from-XNU code is short and matches our design in `A740-POWERON.md` section 6 almost line for line
   (same SMCCC encoding, same arginfo, same order). Differences that matter for us are in section 4 (16K vs 8K carve-out alignment,
   `IS_SUPPORTED` is never used, where in the sequence the zap goes, secure-mode symptoms to look for).
6. Recommendation (details in section 3.6): the kernel GPU driver and the user-client are the shared critical path. Build them
   first in Sailfish's style. Then do **Turnip + SweetMetal first**, because Metal already renders our whole UI on lavapipe and the
   only change is the Vulkan ICD; keep the GL route as the fallback if (a) Turnip/KGSL-shim bring-up on a7xx drags, or (b) we want a
   cheap "GPU draws something in backboardd" milestone before metal2vulkan is robust. Do not do both fully.

---------------------------------------------------------------------------------------------------------------------------

## 1. Architecture of their GPU stack

### 1.1 Layers

```
CoreAnimation GLES renderer (CA::OGL::GLESContext, in backboardd)
  -> EAGLContext methods swizzled by sweetgl.dylib (tools/sweetgl/eagl.c) -> 1036-slot GLI dispatch table -> Mesa GLES 3.1
     -> Gallium freedreno (a5xx for them) -> libdrm/msm *emulation* (tools/sweetgl/sweet_drm.c)
        -> IOConnectCall* on class "SweetGPU" (iokit/Kernel/arm/SweetGPU.cpp, patches 0138/0150/0151)
           -> bsd/dev/sweet_gpu.c: GPU SMMU context bank + 4K page tables, ring, CP_INDIRECT_BUFFER, fence, zap, clocks
```
Display: `AppleCLCD::swap` (iokit/Kernel/arm/SweetFramebuffer.cpp, patch 0164) takes CA's IOSurface and scales it onto the panel
with a hand-assembled GPU compute kernel (`sweet_gpu_quad`), double-buffered, flipped through MDP registers. This is separate from Mesa.

### 1.2 Kernel files

| File (in xnu) | Introduced by | Role |
|---|---|---|
| `bsd/dev/sweet_gpu.c` (~2200 lines) | 0119-0136, 0137, 0138, 0150, 0155, 0156, 0161, 0164 | bring-up (PMIC rail, GDSC, clocks), GPU SMMU, ring/CP, zap, VA allocator, quad kernels, submit/wait/hang detect |
| `bsd/dev/sweet_gpu.h` | 0138, 0150 | kernel API: `sweet_gpu_vm_map/unmap`, `sweet_gpu_quad`, `sweet_gpu_submit`, `sweet_gpu_wait`, `sweet_gpu_get_info` |
| `bsd/dev/sweet_gpu_shaders.h` | 0131-0143 | machine code of hand-written compute kernels (built by `tools/ir3asm` from `tools/gpu-shaders/*.asm`) |
| `iokit/Kernel/arm/SweetGPU.cpp` | 0138, 0150, 0151 | IOService `SweetGPU` (IOResources match) + `SweetGPUUserClient` |
| `iokit/Kernel/arm/SweetSurface.cpp` (existing from ios-sweet 0046) | 0152-0162 | IOSurface root; the GPU imports its `IOMemoryDescriptor` |
| `iokit/Kernel/arm/SweetFramebuffer.cpp` | 0164, 0166, 0167, 0168 | AppleCLCD, GPU present, `IOAcceleratorES` nub |

### 1.3 User-client API (all `IOConnectCall*`; `0138.patch:539-600`, `0150.patch:865-945`, `0151.patch`)

Service lookup: `IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"))`, `IOServiceOpen(.., type 0)`
(`tools/sweetgl/sweet_drm.c:55-76`). The service only registers after the GPU is up, so callers retry (their self test retries 120 s,
`sweetgl.c:278-292`). All selectors return `kIOReturnNotReady` until `sweet_gpu_ready()`.

| Sel | Name | In | Out | Notes |
|---|---|---|---|---|
| 0 | map user memory | addr, len (+flags) | GPU VA | wires user pages (`withAddressRange` task) and maps them into the GPU VA window; 64 MB cap |
| 1 | map IOSurface | surface id | GPU VA, alloc size | |
| 2 | unmap | GPU VA | | |
| 3 | quad | `struct sweet_gpu_quad` | | kernel-built compute kernel (scale/blend), synchronous, bounds-checked against this client's maps |
| 10 | bo new | size | handle, GPU VA, **user address** | `IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, KernelUserShared)`, zeroed, wired; mapped into the caller with `createMappingInTask(fTask, 0, kIOMapAnywhere)` |
| 11 | bo free | handle | | waits for the last submit that listed the bo (2 s); if the GPU is hung the memory is leaked on purpose, not freed under a possibly waking GPU |
| 12 | bo import | IOSurface id | handle, GPU VA, size | maps the surface's pages (`getPhysicalSegment(.., kIOMemoryMapperNone)`); retains the `SweetSurface` |
| 13 | submit | struct: `{u32 nr_ibs, nr_bos}` + `ibs[] {u64 iova, u32 dwords, u32 pad}` + `u32 handles[]` (passed as input struct or `structureInputDescriptor` if > 4 KB) | sequence number | validates every IB lies inside one GPU mapping, cleans+invalidates CPU cache for every listed bo, emits `CP_INDIRECT_BUFFER_PFE` per IB then `CP_SET_RENDER_MODE` + `CP_EVENT_WRITE CACHE_FLUSH_TS` that writes the sequence number to a fence page |
| 14 | wait | seqno, timeout ms (0 = poll) | | handled *without* the client lock; spins 2 ms then `delay(200)`; fence stuck 2 s with work outstanding = GPU declared hung, service off until reboot |
| 15 | info | | chip id, GMEM size, GMEM base, max Hz, (fence page addr, dropped in 0151) | |
| 16 | bo cpu prep | handle | | after the last submit completed, drop the CPU's cached lines so the CPU sees GPU writes |

Caps: 16384 BOs/client, 256 MB/BO, 768 MB/client, 256 IBs/submit. **One shared GPU address space for all clients** (stated in the
header comment of 0150: a client can read/write another's GPU memory; accepted for bring-up).

### 1.4 BO / VA / fence model

- **BO memory** = wired kernel pages (16K pages) from an `IOBufferMemoryDescriptor`. They are given to the GPU through the GPU SMMU's
  stage-1 page table: `sweet_gpu_vm_map(pa[], npages)` (0150.patch ~lines 40-200) builds 4 KB PTEs (each 16K kernel page = 4 PTEs),
  GPU VA window 8-16 GB (`GVA_BASE 0x200000000`, `0137.patch:76`), L1/L2 pre-built, L3 tables on demand, page granular first-fit with a
  rover, 16384 mapping slots, `g_owner[page]` reverse map. Below 8 GB the SMMU keeps the identity map of DRAM for kernel-internal buffers.
- **Padding, not guards** (0156): each mapping is followed by 8 pages (128 KB) that all map one shared scratch page. An unmapped
  guard page made the A530 hang on texture/vertex fetch overruns even with faults terminated.
- **Fence**: one global monotonically increasing u32 (`g_submit_seq`); a page written by the CP via `CACHE_FLUSH_TS`; waiters poll it
  after `flush_dcache64`. Each BO remembers the last sequence that listed it. No per-context timestamps, no sync files.
- **How user space maps BOs without device mmap** (the question you asked): the *kernel* creates the user mapping inside selector 10
  (`createMappingInTask`) and returns the user address. `sweet_drm.c` keeps `{iova, map, size}` per handle; Mesa's `os_mmap(fd, offset)`
  in `freedreno_bo.c` is replaced by `sweet_drm_bo_map(fd, offset)` (`tools/sweetgl/mesa.patch:6-17`), which turns the fake offset
  `handle << 32` back into that address (`sweet_drm.c:456-465`). `os_munmap` is a no-op; the mapping lives until the BO is closed.
- **Cache coherency (the big pitfall, 0150 -> 0151)**: first attempt mapped BOs write-combined in the caller (`kIOMapWriteCombineCache`).
  **That froze the boot (0150a)**: a WC user mapping of managed kernel pages makes the pmap rewrite the physmap PTEs. Fix: map them
  *cached* and do software maintenance: `flush_dcache64(pa, PAGE_SIZE, TRUE)` over every page of every bo in a submit's handle list
  (clean+invalidate, `0151.patch` ~submit()), plus selector 16 after waiting, before the CPU reads GPU output. GPU PTEs are normal
  non-cacheable (AttrIndx 0, outer-shareable). Comment in the code: "ponytail: whole bo every submit; track CPU-dirty bos if this shows in profiles".
- **msm emulation** (`tools/sweetgl/sweet_drm.c`, 529 lines): replaces libdrm symbols (`drmOpenWithType`, `drmCommandWrite[Read]`, `drmIoctl`,
  `drmGetVersion`, `drmGetCap`, syncobj/prime stubs returning -ENOSYS) and answers the msm UAPI: GET_PARAM (GPU_ID, CHIP_ID, GMEM_SIZE,
  GMEM_BASE, MAX_FREQ, TIMESTAMP, PRIORITIES=1, FAULTS/SUSPENDS=0), GEM_NEW, GEM_INFO (GET_OFFSET, GET_IOVA, GET_FLAGS = WC), GEM_CPU_PREP/FINI,
  GEM_SUBMIT (each CMD_BUF becomes an IB at `bo iova + submit_offset`; relocs are refused: needs UAPI 1.7 softpin, `drmVersion` says 1.7.0),
  WAIT_FENCE (converts the absolute CLOCK_MONOTONIC deadline to ms), GEM_MADVISE, SUBMITQUEUE_* (fake ids). The fake "DRM fd" is a dup of
  `/dev/null`. No fence fds, no syncobj, no dma-buf.

### 1.5 EAGL shim and the CA renderer path (`tools/sweetgl/eagl.c`, `sweetgl.c`)

- Injection: `sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib` (README boot args). `sweetgl.dylib` = Mesa static libs
  + shim, ad-hoc signed (`codesign -f -s -`), padded to 64 MiB so it can be rewritten in place on the phone (`build.sh:48-55`). Constructor
  `sweetgl_load` (`sweetgl.c:297-315`): in SpringBoard it only installs the vibrant-fill workaround; elsewhere (backboardd) it calls
  `sweetgl_eagl_install()`.
- `sweetgl_eagl_install` (`eagl.c:2207-2235`) swizzles `EAGLContext`: `initWithAPI:properties:`, `dealloc`, `API`, `setParameter:to:`,
  `getMacroContextPrivate`, `texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:`,
  `attachImage:toCoreSurface:invertedRender:`, `+setCurrentContext:`, `+currentContext`. For contexts it created the original EAGL code
  never runs (it falls back to Apple's EAGL if Mesa context creation fails, `eagl.c:1740-1745`).
- **How CA picks GL**: nothing selects it explicitly (comment at `eagl.c:2206`: "CA picks its GLES renderer by itself once
  initWithAPI:3 succeeds"). Metal is passed over because no `_MTLDevice` exists: their `IOAcceleratorES` service is a bare `IOService`
  without `MetalPluginName` (`0166.patch:42-68`, with `sweetaccel=0` to omit). Its only purpose is that `CAMetalContextCreate` polls
  `IOServiceMatching("IOAcceleratorES")` 300 x 100 ms in every CA process (30 s of boot time, `0166.patch` comment, idea credited to
  ios-topaz 0138). Earlier (10-19) they ran CA's *CPU* renderer `CA::OGL::SW` with `CA_BACKDROP_SCALE=1` via sweetenv (commit 37d93c0) and
  tried offloading `CA::OGL::SW::draw_polygon` to the GPU (`tools/sweetgpu-ca`, hook at cache address 0x188441a58), then dropped that for GL.
- **The GLI dispatch table**: CA's GL renderer does not call `gl*` symbols; it calls `disp[i](rctx, ...)` through the table returned by
  `getMacroContextPrivate` (`{rctx, disp[1036]}`). Layout was extracted from `GLEngine`'s `gliInitDispatchTable`
  (`tools/gles-dispatch-layout.py` -> `tools/sweetgl/ios-dispatch.txt`, 410 names); `gen-dispatch.py` generates one wrapper per slot that
  makes the context current and calls the Mesa entry of the same name (hand-written ones in `eagl.c`, e.g. `sgl_Flush` = `glFinish`).
- **Mesa side** (`sweetgl.c`): a minimal Gallium frontend (after Haiku's hgl), surfaceless, `st_api_create_context` with GLES 3.1,
  `fd_drm_screen_create_renderonly(fd, NULL, &cfg)`; renderonly stubbed. Env set before screen creation: `FD_MESA_DEBUG=sysmem,nofp16`
  (**sysmem**: GMEM tile restore/resolve overran unpadded IOSurfaces and faulted on the display surface, boot 0154a; **nofp16**: CA's
  shaders assume Apple's mediump is enough, fp16 broke pod corners/wallpaper), `IR3_SHADER_DEBUG=nofp16`.
- **IOSurface as texture/render target**: `get_egl_image` frontend hook (`eagl.c:1364-1386`), `bind_surface` (`:1619`), import via
  selector 12; one Mesa resource per IOSurface (commit d140e12, so render-then-sample through two textures is ordered); surfaces with
  non-64-byte rows are copied into a texture instead (`copy_surface`, `:1559`). Patch `mesa.patch` allows imported linear strides that are
  64-byte multiples on a5xx (`freedreno_resource.c`).
- **Sync model**: `glFlush` == `glFinish` (`eagl.c:1297-1330`, "CA hands rendered IOSurfaces to the display without waiting for the GPU"),
  with a 10 s flush-time statistics line. Simple and safe, costs latency/CPU idle.
- **Mesa build** (`tools/sweetgl/mesa-setup.sh`): `meson setup ../mesa-ios --cross-file ios-cross.txt --buildtype=release
  -Db_ndebug=true -Ddefault_library=static -Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dgallium-drivers=freedreno
  -Dvulkan-drivers= -Dfreedreno-kmds=msm -Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dzlib=disabled
  -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dperfetto=false -Dgallium-va=disabled -Dgallium-rusticl=false -Dtools=
  -Dteflon=false`. Cross file: Xcode 15.4 iPhoneOS17.5 SDK, `-miphoneos-version-min=16.0`, `host_machine.system()='darwin'`,
  `subsystem='ios'`, `pkg-config='false'`, extra `-I tools/sweetgl/include` (compat headers; the dir is not in the repo listing, it is
  generated/local). `mesa.patch` (48 lines): meson `subdir('drm')` also on darwin, the `os_mmap` hook, one a5xx stride tweak.
  `build.sh` then builds the static libs by name (libmesa, libgallium, libfreedreno, freedrenowinsys, libfreedreno_drm/ir3/fdl/common, isa,
  glsl, nir, glapi bridge, util...) and links them into one dylib with only `sweetgl_*` exported (`-exported_symbols_list`).
  Our `docs/dm3q/gpu/TURNIP-IOS.md` build already solves the same darwin problems for Turnip (bison, zlib, no `-undefined dynamic_lookup`
  in the cross file); reuse that cross file for Gallium.
- **CA GL renderer gaps they had to emulate** (this is the fidelity cost of the GL route; commits 4e104d9, 0a62c62, 8f5cd89, c3a5101, 48b0226):
  `GL_TEXTURE_RECTANGLE` (targets rewritten to 2D, `sampler2DRect` shaders rewritten with a per-draw texel scale), `GL_EXT_shader_framebuffer_fetch`
  (CA drops blend-mode layers without it), CA texture functions that exist only in its Metal path (0x2d vibrant colour matrix, 0x14
  colour matrix w/o premultiply, 0x26/0x27 variable blur) patched into QuartzCore's data tables in-process (`sgl_install_texfns`,
  `eagl.c:2150-2200`, `vm_protect(..VM_PROT_COPY)` writes at hard-coded 20H392 shared-cache addresses, e.g. `0x1d5b44a10`, `0x18855a1e4`),
  CA's "format 0" images being A,R,G,B in memory, Apple tile-semantics (a clear does not reach memory before the pass ends), a hook at
  `0x18840433c`. They are **cache-version specific**.

---------------------------------------------------------------------------------------------------------------------------

## 2. What ports directly to our tree

Our patch numbers: ours are `0100-0110` for dm3q after the ios-sweet `0001-0081`; their numbers collide (their 0104-0168 are *their*
Pixel work on top of `0001-0103`). Do not apply theirs by number. The right mapping:

| Their patch | What | Port to SM8550 / our tree | Priority |
|---|---|---|---|
| **0104** sweetenv boot-arg (`kern_exec.c`, 86 lines) | `sweetenv=NAME:VAR=VAL[,VAR=VAL]` appends env vars to exec of NAME (uses `exec_add_user_string`, `ip_argspace`, `ip_envc`) | Applies as is (guard `ARM64_BOARD_CONFIG_SWEET` -> our board macro; our 0109 uses `#if defined(SWEET)`: check which our tree defines). Hunk context is generic kern_exec.c. | Medium: not needed for Metal (Metal loads our bundle itself), needed to inject GL, tracers, or `CA_*` knobs without re-signing launchd plists |
| **0163** several NAME entries (`;`) | spec 512 bytes, `;` separated entries | Apply right after 0104 (as a single combined patch) | with 0104 |
| **0139** AMFI dyld policy (`sweet_amfi.c`, 45 lines) | adds `mpo_policy_syscall` to the AMFI MAC policy and renames `mpc_name` from `"SWEET AMFI"` to `"AMFI"`; op 0x5a (`amfi_check_dyld_policy_self`, arg = `{u64 in_flags; u64 *out_flags}`) writes `0x7b` to `*out_flags` | **Exactly the blocker recorded in our NOTES.md ~line 438** ("DYLD_INSERT_LIBRARIES is not an option: ... registers as SWEET AMFI without mpo_policy_syscall"). Apply to our `bsd/kern/sweet_amfi.c` (ours has 0035/0036); only the context of `sweet_amfi_mac_ops`/`mpc_name` may need a rebase. Our sandbox policy (0050/0064) also sets `mpo_policy_syscall` under its own name, so the rename to "AMFI" does not collide. INFERRED from dyld source (not read here): 0x7b = allow @paths(1) + path vars(2) + fallback paths(8) + print vars(0x10) + failed library insertion(0x20) + interposing(0x40); not custom shared cache(4), not embedded vars(0x80). | Cheap, unlocks injection |
| **0166** `IOAcceleratorES` nub + IOKit quiesce (`IOStartIOKit.cpp`, `IOService.cpp`) | without kernelmanagerd nothing releases the registry busy count, IOKit never quiesces (breaks `IOKitWaitQuiet` callers, e.g. configd); the nub exists so CA's 300 x 100 ms poll succeeds at once | Quiesce hunks: check whether we hit the same stall (our boot reaches the lock screen, so maybe we already tolerate it; cheap to test). The nub is already our `0109-dm3q-metal-accelerator-nub`; it has MetalPluginName so Metal is used, which is what we want. | Check |
| **0165** `IO_MAP_SIZE` 8 -> 64 MB (`osfmk/arm/io_map.c`) | `ml_io_map` space ran out ("kmem_alloc panics depending on boot-args", 0164e/h) | Do the same before the GPU driver: A740 needs GPU regs (0x3d00000..), GMU (0x3d6a000, 0x3d50000 RSCC), SMMU, GCC/GPU_CC, RPMh, PDC, plus carve-out mappings. | **High** |
| **0138** user client (`SweetGPU.cpp`, `sweet_gpu.h`, `KernelConfigTables.cpp` entry, `files.arm64`) | IOService `SweetGPU` matched on `IOResources` + `SweetGPUUserClient` with `externalMethod` | Take the *structure* (class pair, `kIOUserClientClassKey`, `initWithTask` with `fTask`, per-client table, clientClose cleanup). Replace selectors 0-3 (quad) with nothing; keep 10-16 shape. See 3.4 for which API (msm-like or KGSL-like). Note it matches on IOResources with a category, registers only when the GPU is ready. | **High** |
| **0150/0151** BO + submit + fence (`sweet_gpu.c`, `SweetGPU.cpp`) | the BO/VA/IB/fence machinery above | Port the *design*, rewrite the hardware parts: A740 ring is written through the AP-side `CP_RB_WPTR` with the GMU keeping GX powered (no IFPC); `CP_INDIRECT_BUFFER` is the a6xx/a7xx packet (`CP_INDIRECT_BUFFER` 0x3f, 64-bit addr + size), `CP_EVENT_WRITE` with `CACHE_FLUSH_TS`/`CCU_...` for the fence, `CP_SET_RENDER_MODE`/`CP_SET_MARKER` details differ on a7xx (copy from Mesa's `fd6_emit`/Turnip `tu_cs` or the msm kernel `a6xx_submit`). Keep: validate IBs lie inside a mapping, bo handle list -> cache maintenance, hang watchdog, wait without the client lock. | **High** |
| **0137** VA space (4 KB tables, 8-16 GB window, `sweet_gpu_vm_map`) | table walker owned by the kernel | Port to the kgsl-smmu context bank. SM8550's GPU SMMU (0x3da0000, MMU-500 style: sCR0, SMR at +0x800, S2CR at +0xc00, CBs from `(NUMPAGE/2)*pagesize`) has the same register model they program. VA size: A7xx 48-bit; keep a smaller window (e.g. 39-bit, 4 KB granule, L1 root) for simplicity: Turnip's `va_start`/`UCHE_TRAP_BASE` need to be inside it or told to the userland (3.2). Our `PROBE2-REVIEW.md` already lays out the same CB programming for the GMU (SID 5). | **High** |
| **0121/0122/0123/0124** GPU SMMU identity/trace/stream-0/no-wzr | see lessons 2.2 | Lessons only; code is Pixel-specific (their SMMU at 0xb40000, SID 0 = gfx3d_user, SID 2 = TZ secure). Our SIDs: GMU = 5 (from DT, `A740-POWERON.md`), GPU user stream(s) from the kgsl-smmu node. | Lessons |
| **0130** zap shader through TZ PAS | see section 4 | Port with edits | **High (later)** |
| **0155** non-fatal SMMU faults (no CFRE) | context bank SCTLR without `CFRE`: a faulting GPU access is *terminated* (reads 0, writes dropped) instead of aborted; FSR/FAR/FSYNR0 logged at each submit with "mapping below" | Take as is for bring-up; add the fault report to every submit/wait timeout path. | High |
| **0156** VM padding to a scratch page | see 1.4 | Take as is; a7xx fetch overrun behaviour is UNVERIFIED but same class of hardware. | High |
| **0161** re-read the GFX S2 rail registers until the type answers | PMIC SPMI read returned partial data right after boot (`type 00`) | PMIC-specific (PMI8994). Equivalent lesson: our rails go through RPMh and cmd-db and are write-and-verify already (probe 1: votes rc 0). Retry-reads on any status register that is read right after a rail transition. | Lesson |
| **0164** GPU present | see 2.3 | Idea only | later |
| **0152-0162** IOSurface changes | row-pitch rules (64-byte rows, explicit row bytes rounded to 64 only for backboardd, client-memory surfaces keep the creator's pitch, wrap selectors 6/7 refused (a faked success crashed CA's GLES path), shadow framebuffer stride 64) | Review each against our `SweetSurface` (ours is the same 0046 base). They matter for *any* GPU path that imports IOSurfaces by pages, including Turnip, because GPU texture/linear-RT pitch alignment is 64 bytes on a6xx/a7xx too. | Medium, when importing surfaces |
| **tools/ir3asm** + `tools/gpu-shaders/*.asm` | Mesa's ir3 assembler built natively; hard-coded A530 compiler struct (`main.c`: `info.chip = 5`, `num_sp_cores`, `fibers_per_sp`...) | Optional. For a7xx build the same tool with the A740 values from `freedreno_devices.py` (FD740 entry ~line 1232) or call `ir3_compiler_create` with `fd_dev_id{.chip_id=0x43050a01}`. Not needed if the compositor/scaler goes through Mesa/Turnip. | Optional |
| **tools/sweetgpu-ca** | CA::OGL::SW::draw_polygon hook + GPU offload experiment | Abandoned by them; ignore. | Skip |

### 2.1 The IOKit-user-client pattern vs our `/dev/kgsl-3d0` design

Our `KGSL-XNU-DESIGN.md` recommends a BSD cdev plus either a private map ioctl (phase 1) or a `d_mmap` hook patch in `kern_mman.c`
(phase 2). Sailfish proves a third route that needs **no XNU core edit at all and has working precedent on this exact kernel**:
`IOBufferMemoryDescriptor` (kernel_task, `kIOMemoryKernelUserShared`) + `createMappingInTask`, owned by the user client until the BO
is freed or the client closes (their `freeBo`, `clientClose`). The hazard noted in our design doc (an `IOMemoryMap` owning the mapping,
so Turnip's `munmap`/`MAP_FIXED` replacement semantics could double-free) is real, but Sailfish avoids it by making munmap a no-op in
the emulation layer. For Turnip, patch `kgsl_bo_map`/`kgsl_bo_user_map` and `safe_ioctl` under `#ifdef __APPLE__` so that the KGSL
"ioctls" become `IOConnectCall*` and the BO "mmap" returns the address that selector 10 already produced (Section 3.4).

### 2.2 Lessons and pitfalls (SMMU, Gunyah, caches, faults), ordered by how likely they bite us

1. **Never store `wzr`/`xzr` to firmware-trapped registers** (0124): "firmware-trapped S2CR stalled on `str wzr`". They force the value
   through a real register in the `wr()` helper (`"r"(v)`, asm `str %w0,[%1]`). This is the same class as our Gunyah finding in NOTES
   (emulated MMIO needs plain single-register LDR/STR with a valid ESR syndrome; pair/post-index give ISV=0 -> injected SEA). Add `xzr`/`wzr`
   to that rule for every SMMU/GMU register write we do from XNU (the XNU build does not use our shim's inline-asm helpers).
2. **SMR must not match every stream** (0123): an SMR with a full mask also matched TZ's secure stream and "stalled the CPU on the S2CR
   write (the firmware traps it)". Match only the stream(s) you own (SMR0 = ID, mask 0). On SM8550, probe 2 (`PROBE2-REVIEW.md` finding 2)
   already plans the survey first; keep that order and never write sCR0 until the survey shows who owns it.
3. **The SMMU firmware ignores `sCR0.CLIENTPD` (bypass)** on their SoC and leaves the stream in FAULT: "the first CP fetch reset the phone"
   (0120a). Do not count on bypass or on ABL's leftovers; always translate via your own context bank (0121). We already do that for UFS/USB.
4. **A hung GPU takes the SoC with it** when accesses fault while the SMMU aborts: 0155 switches the context bank to terminate faulting
   accesses (no CFRE) and logs FSR/FAR in `sweet_gpu_submit`. Combined with 0156's scratch-page padding the GPU survived texture overruns.
   Expect the same on A740 (UCHE/TP prefetch past BO ends).
5. **WC user mappings of kernel-managed pages freeze XNU** (0150a -> 0151): use cached mappings + software clean/invalidate per submit and a
   cpu-prep. Turnip assumes `WC` for non-cached BOs (TURNIP-IOS.md 4.3); the KGSL shim must therefore advertise only the *cached* flavours
   and do the maintenance in the kernel (or answer the property/flag probes so that `has_cached_coherent_memory=false`, and make sure
   Turnip's own `tu_bo_sync_cache` is a no-op or implemented with `dc civac` on the user VA).
   Check whether the A740 SMMU can mark PTEs inner-shareable/cacheable (IO-coherent): that removes all of this (UNVERIFIED; `kgsl-smmu`
   DT node in `fw/dts/merged-rev9.dts` ~line 28966 should say `dma-coherent` or not).
6. **GPU VA page size vs CPU page size**: GPU pages 4 KB, XNU 16 KB. Sailfish splits each kernel page into 4 PTEs and requires 16K-multiple
   BO sizes (`mmapsize` rounding, `sweet_drm.c:222`). Same for us (already noted in TURNIP-IOS.md 4.3).
7. **Cache maintenance addresses**: `flush_dcache64(pa, len, TRUE)` works on *physical* addresses through the physmap; for non-RAM pages
   (their panel buffers outside XNU DRAM) it must be skipped with `pmap_valid_page` (0164 `vm_cache`).
8. **Hang policy**: on a 2 s stuck fence they set `g_ready=false` and never recover until reboot (no GPU reset). A740 recovery is harder
   (GMU reset + HFI), so keep the same "fail closed" initially but log CP/RBBM state (`cp_state`, `smmu_faults`) at that moment.
9. **Service readiness**: IOKit clients race the GPU bring-up; the user client returns NotReady and callers retry. Their bring-up in
   service mode skips the multi-second diagnostic pauses (0140). Do the same: our bring-up will take longer (GMU/HFI), so the
   `IOAcceleratorES`/SweetMetal path must tolerate a late GPU (fall back to lavapipe if the user client is absent: nice migration property).
10. **io map exhaustion** (0165): many `ml_io_map` pages -> `kmem_alloc` panic. 64 MB.
11. **Thermals/clocks**: they pinned 300 MHz for service mode, then 400/600 MHz with an explicit rail raise (open-loop voltages, no CPR).
    We have no thermal driver and the phone already got hot with lavapipe (NOTES ~22:00). Start the A740 at the lowest GMU-approved
    level and keep our `0110` thermal guard hooked to GPU activity.
12. **Gunyah has no equivalent in their world.** They run with TZ only; all their SMC calls are plain TZ calls. Gunyah-specific unknowns
    (stage-2 mapping of the zap carve-out, whether PAS runs through Gunyah, who owns the SMMU, VM-ownership of the GPU registers) are
    untested by them and remain our probes.

### 2.3 Presentation (GPU scale, double buffer, 60 Hz) and what it means for us

Their pipeline (0164, `SweetFramebuffer.cpp`; also 0087/0090/0092 earlier in history):
1. backboardd renders with CA's GL renderer into IOSurfaces; `glFlush` == `glFinish` so the GPU is done before CA calls `IOMobileFramebuffer`
   `SwapEnd`.
2. `AppleCLCD` receives the swap, and for each enabled layer calls `gpuLayer()`: the logical (iPhone-sized) rect is mapped to panel pixels
   and a **bilinear scaling quad** runs on the GPU (`sweet_gpu_quad`, a hand-assembled compute kernel, `sample4.asm`) from the swap surface
   (mapped read-only into the GPU VA window, up to 4 kept mapped, oldest evicted, `gpuMap()`) into one of **two 32 bpp panel buffers**
   carved out of the unused lower RAM bank (`0xc0800000`, `0xc1000000`, 8 MB each, below 4 GB because the MDP fetches without its SMMU).
3. The flip is a **register write**: VIG0 source address/stride/format (`SRC0_ADDR`, `SRC_YSTRIDE0`, `SRC_FORMAT`, `UNPACK`) then
   `CTL0_FLUSH` before the next START (command-mode panel, `pe_serial.c`, `sweet_fbcon_flip`). The CPU never copies pixels.
4. Refresh default raised 30 -> 60 Hz (`hz = 60`); CA's CPU work (not the GPU) became the limit (15-35 fps).

For the DM3Q: our panel path today is "ABL framebuffer at 0xb8000000, 1440x3088" with CPU scale of the iPhone-X-sized (1125x2436) surface
(0101/0102). To reuse the idea: (a) GPU scaling needs only a texture-sample blit, which Turnip/Mesa or `vkCmdBlitImage` give for free once the
render path is on the GPU; the CPU copy of a 1125x2436 BGRA surface is the thing to eliminate. (b) The flip needs the SM8550 DPU
(SSPP source address registers); under Gunyah the DPU registers may or may not be reachable. UNVERIFIED: we have not probed whether the DPU is
live and writable. Cheapest first step: keep the ABL framebuffer scanout and let the GPU write the scaled frame *into* it (one buffer, tearing
possible), as Sailfish did before 0164; double buffering follows only if the DPU is accessible.

---------------------------------------------------------------------------------------------------------------------------

## 3. GL route vs our Metal route for the A740

### 3.1 Does Mesa freedreno Gallium (GLES) support A740 today?  (checked in `third_party/mesa`, main af17d14 = 27.0.0-devel)

- **Yes.** `src/gallium/drivers/freedreno/a6xx/` is a chip-templated driver; `fd6_emit.cc`, `fd6_gmem.cc` (20 hits), `fd6_program.cc` (19),
  `fd6_rasterizer.cc` (6), `fd6_barrier.cc`, `fd6_blitter.cc`, `fd6_compute.cc`, `fd6_query.cc`, `fd6_texture.cc`, `fd6_zsa.cc`, `fd6_blend.cc`
  contain explicit `CHIP == A7XX` paths; `freedreno_screen.h:281 is_a6xx() => gen >= 6` covers a7xx; `FD_GENX(fd6_context_create)` instantiates both.
- **Device table**: `src/freedreno/common/freedreno_devices.py:1231-1249`: `GPUId(740)`, `GPUId(chip_id=0x43050a01, name="FD740")  # KGSL, no speedbin data`,
  `GPUId(chip_id=0xffff43050a01, name="FD740")  # Default no-speedbin fallback`, `A6xxGPUInfo(CHIP.A7XX, [a7xx_base, a7xx_gen2], num_ccu=6, ...,
  raw_magic_regs = a740_raw_magic_regs)`. Our chip id 0x43050a01 (A740, "C510v2" per `A740-POWERON.md`) is a direct hit. The msm backend reads
  `MSM_PARAM_CHIP_ID` as a 64-bit value that normally carries speedbin bits in the upper half; our emulation can return `0xffff43050a01` (the
  documented no-speedbin fallback) or plain `0x43050a01`.
- **Conformance**: `src/freedreno/ci/deqp-freedreno-a750.toml` runs dEQP GLES2/3/31 + KHR GLES 2/3/31 + GL4.6 (`version_check = "GL ES 3.2.*git"`,
  `renderer_check = "FD750"`), with only ~130 lines of known fails (`freedreno-a750-fails.txt`). A740 is `a7xx_gen2`, A750 `a7xx_gen3`: sibling gen, not
  identical, but the same code. There is no a740 CI job (A740 hardware is not in their farm) -> expect a handful of a740-only issues.
  `docs/features.txt` lists freedreno/a6xx as DONE through GL 4.x; `docs/drivers/freedreno.rst` says GLES 3.2 and GL 4.5.
- What the msm backend asks the kernel for (`src/freedreno/drm/msm/msm_pipe.c:42-250`): `GPU_ID`, `GMEM_SIZE`, `CHIP_ID`, `GMEM_BASE` (only
  with `FD_VERSION_SOFTPIN`), `MAX_FREQ`, `TIMESTAMP`, `PRIORITIES`, `FAULTS`, `SUSPENDS`, `VA_SIZE`, `VA_START`, `UCHE_TRAP_BASE`; BOs via softpin
  (`MSM_INFO_GET_IOVA`). Their `sweet_drm.c` answers all but `VA_SIZE/VA_START/UCHE_TRAP_BASE` (they return -EINVAL for unknown params, which the
  msm backend turns into defaults). For a7xx, `UCHE_TRAP_BASE` must lie inside our GPU VA window (Turnip's KGSL doc says default `0x1fffffffff000`
  if the query fails). Verify when we get there (UNVERIFIED which defaults Gallium uses for a740).
- A740 GMEM: 3 MiB (`GMEM_SIZE=0x300000` in `TURNIP-IOS.md`), base `0x100000`. Sailfish forced **sysmem** rendering (no GMEM) because tile
  restore/resolve overran unpadded IOSurfaces; for a7xx start the same (`FD_MESA_DEBUG=sysmem`), it also removes the dependency on the
  binning/VSC code path and on UBWC.
- Known gaps to keep in mind: UBWC must stay off for imported linear IOSurfaces (Gallium disables it for scanout/imported linear); a7xx
  `GL_EXT_shader_framebuffer_fetch` support exists natively in freedreno a6xx+ (they had to emulate it on a5xx), so that CA workaround may
  disappear (UNVERIFIED).

### 3.2 What each route still needs (everything after "kernel driver + user client")

Shared, identical for both routes (and by far the biggest, riskiest part): A740 GMU/HFI boot, SQE load, SMMU context banks, zap, ring/IB/fence,
cache policy, BO/VA user client, hang handling. Probe 1 is done; probe 2 (SMMU survey, GMU CB, M3 release, HFI) is next.

**GL + EAGL shim ("sweetgl")**
| Item | Effort | Risk |
|---|---|---|
| Gallium freedreno cross build for iOS (static libs + one dylib) | 1-2 days (mesa-setup.sh/build.sh are recipes; our `third_party/mesa` is already building lavapipe + Turnip for iOS with the darwin fixes) | low |
| libdrm/msm emulation over our user client (`sweet_drm.c` ~530 lines) | 1-2 days; a7xx adds `VA_SIZE/UCHE_TRAP_BASE`, fake syncobj if Mesa now insists | low-medium (newer Mesa than theirs: the msm backend may call syncobj/`MSM_SUBMITQUEUE` features) |
| EAGL shim + GLI table (2235 lines + 410-slot layout) | their code is directly reusable (the GLI layout is a property of iOS 16.7 GLEngine; our root is 20H392 as theirs) ~2 days to adapt/build | medium: **hard-coded 20H392 shared-cache addresses** in `eagl.c` (hooks, texture-function table); our cache must be byte-identical (compare `dyld_shared_cache` UUID; both are `20H392`, A11 `arm64`). |
| CA GL renderer fidelity (vibrant colour matrix, blur, rect textures, colour formats, fbfetch, tile semantics) | they spent ~35 commits over a day and a half and the README still lists "Metal-needing apps do not work" | medium: visual bugs forever; every iOS-16 CA effect that is Metal-only needs a shader emulation. |
| Removes our Metal work from the critical path | negative | CoreImage/MPS/SpringBoard blurs currently work on SweetMetal; on GL they use CPU or fail (we already patched CI to "no GL"; the GL path would need that patch undone or bypassed). Also backboardd-only injection: Metal apps would still need SweetMetal. |

**Turnip + metal2vulkan + SweetMetal**
| Item | Effort | Risk |
|---|---|---|
| Turnip for iOS | **already built** (TURNIP-IOS.md, 15.6 MB dylib, 215 strong imports all resolved) | low |
| KGSL shim: either cdev+mmap hook (design doc) or Sailfish-style `IOConnectCall*` inside `tu_knl_kgsl.cc` (3.4) | 2-4 days | medium: Turnip uses more of the KGSL UAPI than Gallium uses of msm: device properties (7 props), GPUMEM_ALLOC_ID with flag echo, GPU_COMMAND with a syncpoint list, timestamps per context (`READTIMESTAMP`, `WAITTIMESTAMP`), optional sparse/VBO (can fail) |
| IOSurface <-> Vulkan image zero copy | Turnip has **no** `VK_EXT_external_memory_host` and its dma-buf import is stubbed out for iOS (TURNIP-IOS.md 2.3 #3). Needs a small patch: let `kgsl_bo_init_dmabuf` take a "surface token" (our selector 12 equivalent) so `vkAllocateMemory` + `VkImportMemoryFdInfoKHR` can wrap an IOSurface; metal2vulkan's runtime currently aliases host pages for lavapipe (NOTES "alias-host-pages" patch), which cannot be used by a real GPU | 3-5 days | **medium-high**: this is where Gallium+EGLImage (their `get_egl_image`) is simpler |
| Present path | CA Metal presents through `CAMetalLayer`/IOSurface swap exactly as today (we already show the lock screen); no EAGL shim, no GL texture functions to emulate. | low |
| Fidelity | Same renderer Apple uses on A11: the metal2vulkan translation covers 115/141 QuartzCore and 55/58 CoreImage shaders (0 translator gaps, rest need pipeline state); MPS works for blur. | low-medium |
| Performance | Unknown: lavapipe 8 cores gave ~20 fps with 600 ms first frame; Turnip is a GPU Vulkan driver but **Metal->SPIR-V->NIR->ir3** has two compile steps per pipeline (CA recreates identical pipelines per draw; our device already caches by key). Shader compile latency on first use. | medium |

### 3.3 Comparison summary

| Criterion | Freedreno GL + EAGL shim | Turnip + metal2vulkan + SweetMetal |
|---|---|---|
| New user-space code | ~3000 lines (eagl/dispatch/sweet_drm) are reusable almost verbatim | KGSL shim (~400-600 lines) + IOSurface import patch for Turnip |
| What we have today | nothing for GL (we explicitly patched CoreImage to no-GL) | whole stack running on lavapipe (lock screen, MPS, pipeline cache) |
| CA path | `CA::OGL::GLESContext`, with Metal-only features emulated by patches into shared-cache data | `CA::OGL::MetalContext`, unmodified |
| Apps | only CA/backboardd benefits; apps using Metal (SpringBoard MPS, CoreImage CIContext) still need SweetMetal or fail | system-wide (every process through the bundle) |
| Version fragility | high: hard-coded 20H392 addresses | low: public Metal API surface |
| Debuggability of the GPU itself | GL is simpler to trace (`SWEETGL_TRACE`, per-draw readbacks in their code) | an extra translator in the loop |
| Risk to hit "first GPU pixels" quickly | lowest (one library, one client, no Vulkan WSI) | low-medium (needs the IOSurface import) |
| Multi-process | single backboardd client | many clients: the shared GPU address space and cache scheme matter much more |

### 3.4 Recommended kernel/user interface (applies to both)

One IOKit `SweetGPU` service/user client (Sailfish 0138/0150 shape), and **two thin user-space shims on top of it**:
- `sweet_drm.c`-style msm emulation (Gallium freedreno; optional, only if the GL route is pursued);
- a KGSL emulation compiled **into** `libvulkan_freedreno.dylib`: under `#ifdef __APPLE__` in `tu_knl_kgsl.cc` route `safe_ioctl()` (a switch on
  the ~14 ioctls of TURNIP-IOS.md 4.1) and the two `mmap` call sites (`kgsl_bo_map`, `kgsl_bo_user_map`) to `IOConnectCall*`. This reuses
  Sailfish's proven BO/map/submit/wait/info selectors, avoids `/dev/kgsl-3d0`, avoids the XNU `d_mmap` core patch, and keeps the existing
  design doc's property table (4.2) and flag-echo rules (4.1) as the *contract* for the user client. The `id << 14` offset trick becomes
  unnecessary.
- Kernel fence: replace Sailfish's single global counter with per-queue timestamps (`kgsl_drawctxt` = a queue id; `READTIMESTAMP_CTXTID` is
  "retired"), which can still be one global fence internally (a queue's timestamp = last global seq it submitted).

### 3.5 What Sailfish says about running both

They never had a Metal path; the README states "No Metal. CoreAnimation falls back to its OpenGL ES renderer". Their device nub (0166)
shows the coexistence rule we already follow: **the presence of `MetalPluginName` on `IOAcceleratorES` is what flips CA between GL and Metal**.
Therefore both routes can live in one image behind a boot-arg: `sweetmetal=1` (our 0109 nub with plugin) vs. a nub without the plugin
properties (`sweetaccel` in theirs). That makes "GL as a diagnostic/fallback renderer" cheap even if Metal is the main path: the only extra
cost is carrying `sweetgl.dylib` and the sweetenv/AMFI patches.

### 3.6 Recommended order

0. Prereqs that help both: **0165 (64 MB io map), 0104+0163+0139 (sweetenv + AMFI dyld policy)**. Cheap, no GPU needed, unlock DYLD injection
   for tracers and for any GL experiment. Also test whether the IOKit quiesce hunk of 0166 fixes anything in our boot logs.
1. **Probe 2 and 3 on the GPU (SMMU survey, GMU CB, M3 release/HFI, GX on, SQE load, `CP_ME_INIT` + `CP_MEM_WRITE` to a scratch page, then zap).**
   Same stepping Sailfish used: power -> CP executes a write (works in secure mode!) -> zap -> leave secure mode -> blit/compute -> IB submit.
   Use the quad/compute idea as the *first user-visible* test only if it's cheap: a fill/clear of the panel buffer by the GPU is the best
   "pixels on the glass" milestone (see 2.3) and proves SMMU + cache policy + zap + display together.
2. Port the **user client** (selectors 10-16 shape) and the VA/BO/submit/fence code. Add the `sweet_drm`-style or KGSL-style shim.
3. **Turnip first**, with SweetMetal: replace lavapipe by Turnip in the metal2vulkan runtime (ICD path), keep lavapipe as runtime fallback
   when the user client is absent or NotReady. Reasons: (i) Metal already renders the whole UI, so the visible delta is only speed/heat; (ii) the only
   new risks are Turnip a7xx correctness and the IOSurface import; (iii) the work is system-wide for free; (iv) no shared-cache-address patches.
4. **Gallium GL as the fallback experiment**, started only if after ~a week Turnip cannot draw (e.g. a7xx-specific hangs in Turnip's GMEM/sysmem
   path, or metal2vulkan pipeline compile latency is unusable). The GL route costs little to *try* because most of it exists, but it throws away
   Metal fidelity; do it as a boot-arg alternative, not a replacement.
5. Presentation: GPU-write directly into the ABL framebuffer (scaled), then double buffering if the DPU is accessible.

Why not GL first even though it is "a fast usable UI"? Because our measured situation is the opposite of Sailfish's: they needed *any* renderer
(CPU CA was far too slow on 2 GB Kryo); we already have a working one that only needs a faster Vulkan ICD. The GL stack's cost is the long tail of CA
fidelity patches, not the first triangle.

---------------------------------------------------------------------------------------------------------------------------

## 4. Zap shader from XNU: Sailfish's method vs our situation

### 4.1 Their method (`0130-sailfish-gpu-zap-shader.patch`)

- Files go in via the boot payload (`mkpayload.py --blob GPU-ZAP-MDT=a530_zap.mdt --blob GPU-ZAP-SEG=a530_zap.b02`) and are read from `/chosen/memory-map`;
  `blob(name,&pa,&len)` finds them; no filesystem needed.
- **SMCCC (`0130.patch:48-83`)**: `smc #0`, fn id = `(1<<30 [SMC64]) | (2<<24 [SIP owner]) | (svc<<8) | cmd`, `x1 = arginfo`, `x2..x4 = args`, `x5 = 0`, results `x0` (status)
  and `x1`. Loop rules: if returned `x0 == 1` (INTERRUPTED) call again with `x0 = 1` and the returned `x6`; if `x0 == -12` (EBUSY) `delay(30000)` and retry (up to 20).
  Clobbers x8-x17 declared.
- **Sequence (`:123-145`)**, svc 2 (PIL/PAS):
  1. `PAS_INIT_IMAGE` svc 2 cmd 1, `arginfo = 2 | (2<<6)` (2 args: VAL, RW), args `(pas_id=13, metadata PA)`. Metadata = the whole `.mdt` file (ELF header +
     phdrs + hash segment), copied into a fresh page (`gpu_page`), `flush_dcache64(meta, PAGE_SIZE, TRUE)` first. Success = `x0==0 && x1==0`.
  2. `PAS_MEM_SETUP` svc 2 cmd 2, `arginfo = 3` (3 VAL args): `(13, carve-out base 0x90f00000, size = aligned span of the loadable segment)`.
  3. Copy the loadable segment into the carve-out with **32-bit stores through `ml_io_map(ZAP_PA, 1 MB)`** (device memory mapping) after MEM_SETUP and before auth;
     zero-fill up to `memsz`.
  4. `PAS_AUTH_AND_RESET` svc 2 cmd 5, `arginfo = 1`, arg `(13)`.
- ELF parse: finds exactly one PT_LOAD whose flags field bits 26:24 are not 2 (that field is the MDT "segment type", 2 = hash), `phoff` at 0x1c,
  `phnum` at 0x2c, 32-bit phdrs; requires `filesz == len(b02)` (their `.b02` is the loadable segment; **our files are numbered differently**, see 4.3).
- Where it sits: **after** CP_ME_INIT works (the CP runs and `CP_MEM_WRITE` lands in *secure* mode), **before** the first command that writes normal
  memory through the RB. Then `CP_SET_SECURE_MODE 0` (`:~190`). Symptoms they saw without it, which are a checklist for us:
  - 0128a: blits "complete" (fence lands, no fault) but destinations stay zero: in secure mode the RB may only write secure memory.
  - 0129a: `CP_SET_SECURE_MODE 0` without the zap shader **hangs the CP**.
  - 0125: writing `RBBM_SECVID_TRUST_CNTL = 0` from the AP (what mainline does when there is no zap) **reset the phone** (TZ-protected register) -> they
    removed it. Do not write the SECVID registers on SM8550 either.
- They did not probe `PAS_IS_SUPPORTED` or `IS_CALL_AVAIL` at all; they just called.

### 4.2 Comparison with `A740-POWERON.md` section 6 (our design)

Matches exactly: fn encoding (`0x42000201` = (1<<30)|(2<<24)|(2<<8)|1 + SMC64 bit), arginfo for INIT_IMAGE (our doc says 0x82 = 2 | (0<<4) | (2<<6); theirs
`2 | (2<<6)` = 0x82), MEM_SETUP arginfo 3, AUTH_AND_RESET arginfo 1, order INIT -> MEM_SETUP -> copy -> AUTH, metadata = full `.mdt` in a page-aligned
physically contiguous buffer, `-12` retry, INTERRUPTED resume. Differences that matter:

| Item | Sailfish | Us (SM8550) | Consequence |
|---|---|---|---|
| Caller | XNU kernel at boot, 16K pages, MMU on | shim probes at EL1 with MMU off (so far) | In the shim there is no 16K problem, no `ml_io_map`; metadata buffer physical = virtual. Doing the whole zap load in the shim (before the jump to XNU) is attractive: simple, deterministic, and the GPU secure state survives the handoff (INFERRED). If done in XNU, see next row. |
| Carve-out | `gpu_mem` 0x90f00000, **1 MB**, 16K-aligned | `gpu_micro_code_mem` 0x9b09a000 **8 KiB**, no-map (DT 1733-1737, `A740-POWERON.md` section 6) | 0x9b09a000 & 0x3fff = 0x2000: **not 16K aligned**. `ml_io_map` of that 8 KiB region would have to map the surrounding 16K page 0x9b098000-0x9b09c000, which includes 8 KiB of someone else's reserved memory (likely protected by XPU/Gunyah -> a synchronous external abort on any access, or a SoC reset like the TLMM page). In XNU, map only via a 4K-granule path or do the copy in the shim. |
| Image size | `memsz` from phdr; required `filesz == len(b02)` | image = 0x430 B (b01), hash segment 0xf38 B (b02), ELF header b00 (148 B) | Do **not** copy their `b02` rule: our loadable PT_LOAD segment is `a740_zap.b01`; b02 is the hash. Select by phdr (type 1, not `flags>>24 == 2`) and use `filesz` from the phdr (0x430). |
| `IS_CALL_AVAIL` | not used | =1 for PAS init and auth (probe 1 S10) | consistent: the SMC channel and these two calls exist behind Gunyah |
| `PAS_IS_SUPPORTED` | not used | =0 for 13 and also for 1/4/18 (adsp/modem/cdsp) | see 4.3 |
| Hypervisor | none | Gunyah (EL2) between EL1 and TZ | unknown |

### 4.3 What to conclude about `PAS_IS_SUPPORTED = 0` for every id

- Their code never queried it, and (INFERRED from the Linux sources named in our doc; `adreno_zap_shader_load` -> `zap_shader_load_mdt` ->
  `qcom_mdt_load` -> `qcom_scm_pas_init_image`) the msm GPU path does not either: only remoteproc `qcom_q6v5_pas` calls
  `qcom_scm_pas_supported()` before probing. Since the same query also answers 0 for adsp/modem/cdsp, which certainly exist, the answer is not
  meaningful on this TZ/Gunyah combination (as already written in NOTES). **Do not treat it as "no zap here".**
- The decisive test is the one Sailfish did: call `PAS_INIT_IMAGE` for id 13 with the real metadata. Its return code is informative and not
  destructive: 0 = TZ accepted the signed hash table; non-zero (e.g. -EINVAL, -EOPNOTSUPP/`-4`, or an "access denied" SCM error) tells whether PAS 13
  is refused by TZ/Gunyah. A reset/stall in that call would be new information (their only reset was the SECVID write).
- Gunyah risk list (all UNVERIFIED): (a) Gunyah may filter or proxy SCM calls from the primary VM (Android's KGSL does the same calls, so the primary
  VM is expected to be allowed; IS_CALL_AVAIL=1 supports that); (b) the metadata buffer physical address is passed unchanged: with identity IPA=PA
  for the primary VM this is fine (the UFS DMA addresses we already use behave that way); (c) after `PAS_MEM_SETUP` the carve-out may become
  non-accessible to EL1 (it is protected for TZ's authentication); Linux copies the segment between MEM_SETUP and AUTH via `memremap(MEMREMAP_WC)`
  of the reserved region, so EL1 must still be able to write it at that time; (d) TZ makes the metadata page secure for the duration of the call:
  never reuse that page afterwards (our doc already says so); (e) the signature: the Samsung-signed `a740_zap.mdt/.mbn` from this very phone
  should authenticate on this phone, key/anti-rollback unknown.
- Suggested probe order for the zap: (1) after SQE + `CP_ME_INIT` + `CP_MEM_WRITE` landing in secure mode (proves GX is up and CP fetch works), do
  INIT_IMAGE only and print x0/x1; (2) MEM_SETUP and print; (3) copy 0x430 bytes with plain `str w` (one register at a time) to 0x9b09a000 and read back; (4)
  AUTH_AND_RESET; (5) emit `CP_SET_SECURE_MODE 0` + a `CP_MEM_WRITE` marker (they used 0x5ec0de00) and a CP write of a normal page; (6) only then try a CP
  compute/blit. If (5) hangs the CP, the zap did not take effect (their 0129a symptom).
- A7xx note: Linux calls `qcom_scm_gpu_init_regs` (`QCOM_SCM_SVC_GPU`) for A750+ only (`A740-POWERON.md` unknowns); Sailfish's A530 has no analogue.
  Probe 1 says `GPU_INIT_REGS not available`, consistent with A740 not needing it.

### 4.4 What if PAS 13 is refused

Sailfish's experience says there is no software path out of secure mode without the zap shader on a TZ-locked device (SECVID write resets the SoC;
`CP_SET_SECURE_MODE 0` hangs). Fallback ideas, all weaker: run the GPU in secure mode and write to *secure* memory only (useless for display),
or find whether Gunyah/TZ lets the primary VM use the `gpu_init_regs`/`secure_init` calls. Do not invest in this unless INIT_IMAGE actually fails.

---------------------------------------------------------------------------------------------------------------------------

## 5. Concrete next steps for our tree (short list)

1. Port 0165 (io map 64 MB), 0104+0163 (sweetenv), 0139 (AMFI dyld policy) into `port/xnu/patches/` as dm3q patches (renumbered, `0111`..). No GPU needed to validate:
   `sweetenv=backboardd:DYLD_PRINT_LIBRARIES=1` (or inject `libvulkan_shim`/tracer).
2. Finish probe 2/3 per `PROBE2-REVIEW.md`, then add zap in the order of 4.3, do it in the shim first.
3. Replace the cdev design by the `SweetGPU` IOKit user client (selectors 10-16 shape); keep the KGSL property/ioctl tables of `TURNIP-IOS.md` as the contract; put the KGSL
   emulation inside `tu_knl_kgsl.cc` under `__APPLE__`.
4. Decide cache policy early: cached user mappings + kernel clean/invalidate at submit and cpu-prep (Sailfish), or SMMU IO-coherent PTE attributes if DT shows
   `dma-coherent` (UNVERIFIED).
5. IOSurface import for Turnip (token through the dma-buf import hook) before the first Metal-on-Turnip frame.
6. Keep `sweetgl` as a documented, optional experiment; clone its `sweet_drm.c`, `sweetgl.c`, `eagl.c`, `gen-dispatch.py`, `ios-dispatch.txt` only if Turnip stalls.

## Appendix: key file/line index in the Sailfish clone

- Architecture/README: `README.md` (status, boot args, layout), `xnu/README.md` (build only).
- Boot args in use: `README.md` step 4 (`sweetgpu=8 sweetgpuclk=600 sweetddr=1804 sweetenv=backboardd:DYLD_INSERT_LIBRARIES=/usr/lib/sweetgl.dylib`).
- sweetenv: `xnu/patches/0104-...:17-58`, `0163-...` (multi-entry). AMFI: `xnu/patches/0139-...:15-42`.
- User client: `xnu/patches/0138-...:539-600` (selectors 0-3), `0150-...:865-945` (10-15), `0151-...` (16, cached BOs).
- VA space: `0137-...:76-190`, rewritten page-granular in `0150-...` (~lines 40-200); padding `0156`; faults `0155`.
- Zap: `0130-...:29-31` (ZAP_PA, ZAP_SIZE, GPU_PAS_ID), `:48-83` (scm_call), `:85-148` (zap_load).
- SMMU: `0121-...` (identity, S2CR/SMR/CB programming), `0123-...` (SMR0 stream 0 only), `0124-...` (no wzr), `0125-...` (no TRUST_CNTL write).
- GPU present: `0164-...` (gpuStart/gpuMap/gpuLayer, `sweet_fbcon_flip` MDP register flip).
- IOAcceleratorES + quiesce: `0166-...`.
- userland: `tools/sweetgl/{sweet_drm.c, sweetgl.c, eagl.c, gen-dispatch.py, ios-dispatch.txt, mesa-setup.sh, build.sh, ios-cross.txt, mesa.patch}`,
  `tools/gles-dispatch-layout.py`, `tools/ir3asm/main.c`, `tools/gpu-shaders/*.asm`, `tools/sweetgpu-ca/sweetgpu-ca.c` (superseded).
