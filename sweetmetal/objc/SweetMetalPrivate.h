/* SweetMetal.bundle internals shared by SweetMetal.m (device, buffers, queues, compute) and
 * SweetMetalRender.m (textures, samplers, render, blit). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include "sweetmetal.h"

@interface _MTLDevice : NSObject
- (instancetype)init;
@end

@interface _MTLFunction : NSObject
- (id)bitcodeData;
- (NSString *)name;
@end

#define SMLOG(...) do { fprintf(stderr, "SWEETMETAL " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* A selector a client sends that the class does not implement (methodSignatureForSelector: finds
 * it in an adopted protocol, or makes one up): logged once (class and selector) and, while the driver is being filled in, answered with 0/nil instead of an exception, so one
 * run collects every gap. SWEETMETAL_STRICT=1 restores Metal's exception. A method of a private
 * superclass that is abstract (doesNotRecognizeSelector from inside _MTLDevice) still throws. */
void sm_log_missing(id obj, SEL sel);
NSMethodSignature *sm_fake_signature(SEL sel);  /* id return, one word argument per ':' */
BOOL sm_strict(void);
BOOL sm_trace(void);
#define SMTRACE(...) do { if (sm_trace()) SMLOG("trace " __VA_ARGS__); } while (0)

#define MISSING_LOGGER \
	-(NSMethodSignature *)methodSignatureForSelector:(SEL)sel \
	{ \
		NSMethodSignature *sig = [super methodSignatureForSelector:sel]; \
		return sig || sm_strict() ? sig : sm_fake_signature(sel); \
	} \
	-(void)forwardInvocation:(NSInvocation *)inv \
	{ \
		uint8_t zero[64] = { 0 }; \
		if (sm_strict()) \
			[self doesNotRecognizeSelector:inv.selector]; \
		sm_log_missing(self, inv.selector); \
		if (inv.methodSignature.methodReturnLength <= sizeof zero) \
			[inv setReturnValue:zero]; \
	} \
	-(void)doesNotRecognizeSelector:(SEL)sel \
	{ \
		sm_log_missing(self, sel); \
		[super doesNotRecognizeSelector:sel]; \
	}

NSError *sm_error(NSString *what);

/* The pixel format sweetmetal stores for a Metal one. CoreAnimation renders the P3 display's
 * surfaces ('b3a8') in the 32-bit extended-range formats; the kernel's framebuffer copies plane 0
 * as BGRA8 and SDR content fits 8 bits, so those formats are stored as BGRA8 (same 4 bytes per
 * pixel; the alpha plane of RGB10A8_2P_XR10 is not used). Textures report the format asked. */
static inline uint32_t sm_pixel_format(MTLPixelFormat f)
{
	switch ((uint32_t)f) {
	case 550: /* RGB10A8_2P_XR10 */
	case 554: /* BGR10_XR */
		return 80; /* BGRA8Unorm */
	case 551: /* RGB10A8_2P_XR10_sRGB */
	case 555: /* BGR10_XR_sRGB */
		return 81; /* BGRA8Unorm_sRGB */
	default:
		return (uint32_t)f;
	}
}

@interface SweetMTLBuffer : NSObject <MTLBuffer>
@property (readonly) sm_buffer *sm;
- (instancetype)initWithDevice:(id<MTLDevice>)device buffer:(sm_buffer *)b options:(MTLResourceOptions)options;
@end

@interface SweetMTLComputeCommandEncoder : NSObject <MTLComputeCommandEncoder>
@property (readonly) sm_compute_encoder *sm;
@property (readonly) NSMutableArray *held;
@end

@interface SweetMTLCommandBuffer : NSObject <MTLCommandBuffer>
@property (readonly) sm_command_buffer *sm;
@property (readonly) NSMutableArray *held;  /* resources kept alive until completion */
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue cb:(sm_command_buffer *)cb;
@end

@interface SweetMTLCommandQueue : NSObject <MTLCommandQueue>
- (instancetype)initWithDevice:(id<MTLDevice>)device queue:(sm_queue *)q;
- (dispatch_queue_t)completionQueue;
@end

@interface SweetMTLDevice : _MTLDevice <MTLDevice>
@property (readonly) sm_device *sm;
- (sm_library *)airLibraryFor:(id<MTLFunction>)function error:(NSError **)error;
- (NSString *)keyForFunction:(id<MTLFunction>)function;
- (id)cachedPipeline:(NSString *)key;
- (void)cachePipeline:(id)p key:(NSString *)key;
@end
