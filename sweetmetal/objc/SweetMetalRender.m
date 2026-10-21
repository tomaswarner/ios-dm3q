/* SweetMetal.bundle slice 2: textures (own and over IOSurfaces), samplers, depth/stencil states,
 * render pipelines (with vertex descriptors), render and blit encoders, no-copy buffers. Metal
 * enums cross to sweetmetal as their raw values. */
#import "SweetMetalPrivate.h"
#import <IOSurface/IOSurfaceRef.h>
#include <unistd.h>

/* ---- textures ----------------------------------------------------------------------------- */

@interface SweetMTLTexture : NSObject <MTLTexture>
@property (readonly) sm_texture *sm;
- (instancetype)initWithDevice:(id<MTLDevice>)device texture:(sm_texture *)t descriptor:(MTLTextureDescriptor *)d;
@end

@implementation SweetMTLTexture {
	id<MTLDevice> _device;
	MTLTextureDescriptor *_d;
	sm_buffer *_backing;     /* IOSurface bytes, for a surface's texture */
	IOSurfaceRef _surface;
	NSUInteger _plane;
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device texture:(sm_texture *)t descriptor:(MTLTextureDescriptor *)d
{
	if ((self = [super init])) {
		_device = device;
		_sm = t;
		_d = [d copy];
	}
	return self;
}

- (void)setSurface:(IOSurfaceRef)s plane:(NSUInteger)plane backing:(sm_buffer *)b
{
	_surface = (IOSurfaceRef)CFRetain(s);
	_plane = plane;
	_backing = b;
}

- (void)dealloc
{
	sm_texture_release(_sm);
	sm_buffer_release(_backing);
	if (_surface)
		CFRelease(_surface);
}

- (id<MTLDevice>)device { return _device; }
- (MTLTextureType)textureType { return _d.textureType; }
- (MTLPixelFormat)pixelFormat { return _d.pixelFormat; }
- (NSUInteger)width { return _d.width; }
- (NSUInteger)height { return _d.height; }
- (NSUInteger)depth { return _d.depth; }
- (NSUInteger)mipmapLevelCount { return _d.mipmapLevelCount; }
- (NSUInteger)sampleCount { return _d.sampleCount; }
- (NSUInteger)arrayLength { return _d.arrayLength; }
- (MTLTextureUsage)usage { return _d.usage; }
- (MTLResourceOptions)resourceOptions { return _d.resourceOptions; }
- (MTLStorageMode)storageMode { return _d.storageMode; }
- (MTLCPUCacheMode)cpuCacheMode { return _d.cpuCacheMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (MTLTextureSwizzleChannels)swizzle { return MTLTextureSwizzleChannelsDefault; }
- (BOOL)isFramebufferOnly { return NO; }
- (BOOL)isShareable { return NO; }
- (IOSurfaceRef)iosurface { return _surface; }
- (uint64_t)protectionOptions { return 0; }
- (NSUInteger)iosurfacePlane { return _plane; }
- (id<MTLTexture>)parentTexture { return nil; }
- (id<MTLBuffer>)buffer { return nil; }
- (id<MTLResource>)rootResource { return nil; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)allocatedSize { return 0; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
- (void)makeAliasable { }
- (BOOL)isAliasable { return NO; }

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice withBytes:(const void *)bytes
	  bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi
{
	size_t len = bpr * r.size.height;
	if (!sm_texture_replace_region(_sm, r.origin.x, r.origin.y, r.size.width, r.size.height, level, slice, bytes, len, bpr, bpi))
		sm_error(@"replaceRegion");
}

- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)bpr
{
	[self replaceRegion:r mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:bpr bytesPerImage:0];
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr bytesPerImage:(NSUInteger)bpi fromRegion:(MTLRegion)r
     mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
{
	size_t len = bpr * r.size.height;
	if (!sm_texture_get_bytes(_sm, bytes, len, bpr, bpi, r.origin.x, r.origin.y, r.size.width, r.size.height, level, slice))
		sm_error(@"getBytes");
}

- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)bpr fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)level
{
	[self getBytes:bytes bytesPerRow:bpr bytesPerImage:0 fromRegion:r mipmapLevel:level slice:0];
}
MISSING_LOGGER
@end

static sm_texture_desc texture_desc(MTLTextureDescriptor *d)
{
	return (sm_texture_desc){
		.texture_type = (uint32_t)d.textureType, .pixel_format = (uint32_t)d.pixelFormat,
		.width = d.width, .height = d.height, .depth = d.depth,
		.mipmap_level_count = d.mipmapLevelCount, .sample_count = d.sampleCount,
		.array_length = d.arrayLength, .usage = (uint32_t)d.usage, .resource_options = d.resourceOptions,
	};
}

/* ---- states ------------------------------------------------------------------------------- */

#define STATE_CLASS(Name, Proto, ctype, release) \
	@interface Name : NSObject <Proto> \
	@property (readonly) ctype *sm; \
	@end \
	@implementation Name { \
		id<MTLDevice> _device; \
	} \
	@synthesize label; \
	-(instancetype)initWithDevice:(id<MTLDevice>)device sm:(ctype *)p \
	{ \
		if ((self = [super init])) { \
			_device = device; \
			_sm = p; \
		} \
		return self; \
	} \
	-(void)dealloc { release(_sm); } \
	-(id<MTLDevice>)device { return _device; } \
	MISSING_LOGGER \
	@end

STATE_CLASS(SweetMTLSamplerState, MTLSamplerState, sm_sampler, sm_sampler_release)
STATE_CLASS(SweetMTLDepthStencilState, MTLDepthStencilState, sm_depth_stencil, sm_depth_stencil_release)

@interface SweetMTLRenderPipelineState : NSObject <MTLRenderPipelineState>
@property (readonly) sm_render_pipeline *sm;
@end

@implementation SweetMTLRenderPipelineState {
	id<MTLDevice> _device;
	sm_library *_libs[2];
	sm_function *_fns[2];
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device sm:(sm_render_pipeline *)p libs:(sm_library **)libs fns:(sm_function **)fns
{
	if ((self = [super init])) {
		_device = device;
		_sm = p;
		memcpy(_libs, libs, sizeof _libs);
		memcpy(_fns, fns, sizeof _fns);
	}
	return self;
}

- (void)dealloc
{
	sm_render_pipeline_release(_sm);
	for (int i = 0; i < 2; i++)
		sm_function_release(_fns[i]);   /* _libs belong to the device's AIR cache */
}

- (id<MTLDevice>)device { return _device; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 0; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
MISSING_LOGGER
@end

/* ---- device: resources, states, pipelines ------------------------------------------------- */

static void run_deallocator(void *bytes, uint64_t length, void *ctx)
{
	void (^block)(void *, NSUInteger) = (__bridge_transfer void (^)(void *, NSUInteger))ctx;
	if (block)
		block(bytes, (NSUInteger)length);
}

static void release_surface(void *bytes, uint64_t length, void *ctx)
{
	CFRelease((IOSurfaceRef)ctx);
}

/* Everything of a render pipeline descriptor the pipeline is made from, as a cache key. */
static NSString *render_key(SweetMTLDevice *dev, MTLRenderPipelineDescriptor *d)
{
	NSMutableString *k = [NSMutableString stringWithFormat:@"render|%@|%@|%lu|%lu|%lu", [dev keyForFunction:d.vertexFunction],
		[dev keyForFunction:d.fragmentFunction], (unsigned long)d.depthAttachmentPixelFormat,
		(unsigned long)d.stencilAttachmentPixelFormat, (unsigned long)d.rasterSampleCount];
	for (int i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
		if (c.pixelFormat == MTLPixelFormatInvalid)
			continue;
		[k appendFormat:@"|c%d:%lu,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu", i, (unsigned long)c.pixelFormat, c.blendingEnabled,
			(unsigned long)c.sourceRGBBlendFactor, (unsigned long)c.destinationRGBBlendFactor,
			(unsigned long)c.rgbBlendOperation, (unsigned long)c.sourceAlphaBlendFactor,
			(unsigned long)c.destinationAlphaBlendFactor, (unsigned long)c.alphaBlendOperation, (unsigned long)c.writeMask];
	}
	MTLVertexDescriptor *vd = d.vertexDescriptor;
	for (int i = 0; vd && i < 31; i++) {
		MTLVertexAttributeDescriptor *a = vd.attributes[i];
		MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
		if (a.format != MTLVertexFormatInvalid)
			[k appendFormat:@"|a%d:%lu,%lu,%lu", i, (unsigned long)a.format, (unsigned long)a.offset, (unsigned long)a.bufferIndex];
		if (l.stride)
			[k appendFormat:@"|l%d:%lu,%lu,%lu", i, (unsigned long)l.stride, (unsigned long)l.stepFunction, (unsigned long)l.stepRate];
	}
	return k;
}

@implementation SweetMTLDevice (Render)

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options
			      deallocator:(void (^)(void *pointer, NSUInteger length))deallocator
{
	void *ctx = deallocator ? (__bridge_retained void *)[deallocator copy] : NULL;
	sm_buffer *b = sm_buffer_new_no_copy(self.sm, pointer, length, options, ctx ? run_deallocator : NULL, ctx);
	if (!b) {
		sm_error(@"newBufferWithBytesNoCopy");
		if (ctx)
			CFRelease(ctx);
		return nil;
	}
	return [[SweetMTLBuffer alloc] initWithDevice:self buffer:b options:options];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
	SMTRACE("texture %lux%lu fmt %lu usage %lx", (unsigned long)d.width, (unsigned long)d.height,
		(unsigned long)d.pixelFormat, (unsigned long)d.usage);
	sm_texture_desc td = texture_desc(d);
	sm_texture *t = sm_texture_new(self.sm, &td);
	if (!t) {
		sm_error(@"newTextureWithDescriptor");
		return nil;
	}
	SweetMTLTexture *tex = [[SweetMTLTexture alloc] initWithDevice:self texture:t descriptor:d];
	SMTRACE("  -> texture %p storage %lu", (__bridge void *)tex, (unsigned long)d.storageMode);
	return tex;
}

/* A surface's plane: the surface's pages as a no-copy buffer (the runtime shares one import among
 * buffers over the same bytes), the texture over the plane's offset and row pitch. */
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane
{
	size_t planes = IOSurfaceGetPlaneCount(s);
	uint8_t *base = IOSurfaceGetBaseAddress(s);
	uint64_t offset = planes ? (uint64_t)((uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, plane) - base) : 0;
	uint64_t bpr = planes ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : IOSurfaceGetBytesPerRow(s);
	size_t page = (size_t)getpagesize();
	uint64_t len = (IOSurfaceGetAllocSize(s) + page - 1) & ~(uint64_t)(page - 1);
	CFRetain(s);
	sm_buffer *b = sm_buffer_new_no_copy(self.sm, base, len, MTLResourceStorageModeShared, release_surface, (void *)s);
	if (!b) {
		CFRelease(s);
		sm_error(@"iosurface buffer");
		return nil;
	}
	sm_texture_desc td = texture_desc(d);
	sm_texture *t = sm_texture_new_iosurface(b, &td, offset, bpr, planes > 0);
	if (!t) {
		sm_error(@"newTextureWithDescriptor:iosurface:");
		sm_buffer_release(b);
		return nil;
	}
	SweetMTLTexture *tex = [[SweetMTLTexture alloc] initWithDevice:self texture:t descriptor:d];
	[tex setSurface:s plane:plane backing:b];
	return tex;
}

- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d
{
	sm_sampler_desc sd = {
		(uint32_t)d.minFilter, (uint32_t)d.magFilter, (uint32_t)d.mipFilter,
		(uint32_t)d.sAddressMode, (uint32_t)d.tAddressMode, (uint32_t)d.rAddressMode, d.normalizedCoordinates,
	};
	sm_sampler *s = sm_sampler_new(self.sm, &sd);
	if (!s) {
		sm_error(@"newSamplerState");
		return nil;
	}
	return [[SweetMTLSamplerState alloc] initWithDevice:self sm:s];
}

static sm_stencil_desc stencil_desc(MTLStencilDescriptor *s)
{
	return (sm_stencil_desc){
		(uint32_t)s.stencilCompareFunction, (uint32_t)s.stencilFailureOperation,
		(uint32_t)s.depthFailureOperation, (uint32_t)s.depthStencilPassOperation, s.readMask, s.writeMask,
	};
}

- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d
{
	sm_depth_stencil_desc dd = {
		(uint32_t)d.depthCompareFunction, d.depthWriteEnabled,
		stencil_desc(d.frontFaceStencil), stencil_desc(d.backFaceStencil),
	};
	sm_depth_stencil *s = sm_depth_stencil_new(self.sm, &dd);
	if (!s) {
		sm_error(@"newDepthStencilState");
		return nil;
	}
	return [[SweetMTLDepthStencilState alloc] initWithDevice:self sm:s];
}

- (sm_function *)airFunction:(id<MTLFunction>)f lib:(sm_library **)lib error:(NSError **)error
{
	*lib = [self airLibraryFor:f error:error];
	if (!*lib)
		return NULL;
	sm_function *fn = sm_library_function(*lib, f.name.UTF8String);
	if (!fn && error)
		*error = sm_error([NSString stringWithFormat:@"function %@", f.name]);
	return fn;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error
{
	NSString *key = render_key(self, d);
	id cached = [self cachedPipeline:key];
	if (cached)
		return cached;
	SMTRACE("render pipeline %s/%s color0 %lu", d.vertexFunction.name.UTF8String, d.fragmentFunction.name.UTF8String,
		(unsigned long)d.colorAttachments[0].pixelFormat);
	sm_library *libs[2] = { NULL, NULL };
	sm_function *fns[2] = { NULL, NULL };
	sm_render_pipeline *p = NULL;
	sm_vertex_descriptor vd;
	sm_render_pipeline_desc pd;
	memset(&pd, 0, sizeof pd);
	if (!d.vertexFunction || !(fns[0] = [self airFunction:d.vertexFunction lib:&libs[0] error:error]))
		goto fail;
	if (d.fragmentFunction && !(fns[1] = [self airFunction:d.fragmentFunction lib:&libs[1] error:error]))
		goto fail;
	pd.vertex = fns[0];
	pd.fragment = fns[1];
	for (int i = 0; i < 8; i++) {
		MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
		pd.color[i] = (sm_color_attachment){
			(uint32_t)c.pixelFormat, c.blendingEnabled,
			(uint32_t)c.sourceRGBBlendFactor, (uint32_t)c.destinationRGBBlendFactor, (uint32_t)c.rgbBlendOperation,
			(uint32_t)c.sourceAlphaBlendFactor, (uint32_t)c.destinationAlphaBlendFactor, (uint32_t)c.alphaBlendOperation,
			(uint32_t)c.writeMask,
		};
	}
	pd.depth_format = (uint32_t)d.depthAttachmentPixelFormat;
	pd.stencil_format = (uint32_t)d.stencilAttachmentPixelFormat;
	pd.raster_sample_count = (uint32_t)d.rasterSampleCount;
	if (d.vertexDescriptor) {
		memset(&vd, 0, sizeof vd);
		for (int i = 0; i < 31; i++) {
			MTLVertexAttributeDescriptor *a = d.vertexDescriptor.attributes[i];
			vd.attributes[i] = (sm_vertex_attribute){ (uint32_t)a.format, (uint32_t)a.offset, (uint32_t)a.bufferIndex };
			MTLVertexBufferLayoutDescriptor *l = d.vertexDescriptor.layouts[i];
			uint32_t stride = l.stride == NSUIntegerMax ? UINT32_MAX : (uint32_t)l.stride;
			vd.layouts[i] = (sm_vertex_layout){ stride, (uint32_t)l.stepFunction, (uint32_t)l.stepRate };
		}
		pd.vertex_descriptor = &vd;
	}
	p = sm_render_pipeline_new(self.sm, &pd);
	if (!p) {
		NSError *e = sm_error([NSString stringWithFormat:@"render pipeline %@/%@", d.vertexFunction.name, d.fragmentFunction.name]);
		if (error)
			*error = e;
		goto fail;
	}
	{
		SweetMTLRenderPipelineState *state = [[SweetMTLRenderPipelineState alloc] initWithDevice:self sm:p libs:libs fns:fns];
		[self cachePipeline:state key:key];
		return state;
	}
fail:
	for (int i = 0; i < 2; i++)
		sm_function_release(fns[i]);
	return nil;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
							   options:(MTLPipelineOption)options
							reflection:(MTLAutoreleasedRenderPipelineReflection *)reflection
							     error:(NSError **)error
{
	if (reflection)
		*reflection = nil;
	return [self newRenderPipelineStateWithDescriptor:d error:error];
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d
			   completionHandler:(MTLNewRenderPipelineStateCompletionHandler)handler
{
	NSError *error = nil;
	id<MTLRenderPipelineState> p = [self newRenderPipelineStateWithDescriptor:d error:&error];
	handler(p, error);
}

- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(MTLPipelineOption)options
			   completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	id<MTLRenderPipelineState> p = [self newRenderPipelineStateWithDescriptor:d error:&error];
	handler(p, nil, error);
}

@end

/* ---- render encoder ----------------------------------------------------------------------- */

@interface SweetMTLRenderCommandEncoder : NSObject <MTLRenderCommandEncoder>
@end

@implementation SweetMTLRenderCommandEncoder {
	id<MTLDevice> _device;
	sm_render_encoder *_e;
	NSMutableArray *_held;
	id<MTLBuffer> _vbuf[31], _fbuf[31];   /* for set*BufferOffset: */
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device encoder:(sm_render_encoder *)e held:(NSMutableArray *)held
{
	if ((self = [super init])) {
		_device = device;
		_e = e;
		_held = held;
	}
	return self;
}

- (id<MTLDevice>)device { return _device; }

#define CHECK(call, what) do { if (!(call)) sm_error(@what); } while (0)
#define HOLD(obj) do { if (obj) [_held addObject:obj]; } while (0)

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)s
{
	SMTRACE("set pipeline %p", (__bridge void *)s);
	HOLD(s);
	CHECK(sm_render_set_pipeline(_e, ((SweetMTLRenderPipelineState *)s).sm), "setRenderPipelineState");
}

- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	HOLD(b);
	if (i < 31)
		_vbuf[i] = b;
	CHECK(sm_render_set_vertex_buffer(_e, ((SweetMTLBuffer *)b).sm, offset, (uint32_t)i), "setVertexBuffer");
}

- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	[self setVertexBuffer:i < 31 ? _vbuf[i] : nil offset:offset atIndex:i];
}

- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained *)bs offsets:(const NSUInteger *)offs withRange:(NSRange)r
{
	for (NSUInteger k = 0; k < r.length; k++)
		[self setVertexBuffer:bs[k] offset:offs[k] atIndex:r.location + k];
}

- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)i
{
	CHECK(sm_render_set_vertex_bytes(_e, bytes, len, (uint32_t)i), "setVertexBytes");
}

- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	HOLD(t);
	CHECK(sm_render_set_vertex_texture(_e, ((SweetMTLTexture *)t).sm, (uint32_t)i), "setVertexTexture");
}

- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
	HOLD(s);
	CHECK(sm_render_set_vertex_sampler(_e, ((SweetMTLSamplerState *)s).sm, (uint32_t)i), "setVertexSamplerState");
}

- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	HOLD(b);
	if (i < 31)
		_fbuf[i] = b;
	CHECK(sm_render_set_fragment_buffer(_e, ((SweetMTLBuffer *)b).sm, offset, (uint32_t)i), "setFragmentBuffer");
}

- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)i
{
	[self setFragmentBuffer:i < 31 ? _fbuf[i] : nil offset:offset atIndex:i];
}

- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained *)bs offsets:(const NSUInteger *)offs withRange:(NSRange)r
{
	for (NSUInteger k = 0; k < r.length; k++)
		[self setFragmentBuffer:bs[k] offset:offs[k] atIndex:r.location + k];
}

- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)i
{
	CHECK(sm_render_set_fragment_bytes(_e, bytes, len, (uint32_t)i), "setFragmentBytes");
}

- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	HOLD(t);
	CHECK(sm_render_set_fragment_texture(_e, ((SweetMTLTexture *)t).sm, (uint32_t)i), "setFragmentTexture");
}

- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained *)ts withRange:(NSRange)r
{
	for (NSUInteger k = 0; k < r.length; k++)
		[self setFragmentTexture:ts[k] atIndex:r.location + k];
}

- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
	HOLD(s);
	CHECK(sm_render_set_fragment_sampler(_e, ((SweetMTLSamplerState *)s).sm, (uint32_t)i), "setFragmentSamplerState");
}

- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)ss withRange:(NSRange)r
{
	for (NSUInteger k = 0; k < r.length; k++)
		[self setFragmentSamplerState:ss[k] atIndex:r.location + k];
}

- (void)setViewport:(MTLViewport)v
{
	CHECK(sm_render_set_viewport(_e, v.originX, v.originY, v.width, v.height, v.znear, v.zfar), "setViewport");
}

- (void)setScissorRect:(MTLScissorRect)r
{
	CHECK(sm_render_set_scissor(_e, r.x, r.y, r.width, r.height), "setScissorRect");
}

- (void)setDepthStencilState:(id<MTLDepthStencilState>)s
{
	HOLD(s);
	CHECK(sm_render_set_depth_stencil(_e, ((SweetMTLDepthStencilState *)s).sm), "setDepthStencilState");
}

- (void)setStencilReferenceValue:(uint32_t)v { CHECK(sm_render_set_stencil_reference(_e, v, v), "setStencilReferenceValue"); }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b
{
	CHECK(sm_render_set_stencil_reference(_e, f, b), "setStencilReferenceValues");
}
- (void)setCullMode:(MTLCullMode)m { CHECK(sm_render_set_cull_mode(_e, (uint32_t)m), "setCullMode"); }
- (void)setFrontFacingWinding:(MTLWinding)w { CHECK(sm_render_set_front_facing_winding(_e, (uint32_t)w), "setFrontFacingWinding"); }
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)c { CHECK(sm_render_set_depth_bias(_e, b, s, c), "setDepthBias"); }
- (void)setDepthClipMode:(MTLDepthClipMode)m { CHECK(sm_render_set_depth_clip_mode(_e, (uint32_t)m), "setDepthClipMode"); }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { CHECK(sm_render_set_triangle_fill_mode(_e, (uint32_t)m), "setTriangleFillMode"); }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a
{
	CHECK(sm_render_set_blend_color(_e, r, g, b, a), "setBlendColor");
}

- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)n
	 instanceCount:(NSUInteger)inst baseInstance:(NSUInteger)base
{
	SMTRACE("draw prim %lu start %lu count %lu x%lu", (unsigned long)t, (unsigned long)start, (unsigned long)n, (unsigned long)inst);
	CHECK(sm_render_draw(_e, (uint32_t)t, (uint32_t)start, (uint32_t)n, (uint32_t)inst, (uint32_t)base), "drawPrimitives");
}

- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)n instanceCount:(NSUInteger)inst
{
	[self drawPrimitives:t vertexStart:start vertexCount:n instanceCount:inst baseInstance:0];
}

- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)start vertexCount:(NSUInteger)n
{
	[self drawPrimitives:t vertexStart:start vertexCount:n instanceCount:1 baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
		  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)inst
		   baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi
{
	SMTRACE("drawIndexed prim %lu count %lu type %lu x%lu", (unsigned long)t, (unsigned long)n, (unsigned long)it, (unsigned long)inst);
	HOLD(ib);
	CHECK(sm_render_draw_indexed(_e, (uint32_t)t, (uint32_t)n, (uint32_t)it, ((SweetMTLBuffer *)ib).sm, off,
				     (uint32_t)inst, (int32_t)bv, (uint32_t)bi), "drawIndexedPrimitives");
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
		  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)inst
{
	[self drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:inst baseVertex:0 baseInstance:0];
}

- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
		  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)off
{
	[self drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:ib indexBufferOffset:off instanceCount:1 baseVertex:0 baseInstance:0];
}

- (void)endEncoding
{
	if (_e && !sm_render_end_encoding(_e))
		sm_error(@"render endEncoding");
	_e = NULL;
}

/* no tile shading: one "tile" is irrelevant, but CA reads these */
- (NSUInteger)tileWidth { return 16; }
- (NSUInteger)tileHeight { return 16; }
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { }
- (void)useHeap:(id<MTLHeap>)h { }
- (void)textureBarrier { }
- (void)insertDebugSignpost:(NSString *)string { }
- (void)pushDebugGroup:(NSString *)string { }
- (void)popDebugGroup { }
MISSING_LOGGER
@end

/* ---- blit encoder ------------------------------------------------------------------------- */

@interface SweetMTLBlitCommandEncoder : NSObject <MTLBlitCommandEncoder>
@end

@implementation SweetMTLBlitCommandEncoder {
	id<MTLDevice> _device;
	sm_blit_encoder *_e;
	NSMutableArray *_held;
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device encoder:(sm_blit_encoder *)e held:(NSMutableArray *)held
{
	if ((self = [super init])) {
		_device = device;
		_e = e;
		_held = held;
	}
	return self;
}

- (id<MTLDevice>)device { return _device; }

- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)dst
     destinationOffset:(NSUInteger)doff size:(NSUInteger)size
{
	HOLD(src);
	HOLD(dst);
	CHECK(sm_blit_copy_buffer(_e, ((SweetMTLBuffer *)src).sm, so, ((SweetMTLBuffer *)dst).sm, doff, size), "copyFromBuffer:toBuffer:");
}

- (void)fillBuffer:(id<MTLBuffer>)b range:(NSRange)r value:(uint8_t)v
{
	HOLD(b);
	CHECK(sm_blit_fill_buffer(_e, ((SweetMTLBuffer *)b).sm, r.location, r.length, v), "fillBuffer");
}

- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr
    sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)slice destinationLevel:(NSUInteger)level destinationOrigin:(MTLOrigin)o
{
	HOLD(src);
	HOLD(dst);
	CHECK(sm_blit_buffer_to_texture(_e, ((SweetMTLBuffer *)src).sm, so, bpr, bpi, size.width, size.height,
					((SweetMTLTexture *)dst).sm, slice, level, o.x, o.y), "copyFromBuffer:toTexture:");
}

- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)slice sourceLevel:(NSUInteger)level
	   sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst
      destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi
{
	HOLD(src);
	HOLD(dst);
	CHECK(sm_blit_texture_to_buffer(_e, ((SweetMTLTexture *)src).sm, slice, level, o.x, o.y, size.width, size.height,
					((SweetMTLBuffer *)dst).sm, doff, bpr, bpi), "copyFromTexture:toBuffer:");
}

- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
	   sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg
{
	HOLD(src);
	HOLD(dst);
	CHECK(sm_blit_texture_to_texture(_e, ((SweetMTLTexture *)src).sm, ss, sl, so.x, so.y, size.width, size.height,
					 ((SweetMTLTexture *)dst).sm, ds, dl, dorg.x, dorg.y), "copyFromTexture:toTexture:");
}

- (void)copyFromTexture:(id<MTLTexture>)src toTexture:(id<MTLTexture>)dst
{
	[self copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		   sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0
	     destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
}

- (void)generateMipmapsForTexture:(id<MTLTexture>)t
{
	HOLD(t);
	CHECK(sm_blit_generate_mipmaps(_e, ((SweetMTLTexture *)t).sm), "generateMipmaps");
}

- (void)synchronizeResource:(id<MTLResource>)r { }
- (void)endEncoding
{
	if (_e && !sm_blit_end_encoding(_e))
		sm_error(@"blit endEncoding");
	_e = NULL;
}
- (void)insertDebugSignpost:(NSString *)string { }
- (void)pushDebugGroup:(NSString *)string { }
- (void)popDebugGroup { }
MISSING_LOGGER
@end

/* ---- compute encoder: textures and samplers ---------------------------------------------- */

@implementation SweetMTLComputeCommandEncoder (Render)

- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i
{
	if (t)
		[self.held addObject:t];
	if (!sm_compute_set_texture(self.sm, ((SweetMTLTexture *)t).sm, (uint32_t)i))
		sm_error(@"compute setTexture");
}

- (void)setTextures:(const id<MTLTexture> __unsafe_unretained *)ts withRange:(NSRange)r
{
	for (NSUInteger k = 0; k < r.length; k++)
		[self setTexture:ts[k] atIndex:r.location + k];
}

- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i
{
	if (s)
		[self.held addObject:s];
	if (!sm_compute_set_sampler(self.sm, ((SweetMTLSamplerState *)s).sm, (uint32_t)i))
		sm_error(@"compute setSamplerState");
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)ss withRange:(NSRange)r
{
	for (NSUInteger k = 0; k < r.length; k++)
		[self setSamplerState:ss[k] atIndex:r.location + k];
}

@end

/* ---- command buffer: render and blit encoders --------------------------------------------- */

static sm_pass_depth_stencil pass_ds(MTLRenderPassAttachmentDescriptor *a, double clear_depth, uint32_t clear_stencil)
{
	return (sm_pass_depth_stencil){
		((SweetMTLTexture *)a.texture).sm, a.level, a.slice, (uint32_t)a.loadAction, (uint32_t)a.storeAction,
		clear_depth, clear_stencil,
	};
}

@implementation SweetMTLCommandBuffer (Render)

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
	SMTRACE("render pass color0 %p load %lu store %lu", (__bridge void *)d.colorAttachments[0].texture,
		(unsigned long)d.colorAttachments[0].loadAction, (unsigned long)d.colorAttachments[0].storeAction);
	sm_render_pass_desc pass;
	memset(&pass, 0, sizeof pass);
	for (int i = 0; i < 8; i++) {
		MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
		if (!c.texture)
			continue;
		[self.held addObject:c.texture];
		MTLClearColor cc = c.clearColor;
		pass.color[i] = (sm_pass_color){
			((SweetMTLTexture *)c.texture).sm, c.level, c.slice, (uint32_t)c.loadAction, (uint32_t)c.storeAction,
			{ cc.red, cc.green, cc.blue, cc.alpha },
		};
	}
	if (d.depthAttachment.texture) {
		[self.held addObject:d.depthAttachment.texture];
		pass.depth = pass_ds(d.depthAttachment, d.depthAttachment.clearDepth, 0);
	}
	if (d.stencilAttachment.texture) {
		[self.held addObject:d.stencilAttachment.texture];
		pass.stencil = pass_ds(d.stencilAttachment, 0, d.stencilAttachment.clearStencil);
	}
	sm_render_encoder *e = sm_render_encoder_new(self.sm, &pass);
	if (!e) {
		sm_error(@"renderCommandEncoderWithDescriptor");
		return nil;
	}
	return [[SweetMTLRenderCommandEncoder alloc] initWithDevice:self.device encoder:e held:self.held];
}

- (id<MTLBlitCommandEncoder>)blitCommandEncoder
{
	sm_blit_encoder *e = sm_blit_encoder_new(self.sm);
	if (!e) {
		sm_error(@"blitCommandEncoder");
		return nil;
	}
	return [[SweetMTLBlitCommandEncoder alloc] initWithDevice:self.device encoder:e held:self.held];
}

@end
