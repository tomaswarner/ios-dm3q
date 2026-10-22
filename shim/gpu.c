/*
 * DM3Q (SM8550) Adreno 740, probe 1 ("gpu1", DIAG=gpu): CX power, GMU clocks,
 * CX-domain identification and a GMU TCM proof of life, with no GPU or GMU
 * execution. Design and sources: docs/dm3q/gpu/A740-POWERON.md (section 3,
 * steps S1-S10); register offsets from Linux gpucc-sm8550.c / gcc-sm8550.c /
 * a6xx_gmu.xml, addresses checked against the phone's DT.
 *
 * Rules: plain single-register LDR/STR only (Gunyah); never touch the GX
 * domain (0x03d00000-0x03d3ffff except GBIF_CX 0x03d0f000) or the GX GDSC; no
 * PLLs (every GPU_CC RCG stays on XO 19.2 MHz: cold); "NEXT:" printed before
 * each risky step so a reset photo names the culprit; every poll has a
 * timeout. The GMU stays in reset (it cannot run from TCM alone).
 */
#include <stdint.h>
#include "console.h"
#include "rpmh.h"
#include "gpu.h"

#define GCC		0x00100000UL
#define GPUCC		0x03d90000UL
#define CMDDB_BASE	0x81c60000UL

/* GCC GPU branches [Linux gcc-sm8550.c] */
#define GCC_GPU_CFG_AHB		(GCC + 0x71004)
#define GCC_QMIP_GPU_AHB	(GCC + 0x71008)
#define GCC_GPU_MEMNOC_GFX	(GCC + 0x71010)
#define GCC_GPU_SNOC_DVM_GFX	(GCC + 0x71018)
#define GCC_DDRSS_GPU_AXI	(GCC + 0x71154)

/* GPU_CC [Linux gpucc-sm8550.c] */
#define GPUCC_CXO_AON		(GPUCC + 0x9004)
#define GPUCC_DEMET		(GPUCC + 0x900c)
#define GPUCC_XO_CMD		(GPUCC + 0x9010)
#define GPUCC_XO_CFG		(GPUCC + 0x9014)
#define GPUCC_GX_GDSCR		(GPUCC + 0x905c)	/* read only: never touched */
#define GPUCC_CX_GDSCR		(GPUCC + 0x9108)
#define GPUCC_AHB		(GPUCC + 0x911c)
#define GPUCC_SLEEP		(GPUCC + 0x9134)
#define GPUCC_CX_GMU		(GPUCC + 0x913c)
#define GPUCC_CXO		(GPUCC + 0x9144)
#define GPUCC_HUB_CX_INT	(GPUCC + 0x9148)
#define GPUCC_MEMNOC_GFX	(GPUCC + 0x9150)
#define GPUCC_GMU_CMD		(GPUCC + 0x9318)
#define GPUCC_GMU_CFG		(GPUCC + 0x931c)
#define GPUCC_HUB_AON		(GPUCC + 0x93e8)
#define GPUCC_HUB_CMD		(GPUCC + 0x93ec)
#define GPUCC_HUB_CFG		(GPUCC + 0x93f0)
#define GPUCC_CX_GDSC_HW	(GPUCC + 0x953c)
#define GPUCC_HLOS1_SMMU	(GPUCC + 0x7000)

/* GMU (byte = 0x03d00000 + dword << 2) [Linux a6xx_gmu.xml] */
#define GMU_ITCM		0x03d6d000UL	/* GMU VA 0x00000000, 16 KiB */
#define GMU_DTCM		0x03d71000UL	/* GMU VA 0x10004000, 16 KiB */
#define GMU_FW_VERSION		0x03d74fe0UL	/* DTCM VA 0x10007fe0 */

extern const uint8_t _gmu_fw_start[], _gmu_fw_end[];

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
	uint64_t t0 = ticks();
	while (ticks() - t0 < (uint64_t)us * 192 / 10)
		;
}

/* Poll until (rd(a) & mask) == want, at most us microseconds; returns the elapsed us or ~0u. */
static uint32_t poll(uintptr_t a, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t t0 = ticks(), lim = (uint64_t)us * 192 / 10;
	for (;;) {
		uint64_t dt = ticks() - t0;
		if ((rd(a) & mask) == want)
			return (uint32_t)(dt * 10 / 192);
		if (dt > lim)
			return ~0u;
	}
}

static int failed;
static char results[12];	/* per step: 'o' ok, 'X' fail, '-' skipped */

static void step(int n, int ok)
{
	results[n] = ok ? 'o' : 'X';
	failed += !ok;
}

/* ---- S1: cmd-db ARC resources and their level tables ------------------------------- */

struct arc {
	char name[8];	/* inline: no pointers in data (no ABS64 relocations) */
	uint32_t addr;
	uint16_t level[16];
	int n;
};

static struct arc arcs[] = { { "cx.lvl" }, { "mx.lvl" }, { "mxc.lvl" }, { "gfx.lvl" } };

/* entry_header {id[8], prio[2], addr, len u16, offset u16}; aux data at data + rsc.data_offset + entry.offset */
static void arc_table(struct arc *a)
{
	const uintptr_t data = CMDDB_BASE + 8 + 8 * 16 + 8;
	for (int i = 0; i < 8; i++) {
		uintptr_t h = CMDDB_BASE + 8 + 16 * i;
		uint32_t w0 = rd(h), w1 = rd(h + 4);
		uint32_t slv = w0 & 0xffff, hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		if (!slv)
			continue;
		for (uint32_t j = 0; j < cnt; j++) {
			uintptr_t e = data + hoff + 24 * j;
			volatile const char *id = (volatile const char *)e;
			int match = 1, end = 0;
			for (int k = 0; k < 8 && match; k++) {
				char c = end ? 0 : a->name[k];
				end |= !c;
				match = id[k] == c;
			}
			if (!match)
				continue;
			a->addr = rd(e + 16);
			uint32_t lo = rd(e + 20), len = lo & 0xffff, off = lo >> 16;
			uintptr_t aux = data + doff + off;
			a->n = (int)(len / 2) > 16 ? 16 : (int)(len / 2);
			for (int k = 0; k < a->n; k++) {
				uint32_t w = rd((aux + 2 * k) & ~3UL);
				a->level[k] = (uint16_t)(((aux + 2 * k) & 2) ? w >> 16 : w & 0xffff);
			}
			return;
		}
	}
}

static void s1_cmddb(void)
{
	int ok = rpmh_cmddb_ok();
	con_hex2("S1 CMDDB", "ok=", ok, "drv ver=", rpmh_version());
	for (unsigned i = 0; ok && i < sizeof arcs / sizeof arcs[0]; i++) {
		struct arc *a = &arcs[i];
		arc_table(a);
		/* levels as 2 x 16-bit per word, up to 8 shown: L0L1 L2L3 ... */
		con_hex2(a->name, "addr=", a->addr, "n=", (uint32_t)a->n);
		con_hex2(a->name, "lv01=", (uint32_t)a->level[0] << 16 | a->level[1], "lv23=", (uint32_t)a->level[2] << 16 | a->level[3]);
		con_hex2(a->name, "lv45=", (uint32_t)a->level[4] << 16 | a->level[5], "lv67=", (uint32_t)a->level[6] << 16 | a->level[7]);
		con_hex2(a->name, "lv89=", (uint32_t)a->level[8] << 16 | a->level[9], "lvAB=", (uint32_t)a->level[10] << 16 | a->level[11]);
	}
	step(1, ok && arcs[0].addr && arcs[1].addr);
}

/* ---- S2: GCC GPU branches before we touch them --------------------------------------- */

static const uintptr_t gcc_cbcr[] = { GCC_GPU_CFG_AHB, GCC_QMIP_GPU_AHB, GCC_DDRSS_GPU_AXI, GCC_GPU_MEMNOC_GFX, GCC_GPU_SNOC_DVM_GFX };

static void s2_gcc_state(void)
{
	con_hex2("S2 GCC", "cfgahb=", rd(GCC_GPU_CFG_AHB), "qmip=", rd(GCC_QMIP_GPU_AHB));
	con_hex2("S2 GCC", "axi=", rd(GCC_DDRSS_GPU_AXI), "memnoc=", rd(GCC_GPU_MEMNOC_GFX));
	con_hex2("S2 GCC", "snocdvm=", rd(GCC_GPU_SNOC_DVM_GFX), "vote=", rd(GCC + 0x52000));
	step(2, 1);
}

/* ---- S3: RPMh ARC votes (data = index into the level table) ------------------------ */

static int arc_index(const struct arc *a, uint32_t target)
{
	for (int k = 0; k < a->n; k++)
		if (a->level[k] >= target && a->level[k] != 16)
			return k;
	return a->n ? a->n - 1 : -1;
}

static void s3_arc_votes(void)
{
	static const uint32_t target[] = { 256, 128, 128 };	/* cx NOM, mx SVS, mxc SVS; gfx.lvl is the GMU's */
	int ok = 1;
	con_str("NEXT", "S3 ARC votes");
	for (int i = 0; i < 3; i++) {
		const struct arc *a = &arcs[i];
		int idx = a->addr ? arc_index(a, target[i]) : -1;
		if (idx < 0) {
			con_str(a->name, "no table: vote skipped");
			ok = 0;
			continue;
		}
		uint32_t addr = a->addr, data = (uint32_t)idx;
		int rc = rpmh_write(&addr, &data, 1);
		con_hex2(a->name, "idx/level=", (uint32_t)idx << 16 | a->level[idx], "rc=", (uint32_t)rc);
		ok &= rc == 0;
	}
	step(3, ok);
}

/* ---- S4: GCC GPU branches on --------------------------------------------------------- */

static void s4_gcc_on(void)
{
	int ok = 1;
	con_str("NEXT", "S4 GCC GPU branches");
	for (unsigned i = 0; i < sizeof gcc_cbcr / sizeof gcc_cbcr[0]; i++) {
		uintptr_t c = gcc_cbcr[i];
		wr(c, rd(c) | 1u);
		uint32_t us = c == GCC_DDRSS_GPU_AXI ? 0 : poll(c, 1u << 31, 0, 1000);	/* AXI: BRANCH_HALT_SKIP */
		if (c != GCC_GPU_SNOC_DVM_GFX && us == ~0u)	/* SNOC_DVM: HALT_DELAY, a stuck OFF bit is tolerated */
			ok = 0;
		con_hex2("S4 CBCR", "addr=", (uint32_t)c, "val=", rd(c));
	}
	step(4, ok);
}

/* ---- S5: GPU_CC baseline (first GPU_CC access) --------------------------------------- */

static void s5_gpucc_state(void)
{
	con_str("NEXT", "S5 gpucc read");
	con_hex2("S5 GDSC", "cx=", rd(GPUCC_CX_GDSCR), "gx=", rd(GPUCC_GX_GDSCR));
	con_hex2("S5 GDSC", "cxhw=", rd(GPUCC_CX_GDSC_HW), "pll1 l=", rd(GPUCC + 0x1004));
	con_hex2("S5 PLL0", "mode=", rd(GPUCC + 0x0), "l=", rd(GPUCC + 0x4));
	con_hex2("S5 PLL1", "mode=", rd(GPUCC + 0x1000), "user=", rd(GPUCC + 0x1018));
	con_hex2("S5 GMU RCG", "cmd=", rd(GPUCC_GMU_CMD), "cfg=", rd(GPUCC_GMU_CFG));
	con_hex2("S5 HUB RCG", "cmd=", rd(GPUCC_HUB_CMD), "cfg=", rd(GPUCC_HUB_CFG));
	con_hex2("S5 XO RCG", "cmd=", rd(GPUCC_XO_CMD), "cfg=", rd(GPUCC_XO_CFG));
	con_hex2("S5 CBCR", "ahb=", rd(GPUCC_AHB), "cxgmu=", rd(GPUCC_CX_GMU));
	con_hex2("S5 CBCR", "cxo=", rd(GPUCC_CXO), "hubcx=", rd(GPUCC_HUB_CX_INT));
	con_hex2("S5 CBCR", "demet=", rd(GPUCC_DEMET), "cxoaon=", rd(GPUCC_CXO_AON));
	con_hex2("S5 CBCR", "hubaon=", rd(GPUCC_HUB_AON), "smmu=", rd(GPUCC_HLOS1_SMMU));
	step(5, 1);
}

/* ---- S6: always-on branches + CX GDSC on --------------------------------------------- */

static void s6_cx_gdsc(void)
{
	int ok = 1;
	con_str("NEXT", "S6 CX GDSC on");
	wr(GPUCC_CXO_AON, rd(GPUCC_CXO_AON) | 1u);
	wr(GPUCC_DEMET, rd(GPUCC_DEMET) | 1u);	/* the A7xx GMU's DEMET clock */
	uint32_t t1 = poll(GPUCC_CXO_AON, 1u << 31, 0, 1000), t2 = poll(GPUCC_DEMET, 1u << 31, 0, 1000);
	con_hex2("S6 AON", "cxoaon=", rd(GPUCC_CXO_AON), "demet=", rd(GPUCC_DEMET));
	ok &= t1 != ~0u && t2 != ~0u;
	/* gdsc_init: clear HW_CONTROL (1) and SW_OVERRIDE (2); EN_REST 2, EN_FEW 2, CLK_DIS 0xf; RETAIN_FF (11) */
	uint32_t g = rd(GPUCC_CX_GDSCR);
	g &= ~((1u << 1) | (1u << 2) | (0xfu << 20) | (0xfu << 16) | (0xfu << 12));
	g |= (2u << 20) | (2u << 16) | (0xfu << 12) | (1u << 11);
	wr(GPUCC_CX_GDSCR, g);
	wr(GPUCC_CX_GDSCR, g & ~1u);	/* SW_COLLAPSE off: power on */
	udelay(1);
	uint32_t us = poll(GPUCC_CX_GDSCR, 1u << 31, 1u << 31, 1500);
	con_hex2("S6 CX GDSC", "gdscr=", rd(GPUCC_CX_GDSCR), "us=", us);
	con_hex("S6 GDSC hw", rd(GPUCC_CX_GDSC_HW));
	ok &= us != ~0u;
	step(6, ok);
}

/* ---- S7: GMU / GPU_CC branches (all on XO) ------------------------------------------- */

static void s7_gmu_clocks(void)
{
	static const uintptr_t c[] = { GPUCC_CXO, GPUCC_AHB, GPUCC_HUB_CX_INT, GPUCC_CX_GMU, GPUCC_HUB_AON,
				       GPUCC_HLOS1_SMMU, GPUCC_MEMNOC_GFX };
	int ok = 1;
	con_str("NEXT", "S7 GMU clocks");
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		wr(c[i], rd(c[i]) | 1u);
		uint32_t us = poll(c[i], 1u << 31, 0, 1000);
		if (us == ~0u && c[i] != GPUCC_MEMNOC_GFX && c[i] != GPUCC_HLOS1_SMMU)	/* voted branches may lag */
			ok = 0;
		con_hex2("S7 CBCR", "addr=", (uint32_t)c[i], "val=", rd(c[i]));
	}
	con_hex2("S7 RCG", "gmu cfg=", rd(GPUCC_GMU_CFG), "hub cfg=", rd(GPUCC_HUB_CFG));
	step(7, ok);
}

/* ---- S8: CX-domain identification (no GMU execution) --------------------------------- */

static void s8_ident(void)
{
	con_str("NEXT", "S8 GMU window");
	con_hex2("S8 CM3", "sysreset=", rd(0x03d7e000), "bootcfg=", rd(0x03d7e004));
	con_hex2("S8 CM3", "fwbusy=", rd(0x03d7e068), "initres=", rd(0x03d7e070));
	con_hex2("S8 CM3", "cfg=", rd(0x03d7e0b4), "hfistat=", rd(0x03d7e600));
	con_hex2("S8 CACHE", "icache=", rd(0x03d7d000), "dcache=", rd(0x03d7d004));
	con_hex2("S8 GMU", "g2h=", rd(0x03d7e648), "rpmhst=", rd(0x03d7e3b0));
	con_hex2("S8 GEN", "g0=", rd(0x03d7e714), "g1=", rd(0x03d7e718));
	con_hex2("S8 GEN", "g10=", rd(0x03d7e73c), "g11=", rd(0x03d7e740));
	con_hex2("S8 AO", "cxbusy=", rd(0x03d8ec30), "hostint=", rd(0x03d8ec14));
	con_hex2("S8 AO", "fence=", rd(0x03d8ec40), "rscc req=", rd(0x03d8ec1c));
	con_str("NEXT", "S8 cx_misc/gbif");
	con_hex2("S8 CXMISC", "cache0=", rd(0x03d9e004), "tcmret=", rd(0x03d9e0e4));
	con_hex2("S8 GBIF", "cfg=", rd(0x03d0f000), "side0=", rd(0x03d0f00c));
	con_hex("S8 GBIF halt", rd(0x03d0f114));
	con_str("NEXT", "S8 rscc/pdc");
	con_hex2("S8 RSCC", "status0=", rd(0x03d50010), "seqbusy=", rd(0x03d50404));
	con_hex2("S8 PDC", "gpuen=", rd(0x0b284500), "rscc slv=", rd(0x03d5002c));
	step(8, 1);
}

/* ---- S9: GMU TCM load from the phone's gmu_gen70200.bin + firmware version ------------ */

static void s9_tcm(void)
{
	con_str("NEXT", "S9 TCM");
	/* pattern test at the top of each TCM */
	wr(GMU_ITCM + 0x3ff0, 0xa5a5a5a5u);
	wr(GMU_DTCM + 0x3f00, 0x5a5a5a5au);
	uint32_t p1 = rd(GMU_ITCM + 0x3ff0), p2 = rd(GMU_DTCM + 0x3f00);
	wr(GMU_ITCM + 0x3ff0, 0);
	wr(GMU_DTCM + 0x3f00, 0);
	con_hex2("S9 pattern", "itcm=", p1, "dtcm=", p2);
	/* blocks {addr, size, type, value} + size bytes; load the TCM-resident data blocks */
	const uint8_t *f = _gmu_fw_start, *end = _gmu_fw_end;
	uint32_t blocks = 0, loaded = 0, words = 0, bad = 0, first_bad = 0;
	while (f + 16 <= end) {
		uint32_t addr = f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24;
		uint32_t size = f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24;
		uint32_t type = f[8] | f[9] << 8 | f[10] << 16 | (uint32_t)f[11] << 24;
		const uint8_t *body = f + 16;
		if (body + size > end)
			break;
		blocks++;
		uintptr_t dst = 0;
		/* Linux a6xx_gmu_fw_load loads every block with a size (data blocks carry type 0xdeadbeef
		 * in this file; the zero-size ones are PREALLOC requests) */
		(void)type;
		if (size && addr + size <= 0x4000)
			dst = GMU_ITCM + addr;
		else if (size && addr >= 0x10004000u && addr + size <= 0x10008000u)
			dst = GMU_DTCM + (addr - 0x10004000u);
		if (dst) {
			loaded++;
			for (uint32_t o = 0; o + 4 <= size; o += 4) {
				uint32_t w = body[o] | body[o + 1] << 8 | body[o + 2] << 16 | (uint32_t)body[o + 3] << 24;
				wr(dst + o, w);
				words++;
			}
			for (uint32_t o = 0; o + 4 <= size; o += 4) {
				uint32_t w = body[o] | body[o + 1] << 8 | body[o + 2] << 16 | (uint32_t)body[o + 3] << 24;
				if (rd(dst + o) != w && bad++ == 0)
					first_bad = (uint32_t)(dst + o);
			}
		}
		f = body + size;
	}
	con_hex2("S9 fw", "blocks=", blocks, "tcm blocks=", loaded);
	con_hex2("S9 TCM", "words=", words, "mismatch=", bad);
	if (bad)
		con_hex("S9 first bad", first_bad);
	con_hex2("S9 ITCM", "sp=", rd(GMU_ITCM), "reset=", rd(GMU_ITCM + 4));
	uint32_t ver = rd(GMU_FW_VERSION);
	con_hex2("S9 FW VER", "core=", ver, "pwr=", rd(GMU_FW_VERSION + 8));
	con_hex("S9 HFI VER", rd(GMU_FW_VERSION + 0x10));
	step(9, p1 == 0xa5a5a5a5u && p2 == 0x5a5a5a5au && loaded >= 2 && !bad && ver == 0x40010009u);
}

/* ---- S10: TZ (SCM) reachability for the zap shader later (read-only calls) ----------- */

struct smc_res {
	uint64_t a0, a1, a2, a3;
};

static struct smc_res scm(uint64_t fn, uint64_t arginfo, uint64_t a2)
{
	register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = 0, x4 __asm__("x4") = 0, x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
			 :
			 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return (struct smc_res){ x0, x1, x2, x3 };
}

static void s10_scm(void)
{
	con_str("NEXT", "S10 SCM");
	/* IS_CALL_AVAIL (svc 6 cmd 1): x2 = (owner 2 << 24) | svc << 8 | cmd of the probed call */
	struct smc_res r1 = scm(0x42000601, 1, 0x02000205);	/* PAS AUTH_AND_RESET */
	struct smc_res r2 = scm(0x42000601, 1, 0x02000201);	/* PAS INIT_IMAGE */
	struct smc_res r3 = scm(0x42000207, 1, 13);		/* PAS_IS_SUPPORTED(GPU 13) */
	struct smc_res r4 = scm(0x42000601, 1, 0x02002801);	/* GPU INIT_REGS */
	con_hex2("S10 avail auth", "x0=", (uint32_t)r1.a0, "x1=", (uint32_t)r1.a1);
	con_hex2("S10 avail init", "x0=", (uint32_t)r2.a0, "x1=", (uint32_t)r2.a1);
	con_hex2("S10 pas13", "x0=", (uint32_t)r3.a0, "x1=", (uint32_t)r3.a1);
	con_hex2("S10 avail gpuregs", "x0=", (uint32_t)r4.a0, "x1=", (uint32_t)r4.a1);
	/* calibrate PAS_IS_SUPPORTED on peripherals Android loads through PAS: adsp 1, modem 4, cdsp 18 */
	struct smc_res p1 = scm(0x42000207, 1, 1), p4 = scm(0x42000207, 1, 4), p18 = scm(0x42000207, 1, 18);
	con_hex2("S10 pas1/pas4", "x1=", (uint32_t)p1.a1, "x1=", (uint32_t)p4.a1);
	con_hex2("S10 pas18", "x0=", (uint32_t)p18.a0, "x1=", (uint32_t)p18.a1);
	step(10, r1.a0 == 0 && r3.a0 == 0 && r3.a1 == 1);
}

void gpu_probe(void)
{
	for (int i = 0; i < 12; i++)
		results[i] = '-';
	con_str("GPU1", "A740 probe 1: CX power + GMU TCM (no GPU execution)");
	s1_cmddb();
	s2_gcc_state();
	s3_arc_votes();
	s4_gcc_on();
	s5_gpucc_state();
	s6_cx_gdsc();
	s7_gmu_clocks();
	/* the console clips at the bottom (probe 1 ran off it after S7): page 2 for S8-S10 */
	{
		char l1[16] = "S1-7 ";
		for (int i = 1; i <= 7; i++)
			l1[4 + i] = results[i];
		l1[12] = 0;
		con_clear(BLUE);	/* page 2 */
		con_str("GPU1 page 2", "S1-7 on the previous page");
		con_str("GPU1 S1-7", l1);
		con_hex2("CX GDSC", "gdscr=", rd(GPUCC_CX_GDSCR), "cxgmu=", rd(GPUCC_CX_GMU));
	}
	s8_ident();
	s9_tcm();
	s10_scm();
	char line[24] = "S1-10 ";
	for (int i = 1; i <= 10; i++)
		line[5 + i] = results[i];
	line[16] = 0;
	con_str("GPU1 RESULT", line);
	con_hex("GPU1 failed", (uint32_t)failed);
}

/* ===================================================================================== *
 * Probe 2 ("gpu2", DIAG=gpu2): kgsl-smmu stage-1 context for the GMU (stream 5), the
 * firmware's DDR blocks in a carve-out of our own, GMU release, FW_INIT_RESULT and HFI.
 * Design: docs/dm3q/gpu/A740-POWERON.md section 4 (P2.1-P2.3).
 * ===================================================================================== */

#define SMMU		0x03da0000UL	/* kgsl-smmu (qcom,qsmmu-v500, MMU-500) */
#define SMMU_CR0	0x000
#define SMMU_IDR0	0x020
#define SMMU_IDR1	0x024
#define SMMU_IDR2	0x028
#define SMMU_IDR7	0x03c
#define SMMU_GFSR	0x048
#define SMMU_SMR(n)	(0x800 + 4 * (n))
#define SMMU_S2CR(n)	(0xc00 + 4 * (n))
#define GR1_CBAR(n)	(0x000 + 4 * (n))
#define GR1_CBA2R(n)	(0x800 + 4 * (n))
#define CB_SCTLR	0x000
#define CB_ACTLR	0x004
#define CB_TCR2		0x010
#define CB_TTBR0	0x020
#define CB_TCR		0x030
#define CB_MAIR0	0x038
#define CB_MAIR1	0x03c
#define CB_FSR		0x058
#define CB_FAR		0x060
#define CB_FSYNR0	0x068
#define CB_TLBIALL	0x618
#define CB_TLBSYNC	0x7f0
#define CB_TLBSTATUS	0x7f4

#define GMU_SID		5
#define CARVE		0x900000000UL	/* free while no xnu runs: upper DDR bank */
#define CARVE_SIZE	0x100000UL	/* 1 MiB: tables + firmware + regions */

/* GMU VAs (firmware PREALLOC requests + log + HFI) */
#define VA_CODE		0x00004000UL
#define VA_LOG		0x60100000UL
#define VA_HFI		0x60180000UL
#define LOG_SIZE	0x10000UL
#define HFI_SIZE	0x4000UL

static uintptr_t pgsz, cb_base, gr1;
static uint32_t nums2cb, oas;
static uint64_t carve_next;
static uint64_t l1, l2[2];

static void wr64(uintptr_t a, uint64_t v)
{
	__asm__ volatile("str %0, [%1]" : : "r"(v), "r"(a) : "memory");
}

static uint64_t rd64(uintptr_t a)
{
	uint64_t v;
	__asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}

static uint64_t carve_page(void)
{
	uint64_t p = carve_next;
	if (p + 0x1000 > CARVE + CARVE_SIZE) {
		con_str("GPU2", "carve overflow: stop");
		for (;;)
			__asm__ volatile("wfe");
	}
	carve_next += 0x1000;
	for (uint64_t o = 0; o < 0x1000; o += 8)
		wr64(p + o, 0);
	return p;
}

/* L3 table for va (4 KiB granule, 39-bit VA: L1 1 GiB, L2 2 MiB, L3 4 KiB) */
static uint64_t l3_for(uint64_t va)
{
	unsigned i1 = (va >> 30) & 0x1ff, i2 = (va >> 21) & 0x1ff;
	uint64_t t2 = l2[i1 & 1];
	if (!t2) {
		t2 = l2[i1 & 1] = carve_page();
		wr64(l1 + 8 * i1, t2 | 3);
	}
	uint64_t d = rd64(t2 + 8 * i2);
	if (!(d & 1)) {
		d = carve_page() | 3;
		wr64(t2 + 8 * i2, d);
	}
	return d & 0xfffffffff000ULL;
}

/* map [va, va+size) to fresh zeroed, physically consecutive pages; returns the first PA. The
 * L2/L3 tables are made first so no table page lands between the data pages (review finding 1). */
static uint64_t map(uint64_t va, uint64_t size)
{
	for (uint64_t o = 0; o < size; o += 0x1000)
		(void)l3_for(va + o);
	uint64_t pa0 = carve_next;
	for (uint64_t o = 0; o < size; o += 0x1000) {
		uint64_t pa = carve_page();
		uint64_t l3 = l3_for(va + o);
		/* page: AF, inner shareable, AP 00 (RW privileged only), AttrIndx 0 (MAIR0 attr0). Not AP 01:
		 * a page writable unprivileged is implicitly PXN, and the GMU fetches privileged (gpu2 photo:
		 * FSR 0x408 permission fault, FAR 0x4000, FSYNR0 0x63 = privileged instruction fetch, level 3).
		 * Linux/KGSL map these regions IOMMU_PRIV. */
		wr64(l3 + 8 * (((va + o) >> 12) & 0x1ff), pa | (1u << 10) | (3u << 8) | 3);
	}
	return pa0;
}

static void copy_words(uint64_t dst, const uint8_t *src, uint32_t size)
{
	for (uint32_t o = 0; o + 4 <= size; o += 4)
		wr(dst + o, src[o] | src[o + 1] << 8 | src[o + 2] << 16 | (uint32_t)src[o + 3] << 24);
}

static int smmu_survey(uint32_t *nsmr, uint32_t *ncb)
{
	con_str("NEXT", "P1 smmu read");
	uint32_t cr0 = rd(SMMU + SMMU_CR0), idr0 = rd(SMMU + SMMU_IDR0), idr1 = rd(SMMU + SMMU_IDR1);
	uint32_t idr2 = rd(SMMU + SMMU_IDR2);
	con_hex2("P1 SMMU", "cr0=", cr0, "gfsr=", rd(SMMU + SMMU_GFSR));
	con_hex2("P1 SMMU", "idr0=", idr0, "idr1=", idr1);
	con_hex2("P1 SMMU", "idr2=", idr2, "idr7=", rd(SMMU + SMMU_IDR7));
	pgsz = (idr1 >> 31) ? 0x10000 : 0x1000;
	uintptr_t npage = 1u << (((idr1 >> 28) & 7) + 1);
	gr1 = SMMU + pgsz;
	cb_base = SMMU + npage * pgsz;
	*nsmr = idr0 & 0xff;
	*ncb = idr1 & 0xff;
	con_hex2("P1 SMMU", "nsmr=", *nsmr, "ncb=", *ncb);
	nums2cb = (idr1 >> 16) & 0xff;
	oas = (idr2 >> 4) & 0xf;
	con_hex2("P1 caps", "s1ts/sms/4k=", ((idr0 >> 30) & 1) << 8 | ((idr0 >> 27) & 1) << 4 | ((idr2 >> 12) & 1), "nums2cb/oas=", nums2cb << 16 | oas);
	if (!((idr0 >> 30) & 1) || !((idr2 >> 12) & 1)) {
		con_str("P1", "no stage 1 or no 4K granule");
		return 0;
	}
	con_hex2("P1 SMMU", "pgsz=", (uint32_t)pgsz, "cbbase=", (uint32_t)cb_base);
	/* valid SMRs, compactly: index<<16 | id, with S2CR */
	unsigned shown = 0;
	for (uint32_t n = 0; n < *nsmr && n < 128; n++) {
		uint32_t smr = rd(SMMU + SMMU_SMR(n));
		if (!(smr & (1u << 31)))
			continue;
		if (shown++ < 10)
			con_hex2("P1 SMR", "n/smr=", n << 24 | (smr & 0xffffff), "s2cr=", rd(SMMU + SMMU_S2CR(n)));
	}
	con_hex("P1 valid SMRs", shown);
	for (uint32_t c = 0; c < *ncb && c < 4; c++)
		con_hex2("P1 CB", "sctlr=", rd(cb_base + c * pgsz + CB_SCTLR), "fsr=", rd(cb_base + c * pgsz + CB_FSR));
	return *nsmr && *ncb && cb_base + *ncb * pgsz <= SMMU + 0x40000;
}

static void hfi_table(uint64_t pa)
{
	/* a6xx_hfi.h: table {version, size(dwords), qhdr0_offset, qhdr_size, num_queues, active_queues}
	 * then queue headers {status, iova, type, size, msg_size, dropped, rx_wm, tx_wm, rx_req, tx_req,
	 * read_index, write_index}; queues (4 KiB) after the table page */
	static const uint32_t hdr[6] = { 0, 6 + 3 * 12, 6, 12, 3, 3 };
	for (int i = 0; i < 6; i++)
		wr(pa + 4 * i, hdr[i]);
	for (uint32_t q = 0; q < 3; q++) {
		uint64_t h = pa + 24 + 48 * q;
		uint32_t v[12] = { 1, (uint32_t)(VA_HFI + 0x1000 * (q + 1)), (10u << 8) | q, 0x400, 0, 0, 1, 1, 1, 0, 0, 0 };
		for (int i = 0; i < 12; i++)
			wr(h + 4 * i, v[i]);
	}
}

/* ---- probe 3 (DIAG=gpu3): HFI messages after HFI_CTRL_INIT (docs/dm3q/gpu/A740-STAGES-3PLUS.md s3) ---- */

#define HOST2GMU_INTR_SET	0x03d7e650UL
#define GMU2HOST_INTR_CLR	0x03d7e644UL
#define GMU2HOST_INTR_INFO	0x03d7e648UL
#define QSIZE			0x400u		/* dwords per queue */

static uint64_t hfi_pa;
static uint32_t hfi_seq;

static uintptr_t qhdr(int q)
{
	return hfi_pa + 24 + 48 * q;	/* {status, iova, type, size, msg_size, dropped, rx_wm, tx_wm, rx_req, tx_req, read, write} */
}

static uintptr_t qdata(int q)
{
	return hfi_pa + 0x1000 * (q + 1);
}

/* send one command (msg[0] = header filled here) and wait for its ack; returns the ack's error word, or ~0u */
static uint32_t hfi_send(uint32_t id, uint32_t *msg, uint32_t dwords, uint32_t *ack_payload0)
{
	uint32_t seq = ++hfi_seq & 0xfff;
	msg[0] = seq << 20 | 0u << 16 | dwords << 8 | id;
	uint32_t w = rd(qhdr(0) + 44), aligned = (dwords + 3) & ~3u;
	for (uint32_t i = 0; i < aligned; i++)
		wr(qdata(0) + 4 * ((w + i) % QSIZE), i < dwords ? msg[i] : 0xfafafafau);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(qhdr(0) + 44, (w + aligned) % QSIZE);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(HOST2GMU_INTR_SET, 1);
	/* wait for the ack (id 126) carrying our header; drain anything else */
	uint64_t t0 = ticks();
	while (ticks() - t0 < 19200000ULL) {	/* 1 s */
		uint32_t r = rd(qhdr(1) + 40), wi = rd(qhdr(1) + 44);
		if (r == wi) {
			if (rd(GMU2HOST_INTR_INFO) & (1u << 23)) {
				con_hex2("HFI", "CM3 FAULT info=", rd(GMU2HOST_INTR_INFO), "initres=", rd(0x03d7e070));
				return ~0u;
			}
			continue;
		}
		uint32_t h = rd(qdata(1) + 4 * r), sz = (h >> 8) & 0xff, mid = h & 0xff;
		uint32_t d1 = rd(qdata(1) + 4 * ((r + 1) % QSIZE)), d2 = rd(qdata(1) + 4 * ((r + 2) % QSIZE));
		uint32_t d3 = rd(qdata(1) + 4 * ((r + 3) % QSIZE));
		wr(qhdr(1) + 40, ((r + sz + 3) & ~3u) % QSIZE);
		wr(GMU2HOST_INTR_CLR, 1);
		/* this firmware (HFI 2.5) acks with type 1 and the command's own id; newer ones use id 126 */
		uint32_t mtype = (h >> 16) & 0xf;
		if ((mtype == 1 || mtype == 2 || mid == 126) && (d1 >> 20) == seq) {
			(void)d3;
			if (ack_payload0)
				*ack_payload0 = d3;
			con_hex2("HFI ack", "id/seq=", id << 16 | seq, "err/pl0=", (d2 & 0xffff) << 16 | (d3 & 0xffff));
			return d2;
		}
		con_hex2("HFI other", "hdr=", h, "d1=", d1);	/* F2H_MSG_ERR 100 / DEBUG 101 / LOG 102 */
	}
	con_hex2("HFI timeout", "id=", id, "info=", rd(GMU2HOST_INTR_INFO));
	return ~0u;
}

/* ARC vote word: vlvl<<16 | sec_idx<<8 | pri_idx (KGSL ARC_VOTE_SET, Linux a6xx_gmu_rpmh_arc_votes_init) */
static uint32_t arc_vote(const struct arc *pri, const struct arc *sec, uint32_t vlvl)
{
	int p = -1, q = -1;
	for (int k = 0; k < pri->n && p < 0; k++)
		if (pri->level[k] >= vlvl)
			p = k;
	if (p < 0)
		return 0;
	for (int k = 0; k < sec->n && q < 0; k++)
		if (sec->level[k] >= pri->level[p])
			q = k;
	if (q < 0)
		q = sec->n - 1;
	return vlvl << 16 | (uint32_t)q << 8 | (uint32_t)p;
}

/* ---- probe 4 (in the gpu3 build): GPU_CC PLLs, OOB_GPU_SET -> GMU powers GX (A740-STAGES-3PLUS s4) ---- */

static void pll_config(uintptr_t b, uint32_t l, uint32_t alpha)
{
	/* lucid ole: L 0x10 (l | cal 0x44 << 16 | 0x44 << 24), ALPHA 0x14, USER 0x18/0x1c, CONFIG 0x20/24/28,
	 * TEST 0x2c/30/34/38; then MODE.OUTCTRL 0, OPMODE standby, MODE.RESET_N 1 (Linux clk_lucid_ole_pll_configure) */
	wr(b + 0x10, l | 0x44u << 16 | 0x44u << 24);
	wr(b + 0x14, alpha);
	wr(b + 0x20, 0x20485699u);
	wr(b + 0x24, 0x00182261u);
	wr(b + 0x28, 0x82aa299cu);
	wr(b + 0x18, 0);
	wr(b + 0x1c, 5);
	wr(b + 0x2c, 0);
	wr(b + 0x30, 3);
	wr(b + 0x34, 0x9000);
	wr(b + 0x38, 0x34);
	wr(b + 0x0, rd(b + 0x0) & ~1u);
	wr(b + 0x4, 0);
	wr(b + 0x0, rd(b + 0x0) | 4u);
}

static void gpu4_gx_on(void)
{
	con_hex2("P6 PLL0", "mode=", rd(GPUCC + 0x0), "l=", rd(GPUCC + 0x10));
	con_hex2("P6 PLL1", "mode=", rd(GPUCC + 0x1000), "l=", rd(GPUCC + 0x1010));
	if ((rd(GPUCC + 0x10) & 0xff) == 0) {
		con_str("NEXT", "P6 pll config");
		pll_config(GPUCC, 0x1e, 0xbaaa);
		pll_config(GPUCC + 0x1000, 0x16, 0xeaaa);
		con_hex2("P6 PLL l", "pll0=", rd(GPUCC + 0x10), "pll1=", rd(GPUCC + 0x1010));
	}
	wr(0x03d8ec40, 0);			/* AO_AHB_FENCE_CTRL: allow */
	con_str("NEXT", "P6 OOB GPU set");
	wr(GMU2HOST_INTR_CLR, 1u << 31);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(HOST2GMU_INTR_SET, 1u << 30);	/* OOB_GPU_SET */
	uint32_t us = poll(GMU2HOST_INTR_INFO, 1u << 31, 1u << 31, 300000);
	wr(GMU2HOST_INTR_CLR, 1u << 31);
	con_hex2("P6 OOB", "ack us=", us, "info=", rd(GMU2HOST_INTR_INFO));
	uint32_t gx_us = poll(0x03d7e340, 3, 0, 100000);	/* GFX_PWR_CLK_STATUS: GDSC on + clock on */
	con_hex2("P6 GX", "pwrclk=", rd(0x03d7e340), "us=", gx_us);
	con_hex2("P6 GX", "gdscr=", rd(GPUCC_GX_GDSCR), "rpmhst=", rd(0x03d7e3b0));
	if (us == ~0u || gx_us == ~0u) {
		con_hex2("P6 fail", "g0=", rd(0x03d7e714), "g1=", rd(0x03d7e718));
		con_hex2("P6 fail", "fwbusy=", rd(0x03d7e068), "rscc=", rd(0x03d50010));
		con_hex2("P6 fail", "seqbusy=", rd(0x03d50404), "logwp=", rd(0x03d7e738));
		con_str("GPU4 RESULT", "GX did not come up");
		return;
	}
	/* GX is on: first GX-domain reads (were the reset hazard with GX off) */
	con_str("NEXT", "P7 first GX reads");
	wr(0x03d0f114, 0);			/* CX GBIF_HALT */
	wr(0x03d00058, 0);			/* RBBM_GBIF_HALT (GX) */
	con_hex2("P7 RBBM", "status=", rd(0x03d00840), "int0=", rd(0x03d00804));
	con_hex2("P7 RBBM", "gbifhalt=", rd(0x03d00058), "status3=", rd(0x03d0084c));
	con_hex2("P7 CP", "status1=", rd(0x03d02094), "hwfault=", rd(0x03d02094 - 0x10));	/* CP_STATUS_1, CP_HW_FAULT 0x03d02084 */
	con_hex2("P7 CP", "aon ctr=", rd(0x03d02600), "intstat=", rd(0x03d0208c));		/* CP_ALWAYS_ON_COUNTER, CP_INTERRUPT_STATUS */
	con_str("GPU4 RESULT", "GX ON at 220 MHz (OOB held)");
	/* drop the OOB: the GMU may collapse GX again; the M3 is put back in reset by the caller */
	wr(HOST2GMU_INTR_SET, 1u << 31);
}

static void gpu3_hfi(void)
{
	static uint32_t m[160];
	struct arc *cx = &arcs[0], *mx = &arcs[1], *gfx = &arcs[3];
	int ok = 1;
	con_clear(0xff400040U);		/* page 4: purple */
	con_str("GPU3 page 4", "HFI messages (GX stays off)");
	wr(GMU2HOST_INTR_CLR, ~0u);
	__asm__ volatile("dsb sy" ::: "memory");

	/* 1. PERF_TABLE (id 4, 59 dwords): GX off/220/295/348 MHz (cap: no thermal driver), GMU off/220/550 */
	static const uint32_t gx_khz[4] = { 0, 220000, 295000, 348000 }, gx_lvl[4] = { 0, 52, 56, 60 };
	for (int i = 0; i < 59; i++)
		m[i] = 0;
	m[1] = 4;
	m[2] = 3;
	for (int i = 0; i < 4; i++) {
		m[3 + 3 * i] = i ? arc_vote(gfx, mx, gx_lvl[i]) : 0;
		m[4 + 3 * i] = 0xffffffffu;	/* acd: none */
		m[5 + 3 * i] = gx_khz[i];
	}
	m[51] = 0;
	m[52] = 0;				/* cx_votes[0] off */
	m[53] = arc_vote(cx, mx, 64);
	m[54] = 220000;
	m[55] = arc_vote(cx, mx, 128);
	m[56] = 550000;
	con_hex2("PERF gx", "v1=", m[6], "v3=", m[12]);
	con_hex2("PERF cx", "v1=", m[53], "v2=", m[55]);
	ok &= m[6] && m[12] && m[53] && m[55];
	ok &= hfi_send(4, m, 59, 0) == 0;

	/* 2. BW_TABLE (id 3, 160 dwords): a740_build_bw_table single entry */
	for (int i = 0; i < 160; i++)
		m[i] = 0;
	m[1] = 1;			/* bw_level_num */
	m[2] = 1;			/* cnoc_cmds_num */
	m[3] = 3;			/* ddr_cmds_num */
	m[4] = 1;			/* cnoc_wait_bitmask */
	m[5] = 7;			/* ddr_wait_bitmask */
	m[6] = rpmh_cmddb_addr("CN0");	/* cnoc_cmds_addrs[0] */
	m[12] = 0x40000000u;		/* cnoc_cmds_data[0][0] */
	m[18] = 0x60000001u;		/* cnoc_cmds_data[1][0] */
	m[24] = rpmh_cmddb_addr("SH0");	/* ddr_cmds_addrs[0..2] */
	m[25] = rpmh_cmddb_addr("MC0");
	m[26] = rpmh_cmddb_addr("ACV");
	m[32] = m[33] = m[34] = 0x40000000u;	/* ddr_cmds_data[0][0..2] */
	con_hex2("BW addr", "cn0=", m[6], "sh0=", m[24]);
	con_hex2("BW addr", "mc0=", m[25], "acv=", m[26]);
	ok &= hfi_send(3, m, 160, 0) == 0;

	/* 3. CORE_FW_START (id 14), 4. START (id 10) */
	m[1] = 0;
	ok &= hfi_send(14, m, 2, 0) == 0;
	ok &= hfi_send(10, m, 1, 0) == 0;

	/* 5. GX_BW_PERF_VOTE (id 30): blocking ack, perf index 1 = 220 MHz, no bw vote */
	m[1] = 1;
	m[2] = 1;
	m[3] = 0;
	ok &= hfi_send(30, m, 4, 0) == 0;

	con_hex2("GPU3 gmu", "info=", rd(GMU2HOST_INTR_INFO), "rpmhst=", rd(0x03d7e3b0));
	con_hex2("GPU3 gmu", "gxstat=", rd(0x03d7e340), "logwp=", rd(0x03d7e738));
	con_str("GPU3 RESULT", ok ? "5 acks err 0: HFI up" : "HFI failed");
	if (ok)
		gpu4_gx_on();
}

/* RSC/PDC sequencer microcode, KGSL gen7_load_rsc_ucode / gen7_load_pdc_ucode (A740 values), between
 * GMU start and HFI start (review finding 4; docs/dm3q/gpu/PROBE2-REVIEW.md) */
static void gpu2_rsc_pdc(void)
{
	const uintptr_t R = 0x03d50000UL, P = 0x0b280000UL;
	wr(R + 0x10, 1u << 24);			/* RSC_STATUS0_DRV0: disable SDE clock gating */
	wr(R + 0x2c, 1);			/* PDC_SLAVE_ID_DRV0 */
	wr(R + 0x38, 0);
	wr(R + 0x34, 0);			/* HIDDEN_TCS_CMD0 DATA / ADDR */
	wr(R + 0x40, 0);
	wr(R + 0x3c, 0);
	wr(R + 0x48, 0x80000021u);
	wr(R + 0x44, 0);
	wr(R + 0x400, 0);			/* OVERRIDE_START_ADDR */
	wr(R + 0x20, 0x4520);			/* PDC_SEQ_START_ADDR */
	wr(R + 0x24, 0x4510);			/* PDC_MATCH_VALUE_LO */
	wr(R + 0x28, 0x4514);			/* PDC_MATCH_VALUE_HI */
	wr(R + 0x550, 0xeaaae5a0u);		/* SEQ_MEM_0_DRV0 (A740) */
	wr(R + 0x554, 0xe1a1ebabu);
	wr(R + 0x558, 0xa2e0a581u);
	wr(R + 0x55c, 0xecac82e2u);
	wr(R + 0x560, 0x0020edadu);
	wr(P + 0x4520, 0);			/* PDC_GPU_SEQ_START_ADDR */
	wr(P + 0x4500, 0x80000001u);		/* PDC_GPU_ENABLE_PDC */
	__asm__ volatile("dsb sy" ::: "memory");
}

void gpu2_probe(void)
{
	for (int i = 0; i < 12; i++)
		results[i] = '-';
	con_str("GPU2", "A740 probe 2: GMU smmu context + boot");
	s1_cmddb();
	s3_arc_votes();
	s4_gcc_on();
	s6_cx_gdsc();
	s7_gmu_clocks();
	s9_tcm();
	uint32_t g_memnoc = rd(GCC_GPU_MEMNOC_GFX), g_axi = rd(GCC_DDRSS_GPU_AXI);
	uint32_t g_dvm = rd(GCC_GPU_SNOC_DVM_GFX), g_cfg = rd(GCC_GPU_CFG_AHB);
	char l1s[16] = "S1..9 ";
	for (int i = 1; i <= 9; i++)
		l1s[5 + i] = results[i];
	l1s[15] = 0;
	con_clear(BLUE);
	con_str("GPU2 page 2", "power + TCM (o = ok)");
	con_str("GPU2 S1-9", l1s);
	con_hex2("P0 GCC", "memnoc=", g_memnoc, "axi=", g_axi);
	con_hex2("P0 GCC", "dvm=", g_dvm, "cfgahb=", g_cfg);
	if (results[6] != 'o' || results[9] != 'o') {
		con_str("GPU2", "power/TCM failed: stop");
		return;
	}

	uint32_t nsmr, ncb;
	if (!smmu_survey(&nsmr, &ncb)) {
		con_str("GPU2", "smmu survey odd: stop");
		return;
	}
	/* a context bank nobody uses, an SMR slot (reuse one already matching SID 5) */
	uint32_t cb = ~0u, slot = ~0u;
	for (uint32_t c = ncb; c-- > nums2cb;)
		if (!(rd(cb_base + c * pgsz + CB_SCTLR) & 1)) {
			int used = 0;
			for (uint32_t n = 0; n < nsmr && n < 128; n++) {
				uint32_t smr = rd(SMMU + SMMU_SMR(n));
				used |= (smr >> 31) && (rd(SMMU + SMMU_S2CR(n)) & 0xff) == c;
			}
			if (!used) {
				cb = c;
				break;
			}
		}
	for (uint32_t n = 0; n < nsmr && n < 128 && slot == ~0u; n++)
		if (rd(SMMU + SMMU_SMR(n)) == (0x80000000u | GMU_SID))
			slot = n;
	for (uint32_t n = 0; n < nsmr && n < 128 && slot == ~0u; n++)
		if (!(rd(SMMU + SMMU_SMR(n)) >> 31))
			slot = n;
	con_hex2("P2 pick", "cb=", cb, "smr slot=", slot);
	for (uint32_t n = 0; n < nsmr && n < 128; n++) {	/* other SMRs that also match SID 5 (multi-match) */
		uint32_t smr = rd(SMMU + SMMU_SMR(n)), m = (smr >> 16) & 0x7fff, id = smr & 0x7fff;
		if (n != slot && (smr >> 31) && (((id ^ GMU_SID) & ~m & 0x7fff) == 0))
			con_hex2("P2 SMR conflict", "n=", n, "smr=", smr);
	}
	if (cb == ~0u || slot == ~0u)
		return;

	/* page tables and memory */
	con_str("NEXT", "P2 carve-out + tables");
	carve_next = CARVE;
	l1 = carve_page();
	l2[0] = l2[1] = 0;
	uint64_t code = map(VA_CODE, 0x11000);
	(void)map(0x100000, 0x9000);
	(void)map(0x200000, 0x13000);
	(void)map(0x300000, 0x1c000);
	(void)map(0x60000000, 0x4000);	/* fw PERSIST x2 (Linux a7xx dummy is 16 KiB) */
	(void)map(0x60400000, 0x7000);	/* Linux a7xx debug region */
	uint64_t log = map(VA_LOG, LOG_SIZE);
	uint64_t hfi = map(VA_HFI, HFI_SIZE);
	/* the firmware's DDR block {0x4000, 0x10210} */
	const uint8_t *f = _gmu_fw_start, *end = _gmu_fw_end;
	uint32_t copied = 0;
	while (f + 16 <= end) {
		uint32_t addr = f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24;
		uint32_t size = f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24;
		if (f + 16 + size > end)
			break;
		if (size && addr >= VA_CODE && addr + size <= VA_CODE + 0x11000) {
			copy_words(code + (addr - VA_CODE), f + 16, size);
			copied += size;
		}
		f += 16 + size;
	}
	hfi_table(hfi);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("P2 carve", "used=", (uint32_t)(carve_next - CARVE), "code bytes=", copied);
	con_hex2("P2 code", "w0=", rd(code), "l1[0]=", (uint32_t)rd64(l1));

	/* context bank: AArch64 stage 1, 39-bit VA, 4 KiB, non-cacheable walks/memory (MMU-off writes) */
	con_str("NEXT", "P2 smmu program");
	uintptr_t c = cb_base + cb * pgsz;
	wr(c + CB_SCTLR, 0);
	wr(gr1 + GR1_CBA2R(cb), 1);				/* VA64 */
	wr(gr1 + GR1_CBAR(cb), (1u << 16) | (3u << 8) | (0xfu << 12) | (cb + 1));	/* S1 trans, S2 bypass */
	wr(c + CB_TCR2, (oas < 2 ? oas : 2u) | (7u << 15));	/* PASize min(40-bit, OAS), SEP upstream */
	wr(c + CB_TTBR0, (uint32_t)l1);
	wr(c + CB_TTBR0 + 4, (uint32_t)(l1 >> 32));
	wr(c + CB_TCR, 25u | (1u << 23));			/* T0SZ 25, TG0 4K, NC walks, EPD1 */
	wr(c + CB_MAIR0, 0x44);					/* attr0 normal non-cacheable */
	wr(c + CB_MAIR1, 0);
	wr(c + CB_FSR, rd(c + CB_FSR));				/* clear stale faults */
	wr(c + CB_TLBIALL, 0);
	wr(c + CB_TLBSYNC, 0);
	uint32_t tus = poll(c + CB_TLBSTATUS, 1, 0, 10000);
	wr(c + CB_SCTLR, 1u | (1u << 1) | (1u << 2) | (1u << 5));	/* M, TRE, AFE, CFRE */
	wr(SMMU + SMMU_S2CR(slot), cb & 0xff);			/* translate through cb */
	wr(SMMU + SMMU_SMR(slot), 0x80000000u | GMU_SID);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(SMMU + SMMU_GFSR, rd(SMMU + SMMU_GFSR));		/* clear stale global faults */
	uint32_t cr0b = rd(SMMU + SMMU_CR0);
	if (cr0b & 1u)						/* CLIENTPD: every stream would bypass */
		wr(SMMU + SMMU_CR0, cr0b & ~1u);
	uint32_t cr0 = rd(SMMU + SMMU_CR0);
	con_hex2("P2 sCR0", "before=", cr0b, "after=", cr0);
	con_hex2("P2 CB", "sctlr=", rd(c + CB_SCTLR), "tlbsync us=", tus);
	con_hex2("P2 SMR", "smr=", rd(SMMU + SMMU_SMR(slot)), "s2cr=", rd(SMMU + SMMU_S2CR(slot)));
	con_hex2("P2 SMMU", "cr0=", cr0, "cbar=", rd(gr1 + GR1_CBAR(cb)));
	step(10, rd(c + CB_SCTLR) & 1);

	/* page 3 (the console clips at the bottom) */
	{
		uint32_t sctlr = rd(c + CB_SCTLR), smr = rd(SMMU + SMMU_SMR(slot));
		con_str("PHOTO", "page 2 now: page 3 in 10 s");
		udelay(10000000);
		con_clear(0xff004000U);
		con_str("GPU2 page 3", "GMU boot (page 2: smmu)");
		con_hex2("P2 summary", "cb/slot=", cb << 16 | slot, "sctlr=", sctlr);
		con_hex2("P2 summary", "smr=", smr, "cr0=", rd(SMMU + SMMU_CR0));
	}
	/* GMU register setup [KGSL gen7_gmu_register_config / Linux a6xx_gmu_fw_start] */
	con_str("NEXT", "P3 gmu config");
	wr(0x03d7e000, 1);			/* SYSRESET held (Linux a6xx_gmu_start) */
	wr(0x03d9e0e4, 1);			/* CX_MISC TCM_RET_CNTL */
	/* fw prealloc ranges inside the TCMs, zeroed (outside the loaded blocks) */
	for (uintptr_t a = GMU_ITCM + 0x200; a < GMU_ITCM + 0x3cdc; a += 4)
		wr(a, 0);
	for (uintptr_t a = GMU_DTCM; a < GMU_DTCM + 0x2ee0; a += 4)
		wr(a, 0);
	for (uintptr_t a = GMU_DTCM + 0x3000; a < GMU_DTCM + 0x3f84; a += 4)
		wr(a, 0);
	wr(0x03d7e3c4, 1);			/* GMU_CM3_FALNEXT_INTF */
	wr(0x03d7e3c0, 1);			/* GMU_CM3_FAL_INTF */
	wr(0x03d7e070, 0);			/* FW_INIT_RESULT */
	wr(0x03d7e004, 2);			/* CM3_BOOT_CONFIG */
	wr(0x03d7e614, (uint32_t)VA_HFI);	/* HFI_QTBL_ADDR */
	wr(0x03d7e610, 1);			/* HFI_QTBL_INFO */
	wr(0x03d8ec44, 0x80c808a0u);		/* AHB_FENCE_RANGE_0 */
	wr(0x03d7e0b4, 0x4052);			/* CM3_CFG */
	for (uintptr_t a = 0x03d0f00c; a <= 0x03d0f018; a += 4)
		wr(a, 0x00071620u);		/* GBIF_QSB_SIDE0..3 */
	wr(0x03d0f114, 0);			/* GBIF_HALT */
	wr(0x03d7e738, 0);			/* GENERAL_9: log write pointer */
	wr(0x03d7e73c, 0x07020100u);		/* GENERAL_10: GMU chip id (Linux catalog, A740) */
	wr(0x03d7e734, (uint32_t)(VA_LOG & 0xfffff000u) | (uint32_t)(LOG_SIZE / 0x1000 - 1));	/* GENERAL_8 */
	wr(0x03d7d03c, 1);			/* SYS_BUS_CONFIG */
	wr(0x03d7d000, 1);			/* ICACHE_CONFIG */
	wr(0x03d7d004, 1);			/* DCACHE_CONFIG */

	/* release the M3 and wait for init done [(FW_INIT_RESULT & 0x1ff) == 0x100] */
	con_str("NEXT", "P4 gmu release");
	__asm__ volatile("dsb sy" ::: "memory");
	wr(0x03d7e000, 0);
	uint64_t t0 = ticks();
	uint32_t res = 0, seen = 0, last = ~0u;
	while (ticks() - t0 < 19200000ULL * 2) {	/* 2 s: the GMU runs on XO (19.2 MHz), drivers use 200 MHz */
		res = rd(0x03d7e070);
		if (res != last && seen < 6) {
			con_hex2("P4 initres", "val=", res, "ms=", (uint32_t)((ticks() - t0) / 19200));
			seen++;
			last = res;
		}
		if ((res & 0x1ff) == 0x100 || (res & 0xe00))	/* done, or fw assertion/NMI (KGSL) */
			break;
	}
	int init_ok = (res & 0x1ff) == 0x100;
	step(11, init_ok);
	con_hex2("P4 gmu", "fwbusy=", rd(0x03d7e068), "g2h=", rd(0x03d7e648));
	con_hex2("P4 smmu", "fsr=", rd(c + CB_FSR), "far=", rd(c + CB_FAR));
	con_hex2("P4 smmu", "farhi=", rd(c + CB_FAR + 4), "fsynr0=", rd(c + CB_FSYNR0));
	con_hex2("P4 smmu", "gfsr=", rd(SMMU + SMMU_GFSR), "gfsynr0=", rd(SMMU + 0x50));
	if (init_ok) {
		con_str("NEXT", "P5 rsc/pdc");
		gpu2_rsc_pdc();
		con_str("NEXT", "P5 hfi start");
		wr(0x03d7e618, 1);		/* HFI_CTRL_INIT */
		uint32_t us = poll(0x03d7e600, 1, 1, 50000);
		con_hex2("P5 HFI", "status=", rd(0x03d7e600), "us=", us);
		results[0] = us != ~0u ? 'o' : 'X';
#if defined(DIAG_GPU3)
		if (us != ~0u) {
			hfi_pa = hfi;
			gpu3_hfi();
			wr(0x03d7e000, 1);	/* stop the M3 */
			return;
		}
#endif
	}
	con_hex2("P5 GEN", "g0=", rd(0x03d7e714), "g9 logwp=", rd(0x03d7e738));
	con_hex2("P5 log", "w0=", rd(log), "w1=", rd(log + 4));
	con_hex2("P5 log", "w2=", rd(log + 8), "w3=", rd(log + 12));
	con_hex2("P5 hfi q0", "rd idx=", rd(hfi + 24 + 40), "wr idx=", rd(hfi + 24 + 44));
	/* stop the M3 again (nothing else will service it) */
	wr(0x03d7e000, 1);
	char line[24] = "smmu/init/hfi ";
	line[14] = results[10];
	line[15] = results[11];
	line[16] = results[0];
	line[17] = 0;
	con_str("GPU2 RESULT", line);
}
