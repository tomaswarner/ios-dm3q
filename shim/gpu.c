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
	con_hex("S6 CX GDSC hw", rd(GPUCC_CX_GDSC_HW));
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
		if (type == 0 && size && addr + size <= 0x4000)
			dst = GMU_ITCM + addr;
		else if (type == 0 && size && addr >= 0x10004000u && addr + size <= 0x10008000u)
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
	s8_ident();
	s9_tcm();
	s10_scm();
	char line[24] = "S1-10 ";
	for (int i = 1; i <= 10; i++)
		line[5 + i] = results[i];
	line[16] = 0;
	con_str("GPU1 RESULT", line);
	con_hex("GPU1 failed steps", (uint32_t)failed);
}
