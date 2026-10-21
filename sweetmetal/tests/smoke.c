/* sweetmetal smoke test: runtime_examples' scale_and_offset (output[i] = input[i]*scale + i)
 * through the C API. Usage: smoke runtime_examples.bc */
#include "sweetmetal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 1024

static int fail(const char *what)
{
	printf("FAIL %s: %s\n", what, sm_last_error());
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s KERNEL.bc\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	static uint8_t bc[1 << 20];
	size_t bclen = fread(bc, 1, sizeof bc, f);
	fclose(f);

	sm_device *dev = sm_device_create();
	if (!dev)
		return fail("device");
	char name[128];
	sm_device_name(dev, name, sizeof name);
	printf("device: %s\n", name);

	sm_library *lib = sm_library_new_with_air(dev, bc, bclen);
	if (!lib)
		return fail("library");
	sm_function *fn = sm_library_function(lib, "scale_and_offset");
	if (!fn)
		return fail("function");
	sm_compute_pipeline *pso = sm_compute_pipeline_new(dev, fn);
	if (!pso)
		return fail("pipeline");

	sm_buffer *in = sm_buffer_new(dev, N * 4), *out = sm_buffer_new(dev, N * 4),
		  *scale = sm_buffer_new(dev, 4);
	if (!in || !out || !scale)
		return fail("buffer");
	float *pin = (float *)sm_buffer_contents(in), *pout = (float *)sm_buffer_contents(out);
	for (int i = 0; i < N; i++) {
		pin[i] = (float)i;
		pout[i] = -1.0f;
	}
	*(float *)sm_buffer_contents(scale) = 2.5f;

	sm_queue *q = sm_queue_new(dev);
	sm_command_buffer *cb = q ? sm_command_buffer_new(q) : NULL;
	if (!cb)
		return fail("command buffer");
	sm_compute_encoder *enc = sm_compute_encoder_new(cb);
	if (!enc)
		return fail("encoder");
	if (!sm_compute_set_pipeline(enc, pso) || !sm_compute_set_buffer(enc, in, 0, 0) ||
	    !sm_compute_set_buffer(enc, out, 0, 1) || !sm_compute_set_buffer(enc, scale, 0, 2) ||
	    !sm_compute_dispatch_threads(enc, N, 1, 1, 64, 1, 1))
		return fail("encode");
	if (!sm_compute_end_encoding(enc))
		return fail("end encoding");
	if (!sm_command_buffer_commit(cb) || !sm_command_buffer_wait(cb))
		return fail("commit/wait");

	int bad = 0;
	for (int i = 0; i < N; i++)
		if (pout[i] != pin[i] * 2.5f + (float)i && bad++ < 4)
			printf("  [%d] = %g, want %g\n", i, pout[i], pin[i] * 2.5f + (float)i);
	printf("%d/%d correct\n", N - bad, N);

	sm_command_buffer_release(cb);
	sm_queue_release(q);
	sm_buffer_release(in);
	sm_buffer_release(out);
	sm_buffer_release(scale);
	sm_compute_pipeline_release(pso);
	sm_function_release(fn);
	sm_library_release(lib);
	sm_device_release(dev);
	printf(bad ? "SWEETMETAL FAIL\n" : "SWEETMETAL OK\n");
	return bad != 0;
}
