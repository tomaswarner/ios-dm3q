/* SweetMetal.bundle: the Metal driver of the S23 Ultra iOS port, over the sweetmetal C API
 * (metal2vulkan -> Vulkan -> lavapipe now, Turnip later).
 *
 * Metal's _MTLRegisterDevices loads /System/Library/Extensions/<MetalPluginName>.bundle for each
 * IOAcceleratorES service, takes the class named by MetalPluginClassName (a subclass of
 * _MTLDevice) and calls initWithAcceleratorPort:. Apple's MTLSimDriver is the template for the
 * class layout. Library loading stays Apple's (_MTLDevice newLibraryWith*, _MTLLibrary,
 * _MTLFunction); pipelines are made here from each function's AIR (-[_MTLFunction bitcodeData]),
 * so Apple's compiler service and backend plugin are never used.
 *
 * Slice 1: device, buffers, command queue/buffer (synchronous submit), compute pipelines and
 * encoders. Any selector a client sends that is not implemented is logged with its class
 * (SWEETMETAL missing: ...) before the usual exception, to drive what comes next. */
#import "SweetMetalPrivate.h"
#import <objc/runtime.h>

BOOL sm_strict(void)
{
	static int strict = -1;
	if (strict < 0) {
		const char *v = getenv("SWEETMETAL_STRICT");
		strict = v && *v == '1';
	}
	return strict;
}

BOOL sm_trace(void)
{
	static int trace = -1;
	if (trace < 0) {
		const char *v = getenv("SWEETMETAL_TRACE");
		trace = v && *v == '1';
	}
	return trace;
}

NSMethodSignature *sm_fake_signature(SEL sel)
{
	char types[64] = "@@:";
	for (const char *c = sel_getName(sel); *c && strlen(types) < sizeof types - 1; c++)
		if (*c == ':')
			strcat(types, "Q");
	return [NSMethodSignature signatureWithObjCTypes:types];
}

void sm_log_missing(id obj, SEL sel)
{
	static NSMutableSet *seen;
	static dispatch_once_t once;
	dispatch_once(&once, ^{ seen = [NSMutableSet set]; });
	NSString *key = [NSString stringWithFormat:@"-[%s %s]", object_getClassName(obj), sel_getName(sel)];
	@synchronized(seen) {
		if ([seen containsObject:key])
			return;
		[seen addObject:key];
	}
	SMLOG("missing: %s (%s)", key.UTF8String, getprogname());
}

NSError *sm_error(NSString *what)
{
	NSString *msg = [NSString stringWithFormat:@"%@: %s", what, sm_last_error()];
	SMLOG("%s", msg.UTF8String);
	return [NSError errorWithDomain:@"SweetMetal" code:1 userInfo:@{ NSLocalizedDescriptionKey : msg }];
}

/* ---- buffers ------------------------------------------------------------------------------ */

@implementation SweetMTLBuffer {
	id<MTLDevice> _device;
	MTLResourceOptions _options;
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device buffer:(sm_buffer *)b options:(MTLResourceOptions)options
{
	if ((self = [super init])) {
		_device = device;
		_sm = b;
		_options = options;
	}
	return self;
}

- (void)dealloc
{
	sm_buffer_release(_sm);
}

- (id<MTLDevice>)device { return _device; }
- (NSUInteger)length { return (NSUInteger)sm_buffer_length(_sm); }
- (void *)contents { return sm_buffer_contents(_sm); }
- (void)didModifyRange:(NSRange)range { }
- (MTLResourceOptions)resourceOptions { return _options; }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_options & 0xf); }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (NSUInteger)allocatedSize { return self.length; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (void)makeAliasable { }
- (BOOL)isAliasable { return NO; }
MISSING_LOGGER
@end

/* ---- compute pipelines -------------------------------------------------------------------- */

@interface SweetMTLComputePipelineState : NSObject <MTLComputePipelineState>
@property (readonly) sm_compute_pipeline *sm;
@end

@implementation SweetMTLComputePipelineState {
	id<MTLDevice> _device;
	sm_library *_lib;
	sm_function *_fn;
	uint32_t _limits[3];
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device lib:(sm_library *)lib fn:(sm_function *)fn pso:(sm_compute_pipeline *)pso
{
	if ((self = [super init])) {
		_device = device;
		_lib = lib;
		_fn = fn;
		_sm = pso;
		sm_compute_pipeline_limits(pso, _limits);
	}
	return self;
}

- (void)dealloc
{
	sm_compute_pipeline_release(_sm);
	sm_function_release(_fn);   /* _lib belongs to the device's AIR cache */
}

- (id<MTLDevice>)device { return _device; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _limits[0]; }
- (NSUInteger)threadExecutionWidth { return _limits[1]; }
- (NSUInteger)staticThreadgroupMemoryLength { return _limits[2]; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
MISSING_LOGGER
@end

/* ---- command encoding --------------------------------------------------------------------- */


@implementation SweetMTLComputeCommandEncoder {
	id<MTLDevice> _device;
	sm_compute_encoder *_enc;
	SweetMTLComputePipelineState *_pso;
	NSMutableArray *_held;   /* resources kept alive until the command buffer is done */
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device encoder:(sm_compute_encoder *)enc held:(NSMutableArray *)held
{
	if ((self = [super init])) {
		_device = device;
		_enc = enc;
		_held = held;
	}
	return self;
}

- (id<MTLDevice>)device { return _device; }
- (sm_compute_encoder *)sm { return _enc; }
- (NSMutableArray *)held { return _held; }
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)bs offsets:(const NSUInteger *)offs withRange:(NSRange)r
{
	for (NSUInteger k = 0; k < r.length; k++)
		[self setBuffer:bs[k] offset:offs[k] atIndex:r.location + k];
}

- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (!sm_compute_set_buffer_offset(_enc, offset, (uint32_t)index))
		sm_error(@"setBufferOffset");
}

- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index
{
	if (!sm_compute_set_threadgroup_memory(_enc, (uint32_t)length, (uint32_t)index))
		sm_error(@"setThreadgroupMemoryLength");
}

- (void)memoryBarrierWithScope:(MTLBarrierScope)scope
{
	if (!sm_compute_memory_barrier(_enc, (uint32_t)scope))
		sm_error(@"memoryBarrierWithScope");
}

- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained *)resources count:(NSUInteger)count
{
	[self memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
}

- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { }
- (void)useResources:(const id<MTLResource> __unsafe_unretained *)r count:(NSUInteger)n usage:(MTLResourceUsage)u { }
- (void)useHeap:(id<MTLHeap>)h { }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained *)h count:(NSUInteger)n { }

- (void)setComputePipelineState:(id<MTLComputePipelineState>)state
{
	_pso = (SweetMTLComputePipelineState *)state;
	[_held addObject:state];
	if (!sm_compute_set_pipeline(_enc, _pso.sm))
		sm_error(@"setComputePipelineState");
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
	if (buffer)
		[_held addObject:buffer];
	if (!sm_compute_set_buffer(_enc, ((SweetMTLBuffer *)buffer).sm, offset, (uint32_t)index))
		sm_error(@"setBuffer");
}

- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{
	if (!sm_compute_set_bytes(_enc, bytes, length, (uint32_t)index))
		sm_error(@"setBytes");
}

- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)tg
{
	if (!sm_compute_dispatch_threads(_enc, threads.width, threads.height, threads.depth, tg.width, tg.height, tg.depth))
		sm_error(@"dispatchThreads");
}

- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)tg
{
	if (!sm_compute_dispatch_threadgroups(_enc, groups.width, groups.height, groups.depth, tg.width, tg.height, tg.depth))
		sm_error(@"dispatchThreadgroups");
}

- (void)endEncoding
{
	if (_enc && !sm_compute_end_encoding(_enc))
		sm_error(@"endEncoding");
	_enc = NULL;
}

- (void)insertDebugSignpost:(NSString *)string { }
- (void)pushDebugGroup:(NSString *)string { }
- (void)popDebugGroup { }
- (uint64_t)globalTraceObjectID { return (uint64_t)(uintptr_t)self; }
MISSING_LOGGER
@end

@implementation SweetMTLCommandBuffer {
	id<MTLCommandQueue> _queue;
	sm_command_buffer *_cb;
	NSMutableArray *_held, *_scheduled, *_completed;
	NSMutableDictionary *_userDictionary;
	MTLCommandBufferStatus _status;
	NSError *_error;
	dispatch_semaphore_t _done;
}
@synthesize label;

- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue cb:(sm_command_buffer *)cb
{
	if ((self = [super init])) {
		_queue = queue;
		_cb = cb;
		_held = [NSMutableArray array];
		_scheduled = [NSMutableArray array];
		_completed = [NSMutableArray array];
		_status = MTLCommandBufferStatusNotEnqueued;
		_done = dispatch_semaphore_create(0);
	}
	return self;
}

- (void)dealloc
{
	sm_command_buffer_release(_cb);
}

- (sm_command_buffer *)sm { return _cb; }
- (NSMutableArray *)held { return _held; }
- (id<MTLDevice>)device { return _queue.device; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferErrorOption)errorOptions { return MTLCommandBufferErrorOptionNone; }
- (MTLCommandBufferStatus)status { return _status; }
- (NSError *)error { return _error; }
- (CFTimeInterval)kernelStartTime { return 0; }
- (CFTimeInterval)kernelEndTime { return 0; }
- (CFTimeInterval)GPUStartTime { return 0; }
- (CFTimeInterval)GPUEndTime { return 0; }

- (id<MTLComputeCommandEncoder>)computeCommandEncoder
{
	sm_compute_encoder *e = sm_compute_encoder_new(_cb);
	if (!e) {
		sm_error(@"computeCommandEncoder");
		return nil;
	}
	return [[SweetMTLComputeCommandEncoder alloc] initWithDevice:self.device encoder:e held:_held];
}

- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type
{
	return [self computeCommandEncoder];
}

- (void)addScheduledHandler:(MTLCommandBufferHandler)block { [_scheduled addObject:[block copy]]; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)block { [_completed addObject:[block copy]]; }
- (void)enqueue { _status = MTLCommandBufferStatusEnqueued; }

- (void)commit
{
	SMTRACE("commit %p", self);
	_status = MTLCommandBufferStatusCommitted;
	if (!sm_command_buffer_commit(_cb)) {
		_error = sm_error(@"commit");
		_status = MTLCommandBufferStatusError;
		dispatch_semaphore_signal(_done);
		return;
	}
	_status = MTLCommandBufferStatusScheduled;
	for (MTLCommandBufferHandler h in _scheduled)
		h(self);
	/* wait off the client's queues, then run the handlers on the queue's completion queue */
	dispatch_queue_t handlers = [(SweetMTLCommandQueue *)_queue completionQueue];
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		if (!sm_command_buffer_wait(self->_cb)) {
			self->_error = sm_error(@"command buffer");
			self->_status = MTLCommandBufferStatusError;
		} else {
			self->_status = MTLCommandBufferStatusCompleted;
		}
		SMTRACE("completed %p status %ld", (__bridge void *)self, (long)self->_status);
		dispatch_async(handlers, ^{
			for (MTLCommandBufferHandler h in self->_completed)
				h(self);
			[self->_held removeAllObjects];
			dispatch_semaphore_signal(self->_done);
		});
	});
}

- (void)waitUntilScheduled { }
/* SPI CoreAnimation commits with: the work is submitted by the time commit returns here */
- (void)commitAndWaitUntilSubmitted { [self commit]; }
- (void)setProtectionOptions:(uint64_t)o { }
- (uint64_t)protectionOptions { return 0; }
/* SPI MetalPerformanceShaders keeps per-command-buffer state in */
- (NSMutableDictionary *)userDictionary
{
	@synchronized(self) {
		if (!_userDictionary)
			_userDictionary = [NSMutableDictionary dictionary];
		return _userDictionary;
	}
}
- (uint64_t)globalTraceObjectID { return (uint64_t)(uintptr_t)self; }

- (void)waitUntilCompleted
{
	if (_status == MTLCommandBufferStatusNotEnqueued || _status == MTLCommandBufferStatusEnqueued)
		return;
	dispatch_semaphore_wait(_done, DISPATCH_TIME_FOREVER);
	dispatch_semaphore_signal(_done);
}

- (void)pushDebugGroup:(NSString *)string { }
- (void)popDebugGroup { }
MISSING_LOGGER
@end


@implementation SweetMTLCommandQueue {
	id<MTLDevice> _device;
	sm_queue *_q;
	dispatch_queue_t _completion;
}
@synthesize label;

- (instancetype)initWithDevice:(id<MTLDevice>)device queue:(sm_queue *)q
{
	if ((self = [super init])) {
		_device = device;
		_q = q;
	}
	return self;
}

- (void)dealloc
{
	sm_queue_release(_q);
}

- (id<MTLDevice>)device { return _device; }

- (id<MTLCommandBuffer>)commandBuffer
{
	sm_command_buffer *cb = sm_command_buffer_new(_q);
	if (!cb) {
		sm_error(@"commandBuffer");
		return nil;
	}
	return [[SweetMTLCommandBuffer alloc] initWithQueue:self cb:cb];
}

/* SPI: where completion handlers run (CoreAnimation sets its own) */
- (void)setCompletionQueue:(dispatch_queue_t)q { _completion = q; }
- (dispatch_queue_t)completionQueue
{
	return _completion ?: dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
}
- (void)setSubmissionQueue:(dispatch_queue_t)q { }
- (void)setGPUPriority:(NSUInteger)p { }
- (void)setBackgroundGPUPriority:(NSUInteger)p { }

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d { return [self commandBuffer]; }
- (void)insertDebugCaptureBoundary { }
MISSING_LOGGER
@end

/* ---- device ------------------------------------------------------------------------------- */

@implementation SweetMTLDevice {
	sm_device *_dev;
	NSString *_name;
	NSMutableDictionary<NSData *, NSValue *> *_air;   /* bitcode -> sm_library (kept for the device's life) */
	NSMutableDictionary<NSString *, id> *_pipelines; /* descriptor key -> pipeline state */
}

- (sm_device *)sm { return _dev; }

/* Metal's private protocols (MTLAddDevice asserts MTLDeviceSPI); added at run time from the
 * protocols Metal registers, as the SDK has no headers for them. */
+ (void)load
{
	static const char *pairs[][2] = {
		{ "SweetMTLDevice", "MTLDeviceSPI" }, { "SweetMTLBuffer", "MTLBufferSPI" },
		{ "SweetMTLTexture", "MTLTextureSPI" }, { "SweetMTLCommandQueue", "MTLCommandQueueSPI" },
		{ "SweetMTLCommandBuffer", "MTLCommandBufferSPI" },
		{ "SweetMTLComputePipelineState", "MTLComputePipelineStateSPI" },
		{ "SweetMTLRenderPipelineState", "MTLRenderPipelineStateSPI" },
		{ "SweetMTLSamplerState", "MTLSamplerStateSPI" }, { "SweetMTLDepthStencilState", "MTLDepthStencilStateSPI" },
		{ "SweetMTLRenderCommandEncoder", "MTLRenderCommandEncoderSPI" },
		{ "SweetMTLComputeCommandEncoder", "MTLComputeCommandEncoderSPI" },
		{ "SweetMTLBlitCommandEncoder", "MTLBlitCommandEncoderSPI" },
	};
	for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
		Class c = objc_getClass(pairs[i][0]);
		Protocol *p = objc_getProtocol(pairs[i][1]);
		if (c && p)
			class_addProtocol(c, p);
		else
			SMLOG("no protocol %s for %s", pairs[i][1], pairs[i][0]);
	}
}

- (instancetype)initWithAcceleratorPort:(unsigned int)port
{
	if (!(self = [super init]))
		return nil;
	_dev = sm_device_create();
	if (!_dev) {
		sm_error(@"device");
		return nil;
	}
	char name[128];
	sm_device_name(_dev, name, sizeof name);
	/* MetalPerformanceShaders picks its kernels by device name ("Apple A11 GPU", ...); an unknown
	 * name makes it look up functions that do not exist. SWEETMETAL_NAME overrides (tests). */
	const char *override = getenv("SWEETMETAL_NAME");
	_name = override && *override ? @(override) : [NSString stringWithFormat:@"SweetMetal (%s)", name];
	_air = [NSMutableDictionary dictionary];
	_pipelines = [NSMutableDictionary dictionary];
	SMLOG("device up: %s, accelerator port %u", _name.UTF8String, port);
	return self;
}

- (NSString *)name { return _name; }
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count == 1; }

/* What CoreAnimation's new_metal_context asks (_MTLDevice would answer the A11's: tile shaders,
 * memoryless targets, raster order groups...). Only what the Vulkan backend really does is YES. */
- (BOOL)supportsTileShaders { return NO; }
- (BOOL)supportsMemorylessRenderTargets { return NO; }
- (BOOL)supportsRasterOrderGroups { return NO; }
- (BOOL)areRasterOrderGroupsSupported { return NO; }
- (BOOL)supportsSIMDGroup { return NO; }
- (BOOL)supportsNativeHardwareFP16 { return NO; }
- (BOOL)supportsYCBCRFormats12 { return NO; }
- (BOOL)supportsYCBCRPackedFormats12 { return NO; }
- (BOOL)supportsExtendedXR10Formats { return NO; }
- (BOOL)supportsImageBlocks { return NO; }
- (BOOL)supportsImageBlockSampleCoverageControl { return NO; }
- (BOOL)supportsQuadGroup { return NO; }
- (NSString *)vendorName { return @"SweetMetal"; }
- (NSString *)productName { return _name; }
- (NSUInteger)maxColorAttachments { return 8; }
- (NSUInteger)maxFragmentTextures { return 31; }
- (NSUInteger)maxVertexTextures { return 31; }
- (NSUInteger)maxFragmentSamplers { return 16; }
- (NSUInteger)maxVertexSamplers { return 16; }
- (NSUInteger)maxTextureWidth2D { return 16384; }
- (NSUInteger)maxTextureHeight2D { return 16384; }
- (NSUInteger)minConstantBufferAlignmentBytes { return 256; }
- (NSUInteger)iosurfaceTextureAlignmentBytes { return 16; }
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes { return 16; }
- (NSUInteger)deviceLinearTextureAlignmentBytes { return 16; }
- (NSUInteger)deviceLinearReadOnlyTextureAlignmentBytes { return 16; }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)format { return 16; }
- (NSUInteger)maxThreadgroupMemoryLength { return 32768; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTierNone; }
- (MTLArgumentBuffersTier)argumentBuffersSupport { return MTLArgumentBuffersTier1; }
/* AGXA11FamilyDevice answers 5 */
- (NSUInteger)featureProfile { return 5; }

/* AGX's precompiled pipeline caches (CoreAnimation asks for its own); none here. */
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	if (error)
		*error = [NSError errorWithDomain:@"SweetMetal" code:3
					 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}

- (uint64_t)registryID { return 0x53574554; }
- (MTLSize)maxThreadsPerThreadgroup { return MTLSizeMake(1024, 1024, 1024); }
- (BOOL)isLowPower { return NO; }
- (BOOL)isHeadless { return NO; }
- (BOOL)isRemovable { return NO; }
- (BOOL)hasUnifiedMemory { return YES; }
- (uint64_t)recommendedMaxWorkingSetSize { return 2ull << 30; }
- (NSUInteger)maxBufferLength { return 1ull << 30; }

/* iPhone X (A11) answers the Apple4 family. */
- (BOOL)supportsFamily:(MTLGPUFamily)family
{
	return (family >= MTLGPUFamilyApple1 && family <= MTLGPUFamilyApple4) || family == MTLGPUFamilyCommon1 ||
	       family == MTLGPUFamilyCommon2;
}

- (id<MTLCommandQueue>)newCommandQueue
{
	sm_queue *q = sm_queue_new(_dev);
	if (!q) {
		sm_error(@"newCommandQueue");
		return nil;
	}
	return [[SweetMTLCommandQueue alloc] initWithDevice:self queue:q];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n { return [self newCommandQueue]; }

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	sm_buffer *b = sm_buffer_new(_dev, length);
	if (!b) {
		sm_error(@"newBufferWithLength");
		return nil;
	}
	return [[SweetMTLBuffer alloc] initWithDevice:self buffer:b options:options];
}

- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
{
	SweetMTLBuffer *b = [self newBufferWithLength:length options:options];
	memcpy(b.contents, bytes, length);
	return b;
}

/* The AIR bitcode of an _MTLFunction (Apple's library loader made it), or nil. */
static NSData *bitcode(id<MTLFunction> function)
{
	id data = [(_MTLFunction *)function respondsToSelector:@selector(bitcodeData)] ? [(_MTLFunction *)function bitcodeData] : nil;
	return [data isKindOfClass:[NSData class]] ? data : nil;
}

/* A short stable name for a function's code in pipeline cache keys: its name, length and hash. */
- (NSString *)keyForFunction:(id<MTLFunction>)function
{
	NSData *bc = bitcode(function);
	return function ? [NSString stringWithFormat:@"%@#%lu#%lx", function.name, (unsigned long)bc.length, (unsigned long)bc.hash]
			: @"-";
}

/* The AIR module of an _MTLFunction as an sm library, translated once per distinct bitcode; the
 * device keeps it (callers do not release it). */
- (sm_library *)airLibraryFor:(id<MTLFunction>)function error:(NSError **)error
{
	NSData *bc = bitcode(function);
	if (!bc) {
		if (error)
			*error = [NSError errorWithDomain:@"SweetMetal" code:2 userInfo:@{ NSLocalizedDescriptionKey :
				[NSString stringWithFormat:@"no AIR bitcode for %@", function.name] }];
		return NULL;
	}
	@synchronized(_air) {
		NSValue *v = _air[bc];
		if (v)
			return v.pointerValue;
	}
	sm_library *lib = sm_library_new_with_air(_dev, bc.bytes, bc.length);
	if (!lib) {
		NSError *e = sm_error([NSString stringWithFormat:@"AIR of %@", function.name]);
		if (error)
			*error = e;
		return NULL;
	}
	SMTRACE("translated %s (%lu bytes of AIR)", function.name.UTF8String, (unsigned long)bc.length);
	@synchronized(_air) {
		NSValue *v = _air[bc];
		if (v) {
			sm_library_release(lib);
			return v.pointerValue;
		}
		_air[[bc copy]] = [NSValue valueWithPointer:lib];
	}
	return lib;
}

- (id)cachedPipeline:(NSString *)key
{
	@synchronized(_pipelines) {
		return _pipelines[key];
	}
}

- (void)cachePipeline:(id)p key:(NSString *)key
{
	@synchronized(_pipelines) {
		_pipelines[key] = p;
	}
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error
{
	NSString *key = [@"compute|" stringByAppendingString:[self keyForFunction:function]];
	id cached = [self cachedPipeline:key];
	if (cached)
		return cached;
	SMTRACE("compute pipeline %s", function.name.UTF8String);
	sm_library *lib = [self airLibraryFor:function error:error];
	sm_function *fn = lib ? sm_library_function(lib, function.name.UTF8String) : NULL;
	sm_compute_pipeline *pso = fn ? sm_compute_pipeline_new(_dev, fn) : NULL;
	if (!pso) {
		if (lib && error)
			*error = sm_error([NSString stringWithFormat:@"compute pipeline %@", function.name]);
		sm_function_release(fn);
		return nil;
	}
	id p = [[SweetMTLComputePipelineState alloc] initWithDevice:self lib:lib fn:fn pso:pso];
	[self cachePipeline:p key:key];
	return p;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
							    options:(MTLPipelineOption)options
							 reflection:(MTLAutoreleasedComputePipelineReflection *)reflection
							      error:(NSError **)error
{
	if (reflection)
		*reflection = nil;
	return [self newComputePipelineStateWithFunction:function error:error];
}

/* From a descriptor (MetalPerformanceShaders' path): the compute function; linked functions,
 * driver options and the threadgroup-size hint do not change our translation. */
- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d
							      options:(MTLPipelineOption)options
							   reflection:(MTLAutoreleasedComputePipelineReflection *)reflection
								error:(NSError **)error
{
	if (reflection)
		*reflection = nil;
	if (!d.computeFunction) {
		if (error)
			*error = [NSError errorWithDomain:@"SweetMetal" code:4 userInfo:@{ NSLocalizedDescriptionKey : @"no compute function" }];
		return nil;
	}
	return [self newComputePipelineStateWithFunction:d.computeFunction error:error];
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error
{
	return [self newComputePipelineStateWithDescriptor:d options:0 reflection:NULL error:error];
}

- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(MTLPipelineOption)options
			    completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	id<MTLComputePipelineState> p = [self newComputePipelineStateWithDescriptor:d options:options reflection:NULL error:&error];
	handler(p, nil, error);
}

- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function options:(MTLPipelineOption)options
			  completionHandler:(MTLNewComputePipelineStateWithReflectionCompletionHandler)handler
{
	NSError *error = nil;
	id<MTLComputePipelineState> p = [self newComputePipelineStateWithFunction:function error:&error];
	handler(p, nil, error);
}

- (void)newComputePipelineStateWithFunction:(id<MTLFunction>)function
			  completionHandler:(MTLNewComputePipelineStateCompletionHandler)handler
{
	NSError *error = nil;
	id<MTLComputePipelineState> p = [self newComputePipelineStateWithFunction:function error:&error];
	handler(p, error);
}

- (void)dealloc
{
	sm_device_release(_dev);
}

MISSING_LOGGER
@end
