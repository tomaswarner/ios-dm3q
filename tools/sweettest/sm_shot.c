/*
 * sm_shot [scale]: screenshot of the DM3Q panel over sweetrun. Asks IOMobileFramebuffer (our
 * AppleCLCD, xnu SweetFramebuffer.cpp) for layer 0's default surface, which is the panel memory
 * itself, reads it and prints "SHOT w h" then the image (every scale-th pixel, RGB) as base64
 * lines. Mac side: tools/sm_shot.py writes a PNG.
 * Build: xcrun -sdk iphoneos clang -target arm64-apple-ios16.0 -O2 sm_shot.c -o sm_shot
 *        -framework IOSurface -framework CoreFoundation && codesign -s - sm_shot
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void *IOMobileFramebufferRef;
typedef int (*get_main_fn)(IOMobileFramebufferRef *);
typedef int (*get_surface_fn)(IOMobileFramebufferRef, int, IOSurfaceRef *);

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emit(const uint8_t *p, size_t n)
{
	char line[77];
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		line[o++] = b64[v >> 18 & 63];
		line[o++] = b64[v >> 12 & 63];
		line[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
		line[o++] = i + 2 < n ? b64[v & 63] : '=';
		if (o == 76) {
			line[o] = 0;
			puts(line);
			o = 0;
		}
	}
	if (o) {
		line[o] = 0;
		puts(line);
	}
}

int main(int argc, char **argv)
{
	int scale = argc > 1 ? atoi(argv[1]) : 3;
	if (scale < 1)
		scale = 1;
	void *h = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_NOW);
	get_main_fn get_main = h ? (get_main_fn)dlsym(h, "IOMobileFramebufferGetMainDisplay") : NULL;
	get_surface_fn get_surface = h ? (get_surface_fn)dlsym(h, "IOMobileFramebufferGetLayerDefaultSurface") : NULL;
	IOMobileFramebufferRef fb = NULL;
	IOSurfaceRef s = NULL;
	if (!get_main || !get_surface || get_main(&fb) || !fb || get_surface(fb, 0, &s) || !s) {
		printf("SHOT FAIL no framebuffer surface (%s)\n", h ? "lookup" : dlerror());
		return 1;
	}
	IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *base = IOSurfaceGetBaseAddress(s);
	size_t w = IOSurfaceGetWidth(s), hgt = IOSurfaceGetHeight(s), bpr = IOSurfaceGetBytesPerRow(s);
	size_t ow = w / scale, oh = hgt / scale;
	uint8_t *rgb = malloc(ow * oh * 3);
	for (size_t y = 0; y < oh; y++) {
		const uint8_t *row = base + y * scale * bpr;
		for (size_t x = 0; x < ow; x++) {
			const uint8_t *p = row + x * scale * 4;       /* BGRA */
			uint8_t *d = rgb + (y * ow + x) * 3;
			d[0] = p[2];
			d[1] = p[1];
			d[2] = p[0];
		}
	}
	IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
	printf("SHOT %zu %zu\n", ow, oh);
	emit(rgb, ow * oh * 3);
	printf("SHOT END\n");
	return 0;
}
