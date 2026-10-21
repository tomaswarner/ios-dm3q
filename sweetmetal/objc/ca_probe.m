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

/* Frames at the presented size (1125x2436): 20 layers over a background whose color alternates
 * each frame; the time until the frame's color shows up in the target is the frame's latency. */
static void full_screen(id<MTLDevice> dev)
{
	const int fw = 1125, fh = 2436;
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
										       width:fw height:fh mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> target = [dev newTextureWithDescriptor:td];
	CALayer *root = [CALayer layer];
	root.frame = CGRectMake(0, 0, fw, fh);
	for (int i = 0; i < 20; i++) {
		CALayer *l = [CALayer layer];
		l.frame = CGRectMake(40 + (i % 4) * 260, 300 + (i / 4) * 300, 200, 200);
		l.backgroundColor = rgb((i % 3) / 2.0, (i % 5) / 4.0, 0.5);
		l.cornerRadius = 40;
		l.opacity = 0.9;
		[root addSublayer:l];
	}
	CARenderer *r = [CARenderer rendererWithMTLTexture:target options:nil];
	[CATransaction begin];
	r.layer = root;
	r.bounds = CGRectMake(0, 0, fw, fh);
	[CATransaction commit];
	[CATransaction flush];
	for (int frame = 0; frame < 6; frame++) {
		int red = frame & 1;
		[CATransaction begin];
		root.backgroundColor = red ? rgb(1, 0, 0) : rgb(0, 0, 1);
		[CATransaction commit];
		[CATransaction flush];
		CFTimeInterval t0 = CACurrentMediaTime();
		[r beginFrameAtTime:t0 timeStamp:NULL];
		[r addUpdateRect:r.bounds];
		[r render];
		[r endFrame];
		CFTimeInterval t1 = CACurrentMediaTime();
		uint8_t px[4];
		int seen = 0;
		while (CACurrentMediaTime() - t0 < 20) {
			[target getBytes:px bytesPerRow:4 fromRegion:MTLRegionMake2D(5, 5, 1, 1) mipmapLevel:0];
			if (red ? (px[2] > 200 && px[0] < 50) : (px[0] > 200 && px[2] < 50)) {
				seen = 1;
				break;
			}
			usleep(2000);
		}
		printf("frame %d (%s): CPU encode %.1f ms, on screen after %.1f ms%s\n", frame, red ? "red" : "blue",
		       (t1 - t0) * 1e3, (CACurrentMediaTime() - t0) * 1e3, seen ? "" : " NOT SEEN");
	}
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		const char *path = argc > 1 ? argv[1] : "/System/Library/Extensions/SweetMetal.bundle";
		setenv("SWEETMETAL_TRACE", "1", 0);
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		printf("MTLCreateSystemDefaultDevice before: %p (%s)\n", (__bridge void *)dev, dev.name.UTF8String);
		if (!dev) {
			/* no IOAcceleratorES (kernel without sweetmetal=1): register ours in this process */
			Class cls = [[NSBundle bundleWithPath:@(path)] classNamed:@"SweetMTLDevice"];
			dev = [(id)[cls alloc] initWithAcceleratorPort:0];
			if (!dev) {
				printf("FAIL device\n");
				return 1;
			}
			MTLAddDevice(dev);
		}
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
		full_screen(dev);
		printf("CA_PROBE DONE\n");
	}
	return 0;
}
