/* SweetMetal.bundle internals shared by SweetMetal.m (device, buffers, queues, compute) and
 * SweetMetalRender.m (textures, samplers, render, blit). */
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

NSError *sm_error(NSString *what);

@interface SweetMTLBuffer : NSObject <MTLBuffer>
@property (readonly) sm_buffer *sm;
- (instancetype)initWithDevice:(id<MTLDevice>)device buffer:(sm_buffer *)b options:(MTLResourceOptions)options;
@end

@interface SweetMTLCommandBuffer : NSObject <MTLCommandBuffer>
@property (readonly) sm_command_buffer *sm;
@property (readonly) NSMutableArray *held;  /* resources kept alive until completion */
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue cb:(sm_command_buffer *)cb;
@end

@interface SweetMTLDevice : _MTLDevice <MTLDevice>
@property (readonly) sm_device *sm;
- (sm_library *)airLibraryFor:(id<MTLFunction>)function error:(NSError **)error;
@end
