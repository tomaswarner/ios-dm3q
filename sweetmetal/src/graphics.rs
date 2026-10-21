//! Textures, samplers, render and blit: the second slice of the C API. Every Metal enum or
//! option set crosses as its raw value (`MTLPixelFormat`, `MTLTextureUsage`, `MTLLoadAction`,
//! ...), so SweetMetal.bundle passes its descriptors' properties through unchanged; an unknown
//! value fails the call with the runtime's error. Slice 3 adds vertex descriptors, depth/stencil
//! states and attachments, and the encoder's fixed-function state.

use crate::{boxed, set_error};
use metal2vulkan::bind::SamplerDescriptor;
use metal2vulkan::metal::state::{DepthStencilDescriptor, StencilDescriptor};
use metal2vulkan::metal::{
    BarrierScope, BlendFactor, BlendOperation, ColorWriteMask, CompareFunction, CullMode, DepthClipMode,
    IndexType, PixelFormat, PrimitiveType, SamplerAddressMode, SamplerMinMagFilter,
    SamplerMipFilter, StencilOperation, TextureType, TextureUsage, TriangleFillMode, VertexFormat,
    VertexStepFunction, Winding,
};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource, VertexAttributeDescriptor,
    VertexBufferLayoutDescriptor, VertexDescriptor,
};
use metal2vulkan_runtime::{
    BlitCommandEncoder, BlitOption, Buffer, ClearColor, CommandBuffer, ComputeCommandEncoder,
    DepthStencilState, Device, Function, HostMemory, IoSurfacePlane, IoSurfaceYCbCr, LoadAction, Origin,
    PipelineOptions, Region,
    RenderCommandEncoder, RenderPassDescriptor, RenderPipelineState, ResourceOptions, SamplerState,
    ScissorRect, Size, StoreAction, Texture, TextureDescriptor, Viewport,
};
use std::ffi::c_void;
use std::ptr::{self, NonNull};

const MAX_COLOR: usize = 8;
/// `MTLVertexDescriptor`'s attribute and buffer layout slots.
const MAX_VERTEX: usize = 31;

/// A raw Metal enum value as its runtime type, or the error in [`crate::sm_last_error`].
fn raw<T: TryFrom<u32>>(value: u32) -> Option<T>
where
    T::Error: std::fmt::Display,
{
    T::try_from(value).map_err(set_error).ok()
}

fn ok<E: std::fmt::Display>(r: Result<(), E>) -> bool {
    r.map_err(set_error).is_ok()
}

unsafe fn opt<'a, T>(p: *const T) -> Option<&'a T> {
    if p.is_null() {
        None
    } else {
        Some(&*p)
    }
}

release_fn!(sm_texture_release, Texture);
release_fn!(sm_sampler_release, SamplerState);
release_fn!(sm_render_pipeline_release, RenderPipelineState);
release_fn!(sm_depth_stencil_release, DepthStencilState);

// ---- textures ---------------------------------------------------------------------------

/// `MTLTextureDescriptor`'s properties, raw.
#[repr(C)]
pub struct SmTextureDesc {
    pub texture_type: u32,
    pub pixel_format: u32,
    pub width: u64,
    pub height: u64,
    pub depth: u64,
    pub mipmap_level_count: u64,
    pub sample_count: u64,
    pub array_length: u64,
    pub usage: u32,
    pub resource_options: u64,
}

unsafe fn texture_descriptor(d: &SmTextureDesc) -> Option<TextureDescriptor> {
    let (Some(ty), Some(format), Some(usage)) =
        (raw::<TextureType>(d.texture_type), raw::<PixelFormat>(d.pixel_format), raw::<TextureUsage>(d.usage))
    else {
        return None;
    };
    let options = ResourceOptions::from_raw(d.resource_options).map_err(set_error).ok()?;
    let mut td = TextureDescriptor::texture_2d(format, d.width, d.height, false);
    td.texture_type = ty;
    td.depth = d.depth.max(1);
    td.mipmap_level_count = d.mipmap_level_count.max(1);
    td.sample_count = d.sample_count.max(1);
    td.array_length = d.array_length.max(1);
    td.usage = usage;
    td.resource_options = options;
    Some(td)
}

/// `newTextureWithDescriptor:`.
#[no_mangle]
pub unsafe extern "C" fn sm_texture_new(dev: *const Device, d: *const SmTextureDesc) -> *mut Texture {
    if dev.is_null() || d.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    let Some(td) = texture_descriptor(&*d) else { return ptr::null_mut() };
    boxed((*dev).new_texture(&td))
}

/// `newTextureWithDescriptor:iosurface:plane:`, over `buf`: the caller's no-copy buffer of the
/// surface's bytes ([`sm_buffer_new_no_copy`] at `IOSurfaceGetBaseAddress`). `offset` and
/// `bytes_per_row` are the plane's (0 and `IOSurfaceGetBytesPerRow` without planes); `planar` =
/// `IOSurfaceGetPlaneCount` > 0.
#[no_mangle]
pub unsafe extern "C" fn sm_texture_new_iosurface(
    buf: *const Buffer, d: *const SmTextureDesc, offset: u64, bytes_per_row: u64, planar: bool,
) -> *mut Texture {
    if buf.is_null() || d.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    let Some(td) = texture_descriptor(&*d) else { return ptr::null_mut() };
    boxed((*buf).new_texture_with_iosurface_plane(&td, IoSurfacePlane::new(offset, bytes_per_row, planar)))
}

/// The surface facts a two-plane texture over an IOSurface reads (YCbCr formats,
/// `RGB10A8_2P_XR10`): `IOSurfaceGetPixelFormat`, the plane asked of, the surface's size, plane
/// 1's offset (from the surface's base) and bytes per row.
#[repr(C)]
pub struct SmSurfaceFacts {
    pub pixel_format: u32,
    pub plane: u64,
    pub width: u64,
    pub height: u64,
    pub plane1_offset: u64,
    pub plane1_bytes_per_row: u64,
}

/// [`sm_texture_new_iosurface`] with the surface's facts (two-plane formats need them).
#[no_mangle]
pub unsafe extern "C" fn sm_texture_new_iosurface_facts(
    buf: *const Buffer, d: *const SmTextureDesc, offset: u64, bytes_per_row: u64, planar: bool,
    facts: *const SmSurfaceFacts,
) -> *mut Texture {
    if buf.is_null() || d.is_null() || facts.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    let Some(td) = texture_descriptor(&*d) else { return ptr::null_mut() };
    let f = &*facts;
    let ycbcr = IoSurfaceYCbCr::new(f.pixel_format, f.plane, [f.width, f.height], f.plane1_offset, f.plane1_bytes_per_row);
    let plane = IoSurfacePlane::new(offset, bytes_per_row, planar).with_ycbcr(ycbcr);
    boxed((*buf).new_texture_with_iosurface_plane(&td, plane))
}

/// `newBufferWithBytesNoCopy:length:options:deallocator:` callback: `(pointer, length, ctx)`.
pub type SmDeallocator = Option<unsafe extern "C" fn(*mut u8, u64, *mut c_void)>;

struct Ctx(*mut c_void);
// SAFETY: the caller's context is theirs to make thread-safe (the deallocator runs on a runtime
// thread, as Metal's block does).
unsafe impl Send for Ctx {}

/// `newBufferWithBytesNoCopy:length:options:deallocator:` (raw `MTLResourceOptions`); the
/// deallocator (may be null) runs once with `ctx` when the buffer's last use is gone.
#[no_mangle]
pub unsafe extern "C" fn sm_buffer_new_no_copy(
    dev: *const Device, bytes: *mut u8, length: u64, options: u64, dealloc: SmDeallocator, ctx: *mut c_void,
) -> *mut Buffer {
    let (Some(dev), Some(p)) = (opt(dev), NonNull::new(bytes)) else {
        set_error("null argument");
        return ptr::null_mut();
    };
    let options = match ResourceOptions::from_raw(options) {
        Ok(o) => o,
        Err(e) => {
            set_error(e);
            return ptr::null_mut();
        }
    };
    let ctx = Ctx(ctx);
    let dealloc = dealloc.map(|f| -> metal2vulkan_runtime::Deallocator {
        Box::new(move |p: NonNull<u8>, len: u64| {
            let ctx = ctx;
            f(p.as_ptr(), len, ctx.0)
        })
    });
    boxed(dev.new_buffer_with_bytes_no_copy(HostMemory::new(p), length, options, dealloc))
}

/// `-[MTLTexture width]`, `height`, `pixelFormat` (raw).
#[no_mangle]
pub unsafe extern "C" fn sm_texture_width(t: *const Texture) -> u64 {
    opt(t).map_or(0, |t| t.width())
}

#[no_mangle]
pub unsafe extern "C" fn sm_texture_height(t: *const Texture) -> u64 {
    opt(t).map_or(0, |t| t.height())
}

/// `replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:` (2D region).
#[no_mangle]
pub unsafe extern "C" fn sm_texture_replace_region(
    t: *const Texture, x: u64, y: u64, w: u64, h: u64, level: u64, slice: u64,
    bytes: *const u8, len: usize, bytes_per_row: u64, bytes_per_image: u64,
) -> bool {
    let Some(t) = opt(t) else { return false };
    if bytes.is_null() {
        set_error("null bytes");
        return false;
    }
    let data = std::slice::from_raw_parts(bytes, len);
    ok(t.replace_region(Region::new_2d(x, y, w, h), level, slice, data, bytes_per_row, bytes_per_image))
}

/// `getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:` (2D region).
#[no_mangle]
pub unsafe extern "C" fn sm_texture_get_bytes(
    t: *const Texture, bytes: *mut u8, len: usize, bytes_per_row: u64, bytes_per_image: u64,
    x: u64, y: u64, w: u64, h: u64, level: u64, slice: u64,
) -> bool {
    let Some(t) = opt(t) else { return false };
    if bytes.is_null() {
        set_error("null bytes");
        return false;
    }
    let data = std::slice::from_raw_parts_mut(bytes, len);
    ok(t.get_bytes(data, bytes_per_row, bytes_per_image, Region::new_2d(x, y, w, h), level, slice))
}

// ---- samplers ---------------------------------------------------------------------------

/// `MTLSamplerDescriptor`'s common properties, raw.
#[repr(C)]
pub struct SmSamplerDesc {
    pub min_filter: u32,
    pub mag_filter: u32,
    pub mip_filter: u32,
    pub s_address_mode: u32,
    pub t_address_mode: u32,
    pub r_address_mode: u32,
    pub normalized_coordinates: bool,
}

/// `newSamplerStateWithDescriptor:`.
#[no_mangle]
pub unsafe extern "C" fn sm_sampler_new(dev: *const Device, d: *const SmSamplerDesc) -> *mut SamplerState {
    if dev.is_null() || d.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    let d = &*d;
    let mut sd = SamplerDescriptor::new();
    let (Some(min), Some(mag), Some(mip), Some(s), Some(t), Some(r)) = (
        raw::<SamplerMinMagFilter>(d.min_filter),
        raw::<SamplerMinMagFilter>(d.mag_filter),
        raw::<SamplerMipFilter>(d.mip_filter),
        raw::<SamplerAddressMode>(d.s_address_mode),
        raw::<SamplerAddressMode>(d.t_address_mode),
        raw::<SamplerAddressMode>(d.r_address_mode),
    ) else {
        return ptr::null_mut();
    };
    sd.min_filter = min;
    sd.mag_filter = mag;
    sd.mip_filter = mip;
    sd.s_address_mode = s;
    sd.t_address_mode = t;
    sd.r_address_mode = r;
    sd.normalized_coordinates = d.normalized_coordinates;
    boxed((*dev).new_sampler_state(&sd))
}

// ---- render pipelines -------------------------------------------------------------------

/// One `MTLRenderPipelineColorAttachmentDescriptor`, raw; `pixel_format` 0 (Invalid) = unused.
#[repr(C)]
pub struct SmColorAttachment {
    pub pixel_format: u32,
    pub blending_enabled: bool,
    pub source_rgb_blend_factor: u32,
    pub destination_rgb_blend_factor: u32,
    pub rgb_blend_operation: u32,
    pub source_alpha_blend_factor: u32,
    pub destination_alpha_blend_factor: u32,
    pub alpha_blend_operation: u32,
    pub write_mask: u32,
}

/// One `MTLVertexAttributeDescriptor`, raw; `format` 0 (Invalid) = unset.
#[repr(C)]
pub struct SmVertexAttribute {
    pub format: u32,
    pub offset: u32,
    pub buffer_index: u32,
}

/// One `MTLVertexBufferLayoutDescriptor`, raw; `stride` 0 = unset (Metal's default layout),
/// `u32::MAX` = `MTLBufferLayoutStrideDynamic`.
#[repr(C)]
pub struct SmVertexLayout {
    pub stride: u32,
    pub step_function: u32,
    pub step_rate: u32,
}

/// `MTLVertexDescriptor`: attributes and buffer layouts by index.
#[repr(C)]
pub struct SmVertexDescriptor {
    pub attributes: [SmVertexAttribute; MAX_VERTEX],
    pub layouts: [SmVertexLayout; MAX_VERTEX],
}

/// `MTLRenderPipelineDescriptor`'s properties; a null `vertex_descriptor` is nil (shaders read
/// buffers themselves).
#[repr(C)]
pub struct SmRenderPipelineDesc {
    pub vertex: *const Function,
    pub fragment: *const Function,
    pub color: [SmColorAttachment; MAX_COLOR],
    pub depth_format: u32,
    pub stencil_format: u32,
    pub raster_sample_count: u32,
    pub vertex_descriptor: *const SmVertexDescriptor,
}

unsafe fn vertex_descriptor(d: &SmVertexDescriptor) -> Option<VertexDescriptor> {
    let mut vd = VertexDescriptor::new();
    for (i, a) in d.attributes.iter().enumerate() {
        if a.format != 0 {
            let format = raw::<VertexFormat>(a.format)?;
            vd.attributes.insert(i as u32, VertexAttributeDescriptor::new(format, a.offset, a.buffer_index));
        }
    }
    for (i, l) in d.layouts.iter().enumerate() {
        if l.stride != 0 {
            let mut layout = VertexBufferLayoutDescriptor::new((l.stride != u32::MAX).then_some(l.stride));
            layout.step_function = raw::<VertexStepFunction>(l.step_function)?;
            layout.step_rate = l.step_rate;
            vd.layouts.insert(i as u32, layout);
        }
    }
    Some(vd)
}

unsafe fn color_attachment(c: &SmColorAttachment) -> Option<ColorAttachmentDescriptor> {
    let mut a = ColorAttachmentDescriptor::new(raw(c.pixel_format)?);
    a.blending_enabled = c.blending_enabled;
    a.source_rgb_blend_factor = raw::<BlendFactor>(c.source_rgb_blend_factor)?;
    a.destination_rgb_blend_factor = raw::<BlendFactor>(c.destination_rgb_blend_factor)?;
    a.rgb_blend_operation = raw::<BlendOperation>(c.rgb_blend_operation)?;
    a.source_alpha_blend_factor = raw::<BlendFactor>(c.source_alpha_blend_factor)?;
    a.destination_alpha_blend_factor = raw::<BlendFactor>(c.destination_alpha_blend_factor)?;
    a.alpha_blend_operation = raw::<BlendOperation>(c.alpha_blend_operation)?;
    a.write_mask = raw::<ColorWriteMask>(c.write_mask)?;
    Some(a)
}

/// `newRenderPipelineStateWithDescriptor:error:`.
#[no_mangle]
pub unsafe extern "C" fn sm_render_pipeline_new(
    dev: *const Device, d: *const SmRenderPipelineDesc,
) -> *mut RenderPipelineState {
    if dev.is_null() || d.is_null() || (*d).vertex.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    let d = &*d;
    let mut rd = RenderPipelineDescriptor::new(StageSource::Function((*d.vertex).function_descriptor()));
    rd.fragment = opt(d.fragment).map(|f| StageSource::Function(f.function_descriptor()));
    for (i, c) in d.color.iter().enumerate() {
        if c.pixel_format != 0 {
            let Some(a) = color_attachment(c) else { return ptr::null_mut() };
            rd.color_attachments[i] = Some(a);
        }
    }
    if d.depth_format != 0 {
        let Some(f) = raw(d.depth_format) else { return ptr::null_mut() };
        rd.depth_format = Some(f);
    }
    if d.stencil_format != 0 {
        let Some(f) = raw(d.stencil_format) else { return ptr::null_mut() };
        rd.stencil_format = Some(f);
    }
    if d.raster_sample_count != 0 {
        rd.raster_sample_count = d.raster_sample_count;
    }
    if let Some(v) = opt(d.vertex_descriptor) {
        let Some(vd) = vertex_descriptor(v) else { return ptr::null_mut() };
        rd.vertex_descriptor = Some(vd);
    }
    boxed((*dev).new_render_pipeline_state(&rd, &PipelineOptions::default()))
}

// ---- depth/stencil states ----------------------------------------------------------------

/// `MTLStencilDescriptor`, raw.
#[repr(C)]
pub struct SmStencilDesc {
    pub compare_function: u32,
    pub stencil_failure_operation: u32,
    pub depth_failure_operation: u32,
    pub depth_stencil_pass_operation: u32,
    pub read_mask: u32,
    pub write_mask: u32,
}

/// `MTLDepthStencilDescriptor`, raw.
#[repr(C)]
pub struct SmDepthStencilDesc {
    pub depth_compare_function: u32,
    pub depth_write_enabled: bool,
    pub front: SmStencilDesc,
    pub back: SmStencilDesc,
}

fn stencil(s: &SmStencilDesc) -> Option<StencilDescriptor> {
    let mut d = StencilDescriptor::default();
    d.stencil_compare_function = raw::<CompareFunction>(s.compare_function)?;
    d.stencil_failure_operation = raw::<StencilOperation>(s.stencil_failure_operation)?;
    d.depth_failure_operation = raw::<StencilOperation>(s.depth_failure_operation)?;
    d.depth_stencil_pass_operation = raw::<StencilOperation>(s.depth_stencil_pass_operation)?;
    d.read_mask = s.read_mask;
    d.write_mask = s.write_mask;
    Some(d)
}

/// `newDepthStencilStateWithDescriptor:`.
#[no_mangle]
pub unsafe extern "C" fn sm_depth_stencil_new(dev: *const Device, d: *const SmDepthStencilDesc) -> *mut DepthStencilState {
    if dev.is_null() || d.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    let d = &*d;
    let mut dd = DepthStencilDescriptor::default();
    let (Some(compare), Some(front), Some(back)) =
        (raw::<CompareFunction>(d.depth_compare_function), stencil(&d.front), stencil(&d.back))
    else {
        return ptr::null_mut();
    };
    dd.depth_compare_function = compare;
    dd.depth_write_enabled = d.depth_write_enabled;
    dd.front_face_stencil = front;
    dd.back_face_stencil = back;
    boxed((*dev).new_depth_stencil_state(&dd))
}

// ---- render passes and encoders ---------------------------------------------------------

/// One `MTLRenderPassColorAttachmentDescriptor`, raw; a null texture = unused.
#[repr(C)]
pub struct SmPassColor {
    pub texture: *const Texture,
    pub level: u64,
    pub slice: u64,
    pub load_action: u32,
    pub store_action: u32,
    pub clear_color: [f64; 4],
}

/// `MTLRenderPassDepthAttachmentDescriptor` / `...StencilAttachmentDescriptor`, raw; a null
/// texture = unused. `clear_depth` is read for depth, `clear_stencil` for stencil.
#[repr(C)]
pub struct SmPassDepthStencil {
    pub texture: *const Texture,
    pub level: u64,
    pub slice: u64,
    pub load_action: u32,
    pub store_action: u32,
    pub clear_depth: f64,
    pub clear_stencil: u32,
}

#[repr(C)]
pub struct SmRenderPassDesc {
    pub color: [SmPassColor; MAX_COLOR],
    pub depth: SmPassDepthStencil,
    pub stencil: SmPassDepthStencil,
}

fn actions(load: u32, store: u32) -> Option<(LoadAction, StoreAction)> {
    match (LoadAction::from_raw(load.into()), StoreAction::from_raw(store.into())) {
        (Ok(l), Ok(s)) => Some((l, s)),
        (Err(e), _) | (_, Err(e)) => {
            set_error(e);
            None
        }
    }
}

/// `renderCommandEncoderWithDescriptor:`.
#[no_mangle]
pub unsafe extern "C" fn sm_render_encoder_new(
    cb: *const CommandBuffer, d: *const SmRenderPassDesc,
) -> *mut RenderCommandEncoder {
    if cb.is_null() || d.is_null() {
        set_error("null argument");
        return ptr::null_mut();
    }
    let mut pass = RenderPassDescriptor::new();
    for (i, c) in (*d).color.iter().enumerate() {
        let Some(t) = opt(c.texture) else { continue };
        let Some((load, store)) = actions(c.load_action, c.store_action) else { return ptr::null_mut() };
        let a = &mut pass.color_attachments[i];
        a.texture = Some(t.clone());
        a.level = c.level;
        a.slice = c.slice;
        a.load_action = load;
        a.store_action = store;
        let [r, g, b, al] = c.clear_color;
        a.clear_color = ClearColor::new(r, g, b, al);
    }
    let (d, s) = (&(*d).depth, &(*d).stencil);
    if let Some(t) = opt(d.texture) {
        let Some((load, store)) = actions(d.load_action, d.store_action) else { return ptr::null_mut() };
        let a = &mut pass.depth_attachment;
        a.texture = Some(t.clone());
        a.level = d.level;
        a.slice = d.slice;
        a.load_action = load;
        a.store_action = store;
        a.clear_depth = d.clear_depth;
    }
    if let Some(t) = opt(s.texture) {
        let Some((load, store)) = actions(s.load_action, s.store_action) else { return ptr::null_mut() };
        let a = &mut pass.stencil_attachment;
        a.texture = Some(t.clone());
        a.level = s.level;
        a.slice = s.slice;
        a.load_action = load;
        a.store_action = store;
        a.clear_stencil = s.clear_stencil;
    }
    boxed((*cb).render_command_encoder(&pass))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_pipeline(e: *mut RenderCommandEncoder, p: *const RenderPipelineState) -> bool {
    if e.is_null() || p.is_null() {
        return false;
    }
    ok((*e).set_render_pipeline_state(&*p))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_vertex_buffer(e: *mut RenderCommandEncoder, b: *const Buffer, offset: u64, index: u32) -> bool {
    !e.is_null() && ok((*e).set_vertex_buffer(opt(b), offset, index))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_vertex_bytes(e: *mut RenderCommandEncoder, bytes: *const u8, len: usize, index: u32) -> bool {
    !e.is_null() && !bytes.is_null() && ok((*e).set_vertex_bytes(std::slice::from_raw_parts(bytes, len), index))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_vertex_texture(e: *mut RenderCommandEncoder, t: *const Texture, index: u32) -> bool {
    !e.is_null() && ok((*e).set_vertex_texture(opt(t), index))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_fragment_buffer(e: *mut RenderCommandEncoder, b: *const Buffer, offset: u64, index: u32) -> bool {
    !e.is_null() && ok((*e).set_fragment_buffer(opt(b), offset, index))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_fragment_bytes(e: *mut RenderCommandEncoder, bytes: *const u8, len: usize, index: u32) -> bool {
    !e.is_null() && !bytes.is_null() && ok((*e).set_fragment_bytes(std::slice::from_raw_parts(bytes, len), index))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_fragment_texture(e: *mut RenderCommandEncoder, t: *const Texture, index: u32) -> bool {
    !e.is_null() && ok((*e).set_fragment_texture(opt(t), index))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_fragment_sampler(e: *mut RenderCommandEncoder, s: *const SamplerState, index: u32) -> bool {
    !e.is_null() && ok((*e).set_fragment_sampler_state(opt(s), index))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_vertex_sampler(e: *mut RenderCommandEncoder, s: *const SamplerState, index: u32) -> bool {
    !e.is_null() && ok((*e).set_vertex_sampler_state(opt(s), index))
}

/// `setViewport:` (`MTLViewport`: origin, size, znear, zfar).
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_viewport(
    e: *mut RenderCommandEncoder, x: f64, y: f64, w: f64, h: f64, znear: f64, zfar: f64,
) -> bool {
    !e.is_null()
        && ok((*e).set_viewport(Viewport { origin_x: x, origin_y: y, width: w, height: h, znear, zfar }))
}

#[no_mangle]
pub unsafe extern "C" fn sm_render_set_scissor(e: *mut RenderCommandEncoder, x: u64, y: u64, w: u64, h: u64) -> bool {
    !e.is_null() && ok((*e).set_scissor_rect(ScissorRect { x, y, width: w, height: h }))
}

/// `setDepthStencilState:` (null = nil: Metal's default state).
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_depth_stencil(e: *mut RenderCommandEncoder, s: *const DepthStencilState) -> bool {
    !e.is_null() && ok((*e).set_depth_stencil_state(opt(s)))
}

/// `setStencilFrontReferenceValue:backReferenceValue:`.
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_stencil_reference(e: *mut RenderCommandEncoder, front: u32, back: u32) -> bool {
    if e.is_null() {
        return false;
    }
    (*e).set_stencil_reference_values(front, back);
    true
}

/// `setCullMode:` (raw `MTLCullMode`).
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_cull_mode(e: *mut RenderCommandEncoder, mode: u32) -> bool {
    let Some(m) = raw::<CullMode>(mode) else { return false };
    if e.is_null() {
        return false;
    }
    (*e).set_cull_mode(m);
    true
}

/// `setFrontFacingWinding:` (raw `MTLWinding`).
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_front_facing_winding(e: *mut RenderCommandEncoder, winding: u32) -> bool {
    let Some(w) = raw::<Winding>(winding) else { return false };
    if e.is_null() {
        return false;
    }
    (*e).set_front_facing_winding(w);
    true
}

/// `setDepthBias:slopeScale:clamp:`.
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_depth_bias(e: *mut RenderCommandEncoder, bias: f32, slope_scale: f32, clamp: f32) -> bool {
    !e.is_null() && ok((*e).set_depth_bias(bias, slope_scale, clamp))
}

/// `setDepthClipMode:` (raw `MTLDepthClipMode`).
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_depth_clip_mode(e: *mut RenderCommandEncoder, mode: u32) -> bool {
    let Some(m) = raw::<DepthClipMode>(mode) else { return false };
    !e.is_null() && ok((*e).set_depth_clip_mode(m))
}

/// `setTriangleFillMode:` (raw `MTLTriangleFillMode`).
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_triangle_fill_mode(e: *mut RenderCommandEncoder, mode: u32) -> bool {
    let Some(m) = raw::<TriangleFillMode>(mode) else { return false };
    !e.is_null() && ok((*e).set_triangle_fill_mode(m))
}

/// `setBlendColorRed:green:blue:alpha:`.
#[no_mangle]
pub unsafe extern "C" fn sm_render_set_blend_color(e: *mut RenderCommandEncoder, r: f32, g: f32, b: f32, a: f32) -> bool {
    if e.is_null() {
        return false;
    }
    (*e).set_blend_color(r, g, b, a);
    true
}

/// `drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:` (raw `MTLPrimitiveType`).
#[no_mangle]
pub unsafe extern "C" fn sm_render_draw(
    e: *mut RenderCommandEncoder, primitive: u32, start: u32, count: u32, instances: u32, base_instance: u32,
) -> bool {
    let Some(p) = raw::<PrimitiveType>(primitive) else { return false };
    !e.is_null() && ok((*e).draw_primitives(p, start, count, instances.max(1), base_instance))
}

/// `drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:
/// baseVertex:baseInstance:` (raw `MTLPrimitiveType`, `MTLIndexType`).
#[no_mangle]
pub unsafe extern "C" fn sm_render_draw_indexed(
    e: *mut RenderCommandEncoder, primitive: u32, index_count: u32, index_type: u32,
    index_buffer: *const Buffer, index_offset: u64, instances: u32, base_vertex: i32, base_instance: u32,
) -> bool {
    let (Some(p), Some(it)) = (raw::<PrimitiveType>(primitive), raw::<IndexType>(index_type)) else {
        return false;
    };
    let Some(ib) = opt(index_buffer) else {
        set_error("null index buffer");
        return false;
    };
    !e.is_null()
        && ok((*e).draw_indexed_primitives(p, index_count, it, ib, index_offset, instances.max(1), base_vertex, base_instance))
}

/// `endEncoding`: consumes and frees the encoder.
#[no_mangle]
pub unsafe extern "C" fn sm_render_end_encoding(e: *mut RenderCommandEncoder) -> bool {
    !e.is_null() && ok(Box::from_raw(e).end_encoding())
}

/// `setTexture:atIndex:` on a compute encoder.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_set_texture(e: *mut ComputeCommandEncoder, t: *const Texture, index: u32) -> bool {
    !e.is_null() && ok((*e).set_texture(opt(t), index))
}

/// `setSamplerState:atIndex:` on a compute encoder.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_set_sampler(e: *mut ComputeCommandEncoder, s: *const SamplerState, index: u32) -> bool {
    !e.is_null() && ok((*e).set_sampler_state(opt(s), index))
}

/// `setBufferOffset:atIndex:` on a compute encoder.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_set_buffer_offset(e: *mut ComputeCommandEncoder, offset: u64, index: u32) -> bool {
    !e.is_null() && ok((*e).set_buffer_offset(offset, index))
}

/// `setThreadgroupMemoryLength:atIndex:`.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_set_threadgroup_memory(e: *mut ComputeCommandEncoder, length: u32, index: u32) -> bool {
    !e.is_null() && ok((*e).set_threadgroup_memory_length(length, index))
}

/// `memoryBarrierWithScope:` (raw `MTLBarrierScope`).
#[no_mangle]
pub unsafe extern "C" fn sm_compute_memory_barrier(e: *mut ComputeCommandEncoder, scope: u32) -> bool {
    let Some(sc) = raw::<BarrierScope>(scope) else { return false };
    if e.is_null() {
        return false;
    }
    (*e).memory_barrier_with_scope(sc);
    true
}

/// `setBytes:length:atIndex:` on a compute encoder.
#[no_mangle]
pub unsafe extern "C" fn sm_compute_set_bytes(e: *mut ComputeCommandEncoder, bytes: *const u8, len: usize, index: u32) -> bool {
    !e.is_null() && !bytes.is_null() && ok((*e).set_bytes(std::slice::from_raw_parts(bytes, len), index))
}

// ---- blit -------------------------------------------------------------------------------

/// `-[MTLCommandBuffer blitCommandEncoder]`.
#[no_mangle]
pub unsafe extern "C" fn sm_blit_encoder_new(cb: *const CommandBuffer) -> *mut BlitCommandEncoder {
    if cb.is_null() {
        set_error("null command buffer");
        return ptr::null_mut();
    }
    boxed((*cb).blit_command_encoder())
}

#[no_mangle]
pub unsafe extern "C" fn sm_blit_copy_buffer(
    e: *mut BlitCommandEncoder, src: *const Buffer, src_offset: u64, dst: *const Buffer, dst_offset: u64, size: u64,
) -> bool {
    let (Some(s), Some(d)) = (opt(src), opt(dst)) else { return false };
    !e.is_null() && ok((*e).copy_from_buffer_to_buffer(s, src_offset, d, dst_offset, size))
}

#[no_mangle]
pub unsafe extern "C" fn sm_blit_fill_buffer(e: *mut BlitCommandEncoder, b: *const Buffer, start: u64, len: u64, value: u8) -> bool {
    let Some(b) = opt(b) else { return false };
    !e.is_null() && ok((*e).fill_buffer(b, start..start + len, value))
}

/// `copyFromBuffer:...toTexture:...` (2D: w x h at x, y).
#[no_mangle]
pub unsafe extern "C" fn sm_blit_buffer_to_texture(
    e: *mut BlitCommandEncoder, src: *const Buffer, offset: u64, bytes_per_row: u64, bytes_per_image: u64,
    w: u64, h: u64, dst: *const Texture, slice: u64, level: u64, x: u64, y: u64,
) -> bool {
    let (Some(s), Some(d)) = (opt(src), opt(dst)) else { return false };
    !e.is_null()
        && ok((*e).copy_from_buffer_to_texture(
            s, offset, bytes_per_row, bytes_per_image, Size::new(w, h, 1), d, slice, level,
            Origin::new(x, y, 0), BlitOption::None,
        ))
}

/// `copyFromTexture:...toBuffer:...` (2D: w x h at x, y).
#[no_mangle]
pub unsafe extern "C" fn sm_blit_texture_to_buffer(
    e: *mut BlitCommandEncoder, src: *const Texture, slice: u64, level: u64, x: u64, y: u64, w: u64, h: u64,
    dst: *const Buffer, offset: u64, bytes_per_row: u64, bytes_per_image: u64,
) -> bool {
    let (Some(s), Some(d)) = (opt(src), opt(dst)) else { return false };
    !e.is_null()
        && ok((*e).copy_from_texture_to_buffer(
            s, slice, level, Origin::new(x, y, 0), Size::new(w, h, 1), d, offset, bytes_per_row,
            bytes_per_image, BlitOption::None,
        ))
}

/// `copyFromTexture:...toTexture:...` (2D: w x h from sx, sy to dx, dy).
#[no_mangle]
pub unsafe extern "C" fn sm_blit_texture_to_texture(
    e: *mut BlitCommandEncoder, src: *const Texture, src_slice: u64, src_level: u64, sx: u64, sy: u64,
    w: u64, h: u64, dst: *const Texture, dst_slice: u64, dst_level: u64, dx: u64, dy: u64,
) -> bool {
    let (Some(s), Some(d)) = (opt(src), opt(dst)) else { return false };
    !e.is_null()
        && ok((*e).copy_from_texture_to_texture(
            s, src_slice, src_level, Origin::new(sx, sy, 0), Size::new(w, h, 1), d, dst_slice,
            dst_level, Origin::new(dx, dy, 0),
        ))
}

#[no_mangle]
pub unsafe extern "C" fn sm_blit_generate_mipmaps(e: *mut BlitCommandEncoder, t: *const Texture) -> bool {
    let Some(t) = opt(t) else { return false };
    !e.is_null() && ok((*e).generate_mipmaps(t))
}

/// `endEncoding`: consumes and frees the encoder.
#[no_mangle]
pub unsafe extern "C" fn sm_blit_end_encoding(e: *mut BlitCommandEncoder) -> bool {
    !e.is_null() && ok(Box::from_raw(e).end_encoding())
}
