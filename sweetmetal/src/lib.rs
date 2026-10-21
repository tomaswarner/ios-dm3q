//! sweetmetal: a C API over metal2vulkan-runtime for SweetMetal.bundle, the
//! Metal driver of the S23 Ultra iOS port (Metal -> metal2vulkan -> Vulkan ->
//! lavapipe now, Turnip on the Adreno 740 later).
//!
//! Every object is an opaque pointer to a heap box the caller frees with its
//! `_release` function. Fallible calls return null (or false) and leave the
//! reason in [`sm_last_error`]. Encoders follow Metal: `sm_*_end_encoding`
//! consumes the encoder, so its pointer is dead afterwards.
//!
//! Slices: device, buffers, command queue and buffer, library, function,
//! compute pipeline state and compute encoding (here); textures, samplers,
//! render pipelines and encoders, blit, vertex descriptors, depth/stencil,
//! no-copy buffers and IOSurface-backed textures ([`graphics`]). Events and
//! the rest come next, in the order QuartzCore and CoreImage use them.

use metal2vulkan_runtime::{
    Buffer, CommandBuffer, CommandQueue, ComputeCommandEncoder, ComputePipelineState, Device,
    DeviceOptions, Function, Library, MetalLevel, PipelineOptions, ResourceOptions, Size,
};
use std::cell::RefCell;
use std::ffi::{c_char, CStr};
use std::ptr;

thread_local! {
    static LAST_ERROR: RefCell<Vec<u8>> = RefCell::new(b"\0".to_vec());
}

fn set_error(msg: impl std::fmt::Display) {
    LAST_ERROR.with(|e| {
        let mut v = msg.to_string().into_bytes();
        v.retain(|&b| b != 0);
        v.push(0);
        *e.borrow_mut() = v;
    });
}

/// The last failure on this thread, NUL-terminated; valid until the next failing call.
#[no_mangle]
pub extern "C" fn sm_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr() as *const c_char)
}

fn boxed<T>(r: Result<T, impl std::fmt::Display>) -> *mut T {
    match r {
        Ok(v) => Box::into_raw(Box::new(v)),
        Err(e) => {
            set_error(e);
            ptr::null_mut()
        }
    }
}

unsafe fn str_arg<'a>(s: *const c_char) -> Option<&'a str> {
    if s.is_null() {
        set_error("null string");
        return None;
    }
    match CStr::from_ptr(s).to_str() {
        Ok(v) => Some(v),
        Err(e) => {
            set_error(e);
            None
        }
    }
}

macro_rules! release_fn {
    ($name:ident, $ty:ty) => {
        /// Frees the object (a Metal release of the last reference).
        #[no_mangle]
        pub unsafe extern "C" fn $name(p: *mut $ty) {
            if !p.is_null() {
                drop(Box::from_raw(p));
            }
        }
    };
}

mod graphics;

release_fn!(sm_device_release, Device);
release_fn!(sm_buffer_release, Buffer);
release_fn!(sm_queue_release, CommandQueue);
release_fn!(sm_command_buffer_release, CommandBuffer);
release_fn!(sm_library_release, Library);
release_fn!(sm_function_release, Function);
release_fn!(sm_compute_pipeline_release, ComputePipelineState);

/// `MTLCreateSystemDefaultDevice()`: the runtime's preferred Vulkan device. The Metal level
/// (which measured Metal release the device answers as) is pinned to the newest: iOS gets it
/// anyway (not macOS), and a macOS host without a measured release would refuse the device.
#[no_mangle]
pub extern "C" fn sm_device_create() -> *mut Device {
    let mut options = DeviceOptions::default();
    options.metal_level = Some(MetalLevel::NEWEST);
    options.shader_cache.directory = shader_cache_dir();
    options.shader_cache.capacity = 2 << 30;
    options.loader = vulkan_loader();
    boxed(Device::system_default_with(&options))
}

/// Where translated shaders are cached: `SWEETMETAL_CACHE`, else the runtime's per-user default
/// under `HOME`, else (launchd daemons and boot-time tests run without `HOME`)
/// `/private/var/tmp/metal2vulkan-<uid>`.
fn shader_cache_dir() -> Option<std::path::PathBuf> {
    let set = |name| std::env::var_os(name).filter(|v| !v.is_empty());
    if let Some(dir) = set("SWEETMETAL_CACHE") {
        return Some(dir.into());
    }
    if set("HOME").is_some() {
        return None;
    }
    let uid = unsafe { getuid() };
    Some(format!("/private/var/tmp/metal2vulkan-{uid}").into())
}

extern "C" {
    fn getuid() -> u32;
}

/// The Vulkan loader: `SWEETMETAL_VULKAN`, else on iOS (no Khronos loader) our mini-loader
/// `/usr/lib/libvulkan.dylib` (tools/sweettest/libvulkan_shim.c, forwards to lavapipe), else the
/// runtime's platform defaults.
fn vulkan_loader() -> Option<std::path::PathBuf> {
    if let Some(path) = std::env::var_os("SWEETMETAL_VULKAN").filter(|v| !v.is_empty()) {
        return Some(path.into());
    }
    cfg!(target_os = "ios").then(|| "/usr/lib/libvulkan.dylib".into())
}

/// The device name (`-[MTLDevice name]`), copied into `buf` (NUL-terminated, truncated).
#[no_mangle]
pub unsafe extern "C" fn sm_device_name(dev: *const Device, buf: *mut c_char, len: usize) {
    if dev.is_null() || buf.is_null() || len == 0 {
        return;
    }
    let name = (*dev).name();
    let n = name.len().min(len - 1);
    ptr::copy_nonoverlapping(name.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
}

/// `newBufferWithLength:options:` with shared (CPU-visible) storage.
#[no_mangle]
pub unsafe extern "C" fn sm_buffer_new(dev: *const Device, length: u64) -> *mut Buffer {
    if dev.is_null() {
        set_error("null device");
        return ptr::null_mut();
    }
    boxed((*dev).new_buffer(length, ResourceOptions::shared()))
}

/// `-[MTLBuffer contents]`: the CPU address of a shared buffer, or null.
#[no_mangle]
pub unsafe extern "C" fn sm_buffer_contents(buf: *const Buffer) -> *mut u8 {
    if buf.is_null() {
        return ptr::null_mut();
    }
    (*buf).contents().map_or(ptr::null_mut(), |p| p.as_ptr())
}

/// `-[MTLBuffer length]`.
#[no_mangle]
pub unsafe extern "C" fn sm_buffer_length(buf: *const Buffer) -> u64 {
    if buf.is_null() {
        0
    } else {
        (*buf).length()
    }
}

/// `newLibraryWithData:error:` from metallib bytes.
#[no_mangle]
pub unsafe extern "C" fn sm_library_new_with_data(dev: *const Device, data: *const u8, len: usize) -> *mut Library {
    if dev.is_null() || data.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    boxed((*dev).new_library_with_data(std::slice::from_raw_parts(data, len)))
}

/// Bare AIR bitcode (raw or in a bitcode wrapper, the `.air` that `metal -c` writes) as a
/// library. No Metal counterpart (`newLibraryWithData:` refuses AIR); used for shaders we build
/// ourselves and for tests.
#[no_mangle]
pub unsafe extern "C" fn sm_library_new_with_air(dev: *const Device, data: *const u8, len: usize) -> *mut Library {
    if dev.is_null() || data.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    match metal2vulkan::library::Library::from_air(std::slice::from_raw_parts(data, len)) {
        Ok(l) => Box::into_raw(Box::new((*dev).new_library_with_compile_library(l))),
        Err(e) => {
            set_error(e);
            ptr::null_mut()
        }
    }
}

/// `newFunctionWithName:`.
#[no_mangle]
pub unsafe extern "C" fn sm_library_function(lib: *const Library, name: *const c_char) -> *mut Function {
    let Some(name) = str_arg(name) else { return ptr::null_mut() };
    if lib.is_null() {
        set_error("null library");
        return ptr::null_mut();
    }
    boxed((*lib).new_function_with_name(name))
}

/// `newComputePipelineStateWithFunction:error:` (translates the function to SPIR-V).
#[no_mangle]
pub unsafe extern "C" fn sm_compute_pipeline_new(dev: *const Device, f: *const Function) -> *mut ComputePipelineState {
    if dev.is_null() || f.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    boxed((*dev).new_compute_pipeline_state_with_function(&*f, &PipelineOptions::default()))
}

/// `newCommandQueue`.
#[no_mangle]
pub unsafe extern "C" fn sm_queue_new(dev: *const Device) -> *mut CommandQueue {
    if dev.is_null() {
        set_error("null device");
        return ptr::null_mut();
    }
    boxed((*dev).new_command_queue())
}

/// `-[MTLCommandQueue commandBuffer]`.
#[no_mangle]
pub unsafe extern "C" fn sm_command_buffer_new(q: *const CommandQueue) -> *mut CommandBuffer {
    if q.is_null() {
        set_error("null queue");
        return ptr::null_mut();
    }
    boxed((*q).command_buffer())
}

/// `-[MTLCommandBuffer commit]`.
#[no_mangle]
pub unsafe extern "C" fn sm_command_buffer_commit(cb: *const CommandBuffer) -> bool {
    if cb.is_null() {
        return false;
    }
    (*cb).commit().map_err(set_error).is_ok()
}

/// `-[MTLCommandBuffer waitUntilCompleted]`.
#[no_mangle]
pub unsafe extern "C" fn sm_command_buffer_wait(cb: *const CommandBuffer) -> bool {
    if cb.is_null() {
        return false;
    }
    (*cb).wait_until_completed().map_err(set_error).is_ok()
}

/// `-[MTLCommandBuffer computeCommandEncoder]`.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_encoder_new(cb: *const CommandBuffer) -> *mut ComputeCommandEncoder {
    if cb.is_null() {
        set_error("null command buffer");
        return ptr::null_mut();
    }
    boxed((*cb).compute_command_encoder())
}

/// `setComputePipelineState:`.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_set_pipeline(enc: *mut ComputeCommandEncoder, p: *const ComputePipelineState) -> bool {
    if enc.is_null() || p.is_null() {
        return false;
    }
    (*enc).set_compute_pipeline_state(&*p).map_err(set_error).is_ok()
}

/// `setBuffer:offset:atIndex:` (null buffer unbinds).
#[no_mangle]
pub unsafe extern "C" fn sm_compute_set_buffer(enc: *mut ComputeCommandEncoder, buf: *const Buffer, offset: u64, index: u32) -> bool {
    if enc.is_null() {
        return false;
    }
    let b = if buf.is_null() { None } else { Some(&*buf) };
    (*enc).set_buffer(b, offset, index).map_err(set_error).is_ok()
}

/// `dispatchThreads:threadsPerThreadgroup:` (1-D and 2-D/3-D sizes).
#[no_mangle]
pub unsafe extern "C" fn sm_compute_dispatch_threads(
    enc: *mut ComputeCommandEncoder, tx: u64, ty: u64, tz: u64, gx: u64, gy: u64, gz: u64,
) -> bool {
    if enc.is_null() {
        return false;
    }
    (*enc).dispatch_threads(Size::new(tx, ty, tz), Size::new(gx, gy, gz)).map_err(set_error).is_ok()
}

/// `dispatchThreadgroups:threadsPerThreadgroup:`.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_dispatch_threadgroups(
    enc: *mut ComputeCommandEncoder, gx: u64, gy: u64, gz: u64, tx: u64, ty: u64, tz: u64,
) -> bool {
    if enc.is_null() {
        return false;
    }
    (*enc).dispatch_threadgroups(Size::new(gx, gy, gz), Size::new(tx, ty, tz)).map_err(set_error).is_ok()
}

/// `maxTotalThreadsPerThreadgroup`, `threadExecutionWidth`, `staticThreadgroupMemoryLength`.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_pipeline_limits(p: *const ComputePipelineState, out: *mut u32) {
    if p.is_null() || out.is_null() {
        return;
    }
    let p = &*p;
    *out = p.max_total_threads_per_threadgroup();
    *out.add(1) = p.thread_execution_width();
    *out.add(2) = p.static_threadgroup_memory_length();
}

/// `endEncoding`: consumes and frees the encoder.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_end_encoding(enc: *mut ComputeCommandEncoder) -> bool {
    if enc.is_null() {
        return false;
    }
    Box::from_raw(enc).end_encoding().map_err(set_error).is_ok()
}
