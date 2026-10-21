/*
 * DM3Q performance before xnu: nothing in the sweet kernel scales the CPU
 * clusters or the DDR, so iOS runs at whatever ABL left behind.
 *  - CPU: EPSS cpufreq (qcom,cpufreq-epss, 3 domains at 0x17d91000/2000/3000;
 *    Linux qcom-cpufreq-hw epss_soc_data): LUT at +0x100 (src [31:30],
 *    core count [18:16], L_VAL [7:0], f = 19.2 MHz * L_VAL, ends when the
 *    frequency repeats), current domain state +0x20, perf state index +0x320.
 *    Pick the fastest non-turbo row (core count != 1): iOS has no thermal
 *    driver, the hardware limits (LMh/DCVSH) still apply.
 *  - DDR/LLCC: RPMh BCM votes MC0 and SH0 at the top (the BCM clamps to its
 *    highest level) plus ACV (enable mask 0x8), as Linux icc-rpmh does.
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"
#include "perf.h"

#define EPSS(d)		(0x17d91000UL + 0x1000UL * (d))
#define EPSS_ENABLE	0x000
#define EPSS_DOMAIN	0x020
#define EPSS_LUT(i)	(0x100 + 4 * (i))
#define EPSS_PERF	0x320

static inline uint32_t rd(uintptr_t a)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}

static inline void wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(a) : "memory");
}

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void udelay(uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

static uint32_t mhz(uint32_t lut)
{
	return (lut & 0xff) * 192 / 10;
}

static void cpu_domain(unsigned d)
{
	uintptr_t b = EPSS(d);
	uint32_t en = rd(b + EPSS_ENABLE), cur = rd(b + EPSS_DOMAIN), idx = rd(b + EPSS_PERF);
	unsigned best = 0, n = 0;
	uint32_t prev = 0;
	for (unsigned i = 0; i < 40; i++) {
		uint32_t e = rd(b + EPSS_LUT(i)), f = mhz(e);
		if (!f || f == prev)
			break;
		n = i + 1;
		if (((e >> 16) & 7) != 1)	/* not a turbo row */
			best = i;
		prev = f;
	}
	con_hex2("CPU", "dom/en/idx=", d << 24 | (en & 0xff) << 16 | (idx & 0xffff), "cur/n MHz=", mhz(cur) << 16 | n);
	if (!(en & 1) || !n)
		return;
	wr(b + EPSS_PERF, best);
	udelay(2000);
	con_hex2("CPU", "set idx/MHz=", best << 16 | mhz(rd(b + EPSS_LUT(best))), "now MHz=", mhz(rd(b + EPSS_DOMAIN)));
}

static void ddr_votes(void)
{
	static const char *const name[3] = { "MC0", "SH0", "ACV" };
	uint32_t addr[3], data[3];
	int n = 0;
	for (int i = 0; i < 3; i++) {
		uint32_t a = rpmh_cmddb_addr(name[i]);
		if (!a)
			continue;
		uint32_t v = i == 2 ? 0x8u : 0x3fffu;	/* ACV one-hot enable mask; MC0/SH0 top */
		addr[n] = a;
		data[n] = 1u << 30 | 1u << 29 | v << 14 | v;	/* BCM_TCS_CMD(commit, valid, x, y) */
		n++;
	}
	int rc = n ? rpmh_write(addr, data, n) : -9;
	con_hex2("DDR BCM", "mc0 addr=", n ? addr[0] : 0, "n/rc=", (uint32_t)n << 8 | ((uint32_t)rc & 0xff));
}

void perf_preboot(void)
{
	(void)rpmh_version();
	if (rpmh_cmddb_ok())
		ddr_votes();
	for (unsigned d = 0; d < 3; d++)
		cpu_domain(d);
}
