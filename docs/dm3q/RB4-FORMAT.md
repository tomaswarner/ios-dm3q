# IOSurface OSType '_RB4' (0x5f524234), iOS 16.7 20H392

Evidence: DSC `apple/dsc/20H392__iPhone10,3_6`, images extracted with `ipsw dyld extract` and disassembled with `otool -tV`
(scratch: `/private/tmp/claude-501/-Users-tomaswarner-Desktop-hackios-s23/a874681b-7d77-46bb-a8e0-74641f66ed4d/scratchpad/rb4/`).
Addresses are unslid DSC vmaddrs. Items marked UNVERIFIED are inference, not read from code.

## 0. Method / what was NOT found
- A raw scan of all DSC subcache files for `movk wN,#0x5f52,lsl#16` (0x72ABEA40|Rd) finds exactly 5 hits; only 2 are real users:
  QuartzCore (0x1882d6df8) and RenderBox (0x1e68c8820, 0x1e68b46b0). The others are false positives (CorePrediction, ANECompiler).
- NOT in CoreVideo, IOSurface.framework, or Metal.framework (no '_RB4' immediate, no string). So '_RB4' is **not** a CoreVideo
  format: it is a private OSType invented by RenderBox (SwiftUI's renderer) and recognised by QuartzCore. No CVPixelFormatDescription entry exists.
  The plane layout lives in RenderBox's surface creator, not in a table.

## 1. Plane layout of '_RB4' (RenderBox `RB::create_iosurface(MTLPixelFormat, w, h, flags)` @ 0x1e68b4624)
Prologue: `pixel_format_traits(fmt)` (0x1e68c8654) gives fourcc at `traits+0xc` (w23). Test at 0x1e68b46ac..0x1e68b46c0: for
fourcc != 0 and != '_RB4' it calls the generic cached-surface helper (0x1ecc13c3c, fourcc only); for fourcc == '_RB4' (or 0) it
ALWAYS builds an explicit properties dictionary, with a `kIOSurfacePlaneInfo`-style array of two plane dicts.
Dispatch is a byte jump table at 0x1e6945340 indexed by (MTLPixelFormat - 0x21c): bytes `47 61 2f 8a.. 8a 00 00`
(540 -> 0x47, 541 -> 0x61, **542 -> 0x2f**, 543..549 default, 550/551 -> 0). Target = 0x1e68b4718 + byte*4:
- 540 RGB8_420_2P -> 0x1e68b4834: p0 (w,h,bpe 1); p1 ((w+1)/2,(h+1)/2,bpe 2)
- 541 RGB8_422_2P -> 0x1e68b489c: p0 (w,h,bpe 1); p1 ((w+1)/2,h,bpe 2)
- **542 RGB8_444_2P -> 0x1e68b47d4: p0 (w,h,bpe 1) at offset 0; p1 (w,h,bpe 2) at offset = p0 size** (calls at 0x1e68b47ec and 0x1e68b4808)
- 550/551 -> 0x1e68b4718: p0 bpe 4, p1 bpe 1 (the 10-bit XR formats, not _RB4)
Plane helper `RB::(anon)::create_plane_dict(width, height, bytesPerElement, offset, uint64& size_out)` @ 0x1e68c8e84:
rowBytes = IOSurfaceAlignProperty(<plane bytes-per-row key>, width*bpe) (call 0x1ecc14ad0; UNVERIFIED which key), size = height * rowBytes
(size_out), and a 7-key dict {PlaneWidth, PlaneHeight, PlaneBytesPerElement(=bpe), PlaneBytesPerRow, PlaneOffset, PlaneSize, PlaneElementWidth=1}
(key globals loaded from 0x20f7fb2a8..0x20f7fb318, UNVERIFIED names). Whole-surface Width/Height/PixelFormat/AllocSize = p0.size+p1.size+offsets
(0x1e68b4814..0x1e68b4820 `adds x25 = off + size`). No top-level BytesPerElement/BytesPerRow is passed (w26=0 in this branch).

So for width W, height H (both full resolution, "444"):
| plane | size | bytes/elem | bytes/row | offset |
|---|---|---|---|---|
| 0 | W x H | 1 | align(W) | 0 |
| 1 | W x H | 2 | align(2W) | p0.rowBytes*H (the alloc size recorded from create_plane_dict) |
Every texel: 1 byte in plane 0, 2 bytes in plane 1, element size 1x1, divisors 1/1 on both planes.
Your observed top-level bpe 4 / bytesPerRow 3968 (= 987*4 rounded to 64) / alloc 6373376 are kernel-side defaults (SweetSurface
`bytesPerRow` derived from a bpe-4 default), NOT RenderBox's; the real per-plane rows are in your `SWEET SURF:   id .. plane N:` log lines
from patch 0122. Fit check: (align(987)+align(1974))*1605 <= 3968*1605, so both planes fit in the 6373376 alloc. Trust plane offset/bpr from the
plane dicts, not the top-level fields. UNVERIFIED: no actual chronod plane log was available in the repo (grep for `fmt 0x5f524234` found none).

## 2. Component order / what the planes hold
'_RB4' is the "code" (non-converted) RGB flavour of Metal's two-plane 4:4:4 format: it is RenderBox's choice of 8-bit opaque RGB layer storage.
- RenderBox `RB::(anon) layer format picker` (0x1e68c9500..0x1e68c95e4) returns 542 (mov w10,#0x21e at 0x1e68c95e0) as the cheap opaque 8-bit
  format, vs 0x20b (523, YCBCR8_444_2P_sRGB) otherwise; `pixel_format_from_four_cc` maps '_RB4' -> 542 only when flag&2 (0x1e68c8824..0x1e68c88dc).
- Component mapping is Metal's, already measured in `ycbcr.rs` module docs: "The code formats (540 to 545) read the codes, red Cr, green Y, blue Cb,
  each code/255, alpha 1" (measured on M4/M3, same format semantics expected on A11). Therefore:
  - **G = plane0 byte** (the "Y" byte)
  - **plane1 is an interleaved byte pair like CbCr of 420v/444f: byte0 = "Cb" = B, byte1 = "Cr" = R** (order Cb-first is the CoreVideo/Metal
    norm; UNVERIFIED for 542 on AGX, verify against a known-colour RenderBox surface)
  - **A = 1** (no alpha plane; the name "RB" = chroma plane holds R and B, plane 0 holds G, "4" = 4:4:4)
  - no matrix/range conversion for the code formats (542 is not in the converted set).

## 3. QuartzCore consumption
`CA::OGL::four_cc_to_mtl_format(uint fourcc, MetalLimits const&)` @ 0x1882d69f4 (calls `CA::Render::fourcc_compressed_of_type(fourcc,0,0)`
@ 0x1882da078 first; with type 0 the fourcc is unchanged): at 0x1882d6df4..0x1882d6e24, `'_RB4' -> 0x21e (542)` iff capability bit 4 of the
second argument is set, else 0 (unsupported). (Sibling in the same block: 'b3a8' -> 0x226 (550) with bit 0x100.) CA then does
`-[MTLDevice newTextureWithDescriptor:iosurface:plane:]` with format 542; plane 0 is used with the full-surface descriptor (Metal reads both planes). UNVERIFIED: that bit 4 is the A11 "has RGB8_444_2P" capability.

## 4. Who creates the surfaces (UNVERIFIED link to chronod)
RenderBox `create_iosurface` is the only creator of '_RB4' found in the cache. Any SwiftUI/RenderBox render server users (chronod widget
snapshot rendering, backboardd/SpringBoard layers) would produce it for opaque layers; chronod itself is not in the DSC so its link to RenderBox was not checked.

## 5. What metal2vulkan needs
`crates/metal2vulkan-runtime/src/ycbcr.rs`: format 542 over YCbCr OSTypes is already implemented (code format: G=Y, B=Cb, R=Cr, alpha 1, `YCbCrCodes::of`,
`ycbcr_format`). It is refused only because `SURFACES` (line 148, 18 entries) has no `'_RB4'` row (`surface_planes`, the `measured` lookup -> `refused("over a surface of OSType '_RB4'")`).
Minimal change: add a 19th row `(b"_RB4", 8, false, YCbCrRange::Full, C444)` (range only matters for the converted formats; restrict the row to the code
formats 540/541/542 if you do not want 500/503 etc. silently accepted over it) and bump the array length to 19. All other checks pass for the layout above:
`chroma_bytes = 2*cw` (cw = W for 4:4:4) per row, luma bytes W per row, 16-byte alignment of plane offset/bpr (IOSurfaceAlignProperty >= 16), and the
surface's size must equal the texture's (987x1605 etc.). Caller must feed `chroma_offset = p1.offset` and `chroma_bytes_per_row = p1.bytesPerRow`
(not the top-level bytesPerRow 3968) in `IoSurfaceYCbCr`. Check SweetSurface / IOSurface plane base-address reporting returns the plane dict values, since the
top-level fields from the kernel are wrong for this format. Shader-side alternative (no m2v change): sample plane0 as R8Unorm (.r -> G) and plane1 as RG8Unorm
(.r -> B, .g -> R) and recombine as float4(p1.g, p0.r, p1.r, 1).
