/* mtl_probe: loads SweetMetal.dylib (the bundle's code) and drives SweetMTLDevice through the
 * public Metal API, without the kernel's IOAcceleratorES nub:
 *   1. [[SweetMTLDevice alloc] initWithAcceleratorPort:0]
 *   2. Apple's library loader on a system metallib (default: QuartzCore's): function names,
 *      then a compute pipeline from each kernel's AIR (bitcodeData -> metal2vulkan)
 *   3. with the test metallib (tests/sweetmetal_test.metal): compute (scale_and_offset), render
 *      into an IOSurface (CoreAnimation's path), vertex descriptor + depth/stencil + culling,
 *      texture sampling with a blit readback
 * Usage: mtl_probe [DYLIB [SYSTEM.metallib [TEST.metallib]]] */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>

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

#define SIDE 8

static id<MTLRenderPipelineState> pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *v, NSString *f,
					   MTLVertexDescriptor *vd, MTLPixelFormat depth)
{
	MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
	d.vertexFunction = [lib newFunctionWithName:v];
	d.fragmentFunction = [lib newFunctionWithName:f];
	d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	d.vertexDescriptor = vd;
	d.depthAttachmentPixelFormat = depth;
	d.stencilAttachmentPixelFormat = depth;
	NSError *error = nil;
	id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&error];
	char what[128];
	snprintf(what, sizeof what, "render pipeline %s/%s", v.UTF8String, f.UTF8String);
	check(p != nil, what, error);
	return p;
}

static MTLRenderPassDescriptor *pass_into(id<MTLTexture> t, MTLClearColor clear)
{
	MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
	p.colorAttachments[0].texture = t;
	p.colorAttachments[0].loadAction = MTLLoadActionClear;
	p.colorAttachments[0].storeAction = MTLStoreActionStore;
	p.colorAttachments[0].clearColor = clear;
	return p;
}

/* Compares an 8x8 RGBA8 image (rows bpr apart) with want(x, y); prints the grid as chars. */
static int compare(const char *what, const uint8_t *px, size_t bpr, const uint8_t *(^want)(int, int), int (^mark)(int, int))
{
	int bad = 0;
	for (int y = 0; y < SIDE; y++) {
		char row[SIDE + 1];
		for (int x = 0; x < SIDE; x++) {
			const uint8_t *p = px + y * bpr + x * 4, *w = want(x, y);
			int ok = !memcmp(p, w, 4);
			row[x] = ok ? (char)mark(x, y) : 'X';
			if (!ok && bad++ < 3)
				printf("  (%d,%d) = %d %d %d %d, want %d %d %d %d\n", x, y, p[0], p[1], p[2], p[3], w[0], w[1], w[2], w[3]);
		}
		row[SIDE] = 0;
		printf("  %s\n", row);
	}
	printf("%s %s: %d/%d texels\n", bad ? "FAIL" : "ok  ", what, SIDE * SIDE - bad, SIDE * SIDE);
	failures += bad != 0;
	return bad;
}

static const uint8_t RED[4] = { 255, 0, 0, 255 }, GREEN[4] = { 0, 255, 0, 255 }, BLUE[4] = { 0, 0, 255, 255 },
		     YELLOW[4] = { 255, 255, 0, 255 };

/* colored_vertex/fragment: top-left quarter red, cleared blue, straight into an IOSurface. */
static void render_iosurface(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q)
{
	id<MTLRenderPipelineState> p = pipeline(dev, lib, @"colored_vertex", @"colored_fragment", nil, MTLPixelFormatInvalid);
	NSDictionary *props = @{
		(id)kIOSurfaceWidth : @SIDE, (id)kIOSurfaceHeight : @SIDE, (id)kIOSurfaceBytesPerElement : @4,
		(id)kIOSurfacePixelFormat : @((uint32_t)'RGBA'),
	};
	IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)props);
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
										       width:SIDE height:SIDE mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	id<MTLTexture> t = s ? [dev newTextureWithDescriptor:td iosurface:s plane:0] : nil;
	check(t != nil, "IOSurface texture", nil);
	if (!p || !t)
		return;
	const float pos[6][2] = { { -1, 1 }, { 0, 1 }, { -1, 0 }, { 0, 1 }, { 0, 0 }, { -1, 0 } };
	float col[6][4];
	for (int i = 0; i < 6; i++) {
		col[i][0] = col[i][3] = 1;
		col[i][1] = col[i][2] = 0;
	}
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass_into(t, MTLClearColorMake(0, 0, 1, 1))];
	[e setRenderPipelineState:p];
	[e setVertexBytes:pos length:sizeof pos atIndex:0];
	[e setVertexBytes:col length:sizeof col atIndex:1];
	[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[e endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	printf("IOSurface %zux%zu, %zu bytes per row\n", IOSurfaceGetWidth(s), IOSurfaceGetHeight(s), IOSurfaceGetBytesPerRow(s));
	compare("render into IOSurface", IOSurfaceGetBaseAddress(s), IOSurfaceGetBytesPerRow(s),
		^(int x, int y) { return x < 4 && y < 4 ? RED : BLUE; }, ^(int x, int y) { return x < 4 && y < 4 ? '#' : '.'; });
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	CFRelease(s);
}

struct vtx {
	float pos[3];
	uint8_t rgba[4];
};

static void quad(struct vtx *v, float x0, float y0, float x1, float y1, float z, const uint8_t c[4])
{
	const float p[6][2] = { { x0, y1 }, { x1, y1 }, { x0, y0 }, { x1, y1 }, { x1, y0 }, { x0, y0 } };
	for (int i = 0; i < 6; i++) {
		v[i] = (struct vtx){ { p[i][0], p[i][1], z }, { 0 } };
		memcpy(v[i].rgba, c, 4);
	}
}

static id<MTLDepthStencilState> ds(id<MTLDevice> dev, MTLCompareFunction depth, BOOL write, MTLCompareFunction st,
				   MTLStencilOperation pass)
{
	MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
	d.depthCompareFunction = depth;
	d.depthWriteEnabled = write;
	MTLStencilDescriptor *s = [MTLStencilDescriptor new];
	s.stencilCompareFunction = st;
	s.depthStencilPassOperation = pass;
	d.frontFaceStencil = s;
	d.backFaceStencil = s;
	return [dev newDepthStencilStateWithDescriptor:d];
}

static id<MTLTexture> target(id<MTLDevice> dev, MTLPixelFormat f, MTLTextureUsage usage)
{
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:SIDE height:SIDE mipmapped:NO];
	td.usage = usage;
	td.storageMode = MTLStorageModePrivate;
	return [dev newTextureWithDescriptor:td];
}

static const uint8_t *readback(id<MTLCommandBuffer> cb, id<MTLTexture> t, id<MTLBuffer> out)
{
	id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
	[b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(SIDE, SIDE, 1)
		  toBuffer:out destinationOffset:0 destinationBytesPerRow:SIDE * 4 destinationBytesPerImage:SIDE * SIDE * 4];
	[b endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	return out.contents;
}

/* attr_vertex with a vertex descriptor (float3 + uchar4 normalized, interleaved), D32S8:
 * depth Less, stencil Replace then Equal, a back-facing quad culled. */
static void render_depth(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q)
{
	MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
	vd.attributes[0].format = MTLVertexFormatFloat3;
	vd.attributes[1].format = MTLVertexFormatUChar4Normalized;
	vd.attributes[1].offset = 12;
	vd.layouts[0].stride = sizeof(struct vtx);
	id<MTLRenderPipelineState> p = pipeline(dev, lib, @"attr_vertex", @"attr_fragment", vd, MTLPixelFormatDepth32Float_Stencil8);
	id<MTLTexture> color = target(dev, MTLPixelFormatRGBA8Unorm, MTLTextureUsageRenderTarget);
	id<MTLTexture> zs = target(dev, MTLPixelFormatDepth32Float_Stencil8, MTLTextureUsageRenderTarget);
	if (!p || !color || !zs) {
		check(NO, "depth targets", nil);
		return;
	}
	static const uint8_t magenta[4] = { 255, 0, 255, 255 };
	struct vtx v[24];
	quad(v + 0, -1, -1, 0, 1, 0.25f, GREEN);
	quad(v + 6, -1, -1, 1, 1, 0.75f, RED);
	quad(v + 12, -1, 0, 1, 1, 0.0f, YELLOW);
	quad(v + 18, -1, -1, 1, 1, 0.0f, magenta);
	id<MTLBuffer> vb = [dev newBufferWithBytes:v length:sizeof v options:0];
	id<MTLBuffer> out = [dev newBufferWithLength:SIDE * SIDE * 4 options:0];
	MTLRenderPassDescriptor *pass = pass_into(color, MTLClearColorMake(0, 0, 1, 1));
	pass.depthAttachment.texture = zs;
	pass.depthAttachment.clearDepth = 1.0;
	pass.stencilAttachment.texture = zs;
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
	[e setRenderPipelineState:p];
	[e setVertexBuffer:vb offset:0 atIndex:0];
	[e setStencilReferenceValue:1];
	[e setDepthStencilState:ds(dev, MTLCompareFunctionLess, YES, MTLCompareFunctionAlways, MTLStencilOperationReplace)];
	[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[e setDepthStencilState:ds(dev, MTLCompareFunctionLess, YES, MTLCompareFunctionAlways, MTLStencilOperationKeep)];
	[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:6 vertexCount:6];
	[e setDepthStencilState:ds(dev, MTLCompareFunctionAlways, NO, MTLCompareFunctionEqual, MTLStencilOperationKeep)];
	[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:12 vertexCount:6];
	[e setFrontFacingWinding:MTLWindingCounterClockwise];
	[e setCullMode:MTLCullModeBack];
	[e setDepthStencilState:nil];
	[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:18 vertexCount:6];
	[e endEncoding];
	compare("vertex descriptor + depth/stencil + cull", readback(cb, color, out), SIDE * 4,
		^(int x, int y) { return x >= 4 ? RED : y < 4 ? YELLOW : GREEN; },
		^(int x, int y) { return x >= 4 ? 'R' : y < 4 ? 'Y' : 'G'; });
}

/* sampled_fragment: an 8x8 texture (replaceRegion) sampled at texel centers onto an 8x8 target. */
static void render_sampled(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q)
{
	id<MTLRenderPipelineState> p = pipeline(dev, lib, @"colored_vertex", @"sampled_fragment", nil, MTLPixelFormatInvalid);
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
										       width:SIDE height:SIDE mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead;
	id<MTLTexture> src = [dev newTextureWithDescriptor:td];
	id<MTLTexture> dst = target(dev, MTLPixelFormatRGBA8Unorm, MTLTextureUsageRenderTarget);
	if (!p || !src || !dst)
		return;
	static uint8_t texels[SIDE * SIDE][4];
	for (int i = 0; i < SIDE * SIDE; i++) {
		texels[i][0] = (uint8_t)(i * 4);
		texels[i][1] = (uint8_t)(255 - i * 3);
		texels[i][2] = (uint8_t)(i & 1 ? 255 : 0);
		texels[i][3] = 255;
	}
	[src replaceRegion:MTLRegionMake2D(0, 0, SIDE, SIDE) mipmapLevel:0 withBytes:texels bytesPerRow:SIDE * 4];
	const float pos[6][2] = { { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } };
	float col[6][4] = { { 0 } };
	id<MTLBuffer> out = [dev newBufferWithLength:SIDE * SIDE * 4 options:0];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass_into(dst, MTLClearColorMake(0, 0, 0, 0))];
	[e setRenderPipelineState:p];
	[e setVertexBytes:pos length:sizeof pos atIndex:0];
	[e setVertexBytes:col length:sizeof col atIndex:1];
	[e setFragmentTexture:src atIndex:0];
	[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[e endEncoding];
	compare("texture sampling + blit readback", readback(cb, dst, out), SIDE * 4,
		^(int x, int y) { return (const uint8_t *)texels[y * SIDE + x]; }, ^(int x, int y) { return 's'; });
}

/* newFunctionWithName:constantValues:error: (function constants: factor 3, add_one) and a run */
static void specialized(id<MTLDevice> dev, id<MTLLibrary> lib, id<MTLCommandQueue> q)
{
	MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
	float factor = 3;
	bool one = true;
	[cv setConstantValue:&factor type:MTLDataTypeFloat atIndex:0];
	[cv setConstantValue:&one type:MTLDataTypeBool atIndex:1];
	NSError *error = nil;
	id<MTLFunction> f = nil;
	@try {
		f = [lib newFunctionWithName:@"scaled" constantValues:cv error:&error];
	} @catch (NSException *e) {
		printf("exception: %s\n", e.reason.UTF8String);
	}
	printf("specialized function: %s (%s)\n", f ? "ok" : "nil", NSStringFromClass([(id)f class]).UTF8String);
	check(f != nil, "newFunctionWithName:constantValues:", error);
	if (!f)
		return;
	id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:f error:&error];
	check(p != nil, "specialized pipeline", error);
	if (!p)
		return;
	id<MTLBuffer> in = [dev newBufferWithLength:256 options:0], out = [dev newBufferWithLength:256 options:0];
	for (int i = 0; i < 64; i++)
		((float *)in.contents)[i] = i;
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
	[e setComputePipelineState:p];
	[e setBuffer:in offset:0 atIndex:0];
	[e setBuffer:out offset:0 atIndex:1];
	[e dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
	[e endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	int good = 0;
	for (int i = 0; i < 64; i++)
		good += ((float *)out.contents)[i] == i * 3.0f + 1;
	printf("%s function constants: %d/64\n", good == 64 ? "ok  " : "FAIL", good);
	failures += good != 64;
}

/* CoreAnimation's no-copy pattern: write after making the buffer, commit a copy out of it,
 * release the buffer and munmap the pages before the GPU is done; the copy must still read them. */
static void nocopy_unmap(id<MTLDevice> dev)
{
	size_t len = 4 * 16384;
	uint32_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	id<MTLBuffer> src = [dev newBufferWithBytesNoCopy:mem length:len options:0 deallocator:nil];
	for (size_t i = 0; i < len / 4; i++)
		mem[i] = (uint32_t)i * 7;
	check(src.contents == mem, "no-copy contents is the caller's pointer", nil);
	id<MTLBuffer> dst = [dev newBufferWithLength:len options:0];
	id<MTLCommandQueue> q = [dev newCommandQueue];
	id<MTLCommandBuffer> cb = [q commandBuffer];
	id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
	[e copyFromBuffer:src sourceOffset:0 toBuffer:dst destinationOffset:0 size:len];
	[e endEncoding];
	[cb commit];
	src = nil;
	munmap(mem, len);
	[cb waitUntilCompleted];
	int good = 0;
	for (size_t i = 0; i < len / 4; i++)
		good += ((uint32_t *)dst.contents)[i] == (uint32_t)i * 7;
	printf("%s no-copy buffer read after the caller's munmap: %d/%zu words\n", good == (int)(len / 4) ? "ok  " : "FAIL",
	       good, len / 4);
	failures += good != (int)(len / 4);
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		const char *dylib = argc > 1 ? argv[1] : "/usr/local/sweettest/SweetMetal.dylib";
		const char *syslib = argc > 2 ? argv[2] : "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
		const char *testlib = argc > 3 ? argv[3] : "/usr/local/sweettest/sweetmetal_test.metallib";
		Class cls = nil;
		if (strstr(dylib, ".bundle")) {
			/* as Metal's getMetalPluginClassForService: bundle, classNamed:, _MTLDevice subclass */
			NSBundle *b = [NSBundle bundleWithPath:@(dylib)];
			cls = [b classNamed:@"SweetMTLDevice"];
			printf("bundle %s: %s, principalClass %s, subclass of _MTLDevice %d\n", dylib, b ? "found" : "MISSING",
			       NSStringFromClass(b.principalClass).UTF8String, [cls isSubclassOfClass:NSClassFromString(@"_MTLDevice")]);
		} else if (!dlopen(dylib, RTLD_NOW)) {
			printf("FAIL dlopen: %s\n", dlerror());
			return 1;
		} else {
			cls = NSClassFromString(@"SweetMTLDevice");
		}
		id<MTLDevice> dev = [(id)[cls alloc] initWithAcceleratorPort:0];
		check(dev != nil, "SweetMTLDevice initWithAcceleratorPort:0", nil);
		if (!dev)
			return 1;
		printf("device: %s, Apple4 %d, MTLCreateSystemDefaultDevice %p\n", dev.name.UTF8String,
		       [dev supportsFamily:MTLGPUFamilyApple4], (__bridge void *)MTLCreateSystemDefaultDevice());
		system_kernels(dev, syslib);
		if (access(testlib, R_OK) == 0) {
			scale_and_offset(dev, testlib);
			id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(testlib)] error:nil];
			id<MTLCommandQueue> q = [dev newCommandQueue];
			render_iosurface(dev, lib, q);
			render_depth(dev, lib, q);
			render_sampled(dev, lib, q);
			specialized(dev, lib, q);
		} else
			printf("skip  %s (no test metallib)\n", testlib);
		/* an unimplemented selector: logged and answered nil (not SWEETMETAL_STRICT) */
		id<MTLBuffer> b = [dev newBufferWithLength:4096 options:0];
		id none = [(id)b performSelector:NSSelectorFromString(@"sweetNoSuchMethod")];
		check(none == nil, "missing selector answered nil", nil);
		nocopy_unmap(dev);
		printf(failures ? "MTL_PROBE FAIL (%d)\n" : "MTL_PROBE OK\n", failures);
		return failures != 0;
	}
}
