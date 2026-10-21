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
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include "sweetmetal.h"

@interface _MTLDevice : NSObject
- (instancetype)init;
@end

@interface _MTLFunction : NSObject
- (id)bitcodeData;
- (NSString *)name;
@end

#define SMLOG(...) do { fprintf(stderr, "SWEETMETAL " __VA_ARGS__); fputc('\n', stderr); } while (0)

#define MISSING_LOGGER \
	-(void)doesNotRecognizeSelector:(SEL)sel \
	{ \
		SMLOG("missing: -[%s %s]", object_getClassName(self), sel_getName(sel)); \
		[super doesNotRecognizeSelector:sel]; \
	}

static NSError *sm_error(NSString *what)
{
	NSString *msg = [NSString stringWithFormat:@"%@: %s", what, sm_last_error()];
	SMLOG("%s", msg.UTF8String);
	return [NSError errorWithDomain:@"SweetMetal" code:1 userInfo:@{ NSLocalizedDescriptionKey : msg }];
}

/* ---- buffers ------------------------------------------------------------------------------ */

@interface SweetMTLBuffer : NSObject <MTLBuffer>
@property (readonly) sm_buffer *sm;
@end

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
	sm_function_release(_fn);
	sm_library_release(_lib);
}

- (id<MTLDevice>)device { return _device; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return _limits[0]; }
- (NSUInteger)threadExecutionWidth { return _limits[1]; }
- (NSUInteger)staticThreadgroupMemoryLength { return _limits[2]; }
- (BOOL)supportIndirectCommandBuffers { return NO; }
MISSING_LOGGER
@end

/* ---- command encoding --------------------------------------------------------------------- */

@interface SweetMTLComputeCommandEncoder : NSObject <MTLComputeCommandEncoder>
@end

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
- (MTLDispatchType)dispatchType { return MTLDispatchTypeSerial; }

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
MISSING_LOGGER
@end

@interface SweetMTLCommandBuffer : NSObject <MTLCommandBuffer>
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue cb:(sm_command_buffer *)cb;
@end

@implementation SweetMTLCommandBuffer {
	id<MTLCommandQueue> _queue;
	sm_command_buffer *_cb;
	NSMutableArray *_held, *_scheduled, *_completed;
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
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		if (!sm_command_buffer_wait(self->_cb)) {
			self->_error = sm_error(@"command buffer");
			self->_status = MTLCommandBufferStatusError;
		} else {
			self->_status = MTLCommandBufferStatusCompleted;
		}
		for (MTLCommandBufferHandler h in self->_completed)
			h(self);
		[self->_held removeAllObjects];
		dispatch_semaphore_signal(self->_done);
	});
}

- (void)waitUntilScheduled { }

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

@interface SweetMTLCommandQueue : NSObject <MTLCommandQueue>
- (instancetype)initWithDevice:(id<MTLDevice>)device queue:(sm_queue *)q;
@end

@implementation SweetMTLCommandQueue {
	id<MTLDevice> _device;
	sm_queue *_q;
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

- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d { return [self commandBuffer]; }
- (void)insertDebugCaptureBoundary { }
MISSING_LOGGER
@end

/* ---- device ------------------------------------------------------------------------------- */

@interface SweetMTLDevice : _MTLDevice <MTLDevice>
@end

@implementation SweetMTLDevice {
	sm_device *_dev;
	NSString *_name;
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
	_name = [NSString stringWithFormat:@"SweetMetal (%s)", name];
	SMLOG("device up: %s, accelerator port %u", _name.UTF8String, port);
	return self;
}

- (NSString *)name { return _name; }
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

/* The AIR module of an _MTLFunction (Apple's library loader made it), as one sm library. */
- (sm_library *)airLibraryFor:(id<MTLFunction>)function error:(NSError **)error
{
	id data = [(_MTLFunction *)function respondsToSelector:@selector(bitcodeData)] ? [(_MTLFunction *)function bitcodeData] : nil;
	if (![data isKindOfClass:[NSData class]]) {
		if (error)
			*error = [NSError errorWithDomain:@"SweetMetal" code:2 userInfo:@{ NSLocalizedDescriptionKey :
				[NSString stringWithFormat:@"no AIR bitcode for %@ (%@)", function.name, [data class]] }];
		return NULL;
	}
	NSData *bc = data;
	sm_library *lib = sm_library_new_with_air(_dev, bc.bytes, bc.length);
	if (!lib && error)
		*error = sm_error([NSString stringWithFormat:@"AIR of %@", function.name]);
	return lib;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function error:(NSError **)error
{
	sm_library *lib = [self airLibraryFor:function error:error];
	sm_function *fn = lib ? sm_library_function(lib, function.name.UTF8String) : NULL;
	sm_compute_pipeline *pso = fn ? sm_compute_pipeline_new(_dev, fn) : NULL;
	if (!pso) {
		if (lib && error)
			*error = sm_error([NSString stringWithFormat:@"compute pipeline %@", function.name]);
		sm_function_release(fn);
		sm_library_release(lib);
		return nil;
	}
	return [[SweetMTLComputePipelineState alloc] initWithDevice:self lib:lib fn:fn pso:pso];
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
