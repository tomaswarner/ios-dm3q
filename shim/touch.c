/*
 * DM3Q (S23 Ultra) touch probe: STMicro FTS (fts2ba61y) on QUP1 SE4 SPI.
 * Bus: GENI SE at 0xa90000 (qupv3_se4_spi), FIFO mode, SPI mode 0, 8-bit words,
 * CS = TLMM GPIO 47 driven by hand (sec,gpio_spi_cs), IRQ = GPIO 25.
 * Register layout: Linux spi-geni-qcom.c / geni-se.h and sweet_touch.c (0060/0063).
 * STM framing: Samsung drivers/input/sec_input/stm_spi/stm_spi.c (S23 kernel):
 *   write: C0 cmd data... (raw for A0/A2/A4/A7/FA/FB/D0)
 *   read:  write C0 reg; then D1 00 00 + dummy + data   (raw 87 for events)
 * Rails (S2DOS05 LDO2/LDO3) are regulator-boot-on: left on by the bootloader.
 */
#include <stdint.h>
#include "console.h"
#include "touch.h"

#define GCC		0x00100000UL
#define TLMM		0x0f100000UL
#define SE		0x00a90000UL

#define GCC_VOTE	(GCC + 0x52008)	/* wrap1 core_2x 18, core 19, m_ahb 20, s_ahb 21, s4 26 */
#define GCC_S4_RCG	(GCC + 0x184f0)

#define SE_FW_REV	0x068
#define SE_STATUS	0x040
#define SE_MCLK_CFG	0x048
#define SE_CLK_SEL	0x07c
#define SE_DMA_MODE	0x258
#define SE_TX_PACK0	0x260
#define SE_TX_PACK1	0x264
#define SE_RX_PACK0	0x284
#define SE_RX_PACK1	0x288
#define SE_SPI_CPHA	0x224
#define SE_SPI_CPOL	0x230
#define SE_SPI_DEMUX	0x250
#define SE_SPI_TRANS	0x25c
#define SE_SPI_WORDLEN	0x268
#define SE_SPI_TXLEN	0x26c
#define SE_SPI_RXLEN	0x270
#define SE_M_CMD	0x600
#define SE_M_CTRL	0x604
#define SE_M_IRQ	0x610
#define SE_M_IRQ_CLR	0x618
#define SE_TXFIFO	0x700
#define SE_RXFIFO	0x780
#define SE_TXFIFO_ST	0x800
#define SE_RXFIFO_ST	0x804
#define SE_TX_WM	0x80c
#define SE_HW_PARAM0	0xe24
#define SE_FORCE_DEF	0x020
#define SE_OUTPUT_CTRL	0x024
#define SE_CGC_CTRL	0x028
#define SE_M_IRQ_EN	0x614
#define SE_S_IRQ_CLR	0x648
#define SE_GSI_EV_EN	0xe18
#define SE_IRQ_EN	0xe1c
#define SE_DMA_GEN_CFG	0xe30

#define M_DONE		(1u << 0)
#define M_TX_WM		(1u << 30)
#define SPI_TX_RX	7u
/* geni_se_config_packing(8, 4, msb_first): same words as sweet_touch.c */
#define PACK0		0x0007f8feu
#define PACK1		0x000ffefeu

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

static int wait_clear(uintptr_t a, uint32_t mask, uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (rd(a) & mask)
		if (ticks() > end)
			return 0;
	return 1;
}

/* TLMM: CFG at pin*0x1000 (pull [1:0], func [5:2], drive [8:6], oe 9), IO at +4 (in 0, out 1) */
static void gpio_cfg(unsigned pin, unsigned func, unsigned drive_code, int out, int high)
{
	uintptr_t cfg = TLMM + pin * 0x1000UL, io = cfg + 4;
	if (out)
		wr(io, (rd(io) & ~2u) | (high ? 2u : 0u));
	uint32_t v = rd(cfg) & ~0x3ffu;
	v |= (func << 2) | (drive_code << 6) | (out ? 0x200u : 0u);	/* bias-disable: pull 0 */
	wr(cfg, v);
}

static void cs(int high)
{
	uintptr_t io = TLMM + 47 * 0x1000UL + 4;
	wr(io, (rd(io) & ~2u) | (high ? 2u : 0u));
}


/*
 * GPI DMA (GSI) for SE4: dm3q reads GENI_IF_DISABLE_RO = 1 (FIFO interface
 * fused off), so SPI only runs through gpi_dma1 (touch2-4 probes).  Minimal
 * polled port of Linux drivers/dma/qcom/gpi.c + spi-geni-qcom.c setup_gsi_xfer:
 * one GPII, TX chan 0 / RX chan 1, one event ring, no interrupts.
 */
#define GPI_EE		0x009f0000UL	/* gpi-dma@a00000 minus ee offset 0x10000 (upstream sm8550 match data) */
#define GPII		1		/* gpii-mask 0x1e: HLOS owns 1-4 */
#define GPI_SEID	4
#define GPI_PROTO_SPI	1
#define GPII_OFF	(0x4000UL * GPII)
#define CH_CNTXT(k)	(GPI_EE + 0x20000 + GPII_OFF + 0x80 * (k))
#define CH_QOS(k)	(CH_CNTXT(k) + 0x5c)
#define CH_SCRATCH(k)	(CH_CNTXT(k) + 0x60)
#define EV_CNTXT	(GPI_EE + 0x21000 + GPII_OFF)
#define CH_DB(k)	(GPI_EE + 0x22000 + GPII_OFF + 0x8 * (k))
#define EV_DB		(GPI_EE + 0x22100 + GPII_OFF)
#define CH_CMD		(GPI_EE + 0x23008 + GPII_OFF)
#define EV_CMD		(GPI_EE + 0x23010 + GPII_OFF)
#define CNTXT_TYPE_IRQ	(GPI_EE + 0x23080 + GPII_OFF)
#define SRC_CH_IRQ	(GPI_EE + 0x23090 + GPII_OFF)
#define SRC_EV_IRQ	(GPI_EE + 0x23094 + GPII_OFF)
#define SRC_CH_IRQ_CLR	(GPI_EE + 0x230a0 + GPII_OFF)
#define SRC_EV_IRQ_CLR	(GPI_EE + 0x230a4 + GPII_OFF)
#define SRC_IEOB_CLR	(GPI_EE + 0x230c0 + GPII_OFF)
#define GLOB_IRQ_STTS	(GPI_EE + 0x23100 + GPII_OFF)
#define GPII_ERR	(GPI_EE + 0x23200 + GPII_OFF)

#define CMD_ALLOC	0
#define CMD_START	1
#define CMD_STOP	2
#define CMD_RESET	9
#define CMD_DEALLOC	10

#define TRE_DMA		0x10u
#define TRE_GO		0x20u
#define TRE_CONFIG0	0x22u
#define TF_CHAIN	(1u << 0)
#define TF_IEOT		(1u << 9)
#define TF_LINK		(1u << 11)
#define EV_XFER_COMPL	0x22u

#define CH_EL		64
#define EV_EL		128
static uint32_t ch_ring[2][CH_EL * 4] __attribute__((aligned(CH_EL * 16)));
static uint32_t ev_ring[EV_EL * 4] __attribute__((aligned(EV_EL * 16)));
static uint8_t dma_tx[64] __attribute__((aligned(64))), dma_rx[64] __attribute__((aligned(64)));
static unsigned ch_wp[2], ev_rp;
static uint32_t gpi_last_ev[4], gpi_err_info, gpi_events;

static void dcache_flush(const void *p, unsigned n)	/* clean+invalidate (no-op effect with the MMU off) */
{
	for (uintptr_t a = (uintptr_t)p & ~63ul; a < (uintptr_t)p + n; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" : : : "memory");
}

static uint32_t ch_state(int k) { return (rd(CH_CNTXT(k)) >> 20) & 0xf; }
static uint32_t ev_state(void) { return (rd(EV_CNTXT) >> 20) & 0xf; }

/* channel/event command; completion = context state reaching want (or just time when want < 0) */
static int gpi_cmd(uintptr_t reg, uint32_t op, uint32_t chid, int ev, int want)
{
	wr(reg, op << 24 | chid);
	uint64_t end = ticks() + 19200000 / 20;		/* 50 ms */
	while (ticks() < end) {
		uint32_t st = ev ? ev_state() : ch_state(chid);
		if (want >= 0 && st == (uint32_t)want)
			break;
	}
	wr(SRC_CH_IRQ_CLR, rd(SRC_CH_IRQ));
	wr(SRC_EV_IRQ_CLR, rd(SRC_EV_IRQ));
	if (want < 0)
		return 0;
	return (ev ? ev_state() : ch_state(chid)) == (uint32_t)want ? 0 : -1;
}

static uint32_t gpi_init_rc;

/* returns 0, or a step code: 0x1x ev alloc, 0x2x ch alloc, 0x3x ch start */
static uint32_t gpi_init(void)
{
	/* leftovers from the bootloader: stop / reset / de-alloc */
	for (int k = 0; k < 2; k++) {
		if (ch_state(k) == 2)
			gpi_cmd(CH_CMD, CMD_STOP, k, 0, 3);
		if (ch_state(k)) {
			gpi_cmd(CH_CMD, CMD_RESET, k, 0, 1);
			gpi_cmd(CH_CMD, CMD_DEALLOC, k, 0, 0);
		}
	}
	if (ev_state()) {
		gpi_cmd(EV_CMD, CMD_RESET, 0, 1, -1);
		gpi_cmd(EV_CMD, CMD_DEALLOC, 0, 1, 0);
	}
	for (unsigned i = 0; i < EV_EL * 4; i++)
		ev_ring[i] = 0;
	for (unsigned i = 0; i < CH_EL * 4; i++)
		ch_ring[0][i] = ch_ring[1][i] = 0;
	dcache_flush(ev_ring, sizeof(ev_ring));
	dcache_flush(ch_ring, sizeof(ch_ring));

	/* gpi_config_interrupts(DEFAULT): harmless with polling, keeps state like Linux */
	wr(CNTXT_TYPE_IRQ + 8, 0x7f);			/* TYPE_IRQ_MSK */
	wr(GPI_EE + 0x230b8 + GPII_OFF, 1);		/* SRC_IEOB_IRQ_MSK */
	wr(GPI_EE + 0x23098 + GPII_OFF, 3);		/* SRC_CH_IRQ_MSK */
	wr(GPI_EE + 0x2309c + GPII_OFF, 1);		/* SRC_EV_CH_IRQ_MSK */
	wr(GPI_EE + 0x23108 + GPII_OFF, 0xf);		/* GLOB_IRQ_EN */
	wr(GPI_EE + 0x23120 + GPII_OFF, 0xf);		/* GPII_IRQ_EN */
	wr(GPI_EE + 0x23180 + GPII_OFF, 1);		/* INTSET */

	/* gpi_alloc_ev_chan */
	if (gpi_cmd(EV_CMD, CMD_ALLOC, 0, 1, 1))
		return 0x10 | ev_state();
	uintptr_t evp = (uintptr_t)ev_ring;
	wr(EV_CNTXT + 0x00, 16u << 24 | 1u << 16 | 2u);	/* el size, INTTYPE_IRQ, GPI_EV */
	wr(EV_CNTXT + 0x04, sizeof(ev_ring));
	wr(EV_CNTXT + 0x08, (uint32_t)evp);
	wr(EV_CNTXT + 0x0c, (uint32_t)(evp >> 32));
	wr(EV_DB + 4, (uint32_t)(evp >> 32));
	wr(EV_CNTXT + 0x20, 0);
	wr(EV_CNTXT + 0x28, 0);
	wr(EV_CNTXT + 0x2c, 0);
	wr(EV_CNTXT + 0x30, 0);
	wr(EV_CNTXT + 0x34, 0);
	ev_rp = 0;
	wr(EV_DB, (uint32_t)(evp + sizeof(ev_ring) - 16));

	/* gpi_alloc_chan x2: chan 0 = TX (dir out), chan 1 = RX (dir in) */
	for (int k = 0; k < 2; k++) {
		if (gpi_cmd(CH_CMD, CMD_ALLOC, k, 0, 1))
			return 0x20 | k << 3 | ch_state(k);
		uintptr_t rp = (uintptr_t)ch_ring[k];
		wr(CH_CNTXT(k) + 0x00, 16u << 24 | (k == 0 ? 1u : 0u) << 3 | 2u);
		wr(CH_CNTXT(k) + 0x04, CH_EL * 16);
		wr(CH_CNTXT(k) + 0x08, (uint32_t)rp);
		wr(CH_CNTXT(k) + 0x0c, (uint32_t)(rp >> 32));
		wr(CH_DB(k) + 4, (uint32_t)(rp >> 32));
		wr(CH_SCRATCH(k) + 0x0, (uint32_t)(!k) << 16 | GPI_PROTO_SPI << 4 | GPI_SEID);
		wr(CH_SCRATCH(k) + 0x4, 0);
		wr(CH_SCRATCH(k) + 0x8, 0);
		wr(CH_SCRATCH(k) + 0xc, 0);
		wr(CH_QOS(k), 1);
		ch_wp[k] = 0;
	}
	for (int k = 0; k < 2; k++)
		if (gpi_cmd(CH_CMD, CMD_START, k, 0, 2))
			return 0x30 | k << 3 | ch_state(k);
	return 0;
}

static void ch_queue(int k, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
	uint32_t *t = &ch_ring[k][ch_wp[k] * 4];
	t[0] = d0; t[1] = d1; t[2] = d2; t[3] = d3;
	dcache_flush(t, 16);
	ch_wp[k] = (ch_wp[k] + 1) % CH_EL;
}

static void ch_ring_db(int k)
{
	__asm__ volatile("dsb sy" : : : "memory");
	wr(CH_DB(k), (uint32_t)((uintptr_t)ch_ring[k] + ch_wp[k] * 16));
}

/* consume events until both channels reported a transfer completion; bit k = chan k done */
static int gpi_wait(uint64_t end)
{
	int done = 0;
	while (ticks() < end && done != 3) {
		uint32_t hw = rd(EV_CNTXT + 0x10);
		unsigned hw_rp = (hw - (uint32_t)(uintptr_t)ev_ring) / 16 % EV_EL;
		while (ev_rp != hw_rp) {
			uint32_t *e = &ev_ring[ev_rp * 4];
			dcache_flush(e, 16);
			for (int i = 0; i < 4; i++)
				gpi_last_ev[i] = e[i];
			gpi_events++;
			uint32_t type = (e[3] >> 16) & 0xff, chid = e[3] >> 24, code = e[2] >> 24;
			if (type == EV_XFER_COMPL && chid < 2) {
				if (code == 16)			/* MSM_GPI_TCE_UNEXP_ERR */
					return -(int)(0x100 | chid);
				done |= 1 << chid;
			}
			/* recycle: give the slot back to the hardware */
			wr(EV_DB, (uint32_t)((uintptr_t)ev_ring + ev_rp * 16));
			ev_rp = (ev_rp + 1) % EV_EL;
		}
	}
	return done;
}

static int gpi_ok;

/* apps SMMU (MMU-500 @ 0x15000000): how is this stream routed? type 0 = translate, 1 = bypass, 2 = fault */
static void smmu_sid_check(uint32_t sid)
{
	const uintptr_t smmu = 0x15000000UL;
	uint32_t nsmr = rd(smmu + 0x20) & 0xff, found = 0;
	for (uint32_t n = 0; n < nsmr; n++) {
		uint32_t smr = rd(smmu + 0x800 + 4 * n);
		if (!(smr >> 31) || ((smr ^ sid) & ~(smr >> 16) & 0x7fff))
			continue;
		uint32_t s2cr = rd(smmu + 0xc00 + 4 * n);
		con_hex2("SMMU", "gpi smr=", smr, "s2cr=", s2cr);
		found++;
	}
	if (!found)
		con_hex2("SMMU", "gpi sid unmatched cr0=", rd(smmu), "sid=", sid);
}

/*
 * cr0.USFCFG = 1: unmatched streams fault (touch5 rebooted on the first GPI
 * fetch).  Android maps gpi_dma1 (iommus <&apps_smmu 0xb6 0>) at runtime; the
 * bootloader does not.  Gunyah's virtual SMMU turns an S2CR bypass into fault
 * (touch6), so copy the UFS stream's S2CR instead: translate through its
 * context bank, which has SCTLR.M = 0 (stage 1 off, Gunyah's stage 2 only).
 * Returns 1 when the entry reads back as non-fault.
 */
static int smmu_route_sid_like(uint32_t sid, uint32_t like)
{
	const uintptr_t smmu = 0x15000000UL;
	uint32_t nsmr = rd(smmu + 0x20) & 0xff, slot = ~0u, ref = ~0u;
	for (uint32_t n = 0; n < nsmr; n++) {
		uint32_t smr = rd(smmu + 0x800 + 4 * n);
		if (!(smr >> 31))
			continue;
		if (!((smr ^ sid) & ~(smr >> 16) & 0x7fff) && slot == ~0u)
			slot = n;
		if (!((smr ^ like) & ~(smr >> 16) & 0x7fff) && ref == ~0u)
			ref = n;
	}
	if (ref == ~0u)
		return 0;
	uint32_t s2cr_ref = rd(smmu + 0xc00 + 4 * ref);
	if (slot == ~0u)
		for (uint32_t n = nsmr; n-- > 0; )		/* free slots from the top */
			if (!(rd(smmu + 0x800 + 4 * n) >> 31)) {
				slot = n;
				break;
			}
	if (slot == ~0u)
		return 0;
	wr(smmu + 0xc00 + 4 * slot, s2cr_ref);
	wr(smmu + 0x800 + 4 * slot, 1u << 31 | sid);	/* SMR: valid, mask 0 */
	__asm__ volatile("dsb sy" : : : "memory");
	uint32_t s2cr = rd(smmu + 0xc00 + 4 * slot);
	con_hex2("SMMU", "ref s2cr=", ref << 24 | (s2cr_ref & 0xffffff), "new s2cr=", slot << 24 | (s2cr & 0xffffff));
	return ((s2cr >> 16) & 3) != 2;
}

static uint32_t last_fail_status, last_fail_irq, last_fail_fifo, after_tx_status, after_tx_fifo;

/* One full-duplex FIFO transfer of n (<= 64) bytes, CS held low around it. */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	if (gpi_ok) {
		for (unsigned i = 0; i < n; i++) {
			dma_tx[i] = tx[i];
			dma_rx[i] = 0xee;
		}
		dcache_flush(dma_tx, sizeof(dma_tx));
		dcache_flush(dma_rx, sizeof(dma_rx));
		cs(0);
		udelay(2);
		uintptr_t txp = (uintptr_t)dma_tx, rxp = (uintptr_t)dma_rx;
		/* RX first (Linux issues rx before tx) */
		static int first = 1;
		if (first) {
			con_hex2("GPI STEP", "1 rx tre at ", (uint32_t)(uintptr_t)ch_ring[1], "buf ", (uint32_t)rxp);
			udelay(3000000);
		}
		ch_queue(1, (uint32_t)rxp, (uint32_t)(rxp >> 32), n, TRE_DMA << 16 | TF_IEOT);
		ch_ring_db(1);
		if (first) {
			udelay(3000000);
			con_hex2("GPI STEP", "2 rx db ok st=", ch_state(0) << 8 | ch_state(1) << 4 | ev_state(), "glob=", rd(GLOB_IRQ_STTS));
			udelay(3000000);
		}
		/* TX: CONFIG0 (8-bit word, mode 0, packing, clk src 0 div 2), GO duplex, DMA */
		ch_queue(0, (8 - 4) | 1u << 24 | 1u << 25, 0, 2u, TRE_CONFIG0 << 16 | TF_CHAIN);
		ch_queue(0, 3u /* SPI_DUPLEX */, 0, 0, TRE_GO << 16 | TF_CHAIN | TF_LINK);
		ch_queue(0, (uint32_t)txp, (uint32_t)(txp >> 32), n, TRE_DMA << 16 | TF_IEOT);
		ch_ring_db(0);
		if (first) {
			udelay(3000000);
			con_hex2("GPI STEP", "3 tx db ok st=", ch_state(0) << 8 | ch_state(1) << 4 | ev_state(), "evrp=", rd(EV_CNTXT + 0x10));
			first = 0;
		}
		int done = gpi_wait(ticks() + 19200000 / 10);
		udelay(2);
		cs(1);
		dcache_flush(dma_rx, sizeof(dma_rx));
		for (unsigned i = 0; i < n; i++)
			rx[i] = dma_rx[i];
		if (done != 3) {
			last_fail_status = (uint32_t)done;
			last_fail_irq = ch_state(0) << 4 | ch_state(1);
			last_fail_fifo = rd(GLOB_IRQ_STTS) << 16 | (rd(CNTXT_TYPE_IRQ) & 0xffff);
			gpi_err_info = rd(GPII_ERR);
			return done < 0 ? done : -2;
		}
		return 0;
	}
	if (!wait_clear(SE + SE_STATUS, 1u, 100000))
		return -1;
	wr(SE + SE_M_IRQ_CLR, 0xffffffffu);
	wr(SE + SE_SPI_TXLEN, n);
	wr(SE + SE_SPI_RXLEN, n);
	cs(0);
	udelay(2);
	wr(SE + SE_M_CMD, SPI_TX_RX << 27);
	unsigned sent = 0, got = 0;
	while (sent < n && sent < 64) {			/* 16-word FIFO: the whole message */
		uint32_t w = 0;
		for (unsigned j = 0; j < 4 && sent + j < n; j++)
			w |= (uint32_t)tx[sent + j] << (8 * j);
		wr(SE + SE_TXFIFO, w);
		sent += 4;
	}
	after_tx_status = rd(SE + SE_STATUS);
	after_tx_fifo = rd(SE + SE_TXFIFO_ST);
	uint64_t end = ticks() + 19200000 / 10;		/* 100 ms */
	int rc = -2;
	while (ticks() < end) {
		uint32_t irq = rd(SE + SE_M_IRQ);
		if ((irq & M_TX_WM) && sent < n) {
			while (sent < n) {		/* FIFO >= 16 words: a whole message fits */
				uint32_t w = 0;
				for (unsigned j = 0; j < 4 && sent + j < n; j++)
					w |= (uint32_t)tx[sent + j] << (8 * j);
				wr(SE + SE_TXFIFO, w);
				sent += 4;
			}
			wr(SE + SE_TX_WM, 0);
			wr(SE + SE_M_IRQ_CLR, M_TX_WM);
		}
		uint32_t cnt = rd(SE + SE_RXFIFO_ST) & 0x1ffffffu;
		while (cnt--) {
			uint32_t w = rd(SE + SE_RXFIFO);
			for (unsigned j = 0; j < 4 && got < n; j++)
				rx[got++] = (uint8_t)(w >> (8 * j));
		}
		if (irq & M_DONE) {
			cnt = rd(SE + SE_RXFIFO_ST) & 0x1ffffffu;
			while (cnt--) {
				uint32_t w = rd(SE + SE_RXFIFO);
				for (unsigned j = 0; j < 4 && got < n; j++)
					rx[got++] = (uint8_t)(w >> (8 * j));
			}
			rc = got == n ? 0 : -3;
			break;
		}
	}
	if (rc) {
		last_fail_status = rd(SE + SE_STATUS);
		last_fail_irq = rd(SE + SE_M_IRQ);
		last_fail_fifo = (rd(SE + SE_TXFIFO_ST) & 0xffff) << 16 | (rd(SE + SE_RXFIFO_ST) & 0xffff);
		wr(SE + SE_M_CTRL, 2);			/* M_GENI_CMD_ABORT */
		wait_clear(SE + SE_STATUS, 1u, 20000);
		for (int i = 0; i < 64 && (rd(SE + SE_RXFIFO_ST) & 0x1ffffffu); i++)
			(void)rd(SE + SE_RXFIFO);
	}
	udelay(2);
	cs(1);
	wr(SE + SE_M_IRQ_CLR, 0xffffffffu);
	return rc;
}

static uint8_t tbuf[64], rbuf[64];

static int stm_write(const uint8_t *b, unsigned n)	/* already framed */
{
	for (unsigned i = 0; i < n; i++)
		tbuf[i] = b[i];
	return spi_xfer(tbuf, rbuf, n);
}

/* event read: raw 87, then 1 dummy byte; the 16 data bytes start at rx[2] */
static int stm_read_event(uint8_t *ev)
{
	for (unsigned i = 0; i < 18; i++)
		tbuf[i] = 0;
	tbuf[0] = 0x87;
	int rc = spi_xfer(tbuf, rbuf, 18);
	for (unsigned i = 0; i < 16; i++)
		ev[i] = rbuf[2 + i];
	return rc;
}

/* default read: write C0 reg, then D1 00 00 + dummy; data from rx[4] */
static int stm_read_reg(uint8_t reg, uint8_t *out, unsigned n)
{
	const uint8_t pre[2] = { 0xc0, reg };
	if (stm_write(pre, 2))
		return -1;
	for (unsigned i = 0; i < n + 4; i++)
		tbuf[i] = 0;
	tbuf[0] = 0xd1;
	int rc = spi_xfer(tbuf, rbuf, n + 4);
	for (unsigned i = 0; i < n; i++)
		out[i] = rbuf[4 + i];
	return rc;
}

static uint32_t be(const uint8_t *e, int a, int b, int c, int d)
{
	return (uint32_t)e[a] << 24 | (uint32_t)e[b] << 16 | (uint32_t)e[c] << 8 | e[d];
}

void touch_probe(void)
{
	con_str("TOUCH", "dm3q STM fts2ba61y probe (SPI SE4)");

	/* 1. clocks before touching the SE */
	wr(GCC_VOTE, rd(GCC_VOTE) | (0xfu << 18) | (1u << 26));
	/* only the SE clock is checked: the wrapper AHB branches are voted + HW-gated (BRANCH_HALT_VOTED),
	 * their halt bit does not show them on (dm3q touch1: s_ahb read as off while working) */
	int ok = wait_clear(GCC + 0x184e4, 1u << 31, 20000);
	con_hex2("TCLK", "vote=", rd(GCC_VOTE), "rcgcfg=", rd(GCC_S4_RCG + 4));
	con_hex2("TCLK", "s4cbcr=", rd(GCC + 0x184e4), "sahb=", rd(GCC + 0x23280));
	if (!ok)
		return;

	/* 2. SE identity */
	uint32_t fw = rd(SE + SE_FW_REV);
	con_hex2("SE4", "fwrev=", fw, "status=", rd(SE + SE_STATUS));
	con_hex2("SE4", "ifdis=", rd(SE + 0x064), "qupver=", rd(0x00ac0000 + 0x4));	/* GENI_IF_DISABLE_RO; QUPV3_HW_VER */
	con_hex2("SE4", "hwp0=", rd(SE + SE_HW_PARAM0), "clksel/mclk=", rd(SE + SE_CLK_SEL) << 16 | (rd(SE + SE_MCLK_CFG) & 0xffff));
	if (((fw >> 8) & 0xff) != 1) {
		con_str("SE4", "protocol is not SPI (1): stop");
		return;
	}

	/* 3. pins + SPI setup (mode 0, 8-bit, FIFO, ~9.6 MHz from a 19.2 MHz SE clock) */
	gpio_cfg(44, 1, 2, 0, 0);	/* miso */
	gpio_cfg(45, 1, 2, 0, 0);	/* mosi */
	gpio_cfg(46, 1, 2, 0, 0);	/* clk  */
	gpio_cfg(47, 0, 2, 1, 1);	/* CS as GPIO, high */
	gpio_cfg(25, 0, 0, 0, 0);	/* irq input */
	/* geni_se_io_init + geni_se_irq_clear + geni_se_select_fifo_mode (Linux) */
	wr(SE + SE_CGC_CTRL, rd(SE + SE_CGC_CTRL) | 0x7fu);
	wr(SE + SE_DMA_GEN_CFG, rd(SE + SE_DMA_GEN_CFG) | 0xfu);
	wr(SE + SE_OUTPUT_CTRL, 0x7fu);
	wr(SE + SE_FORCE_DEF, 1u);
	wr(SE + SE_GSI_EV_EN, 0);
	wr(SE + SE_M_IRQ_CLR, 0xffffffffu);
	wr(SE + SE_S_IRQ_CLR, 0xffffffffu);
	/* geni_se_select_gpi_mode */
	wr(SE + SE_IRQ_EN, 0);
	wr(SE + SE_M_IRQ_EN, rd(SE + SE_M_IRQ_EN) & ~(1u | (1u << 26) | (1u << 27) | (1u << 30)));
	wr(SE + SE_DMA_MODE, 1);
	wr(SE + SE_GSI_EV_EN, rd(SE + SE_GSI_EV_EN) | 0xfu);
	wr(SE + SE_SPI_CPHA, 0);
	wr(SE + SE_SPI_CPOL, 0);
	wr(SE + SE_SPI_DEMUX, 0);
	wr(SE + SE_SPI_TRANS, 0);
	wr(SE + SE_SPI_WORDLEN, 8 - 4);
	wr(SE + SE_TX_PACK0, PACK0);
	wr(SE + SE_TX_PACK1, PACK1);
	wr(SE + SE_RX_PACK0, PACK0);
	wr(SE + SE_RX_PACK1, PACK1);
	wr(SE + SE_CLK_SEL, 0);
	wr(SE + SE_MCLK_CFG, (2u << 4) | 1u);
	smmu_sid_check(0xb6);
	int smmu_ok = smmu_route_sid_like(0xb6, 0x60);	/* like UFS */
	smmu_sid_check(0xb6);
	gpi_init_rc = smmu_ok ? gpi_init() : 0xdead;
	gpi_ok = !gpi_init_rc;
	con_hex2("GPI", "init=", gpi_init_rc, "ch/ev st=", ch_state(0) << 8 | ch_state(1) << 4 | ev_state());
	if (gpi_ok) {
		con_str("GPI", "first DMA in 8 s: photograph now");
		udelay(8000000);
	}
	con_hex2("PINS", "irq25=", rd(TLMM + 25 * 0x1000UL + 4) & 1, "cs47=", rd(TLMM + 47 * 0x1000UL + 4));

	/* 4. STM: system reset, wait for READY (status event, type INFO, id 0) */
	static const uint8_t rst[6] = { 0xfa, 0x20, 0x00, 0x00, 0x24, 0x81 };
	int rc = stm_write(rst, 6);
	udelay(30000);
	uint8_t ev[16];
	int ready = 0, tries;
	for (tries = 0; tries < 60 && !ready; tries++) {
		rc = stm_read_event(ev);
		if (!rc && (ev[0] & 3) == 1 && ((ev[0] >> 2) & 0xf) == 2 && ev[1] == 0)
			ready = 1;
		else
			udelay(20000);
	}
	con_hex2("STM RST", "rc/ready=", (uint32_t)rc << 8 | ready, "tries=", tries);
	con_hex2("GPI FAIL", "done=", last_fail_status, "chst=", last_fail_irq);
	con_hex2("GPI FAIL", "glob/type=", last_fail_fifo, "err=", gpi_err_info);
	con_hex2("GPI EV", "n=", gpi_events, "w3=", gpi_last_ev[3]);
	con_hex2("GPI EV", "w0=", gpi_last_ev[0], "w2=", gpi_last_ev[2]);
	con_hex2("STM EV", "", be(ev, 0, 1, 2, 3), "", be(ev, 4, 5, 6, 7));
	uint8_t id[8] = { 0 };
	rc = stm_read_reg(0x22, id, 5);		/* STM_TS_READ_DEVICE_ID */
	con_hex2("STM ID", "rc=", (uint32_t)rc, "id=", be(id, 0, 1, 2, 3));
	uint8_t ver[8] = { 0 };
	rc = stm_read_reg(0x24, ver, 8);	/* STM_TS_READ_FW_VERSION */
	con_hex2("STM FW", "", be(ver, 0, 1, 2, 3), "", be(ver, 4, 5, 6, 7));

	/* 5. scan on (A0 00 01: mutual + self), then show touches for ~30 s */
	static const uint8_t scan[3] = { 0xa0, 0x00, 0x01 };
	rc = stm_write(scan, 3);
	con_hex2("STM SCAN", "rc=", (uint32_t)rc, "", 0);
	con_str("TOUCH", "touch the screen now (30 s)");
	int line = con_get_row(), shown = 0, n = 0;
	uint64_t end = ticks() + 19200000ULL * 30;
	while (ticks() < end) {
		if (stm_read_event(ev)) {
			udelay(5000);
			continue;
		}
		if ((ev[0] & 3) == 0 && ev[0]) {	/* coordinate event */
			uint32_t tid = (ev[0] >> 2) & 0xf, act = ev[0] >> 6;
			uint32_t x = (uint32_t)ev[1] << 4 | (ev[3] >> 4), y = (uint32_t)ev[2] << 4 | (ev[3] & 0xf);
			n++;
			con_set_row(line + (shown % 20));
			con_hex2("TOUCH", "n/id/act=", (uint32_t)n << 16 | tid << 8 | act, "x/y=", x << 16 | y);
			shown++;
		} else
			udelay(5000);
	}
	con_set_row(line + 21);
	con_hex2("TOUCH", "events=", (uint32_t)n, "", 0);
}
