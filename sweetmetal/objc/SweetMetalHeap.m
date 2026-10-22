/*
 * MTLHeap for SweetMetal: SpringBoard (iOS 16) makes heaps and asks the device for heap sizes. A heap
 * here is a budget, not one memory block: each resource it makes is an ordinary device resource
 * (metal2vulkan suballocates them itself), counted against the heap's size so that
 * usedSize / maxAvailableSizeWithAlignment: answer as Metal's do. Aliasing (makeAliasable) is a no-op,
 * which Metal allows: an aliasable resource may keep its memory.
 */
#import "SweetMetalPrivate.h"

/* A generous, deterministic estimate of a texture's footprint: every mip level, 16-byte texels at most,
 * rows and the whole rounded up like GPU allocations are. */
static uint64_t texture_bytes(MTLTextureDescriptor *d)
{
	uint64_t bpp = 16;
	switch ((uint32_t)d.pixelFormat) {
	case 10: case 11: case 12: case 13: case 14: case 1:    /* 8-bit one channel */
		bpp = 1;
		break;
	case 30: case 31: case 32: case 33: case 34:            /* 8-bit two channel */
	case 20: case 22: case 23: case 24: case 25:            /* 16-bit one channel */
		bpp = 2;
		break;
	case 70: case 71: case 72: case 73: case 74: case 80: case 81: case 90: case 91: case 92: case 93: case 94:
	case 252: case 253: case 255: case 260: case 261: case 550: case 551: case 552: case 553: case 554: case 555:
		bpp = 4;
		break;
	case 103: case 104: case 105: case 110: case 111: case 112: case 113: case 114: case 115:
		bpp = 8;
		break;
	default:
		break;
	}
	uint64_t total = 0, w = d.width, h = d.height ? d.height : 1, z = d.depth ? d.depth : 1;
	for (NSUInteger m = 0; m < (d.mipmapLevelCount ? d.mipmapLevelCount : 1); m++) {
		total += ((w * bpp + 255) & ~255ull) * h * z;
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
		z = z > 1 ? z / 2 : 1;
	}
	uint64_t layers = (d.arrayLength ? d.arrayLength : 1) * (d.textureType == MTLTextureTypeCube ||
	    d.textureType == MTLTextureTypeCubeArray ? 6 : 1) * (d.sampleCount ? d.sampleCount : 1);
	return (total * layers + 0xffff) & ~0xffffull;
}

@interface SweetMTLHeap : NSObject <MTLHeap>
@end

@implementation SweetMTLHeap {
	SweetMTLDevice *_device;
	MTLHeapDescriptor *_desc;
	uint64_t _used;
	NSString *_label;
}

- (instancetype)initWithDevice:(SweetMTLDevice *)device descriptor:(MTLHeapDescriptor *)desc
{
	if ((self = [super init])) {
		_device = device;
		_desc = [desc copy];
	}
	return self;
}

- (id<MTLDevice>)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { _label = [label copy]; }
- (MTLStorageMode)storageMode { return _desc.storageMode; }
- (MTLCPUCacheMode)cpuCacheMode { return _desc.cpuCacheMode; }
- (MTLHazardTrackingMode)hazardTrackingMode { return _desc.hazardTrackingMode; }
- (MTLResourceOptions)resourceOptions { return _desc.resourceOptions; }
- (MTLHeapType)type { return _desc.type; }
- (NSUInteger)size { return _desc.size; }
- (NSUInteger)usedSize { return _used; }
- (NSUInteger)currentAllocatedSize { return _desc.size; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)alignment
{
	return _used < _desc.size ? (_desc.size - _used) & ~(uint64_t)(alignment ? alignment - 1 : 0) : 0;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state { return MTLPurgeableStateNonVolatile; }

- (MTLResourceOptions)optionsFor:(MTLResourceOptions)options
{
	return (options & ~(MTLResourceStorageModeMask | MTLResourceCPUCacheModeMask)) |
	    ((MTLResourceOptions)_desc.storageMode << MTLResourceStorageModeShift) |
	    ((MTLResourceOptions)_desc.cpuCacheMode << MTLResourceCPUCacheModeShift);
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	uint64_t bytes = (length + 0xffff) & ~0xffffull;
	if (_desc.size && _used + bytes > _desc.size)
		return nil;
	id<MTLBuffer> b = [_device newBufferWithLength:length options:[self optionsFor:options]];
	if (b)
		_used += bytes;
	return b;
}

- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options offset:(NSUInteger)offset
{
	return [self newBufferWithLength:length options:options];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d
{
	uint64_t bytes = texture_bytes(d);
	if (_desc.size && _used + bytes > _desc.size)
		return nil;
	MTLTextureDescriptor *td = [d copy];
	td.storageMode = _desc.storageMode;
	td.cpuCacheMode = _desc.cpuCacheMode;
	id<MTLTexture> t = [_device newTextureWithDescriptor:td];
	if (t)
		_used += bytes;
	return t;
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset
{
	return [self newTextureWithDescriptor:d];
}

@end

@implementation SweetMTLDevice (Heap)

- (id<MTLHeap>)newHeapWithDescriptor:(MTLHeapDescriptor *)desc
{
	/* Placement heaps alias their resources at the offsets the caller picks (SpringBoard's text
	 * lost its glyphs with separate allocations): not made here, the caller's fallback (nil, as
	 * before heaps) keeps working. Automatic heaps only. */
	fprintf(stderr, "SWEETMETAL heap %lu bytes storage %lu type %ld%s\n", (unsigned long)desc.size,
	    (unsigned long)desc.storageMode, (long)desc.type, desc.type == MTLHeapTypeAutomatic ? "" : " (not made)");
	if (desc.type != MTLHeapTypeAutomatic)
		return nil;
	return [[SweetMTLHeap alloc] initWithDevice:self descriptor:desc];
}

- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d
{
	return (MTLSizeAndAlign){ texture_bytes(d), 0x10000 };
}

- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)length options:(MTLResourceOptions)options
{
	return (MTLSizeAndAlign){ (length + 0xffff) & ~0xffffull, 0x10000 };
}

@end
