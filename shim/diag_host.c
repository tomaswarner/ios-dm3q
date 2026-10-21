/* Render the diag screen on the host from a real DTB:
 *   cc -DDIAG_HOST -o out/diag_host diag_host.c diag.c console.c
 *   ./out/diag_host DTB out/diag.ppm */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"

struct res { uint64_t a0, a1, a2, a3; };
void shim_main(uintptr_t fdt);

static uint32_t host_fb[FB_H * FB_STRIDE / 4];
static uint64_t ticks;

uint64_t host_rsr(const char *r)
{
	if (!strcmp(r, "cntfrq_el0"))
		return 19200000;
	if (!strcmp(r, "cntvct_el0"))
		return ticks += 4800000;	/* a quarter second per read */
	if (!strcmp(r, "CurrentEL"))
		return 1 << 2;
	return 0x1234;
}

uint32_t host_rd32(uintptr_t a)
{
	return (uint32_t)a;
}

struct res host_smccc(int hvc, uint64_t fn)
{
	return (struct res){ hvc ? 0xc1d58fcd : 0x10001, fn, 0, 0 };
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	FILE *f = fopen(argv[1], "rb");
	static unsigned char dtb[1 << 21];
	fread(dtb, 1, sizeof dtb, f);
	fclose(f);
	con_fb = host_fb;
	shim_main((uintptr_t)dtb);
	f = fopen(argv[2], "wb");
	fprintf(f, "P6 %d %d 255\n", FB_W, FB_H);
	for (int i = 0; i < FB_W * FB_H; i++) {
		unsigned char px[3] = { host_fb[i] >> 16, host_fb[i] >> 8, host_fb[i] };
		fwrite(px, 1, 3, f);
	}
	fclose(f);
	return 0;
}
