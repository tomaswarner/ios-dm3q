/* sweetmetal smoke test over the C API, with runtime_examples' shaders:
 *   compute: scale_and_offset (output[i] = input[i]*scale + i)
 *   render:  colored_vertex/colored_fragment, a triangle into an 8x8 render target, blit readback
 *   texture: invert_texels (compute, texture read/write), replaceRegion/getBytes
 * Usage: smoke [KERNEL.bc] (default: the phone's sweettest copy, so the
 * boot-time runner can start it without arguments) */
#include "sweetmetal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 1024
#define SIDE 8

/* Raw Metal values (Metal headers). */
#define MTLPixelFormatRGBA8Unorm 70
#define MTLTextureType2D 2
#define MTLTextureUsageShaderRead 1
#define MTLTextureUsageShaderWrite 2
#define MTLTextureUsageRenderTarget 4
#define MTLResourceStorageModePrivate (2 << 4)
#define MTLLoadActionClear 2
#define MTLStoreActionStore 1
#define MTLPrimitiveTypeTriangle 3
#define MTLColorWriteMaskAll 0xf

static int failed;

static void fail(const char *what)
{
	printf("FAIL %s: %s\n", what, sm_last_error());
	failed++;
}

static int test_compute(sm_device *dev, sm_library *lib, sm_queue *q)
{
	sm_function *fn = sm_library_function(lib, "scale_and_offset");
	sm_compute_pipeline *pso = fn ? sm_compute_pipeline_new(dev, fn) : NULL;
	if (!pso) {
		fail("compute pipeline");
		return 1;
	}
	sm_buffer *in = sm_buffer_new(dev, N * 4), *out = sm_buffer_new(dev, N * 4), *scale = sm_buffer_new(dev, 4);
	float *pin = (float *)sm_buffer_contents(in), *pout = (float *)sm_buffer_contents(out);
	for (int i = 0; i < N; i++) {
		pin[i] = (float)i;
		pout[i] = -1.0f;
	}
	*(float *)sm_buffer_contents(scale) = 2.5f;

	sm_command_buffer *cb = sm_command_buffer_new(q);
	sm_compute_encoder *enc = sm_compute_encoder_new(cb);
	if (!enc || !sm_compute_set_pipeline(enc, pso) || !sm_compute_set_buffer(enc, in, 0, 0) ||
	    !sm_compute_set_buffer(enc, out, 0, 1) || !sm_compute_set_buffer(enc, scale, 0, 2) ||
	    !sm_compute_dispatch_threads(enc, N, 1, 1, 64, 1, 1) || !sm_compute_end_encoding(enc) ||
	    !sm_command_buffer_commit(cb) || !sm_command_buffer_wait(cb)) {
		fail("compute encode/run");
		return 1;
	}
	int bad = 0;
	for (int i = 0; i < N; i++)
		if (pout[i] != pin[i] * 2.5f + (float)i && bad++ < 4)
			printf("  [%d] = %g, want %g\n", i, pout[i], pin[i] * 2.5f + (float)i);
	printf("compute: %d/%d correct\n", N - bad, N);
	sm_command_buffer_release(cb);
	sm_buffer_release(in);
	sm_buffer_release(out);
	sm_buffer_release(scale);
	sm_compute_pipeline_release(pso);
	sm_function_release(fn);
	return bad != 0;
}

static int test_render(sm_device *dev, sm_library *lib, sm_queue *q)
{
	sm_function *vf = sm_library_function(lib, "colored_vertex");
	sm_function *ff = sm_library_function(lib, "colored_fragment");
	if (!vf || !ff) {
		fail("render functions");
		return 1;
	}
	sm_render_pipeline_desc pd;
	memset(&pd, 0, sizeof(pd));
	pd.vertex = vf;
	pd.fragment = ff;
	pd.color[0].pixel_format = MTLPixelFormatRGBA8Unorm;
	pd.color[0].source_rgb_blend_factor = pd.color[0].source_alpha_blend_factor = 1;  /* One */
	pd.color[0].write_mask = MTLColorWriteMaskAll;
	sm_render_pipeline *rp = sm_render_pipeline_new(dev, &pd);
	if (!rp) {
		fail("render pipeline");
		return 1;
	}
	sm_texture_desc td = {
		.texture_type = MTLTextureType2D, .pixel_format = MTLPixelFormatRGBA8Unorm,
		.width = SIDE, .height = SIDE, .depth = 1, .mipmap_level_count = 1, .sample_count = 1,
		.array_length = 1, .usage = MTLTextureUsageRenderTarget,
		.resource_options = MTLResourceStorageModePrivate,
	};
	sm_texture *target = sm_texture_new(dev, &td);
	if (!target) {
		fail("render target");
		return 1;
	}
	/* The top-left quarter as two triangles (clip space, y up), red-orange; cleared blue. */
	const float pos[6][2] = { { -1, 1 }, { 0, 1 }, { -1, 0 }, { 0, 1 }, { 0, 0 }, { -1, 0 } };
	float col[6][4];
	for (int i = 0; i < 6; i++) {
		col[i][0] = 1.0f;
		col[i][1] = 0.2f;
		col[i][2] = 0.0f;
		col[i][3] = 1.0f;
	}
	sm_buffer *pb = sm_buffer_new(dev, sizeof(pos)), *cbuf = sm_buffer_new(dev, sizeof(col));
	memcpy(sm_buffer_contents(pb), pos, sizeof(pos));
	memcpy(sm_buffer_contents(cbuf), col, sizeof(col));
	sm_buffer *readback = sm_buffer_new(dev, SIDE * SIDE * 4);

	sm_render_pass_desc pass;
	memset(&pass, 0, sizeof(pass));
	pass.color[0].texture = target;
	pass.color[0].load_action = MTLLoadActionClear;
	pass.color[0].store_action = MTLStoreActionStore;
	pass.color[0].clear_color[2] = 1.0;
	pass.color[0].clear_color[3] = 1.0;

	sm_command_buffer *cb = sm_command_buffer_new(q);
	sm_render_encoder *re = sm_render_encoder_new(cb, &pass);
	if (!re || !sm_render_set_pipeline(re, rp) || !sm_render_set_vertex_buffer(re, pb, 0, 0) ||
	    !sm_render_set_vertex_buffer(re, cbuf, 0, 1) ||
	    !sm_render_draw(re, MTLPrimitiveTypeTriangle, 0, 6, 1, 0) || !sm_render_end_encoding(re)) {
		fail("render encode");
		return 1;
	}
	sm_blit_encoder *be = sm_blit_encoder_new(cb);
	if (!be || !sm_blit_texture_to_buffer(be, target, 0, 0, 0, 0, SIDE, SIDE, readback, 0, SIDE * 4, SIDE * SIDE * 4) ||
	    !sm_blit_end_encoding(be) || !sm_command_buffer_commit(cb) || !sm_command_buffer_wait(cb)) {
		fail("blit/run");
		return 1;
	}
	const uint8_t drawn[4] = { 255, 51, 0, 255 }, cleared[4] = { 0, 0, 255, 255 };
	const uint8_t *px = sm_buffer_contents(readback);
	int bad = 0;
	char rows[SIDE][SIDE + 1];
	for (int y = 0; y < SIDE; y++) {
		for (int x = 0; x < SIDE; x++) {
			int inside = x < SIDE / 2 && y < SIDE / 2;
			const uint8_t *p = px + (y * SIDE + x) * 4;
			int ok = !memcmp(p, inside ? drawn : cleared, 4);
			rows[y][x] = ok ? (inside ? '#' : '.') : 'X';
			if (!ok && bad++ < 4)
				printf("  texel (%d,%d) = %d %d %d %d\n", x, y, p[0], p[1], p[2], p[3]);
		}
		rows[y][SIDE] = 0;
	}
	for (int y = 0; y < SIDE; y++)
		printf("  %s\n", rows[y]);
	printf("render: %d/%d texels correct\n", SIDE * SIDE - bad, SIDE * SIDE);
	sm_command_buffer_release(cb);
	sm_buffer_release(pb);
	sm_buffer_release(cbuf);
	sm_buffer_release(readback);
	sm_texture_release(target);
	sm_render_pipeline_release(rp);
	sm_function_release(vf);
	sm_function_release(ff);
	return bad != 0;
}

static int test_texture(sm_device *dev, sm_library *lib, sm_queue *q)
{
	sm_function *fn = sm_library_function(lib, "invert_texels");
	sm_compute_pipeline *pso = fn ? sm_compute_pipeline_new(dev, fn) : NULL;
	if (!pso) {
		fail("invert pipeline");
		return 1;
	}
	sm_texture_desc td = {
		.texture_type = MTLTextureType2D, .pixel_format = MTLPixelFormatRGBA8Unorm,
		.width = SIDE, .height = SIDE, .depth = 1, .mipmap_level_count = 1, .sample_count = 1,
		.array_length = 1, .usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite,
		.resource_options = 0,   /* shared */
	};
	sm_texture *src = sm_texture_new(dev, &td), *dst = sm_texture_new(dev, &td);
	if (!src || !dst) {
		fail("textures");
		return 1;
	}
	uint8_t in[SIDE * SIDE * 4], out[SIDE * SIDE * 4];
	for (int i = 0; i < SIDE * SIDE; i++) {
		in[i * 4 + 0] = (uint8_t)(i * 4);
		in[i * 4 + 1] = (uint8_t)(255 - i);
		in[i * 4 + 2] = (uint8_t)(i * 3);
		in[i * 4 + 3] = (uint8_t)(100 + i);
	}
	memset(out, 0, sizeof(out));
	if (!sm_texture_replace_region(src, 0, 0, SIDE, SIDE, 0, 0, in, sizeof(in), SIDE * 4, 0)) {
		fail("replace_region");
		return 1;
	}
	sm_command_buffer *cb = sm_command_buffer_new(q);
	sm_compute_encoder *enc = sm_compute_encoder_new(cb);
	if (!enc || !sm_compute_set_pipeline(enc, pso) || !sm_compute_set_texture(enc, src, 0) ||
	    !sm_compute_set_texture(enc, dst, 1) || !sm_compute_dispatch_threads(enc, SIDE, SIDE, 1, 4, 4, 1) ||
	    !sm_compute_end_encoding(enc) || !sm_command_buffer_commit(cb) || !sm_command_buffer_wait(cb)) {
		fail("invert encode/run");
		return 1;
	}
	if (!sm_texture_get_bytes(dst, out, sizeof(out), SIDE * 4, 0, 0, 0, SIDE, SIDE, 0, 0)) {
		fail("get_bytes");
		return 1;
	}
	int bad = 0;
	for (int i = 0; i < SIDE * SIDE * 4; i++) {
		int want = (i % 4 == 3) ? in[i] : 255 - in[i];
		int d = out[i] - want;
		if ((d > 1 || d < -1) && bad++ < 4)
			printf("  byte %d = %d, want %d\n", i, out[i], want);
	}
	printf("texture: %d/%d bytes correct\n", SIDE * SIDE * 4 - bad, SIDE * SIDE * 4);
	sm_command_buffer_release(cb);
	sm_texture_release(src);
	sm_texture_release(dst);
	sm_compute_pipeline_release(pso);
	sm_function_release(fn);
	return bad != 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/usr/local/sweettest/runtime_examples.bc";
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 2;
	}
	static uint8_t bc[1 << 20];
	size_t bclen = fread(bc, 1, sizeof bc, f);
	fclose(f);

	sm_device *dev = sm_device_create();
	if (!dev) {
		fail("device");
		return 1;
	}
	char name[128];
	sm_device_name(dev, name, sizeof name);
	printf("device: %s\n", name);
	sm_library *lib = sm_library_new_with_air(dev, bc, bclen);
	sm_queue *q = lib ? sm_queue_new(dev) : NULL;
	if (!q) {
		fail("library/queue");
		return 1;
	}
	int bad = test_compute(dev, lib, q);
	bad += test_render(dev, lib, q);
	bad += test_texture(dev, lib, q);

	sm_queue_release(q);
	sm_library_release(lib);
	sm_device_release(dev);
	printf(bad || failed ? "SWEETMETAL FAIL\n" : "SWEETMETAL OK\n");
	return bad || failed;
}
