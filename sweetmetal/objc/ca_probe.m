/* ca_probe: CoreAnimation's Metal renderer on SweetMetal, in this process only (no kernel nub,
 * no flash): loads SweetMetal.bundle, registers the device with Metal's exported MTLAddDevice (as
 * _MTLRegisterDevices would for an IOAcceleratorES service), checks MTLCreateSystemDefaultDevice,
 * then renders a CALayer tree with +[CARenderer rendererWithMTLTexture:options:] into one of our
 * textures and reads it back. Missing driver selectors show up as "SWEETMETAL missing:".
 * Usage: ca_probe [BUNDLE] */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern void MTLAddDevice(id<MTLDevice> device);

@interface NSObject (SweetMetalProbe)
- (id)initWithAcceleratorPort:(unsigned int)port;
@end

#define W 64
#define H 64

static CGColorRef rgb(double r, double g, double b)
{
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGFloat c[4] = { r, g, b, 1 };
	CGColorRef col = CGColorCreate(cs, c);
	CGColorSpaceRelease(cs);
	return col;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		const char *path = argc > 1 ? argv[1] : "/System/Library/Extensions/SweetMetal.bundle";
		setenv("SWEETMETAL_TRACE", "1", 0);
		printf("MTLCreateSystemDefaultDevice before: %p\n", (__bridge void *)MTLCreateSystemDefaultDevice());
		Class cls = [[NSBundle bundleWithPath:@(path)] classNamed:@"SweetMTLDevice"];
		id<MTLDevice> dev = [(id)[cls alloc] initWithAcceleratorPort:0];
		if (!dev) {
			printf("FAIL device\n");
			return 1;
		}
		MTLAddDevice(dev);
		id<MTLDevice> sys = MTLCreateSystemDefaultDevice();
		printf("MTLCreateSystemDefaultDevice after MTLAddDevice: %s (%s)\n", sys == dev ? "OURS" : "other",
		       sys.name.UTF8String);

		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											       width:W height:H mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> target = [dev newTextureWithDescriptor:td];
		printf("probe target texture %p\n", (__bridge void *)target);

		/* red background, a blue square (rounded) in the top-left quarter, a half-transparent
		 * green bar across the bottom */
		CALayer *root = [CALayer layer];
		root.frame = CGRectMake(0, 0, W, H);
		root.backgroundColor = rgb(1, 0, 0);
		CALayer *square = [CALayer layer];
		square.frame = CGRectMake(0, 0, W / 2, H / 2);
		square.backgroundColor = rgb(0, 0, 1);
		square.cornerRadius = 4;
		[root addSublayer:square];
		CALayer *bar = [CALayer layer];
		bar.frame = CGRectMake(0, H - 16, W, 16);
		bar.backgroundColor = rgb(0, 1, 0);
		bar.opacity = 0.5;
		[root addSublayer:bar];
		[CATransaction flush];

		CARenderer *r = nil;
		@try {
			r = [CARenderer rendererWithMTLTexture:target options:nil];
		} @catch (NSException *e) {
			printf("exception creating CARenderer: %s\n", e.reason.UTF8String);
		}
		printf("CARenderer: %s\n", r ? "created" : "nil");
		if (!r)
			return 1;
		[CATransaction begin];
		r.layer = root;
		r.bounds = CGRectMake(0, 0, W, H);
		[CATransaction commit];
		[CATransaction flush];
		@try {
			[r beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];
			[r addUpdateRect:r.bounds];
			[r render];
			[r endFrame];
		} @catch (NSException *e) {
			printf("exception rendering: %s\n", e.reason.UTF8String);
		}
		/* CA commits its own command buffer; give it a moment, then read back */
		sleep(10);
		static uint8_t px[W * H * 4];
		[target getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
		for (int y = 0; y < H; y += 8) {
			printf("  ");
			for (int x = 0; x < W; x += 4) {
				const uint8_t *p = px + (y * W + x) * 4;   /* BGRA */
				char c = p[2] > 200 && p[1] < 60 && p[0] < 60 ? 'R' : p[0] > 200 && p[2] < 60 ? 'B'
				       : p[1] > 100 && p[2] > 100 ? 'g' : p[3] == 0 ? '.' : '?';
				putchar(c);
			}
			putchar('\n');
		}
		const uint8_t *c = px + ((H / 4) * W + W / 4) * 4, *e = px + ((H / 4) * W + 3 * W / 4) * 4,
			      *b = px + ((H - 8) * W + 3 * W / 4) * 4;
		printf("square %d %d %d %d, background %d %d %d %d, bar %d %d %d %d (BGRA)\n", c[0], c[1], c[2], c[3],
		       e[0], e[1], e[2], e[3], b[0], b[1], b[2], b[3]);
		printf("CA_PROBE DONE\n");
	}
	return 0;
}
