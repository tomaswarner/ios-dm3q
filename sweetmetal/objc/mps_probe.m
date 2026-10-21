/* mps_probe: MetalPerformanceShaders on the system Metal device (boot with sweetmetal=1):
 * MPSSupportsMTLDevice, then a Gaussian blur of a 64x64 texture. argv[1] = device name override
 * (SWEETMETAL_NAME, set before Metal creates the device in this process). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc > 1) {
			char name[128];
			snprintf(name, sizeof name, "%s", argv[1]);
			for (char *c = name; *c; c++)
				if (*c == '_')
					*c = ' ';
			setenv("SWEETMETAL_NAME", name, 1);
		}
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		printf("device %s, MPSSupportsMTLDevice %d\n", dev.name.UTF8String, MPSSupportsMTLDevice(dev));
		if (!dev)
			return 1;
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
											       width:64 height:64 mipmapped:NO];
		td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
		id<MTLTexture> src = [dev newTextureWithDescriptor:td], dst = [dev newTextureWithDescriptor:td];
		static uint8_t px[64 * 64 * 4];
		for (int i = 0; i < 64 * 64; i++)
			px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = px[i * 4 + 3] = (i % 64) < 32 ? 255 : 0;
		[src replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:px bytesPerRow:256];
		MPSImageGaussianBlur *blur = [[MPSImageGaussianBlur alloc] initWithDevice:dev sigma:4];
		printf("blur kernel %p\n", (__bridge void *)blur);
		id<MTLCommandQueue> q = [dev newCommandQueue];
		id<MTLCommandBuffer> cb = [q commandBuffer];
		[blur encodeToCommandBuffer:cb sourceTexture:src destinationTexture:dst];
		[cb commit];
		[cb waitUntilCompleted];
		[dst getBytes:px bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
		printf("row 32: ");
		for (int x = 24; x < 40; x++)
			printf("%d ", px[(32 * 64 + x) * 4]);
		printf("\nMPS_PROBE DONE status %ld\n", (long)cb.status);
	}
	return 0;
}
