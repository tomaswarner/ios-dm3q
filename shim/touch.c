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

static uint32_t last_fail_status, last_fail_irq, last_fail_fifo;

/* One full-duplex FIFO transfer of n (<= 64) bytes, CS held low around it. */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, unsigned n)
{
	if (!wait_clear(SE + SE_STATUS, 1u, 100000))
		return -1;
	wr(SE + SE_M_IRQ_CLR, 0xffffffffu);
	wr(SE + SE_SPI_TXLEN, n);
	wr(SE + SE_SPI_RXLEN, n);
	wr(SE + SE_TX_WM, 1);
	cs(0);
	udelay(2);
	wr(SE + SE_M_CMD, SPI_TX_RX << 27);
	unsigned sent = 0, got = 0;
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
	wr(SE + SE_M_IRQ_EN, rd(SE + SE_M_IRQ_EN) | 1u | (1u << 3) | (1u << 5) | (1u << 26) | (1u << 27) | (1u << 30));
	wr(SE + SE_DMA_MODE, 0);
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
	con_hex2("SPI FAIL", "st/irq=", last_fail_status << 16 | (last_fail_irq & 0xffff), "fifo t/r=", last_fail_fifo);
	con_hex2("SPI FAIL", "irqhi=", last_fail_irq >> 16, "irqen=", rd(SE + SE_M_IRQ_EN));
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
