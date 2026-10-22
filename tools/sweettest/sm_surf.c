/* sm_surf ID...: an IOSurface's layout (top level and every plane) by id */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	for (int a = 1; a < argc; a++) {
		IOSurfaceRef s = IOSurfaceLookup((IOSurfaceID)atoi(argv[a]));
		if (!s) {
			printf("surface %s: lookup failed\n", argv[a]);
			continue;
		}
		OSType f = IOSurfaceGetPixelFormat(s);
		printf("surface %s: %zux%zu fmt '%c%c%c%c' bpe %zu bpr %zu ew %zu eh %zu alloc %zu planes %zu\n", argv[a],
		    IOSurfaceGetWidth(s), IOSurfaceGetHeight(s), (char)(f >> 24), (char)(f >> 16), (char)(f >> 8), (char)f,
		    IOSurfaceGetBytesPerElement(s), IOSurfaceGetBytesPerRow(s), IOSurfaceGetElementWidth(s),
		    IOSurfaceGetElementHeight(s), IOSurfaceGetAllocSize(s), IOSurfaceGetPlaneCount(s));
		uint8_t *base = IOSurfaceGetBaseAddress(s);
		for (size_t p = 0; p < IOSurfaceGetPlaneCount(s); p++) {
			uint8_t *pb = IOSurfaceGetBaseAddressOfPlane(s, p);
			printf("  plane %zu: %zux%zu bpe %zu bpr %zu ew %zu eh %zu offset %ld comps %zu\n", p,
			    IOSurfaceGetWidthOfPlane(s, p), IOSurfaceGetHeightOfPlane(s, p),
			    IOSurfaceGetBytesPerElementOfPlane(s, p), IOSurfaceGetBytesPerRowOfPlane(s, p),
			    IOSurfaceGetElementWidthOfPlane(s, p), IOSurfaceGetElementHeightOfPlane(s, p),
			    (long)(pb - base), IOSurfaceGetNumberOfComponentsOfPlane(s, p));
		}
		CFRelease(s);
	}
	return 0;
}
