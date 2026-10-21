/*
 * DM3Q (Galaxy S23 Ultra) bring-up diagnostic: replaces shim.c, boots no XNU.
 * One boot answers the questions the port depends on, read-only:
 *   - does the ABL splash framebuffer at 0xb8000000 stay live, and at which
 *     size/pixel order (calibration lines and colour bars)
 *   - exception level, CPU ids and PAC, CNTFRQ
 *   - the DRAM banks ABL writes into the DTB /memory node (8 GB model)
 *   - PSCI/SMCCC versions and the hypervisor UID (Gunyah expected)
 *   - GIC distributor/redistributor ids (do the reads trap under Gunyah?)
 *   - how long until something (Gunyah watchdog?) resets the SoC: the
 *     uptime line keeps counting until then.
 * It never resets by itself and writes nothing but the framebuffer.
 */
#include <stdint.h>
#include "console.h"

#define GICD_BASE	0x17100000UL
#define GICR_BASE	0x17180000UL
#define GICR_PE_SIZE	0x40000UL

#define PSCI_VERSION	0x84000000UL
#define SMCCC_VERSION	0x80000000UL
#define VENDOR_HYP_UID	0x8600ff01UL

static const char *stage;	/* set at runtime: no absolute pointers in data */

struct res { uint64_t a0, a1, a2, a3; };

#ifdef DIAG_HOST	/* diag_host.c: run on the Mac against a real DTB */
uint64_t host_rsr(const char *r);
uint32_t host_rd32(uintptr_t a);
struct res host_smccc(int hvc, uint64_t fn);
#define rsr(r) host_rsr(r)
#define rd32(a) host_rd32(a)
#define smccc(h, fn, a1) host_smccc(h, fn)
#define wfe() return
#else
#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wfe() __asm__ volatile("wfe")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

static struct res smccc(int hvc, uint64_t fn, uint64_t a1)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	if (hvc)
		__asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
	else
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
	return (struct res){ x0, x1, x2, x3 };
}
#endif

/* ---- framebuffer calibration ---- */

static void fill(int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h && y < FB_H; y++)
		for (int x = x0; x < x0 + w && x < FB_W; x++)
			con_fb[y * (FB_STRIDE / 4) + x] = c;
}

static void calibrate(void)
{
	/* lines where a 1080x2316 and a 1440x3088 mode would end */
	fill(1076, 0, 4, FB_H, WHITE);
	fill(FB_W - 4, 0, 4, FB_H, YELLOW);
	fill(0, 2312, FB_W, 4, WHITE);
	fill(0, FB_H - 4, FB_W, 4, YELLOW);
	/* pixel order: these must read red, green, blue left to right */
	fill(24, 2100, 300, 150, 0xffff0000U);
	fill(344, 2100, 300, 150, 0xff00ff00U);
	fill(664, 2100, 300, 150, 0xff0000ffU);
}

/* ---- FDT: /memory reg and /chosen bootargs ---- */

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static int prefix(const char *s, const char *p)
{
	while (*p && *s == *p)
		s++, p++;
	return !*p && (!*s || *s == '@');
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

static void show_bootargs(uintptr_t val, uint32_t len)
{
	/* ABL's cmdline is long; 50 columns fit beside the label, at most 8 lines */
	char buf[51];
	uint32_t i = 0;
	for (int line = 0; line < 8 && i < len; line++) {
		int n = 0;
		while (n < 50 && i < len && ((volatile char *)val)[i])
			buf[n++] = ((volatile char *)val)[i++];
		buf[n] = 0;
		if (!n)
			break;
		con_str(line ? "" : "CMDLINE", buf);
	}
}

static void show_fdt(uintptr_t fdt)
{
	stage = "fdt";
	con_hex("FDT", fdt);
	if (!fdt || be32(fdt) != FDT_MAGIC) {
		con_str("FDT", "bad magic");
		return;
	}
	con_hex2("FDT", "size=", be32(fdt + 4), "ver=", be32(fdt + 20));
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, node = 0, banks = 0;	/* node: 1 memory, 2 chosen */
	uint32_t acells = 2, scells = 2;
	uint64_t total = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			depth++;
			node = depth != 2 ? 0 : prefix(name, "memory") ? 1 : streq(name, "chosen") ? 2 : 0;
			p = align4(skip_str(p));
		} else if (tok == FDT_END_NODE) {
			if (--depth == 0)
				break;
			node = 0;
		} else if (tok == FDT_PROP) {
			uint32_t len = be32(p);
			const char *pname = (const char *)(strings + be32(p + 4));
			uintptr_t val = p + 8;
			if (depth == 1 && streq(pname, "#address-cells"))
				acells = be32(val);
			else if (depth == 1 && streq(pname, "#size-cells"))
				scells = be32(val);
			else if (node == 1 && streq(pname, "reg")) {
				uint32_t step = 4 * (acells + scells);
				for (uint32_t o = 0; step && o + step <= len; o += step) {
					uint64_t base = be32(val + o), size = be32(val + o + 4 * acells);
					if (acells == 2)
						base = base << 32 | be32(val + o + 4);
					if (scells == 2)
						size = size << 32 | be32(val + o + 4 * acells + 4);
					if (!size)
						continue;
					total += size;
					if (banks++ < 12)
						con_hex2("MEM", "base=", base >> 12, "pages=", size >> 12);
				}
			} else if (node == 2 && streq(pname, "bootargs"))
				show_bootargs(val, len);
			p = align4(val + len);
		} else if (tok != FDT_NOP)
			break;
	}
	con_hex2("MEM", "banks=", banks, "MiB=", total >> 20);
}

/* ---- exceptions: show and halt (no reset: photograph the screen) ---- */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_skip();
	con_str("EXC", stage ? stage : "?");
	con_hex("SLOT", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X0", regs[0]);
	con_str("HALT", "photograph this screen");
	for (;;)
		wfe();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		wfe();
}

/* ---- main ---- */

static void show_gic(void)
{
	stage = "gicd read";
	con_hex2("GICD", "typer=", rd32(GICD_BASE + 0x4), "iidr=", rd32(GICD_BASE + 0x8));
	con_hex2("GICD", "ctlr=", rd32(GICD_BASE + 0x0), "pidr2=", rd32(GICD_BASE + 0xffe8));
	stage = "gicr read";
	/* GICR_TYPER[63:32] = affinity, bit 4 = last frame */
	for (int i = 0; i < 8; i++) {
		uintptr_t r = GICR_BASE + i * GICR_PE_SIZE;
		uint32_t lo = rd32(r + 0x8), hi = rd32(r + 0xc);
		con_hex2("GICR", "typer=", lo, "aff=", hi);
		if (lo & 0x10)
			break;
	}
}

void shim_main(uintptr_t fdt)
{
	stage = "clear";
	con_clear(0xff202040U);
	calibrate();
	con_str("DM3Q", "S23 Ultra diag v1 (no XNU)");
	con_skip();

	stage = "cpu";
	con_hex("EL", rsr("CurrentEL") >> 2);
	con_hex("MIDR", rsr("midr_el1"));
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex("SCTLR", rsr("sctlr_el1"));
	con_hex("ISAR1", rsr("id_aa64isar1_el1"));	/* APA/API/GPA/GPI = PAC */
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));	/* granules, PA size */
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_skip();

	show_fdt(fdt);
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smccc(0, PSCI_VERSION, 0).a0, "smccc=", smccc(0, SMCCC_VERSION, 0).a0);
	stage = "hyp uid";
	struct res uid = smccc(1, VENDOR_HYP_UID, 0);
	con_hex2("HYPUID", "", uid.a0, "", uid.a1);
	con_hex2("", "", uid.a2, "", uid.a3);
	con_skip();

	show_gic();
	con_skip();

	stage = "idle";
	con_str("DONE", "now counting; note when it resets");
	int line = con_get_row();
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	for (;;) {
		uint64_t s = (rsr("cntvct_el0") - t0) / f;
		con_set_row(line);
		con_hex2("UPTIME", "s=", s, "cnt=", rsr("cntvct_el0") >> 32);
#ifdef DIAG_HOST
		if (s >= 3)
			return;
#endif
		uint64_t next = t0 + (s + 1) * f;
		while (rsr("cntvct_el0") < next)
			;
	}
}
