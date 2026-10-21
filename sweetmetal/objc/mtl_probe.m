/* mtl_probe: loads SweetMetal.dylib (the bundle's code) and drives SweetMTLDevice through the
 * public Metal API, without the kernel's IOAcceleratorES nub:
 *   1. [[SweetMTLDevice alloc] initWithAcceleratorPort:0]
 *   2. Apple's library loader on a system metallib (default: QuartzCore's): function names,
 *      then a compute pipeline from each kernel's AIR (bitcodeData -> metal2vulkan)
 *   3. with a test metallib holding scale_and_offset: a full compute run through Metal
 * Usage: mtl_probe [DYLIB [SYSTEM.metallib [TEST.metallib]]] */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

@interface NSObject (SweetMetalProbe)
- (id)initWithAcceleratorPort:(unsigned int)port;
@end

static int failures;

static void check(BOOL ok, const char *what, NSError *error)
{
	printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, error ? ": " : "",
	       error ? error.localizedDescription.UTF8String : "");
	failures += !ok;
}

static id<MTLLibrary> load(id<MTLDevice> dev, const char *path)
{
	NSError *error = nil;
	id<MTLLibrary> lib = nil;
	@try {
		lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
	} @catch (NSException *e) {
		printf("exception: %s\n", e.reason.UTF8String);
	}
	char what[512];
	snprintf(what, sizeof what, "library %s (%lu functions)", path, (unsigned long)lib.functionNames.count);
	check(lib != nil, what, error);
	return lib;
}

static void system_kernels(id<MTLDevice> dev, const char *path)
{
	id<MTLLibrary> lib = load(dev, path);
	int vtx = 0, frag = 0, kern = 0, made = 0;
	for (NSString *name in lib.functionNames) {
		id<MTLFunction> f = nil;
		@try {
			f = [lib newFunctionWithName:name];
		} @catch (NSException *e) {
			printf("  %s: exception %s\n", name.UTF8String, e.reason.UTF8String);
			failures++;
			continue;
		}
		if (!f) {
			printf("  %s: newFunctionWithName nil\n", name.UTF8String);
			continue;
		}
		switch (f.functionType) {
		case MTLFunctionTypeVertex: vtx++; break;
		case MTLFunctionTypeFragment: frag++; break;
		case MTLFunctionTypeKernel: {
			kern++;
			NSError *error = nil;
			id<MTLComputePipelineState> p = nil;
			@try {
				p = [dev newComputePipelineStateWithFunction:f error:&error];
			} @catch (NSException *e) {
				printf("  %s: exception %s\n", name.UTF8String, e.reason.UTF8String);
			}
			made += p != nil;
			printf("  kernel %s: %s%s\n", name.UTF8String, p ? "pipeline ok" : "FAIL ",
			       p ? "" : error.localizedDescription.UTF8String);
			break;
		}
		default: break;
		}
	}
	printf("system library: %d vertex, %d fragment, %d kernel (%d compute pipelines)\n", vtx, frag, kern, made);
}

static void scale_and_offset(id<MTLDevice> dev, const char *path)
{
	id<MTLLibrary> lib = load(dev, path);
	id<MTLFunction> f = [lib newFunctionWithName:@"scale_and_offset"];
	NSError *error = nil;
	id<MTLComputePipelineState> p = f ? [dev newComputePipelineStateWithFunction:f error:&error] : nil;
	check(p != nil, "scale_and_offset pipeline", error);
	if (!p)
		return;
	enum { N = 1024 };
	id<MTLBuffer> in = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
	id<MTLBuffer> out = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
	float *pin = in.contents, scale = 2.5f;
	for (int i = 0; i < N; i++)
		pin[i] = (float)i;
	id<MTLCommandQueue> q = [dev newCommandQueue];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:p];
	[e setBuffer:in offset:0 atIndex:0];
	[e setBuffer:out offset:0 atIndex:1];
	[e setBytes:&scale length:4 atIndex:2];
	[e dispatchThreadgroups:MTLSizeMake(N / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	[e endEncoding];
	__block int handler = 0;
	[cb addCompletedHandler:^(id<MTLCommandBuffer> b) { handler = 1; }];
	[cb commit];
	[cb waitUntilCompleted];
	const float *po = out.contents;
	int good = 0;
	for (int i = 0; i < N; i++)
		good += po[i] == pin[i] * scale + (float)i;
	printf("metal compute: %d/%d correct, status %ld, completed handler %s\n", good, N, (long)cb.status,
	       handler ? "ran" : "DID NOT RUN");
	failures += good != N || !handler;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		const char *dylib = argc > 1 ? argv[1] : "/usr/local/sweettest/SweetMetal.dylib";
		const char *syslib = argc > 2 ? argv[2] : "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
		const char *testlib = argc > 3 ? argv[3] : "/usr/local/sweettest/sweetmetal_test.metallib";
		if (!dlopen(dylib, RTLD_NOW)) {
			printf("FAIL dlopen: %s\n", dlerror());
			return 1;
		}
		Class cls = NSClassFromString(@"SweetMTLDevice");
		id<MTLDevice> dev = [(id)[cls alloc] initWithAcceleratorPort:0];
		check(dev != nil, "SweetMTLDevice initWithAcceleratorPort:0", nil);
		if (!dev)
			return 1;
		printf("device: %s, Apple4 %d, MTLCreateSystemDefaultDevice %p\n", dev.name.UTF8String,
		       [dev supportsFamily:MTLGPUFamilyApple4], (__bridge void *)MTLCreateSystemDefaultDevice());
		system_kernels(dev, syslib);
		if (access(testlib, R_OK) == 0)
			scale_and_offset(dev, testlib);
		else
			printf("skip  %s (no test metallib)\n", testlib);
		printf(failures ? "MTL_PROBE FAIL (%d)\n" : "MTL_PROBE OK\n", failures);
		return failures != 0;
	}
}
