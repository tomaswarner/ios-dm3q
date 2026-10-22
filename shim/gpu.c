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
extern const uint8_t _sqe_fw_start[], _sqe_fw_end[];
extern const uint8_t _zap_fw_start[], _zap_fw_end[];

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

/* SCM call with four value arguments (x2..x5) */
static struct smc_res scm4(uint64_t fn, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a0;
	register uint64_t x3 __asm__("x3") = a1, x4 __asm__("x4") = a2, x5 __asm__("x5") = a3;
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
/* DDR bank 0x8_c0000000-0xa_00000000; xnu's RAM ends at 0x9_bcc00000 (tools/mkadt.py HIGH_*), so 0x9_c0000000
 * stays ours for good: 1 MiB used here (tables, firmware, ring), then the handoff page, then a pool for xnu's
 * GPU driver (page tables, buffers) up to GPU_RESERVE_END. */
#define CARVE		0x9c0000000UL
#define CARVE_SIZE	0x100000UL	/* 1 MiB: tables + firmware + regions */
#define HANDOFF		(CARVE + CARVE_SIZE)
#define GPU_POOL	(CARVE + 0x200000UL)
#define GPU_RESERVE_END	0xa00000000UL

#if defined(DIAG_GPUBOOT)
#define GPU_STAGES	1	/* run every stage (2..6) and leave the GPU running for xnu */
#endif
#if defined(DIAG_GPU3) || defined(DIAG_GPU5) || defined(DIAG_GPU6) || defined(GPU_STAGES)
#define RUN_HFI		1
#endif
#if defined(DIAG_GPU5) || defined(DIAG_GPU6) || defined(GPU_STAGES)
#define RUN_CP		1
#endif
#if defined(DIAG_GPU6) || defined(GPU_STAGES)
#define RUN_ZAP		1
#endif
static int gpu_up;		/* CP running, non-secure, handoff written: keep GMU, OOB and GX up */

/* GMU VAs (firmware PREALLOC requests + log + HFI) */
#define VA_CODE		0x00004000UL
#define VA_LOG		0x60100000UL
#define VA_HFI		0x60180000UL
#define LOG_SIZE	0x10000UL
#define HFI_SIZE	0x4000UL

static uintptr_t pgsz, cb_base, gr1;
static uint32_t nums2cb, oas;
static uint64_t carve_next;
static uint64_t map_ap;		/* descriptor AP bits: 0 (GMU, privileged) or 1 << 6 (GPU, unprivileged too) */
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
		wr64(l3 + 8 * (((va + o) >> 12) & 0x1ff), pa | (1u << 10) | (3u << 8) | map_ap | 3);
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

/* ---- probe 5 (DIAG=gpu5): GPU SMMU (SID 0), GX init, SQE, ring, CP_ME_INIT, first commands, no zap ----
 * docs/dm3q/gpu/A740-STAGES-3PLUS.md sections 5-9 (Linux a6xx hw_init / KGSL gen7_start, gen7_rb_start). */

#define GPU_SID		0
#define VA_RING		0x10000000UL
#define VA_MEMPTRS	0x10008000UL
#define VA_PWRUP	0x10009000UL
#define VA_SQE		0x1000b000UL
#define VA_TEST		0x10020000UL
#define CP_RB_RPTR	0x03d02018UL
#define CP_RB_WPTR	0x03d0201cUL

static void cp_dump(const char *why, uintptr_t cbp)
{
	con_str("CP DUMP", why);
	con_hex2("CP", "hwfault=", rd(0x03d02084), "intstat=", rd(0x03d0208c));
	con_hex2("CP", "protect=", rd(0x03d02090), "status1=", rd(0x03d02094));
	con_hex2("RBBM", "status=", rd(0x03d00840), "int0=", rd(0x03d00804));
	con_hex2("RBBM", "status3=", rd(0x03d0084c), "rptr=", rd(CP_RB_RPTR));
	con_hex2("GPU SMMU", "fsr=", rd(cbp + CB_FSR), "far=", rd(cbp + CB_FAR));
	con_hex2("GPU SMMU", "fsynr0=", rd(cbp + CB_FSYNR0), "gfsr=", rd(SMMU + SMMU_GFSR));
}

static int ring_submit(uint64_t ring, uint32_t *wptr, const uint32_t *cmds, uint32_t n, uintptr_t cbp)
{
	for (uint32_t i = 0; i < n; i++)
		wr(ring + 4 * ((*wptr + i) % 8192), cmds[i]);
	*wptr = (*wptr + n) % 8192;
	__asm__ volatile("dsb sy" ::: "memory");
	wr(CP_RB_WPTR, *wptr);
	if (rd(0x03d8ec4c) & 1) {		/* AHB fence dropped the write: retry (Linux fenced write) */
		udelay(100);
		wr(CP_RB_WPTR, *wptr);
	}
	uint64_t t0 = ticks();
	while (ticks() - t0 < 19200000ULL * 2) {
		if (rd(CP_RB_RPTR) == *wptr && !(rd(0x03d00840) & ~1u))
			return (int)((ticks() - t0) / 19200);
	}
	cp_dump("timeout", cbp);
	return -1;
}

/* ---- probe 6 (DIAG=gpu6): zap shader through TZ PAS 13, then CP_SET_SECURE_MODE 0 ----
 * STAGES-3PLUS s8.2 / Linux zap_shader_load_mdt + qcom_mdt_load / KGSL kgsl_zap_shader_load; ios-sailfish 0130.
 * a740_zap.mbn: ELF header + 3 phdrs (0x94 B), PT_LOAD segment at file 0x1000 (0x430 B, paddr 0x1000), hash at 0x2000
 * (0xf38 B). Metadata for INIT_IMAGE = mbn[0:0x94] + mbn[0x2000:0x2f38] (= the .mdt file). Region: DT gpu_micro_code_mem. */
#define ZAP_REGION	0x9b09a000UL
#define ZAP_META	(CARVE + 0xc0000UL)	/* 4 KiB aligned, contiguous, uncached (MMU off), not reused after */

static uint64_t g_ring, g_mem, g_test;

/* Handoff page for xnu's GPU driver (bsd/dev/sweet_gpu.c reads it; layout = struct sweet_gpu_handoff there).
 * All u64. The CP is idle with rptr == wptr; GPU page table = 4 KiB granule, T0SZ 25 (39-bit VA), 3 levels
 * from l1, MAIR0 attr0 = 0x44 (normal NC), data pages AP=01 (bit 6), privileged-fetch pages AP=00. */
static void handoff_write(uint32_t wptr, uintptr_t cbp)
{
	static const uint64_t v[] = {
		0x3034374155504753ULL,	/*  0 magic "SGPUA740" */
		1,			/*  1 version */
		0, 0, 0, 0,		/*  2-5 filled below: cb page, l1, carve_next, wptr */
		SMMU,			/*  6 kgsl-smmu base */
		0,			/*  7 context bank (GPU SIDs 0 and 1) */
		CARVE, CARVE_SIZE,	/*  8-9 shim region */
		GPU_POOL, GPU_RESERVE_END - GPU_POOL,	/* 10-11 free pool for xnu */
		0, VA_RING, 0x8000,	/* 12-14 ring pa (below), gpu va, size (dwords mod 8192) */
		0, VA_MEMPTRS,		/* 15-16 memptrs pa (below), gpu va: +0 rptr shadow, +4 fence, +8 bv rptr */
		VA_SQE, 0x13000,	/* 17-18 sqe gpu va, size */
		0, VA_TEST,		/* 19-20 scratch test page pa (below), gpu va */
		0x10021000ULL,		/* 21 first gpu va not used by the shim */
		0x03d00000ULL, 0x40000,	/* 22-23 GPU register block (kgsl_3d0_reg_memory) */
	};
	for (uint32_t i = 0; i < sizeof(v) / 8; i++)
		wr64(HANDOFF + 8 * i, v[i]);
	wr64(HANDOFF + 8 * 2, cbp);
	wr64(HANDOFF + 8 * 3, l1);
	wr64(HANDOFF + 8 * 4, carve_next);
	wr64(HANDOFF + 8 * 5, wptr);
	wr64(HANDOFF + 8 * 12, g_ring);
	wr64(HANDOFF + 8 * 15, g_mem);
	wr64(HANDOFF + 8 * 19, g_test);
	__asm__ volatile("dsb sy" ::: "memory");
	gpu_up = 1;
	con_hex2("HANDOFF", "pa lo=", (uint32_t)HANDOFF, "l1 lo=", (uint32_t)l1);
}

static void gpu6_zap(uint64_t ring, uint32_t *wptr, uint64_t test, uintptr_t cbp)
{
	con_clear(0xff006060U);		/* page 6: teal */
	con_str("GPU6 page 6", "zap shader (TZ PAS 13) + leave secure mode");
	const uint8_t *z = _zap_fw_start;
	uint32_t zlen = (uint32_t)(_zap_fw_end - _zap_fw_start);
	con_hex2("Z mbn", "len=", zlen, "magic=", (uint32_t)z[0] | z[1] << 8 | z[2] << 16 | (uint32_t)z[3] << 24);
	if (zlen < 0x2f38 || z[0] != 0x7f) {
		con_str("GPU6", "bad zap image");
		return;
	}
	/* metadata: ELF header + phdrs, then the hash segment */
	for (uint32_t o = 0; o < 0x1000; o += 4)
		wr(ZAP_META + o, 0);
	copy_words(ZAP_META, z, 0x94);
	copy_words(ZAP_META + 0x94, z + 0x2000, 0xf38);
	__asm__ volatile("dsb sy" ::: "memory");

	con_str("NEXT", "Z1 PAS_INIT_IMAGE");
	struct smc_res r1 = scm4(0x42000201, 0x82, 13, ZAP_META, 0, 0);
	con_hex2("Z1 init", "x0=", (uint32_t)r1.a0, "x1=", (uint32_t)r1.a1);
	if (r1.a0 || r1.a1) {
		con_str("GPU6 RESULT", "INIT_IMAGE refused");
		return;
	}
	/* phone's mdt_loader.ko (__qcom_mdt_load, disassembled): size = ALIGN(paddr + memsz, 4K) - min paddr = 0x1000,
	 * not the 0x430 segment length (gpu6: 0x430 -> x0 0xFFCFFFBA). Fallback 0x2000 (whole DT region) after
	 * PAS_SHUTDOWN + a fresh INIT_IMAGE, as mdt_loader's error path does. */
	static const uint32_t sizes[2] = { 0x1000, 0x2000 };
	struct smc_res r2 = { 0 };
	for (int i = 0; i < 2; i++) {
		if (i) {
			struct smc_res sd = scm4(0x42000206, 1, 13, 0, 0, 0);
			r1 = scm4(0x42000201, 0x82, 13, ZAP_META, 0, 0);
			con_hex2("Z1 retry", "shut x0=", (uint32_t)sd.a0, "init x0=", (uint32_t)r1.a0);
		}
		con_hex2("NEXT", "Z2 MEM_SETUP size=", sizes[i], "try=", (uint32_t)i);
		r2 = scm4(0x42000202, 3, 13, ZAP_REGION, sizes[i], 0);
		con_hex2("Z2 mem", "x0=", (uint32_t)r2.a0, "x1=", (uint32_t)r2.a1);
		if (!r2.a0 && !r2.a1)
			break;
	}
	if (r2.a0 || r2.a1) {
		con_str("GPU6 RESULT", "MEM_SETUP refused");
		return;
	}
	con_str("NEXT", "Z3 copy segment");
	for (uint32_t o = 0; o < 0x1000; o += 4)
		wr(ZAP_REGION + o, 0);
	copy_words(ZAP_REGION, z + 0x1000, 0x430);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("Z3 seg", "w0=", rd(ZAP_REGION), "w last=", rd(ZAP_REGION + 0x42c));
	con_str("NEXT", "Z4 PAS_AUTH_AND_RESET");
	struct smc_res r4 = scm4(0x42000205, 1, 13, 0, 0, 0);
	con_hex2("Z4 auth", "x0=", (uint32_t)r4.a0, "x1=", (uint32_t)r4.a1);
	if (r4.a0 || r4.a1) {
		con_str("GPU6 RESULT", "AUTH_AND_RESET refused");
		return;
	}
	/* CP_SET_SECURE_MODE 0, then a second memory write to prove the non-secure CP still works */
	con_str("NEXT", "Z5 CP_SET_SECURE_MODE 0");
	static const uint32_t unsec[2] = { 0x70e60001, 0 };
	int ms = ring_submit(ring, wptr, unsec, 2, cbp);
	con_hex2("Z5 secure", "ms=", (uint32_t)ms, "secvid=", rd(0x03d3d000));
	if (ms < 0) {
		con_str("GPU6 RESULT", "CP hung leaving secure mode");
		return;
	}
	static const uint32_t again[6] = { 0x703d8003, (uint32_t)VA_TEST + 4, 0, 0x600df00du, 0x70928000, 0x70268000 };
	ms = ring_submit(ring, wptr, again, 6, cbp);
	con_hex2("Z6 write", "ms=", (uint32_t)ms, "test+4=", rd(test + 4));
	int zok = rd(test + 4) == 0x600df00du && rd(0x03d3d000) == 0;
	con_str("GPU6 RESULT", zok ? "ZAP OK: GPU non-secure, writes work" : "zap ran: check secvid/test");
	if (zok)
		handoff_write(*wptr, cbp);
}

static void gpu5_cp(void)
{
	con_clear(0xff803000U);		/* page 5: brown */
	con_str("GPU5 page 5", "GPU smmu + SQE + ring + first commands");

	/* GPU page table: separate L1, unprivileged-accessible pages (GPU APRIV accesses are data) */
	l1 = carve_page();
	l2[0] = l2[1] = 0;
	map_ap = 1u << 6;
	uint64_t ring = map(VA_RING, 0x8000), mem = map(VA_MEMPTRS, 0x1000), pwr = map(VA_PWRUP, 0x2000);
	/* the SQE microcode is fetched as a privileged instruction (APRIV ICACHE): AP 00, or the implicit
	 * PXN of an unprivileged-writable page faults it (gpu5 photo: FSR 0x408 FAR 0x1000B000 FSYNR0 0x63) */
	map_ap = 0;
	uint64_t sqe = map(VA_SQE, 0x13000);
	map_ap = 1u << 6;
	uint64_t test = map(VA_TEST, 0x1000);
	g_ring = ring;
	g_mem = mem;
	g_test = test;
	(void)pwr;	/* power-up reglist left empty (fallback A): no preemption/IFPC here */
	uint32_t sqe_bytes = (uint32_t)(_sqe_fw_end - _sqe_fw_start);
	copy_words(sqe, _sqe_fw_start + 4, sqe_bytes - 4);	/* the microcode starts after the 4-byte header */
	for (uint32_t o = 0; o < 0x1000; o += 4)
		wr(test + o, 0xddddddddu);
	con_hex2("P8 mem", "carve used=", (uint32_t)(carve_next - CARVE), "sqe w1=", rd(sqe));

	/* context bank for SID 0: CB 0 (Linux gives the GPU CB 0), SCTLR | HUPCF */
	con_str("NEXT", "P8 gpu smmu");
	uint32_t cb = 0, slot = 1;
	uintptr_t c = cb_base + cb * pgsz;
	wr(c + CB_SCTLR, 0);
	wr(gr1 + GR1_CBA2R(cb), 1);
	wr(gr1 + GR1_CBAR(cb), (1u << 16) | (3u << 8) | (0xfu << 12) | (cb + 1));
	wr(c + CB_TCR2, (oas < 2 ? oas : 2u) | (7u << 15));
	wr(c + CB_TTBR0, (uint32_t)l1);
	wr(c + CB_TTBR0 + 4, (uint32_t)(l1 >> 32));
	wr(c + CB_TCR, 25u | (1u << 23));
	wr(c + CB_MAIR0, 0x44);
	wr(c + CB_MAIR1, 0);
	wr(c + CB_FSR, rd(c + CB_FSR));
	wr(c + CB_TLBIALL, 0);
	wr(c + CB_TLBSYNC, 0);
	(void)poll(c + CB_TLBSTATUS, 1, 0, 10000);
	wr(c + CB_SCTLR, 1u | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 8));	/* M TRE AFE CFRE HUPCF */
	__asm__ volatile("dsb sy" ::: "memory");
	wr(SMMU + SMMU_S2CR(slot), cb);
	wr(SMMU + SMMU_SMR(slot), 0x80000000u | GPU_SID);
	/* SID 1 too: the phone DT and Linux give the GPU streams 0 AND 1 (same context bank); an unmatched
	 * stream is a global fault, which resets this phone (gpu5b/5c: reset as soon as the SQE ran; sailfish
	 * 0120a likewise). Not SID 2: TZ's gfx3d_secure (sailfish 0122a). */
	wr(SMMU + SMMU_S2CR(slot + 1), cb);
	wr(SMMU + SMMU_SMR(slot + 1), 0x80000000u | 1u);
	con_hex2("P8 CB0", "sctlr=", rd(c + CB_SCTLR), "smr1=", rd(SMMU + SMMU_SMR(slot)));
	con_hex2("P8 SMR2", "smr=", rd(SMMU + SMMU_SMR(slot + 1)), "s2cr=", rd(SMMU + SMMU_S2CR(slot + 1)));

	/* GX register init (STAGES-3PLUS s6, rows tagged R plus cheap S) */
	con_str("NEXT", "P9 gx init");
	wr(0x03d0f114, 0);			/* CX GBIF_HALT */
	wr(0x03d00058, 0);			/* RBBM_GBIF_HALT */
	wr(0x03d3e00c, 0);			/* RBBM_SECVID_TSB_CNTL */
	wr(0x03d3e000, 0);
	wr(0x03d3e004, 0);
	wr(0x03d3e008, 0);			/* TSB trusted base/size */
	wr(0x03d00044, 0x02120212u);		/* RBBM_GBIF_CLIENT_QOS_CNTL */
	wr(0x03d038e8, 0x010240e0u);		/* UCHE_GBIF_GX_CONFIG */
	wr(0x03d0142c, 0xffffffffu);		/* RBBM_PERFCTR_GPU_BUSY_MASKED */
	wr(0x03d03824, 0xfffff000u);
	wr(0x03d03828, 0x0001ffffu);		/* UCHE_TRAP_BASE */
	wr(0x03d0381c, 0xfffff000u);
	wr(0x03d03820, 0x0001ffffu);		/* UCHE_WRITE_THRU_BASE */
	wr(0x03d0382c, 0x01000000u);
	wr(0x03d03830, 0);			/* UCHE_GMEM_RANGE_MIN */
	wr(0x03d03834, 0x012fffffu);
	wr(0x03d03838, 0);			/* UCHE_GMEM_RANGE_MAX */
	wr(0x03d0385c, 0x00800000u);		/* UCHE_CACHE_WAYS */
	wr(0x03d02634, 1);			/* CP_AHB_CNTL */
	wr(0x03d01400, 1);			/* RBBM_PERFCTR_CNTL */
	wr(0x03d0007c, 0x40cfffffu);		/* RBBM_INTERFACE_HANG_INT_CNTL */
	wr(0x03d03864, 0x00000081u);		/* UCHE_CLIENT_PF */
	wr(0x03d038f0, 0x0006690eu);		/* UCHE_CMDQ_CONFIG */
	wr(0x03d02110, 0x6f);			/* CP_APRIV_CNTL (BR) */
	wr(0x03d02b40, 0x0f);			/* CP_BV_APRIV_CNTL */
	wr(0x03d02104, 1);			/* CP_CHICKEN_DBG */
	wr(0x03d02b68, 1);			/* CP_BV_CHICKEN_DBG */
	wr(0x03d238a0, rd(0x03d238a0) | (1u << 11));	/* RB_CMP_DBG_ECO_CNTL */
	wr(0x03d000e0, 0);			/* RBBM_INT_0_MASK: poll only */
	con_hex2("P9 readback", "cacheways=", rd(0x03d0385c), "cmdq=", rd(0x03d038f0));

	/* TZ: let the CP use the SMMU aperture of CB 0 (Linux adreno_hw_init -> qcom_scm_set_gpu_smmu_aperture(0),
	 * KGSL kgsl_set_smmu_aperture; done before the SQE starts). SVC_MP 0x0c cmd 0x1b, ARGS(4). */
	con_str("NEXT", "P10 scm smmu aperture");
	struct smc_res av = scm(0x42000601, 1, 0x02000c1b);	/* IS_CALL_AVAIL(SVC_MP 0x0c, cmd 0x1b) */
	con_hex2("P10 aperture", "avail x0=", (uint32_t)av.a0, "x1=", (uint32_t)av.a1);
	struct smc_res ap = scm4(0x42000c1b, 4, 0xffff0000u | cb, 0xffffffffu, 0xffffffffu, 0xffffffffu);
	con_hex2("P10 aperture", "set x0=", (uint32_t)ap.a0, "x1=", (uint32_t)ap.a1);

	/* SQE + ring */
	con_str("NEXT", "P10 sqe start");
	wr(0x03d020c0, (uint32_t)VA_SQE);
	wr(0x03d020c4, 0);			/* CP_SQE_INSTR_BASE */
	wr(0x03d02000, (uint32_t)VA_RING);
	wr(0x03d02004, 0);			/* CP_RB_BASE */
	wr(0x03d02008, 0x0000020cu);		/* CP_RB_CNTL */
	wr(0x03d02010, (uint32_t)VA_MEMPTRS);
	wr(0x03d02014, 0);			/* CP_RB_RPTR_ADDR */
	wr(0x03d02a60, (uint32_t)(VA_MEMPTRS + 8));
	wr(0x03d02a64, 0);			/* CP_BV_RB_RPTR_ADDR */
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t aon0 = rd(0x03d02600);
	wr(0x03d02020, 1);			/* CP_SQE_CNTL: start */
	udelay(1000);
	con_hex2("P10 SQE", "status1=", rd(0x03d02094), "aon delta=", rd(0x03d02600) - aon0);
	con_hex2("P10 SQE", "hwfault=", rd(0x03d02084), "rptr=", rd(CP_RB_RPTR));

	/* CP_ME_INIT (gen7, 10 dwords; pwrup list empty) */
	con_str("NEXT", "P11 ME_INIT");
	static const uint32_t me_init[10] = { 0x70970001, 0x08000000, 0x70c80007, 0x0000014b, 0x00000003,
					      0x20000000, 0x00000002, (uint32_t)VA_PWRUP, 0, 0x80000000 };
	uint32_t wptr = 0;
	int ms = ring_submit(ring, &wptr, me_init, 10, c);
	con_hex2("P11 ME_INIT", "ms=", (uint32_t)ms, "rptr=", rd(CP_RB_RPTR));
	if (ms < 0) {
		con_str("GPU5 RESULT", "ME_INIT did not complete");
		return;
	}

	/* first commands: A = PKT4 CP_SCRATCH(2), B = CP_MEM_WRITE + cache clean fence (no zap) */
	con_str("NEXT", "P12 test cmds");
	static const uint32_t cmds[13] = { 0x48088501, 0xc0de0001, 0x703d8003, (uint32_t)VA_TEST, 0, 0xcafef00du,
					   0x70928000, 0x70460004, 0x08000031, (uint32_t)(VA_MEMPTRS + 4), 0, 1, 0x70268000 };
	ms = ring_submit(ring, &wptr, cmds, 13, c);
	con_hex2("P12 cmds", "ms=", (uint32_t)ms, "scratch2=", rd(0x03d02214));
	con_hex2("P12 mem", "test=", rd(test), "fence=", rd(mem + 4));
	con_hex2("P12 mem", "rptr shadow=", rd(mem), "secvid=", rd(0x03d3d000));
	int ok = rd(0x03d02214) == 0xc0de0001u, memok = rd(test) == 0xcafef00du;
	con_str("GPU5 RESULT", ok ? (memok ? "CP runs: reg + memory write OK" : "CP runs: reg OK, memory write not seen")
				  : "CP did not execute the test");
	if (!memok)
		cp_dump("after test", c);
#if defined(RUN_ZAP)
	if (ok && memok)
		gpu6_zap(ring, &wptr, test, c);
#endif
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
#if defined(RUN_CP)
	gpu5_cp();
#endif
	/* drop the OOB: the GMU may collapse GX again; the M3 is put back in reset by the caller.
	 * Not when the GPU is handed to xnu: the OOB stays held so GX stays powered. */
	if (!gpu_up)
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
#if !defined(GPU_STAGES)
		con_str("PHOTO", "page 2 now: page 3 in 10 s");
		udelay(10000000);
#endif
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
#if defined(RUN_HFI)
		if (us != ~0u) {
			hfi_pa = hfi;
			gpu3_hfi();
			if (!gpu_up)
				wr(0x03d7e000, 1);	/* stop the M3 (kept running when handed to xnu) */
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

/* Normal boot (DIAG=gpuboot with an xnu payload): the full bring-up of probes 2-6, GPU left running for xnu.
 * Returns 1 when the handoff page is valid. */
int gpu_boot(void)
{
	wr64(HANDOFF, 0);		/* no stale handoff from an earlier boot */
	__asm__ volatile("dsb sy" ::: "memory");
	gpu2_probe();
	return gpu_up;
}
