/* lib_probe METALLIB FUNCTION: loads METALLIB on the system device, reports its function count,
 * whether FUNCTION is listed, newFunctionWithName:, its bitcode size and a compute pipeline. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

@interface NSObject (Bitcode)
- (id)bitcodeData;
@end

int main(int argc, char **argv)
{
	@autoreleasepool {
		const char *path = argc > 1 ? argv[1] : "/System/Library/Frameworks/MetalPerformanceShaders.framework/Frameworks/MPSImage.framework/default.metallib";
		NSString *name = @(argc > 2 ? argv[2] : "D2F6H");
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		NSError *error = nil;
		id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
		printf("device %s, library %s, %lu functions, lists %s: %d (%s)\n", dev.name.UTF8String, lib ? "ok" : "nil",
		       (unsigned long)lib.functionNames.count, name.UTF8String, [lib.functionNames containsObject:name],
		       error.localizedDescription.UTF8String);
		id<MTLFunction> f = [lib newFunctionWithName:name];
		NSData *bc = [(id)f respondsToSelector:@selector(bitcodeData)] ? [(id)f bitcodeData] : nil;
		printf("function %s: %s type %lu, bitcode %lu bytes, constants %lu\n", name.UTF8String, f ? "ok" : "nil",
		       (unsigned long)f.functionType, (unsigned long)bc.length, (unsigned long)f.functionConstantsDictionary.count);
		for (NSString *k in f.functionConstantsDictionary)
			printf("  constant %s\n", k.UTF8String);
		if (f.functionConstantsDictionary.count) {
			MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
			bool yes = true;
			for (NSString *k in f.functionConstantsDictionary)
				[cv setConstantValue:&yes type:MTLDataTypeBool withName:k];
			NSError *e1 = nil;
			id<MTLFunction> s1 = [lib newFunctionWithName:name constantValues:cv error:&e1];
			printf("sync constantValues: %s %s\n", s1 ? "ok" : "nil", e1.localizedDescription.UTF8String);
			dispatch_semaphore_t sem = dispatch_semaphore_create(0);
			[lib newFunctionWithName:name constantValues:cv completionHandler:^(id<MTLFunction> s2, NSError *e2) {
				printf("async constantValues: %s %s\n", s2 ? "ok" : "nil", e2.localizedDescription.UTF8String);
				dispatch_semaphore_signal(sem);
			}];
			dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
			MTLFunctionDescriptor *fd = [MTLFunctionDescriptor functionDescriptor];
			fd.name = name;
			fd.constantValues = cv;
			NSError *e3 = nil;
			id<MTLFunction> s3 = [lib newFunctionWithDescriptor:fd error:&e3];
			printf("descriptor: %s %s\n", s3 ? "ok" : "nil", e3.localizedDescription.UTF8String);
			if (s1)
				f = s1;
		}
		if (f && f.functionType == MTLFunctionTypeKernel) {
			id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:f error:&error];
			printf("pipeline %s %s\n", p ? "ok" : "nil", error.localizedDescription.UTF8String);
		}
	}
	return 0;
}
