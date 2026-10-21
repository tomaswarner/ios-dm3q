/*
 * SM8550 QMP UFS PHY power-on for the DM3Q (S23 Ultra) shim.
 * Tables and sequence from U-Boot drivers/phy/qcom/phy-qcom-qmp-ufs.c
 * (sm8550_ufsphy_cfg, qmp_ufs_power_on; GPL-2.0, Linaro), register offsets
 * resolved from phy-qcom-qmp-{qserdes-com-v6,qserdes-txrx-ufs-v6,pcs-ufs-v6}.h.
 * PHY: ufs_mem_phy at 0x1d80000 (qmp_ufs_offsets_v6 layout).
 */
#include <stdint.h>
#include "ufsphy.h"

#define PHY_BASE	0x01d80000UL
#define SERDES		(PHY_BASE + 0x0000)
#define PCS		(PHY_BASE + 0x0400)
#define TX1		(PHY_BASE + 0x1000)
#define RX1		(PHY_BASE + 0x1200)
#define TX2		(PHY_BASE + 0x1800)
#define RX2		(PHY_BASE + 0x1a00)

#define PCS_PHY_START		0x000	/* SERDES_START bit 0 */
#define PCS_POWER_DOWN_CONTROL	0x004	/* SW_PWRDN bit 0 (1 = powered up) */
#define PCS_SW_RESET		0x008	/* SW_RESET bit 0 */
#define PCS_READY_STATUS	0x1a8	/* PCS_READY bit 0 */

struct phy_reg { uint16_t off; uint8_t val; };

static const struct phy_reg serdes_tbl[] = {
	{ 0x0110, 0xd9 },	/* QSERDES_V6_COM_SYSCLK_EN_SEL */
	{ 0x0174, 0x16 },	/* QSERDES_V6_COM_CMN_CONFIG_1 */
	{ 0x003c, 0x11 },	/* QSERDES_V6_COM_HSCLK_SEL_1 */
	{ 0x009c, 0x00 },	/* QSERDES_V6_COM_HSCLK_HS_SWITCH_SEL_1 */
	{ 0x0120, 0x01 },	/* QSERDES_V6_COM_LOCK_CMP_EN */
	{ 0x0140, 0x04 },	/* QSERDES_V6_COM_VCO_TUNE_MAP */
	{ 0x00f4, 0x0f },	/* QSERDES_V6_COM_PLL_IVCO */
	{ 0x0148, 0x00 },	/* QSERDES_V6_COM_VCO_TUNE_INITVAL2 */
	{ 0x0088, 0x41 },	/* QSERDES_V6_COM_DEC_START_MODE0 */
	{ 0x0070, 0x0a },	/* QSERDES_V6_COM_CP_CTRL_MODE0 */
	{ 0x0074, 0x18 },	/* QSERDES_V6_COM_PLL_RCTRL_MODE0 */
	{ 0x0078, 0x14 },	/* QSERDES_V6_COM_PLL_CCTRL_MODE0 */
	{ 0x0080, 0x7f },	/* QSERDES_V6_COM_LOCK_CMP1_MODE0 */
	{ 0x0084, 0x06 },	/* QSERDES_V6_COM_LOCK_CMP2_MODE0 */
	{ 0x0028, 0x4c },	/* QSERDES_V6_COM_DEC_START_MODE1 */
	{ 0x0010, 0x0a },	/* QSERDES_V6_COM_CP_CTRL_MODE1 */
	{ 0x0014, 0x18 },	/* QSERDES_V6_COM_PLL_RCTRL_MODE1 */
	{ 0x0018, 0x14 },	/* QSERDES_V6_COM_PLL_CCTRL_MODE1 */
	{ 0x0020, 0x99 },	/* QSERDES_V6_COM_LOCK_CMP1_MODE1 */
	{ 0x0024, 0x07 },	/* QSERDES_V6_COM_LOCK_CMP2_MODE1 */
	{ 0x0140, 0x44 },	/* sm8550_ufsphy_hs_b_serdes: QSERDES_V6_COM_VCO_TUNE_MAP */
};

static const struct phy_reg tx_tbl[] = {
	{ 0x007c, 0x05 },	/* QSERDES_UFS_V6_TX_LANE_MODE_1 */
	{ 0x0030, 0x07 },	/* QSERDES_UFS_V6_TX_RES_CODE_LANE_OFFSET_TX */
	{ 0x0108, 0x4c },	/* QSERDES_UFS_V6_TX_FR_DCC_CTRL */
};

static const struct phy_reg rx_tbl[] = {
	{ 0x00d4, 0x0c },	/* QSERDES_UFS_V6_RX_UCDR_FO_GAIN_RATE2 */
	{ 0x0178, 0x0e },	/* QSERDES_UFS_V6_RX_VGA_CAL_MAN_VAL */
	{ 0x0208, 0xc2 },	/* QSERDES_UFS_V6_RX_MODE_RATE_0_1_B0 */
	{ 0x020c, 0xc2 },	/* QSERDES_UFS_V6_RX_MODE_RATE_0_1_B1 */
	{ 0x0214, 0x1a },	/* QSERDES_UFS_V6_RX_MODE_RATE_0_1_B3 */
	{ 0x0220, 0x60 },	/* QSERDES_UFS_V6_RX_MODE_RATE_0_1_B6 */
	{ 0x0238, 0x9e },	/* QSERDES_UFS_V6_RX_MODE_RATE2_B3 */
	{ 0x0244, 0x60 },	/* QSERDES_UFS_V6_RX_MODE_RATE2_B6 */
	{ 0x025c, 0x9e },	/* QSERDES_UFS_V6_RX_MODE_RATE3_B3 */
	{ 0x0260, 0x0e },	/* QSERDES_UFS_V6_RX_MODE_RATE3_B4 */
	{ 0x0264, 0x36 },	/* QSERDES_UFS_V6_RX_MODE_RATE3_B5 */
	{ 0x0270, 0x02 },	/* QSERDES_UFS_V6_RX_MODE_RATE3_B8 */
};

static const struct phy_reg pcs_tbl[] = {
	{ 0x018c, 0x69 },	/* QPHY_V6_PCS_UFS_RX_SIGDET_CTRL2 */
	{ 0x0030, 0x0f },	/* QPHY_V6_PCS_UFS_TX_LARGE_AMP_DRV_LVL */
	{ 0x01f4, 0x43 },	/* QPHY_V6_PCS_UFS_TX_MID_TERM_CTRL1 */
	{ 0x01fc, 0x02 },	/* QPHY_V6_PCS_UFS_MULTI_LANE_CTRL1 */
	{ 0x002c, 0x2b },	/* QPHY_V6_PCS_UFS_PLL_CNTL */
	{ 0x0074, 0x04 },	/* QPHY_V6_PCS_UFS_TX_HSGEAR_CAPABILITY */
	{ 0x00bc, 0x04 },	/* QPHY_V6_PCS_UFS_RX_HSGEAR_CAPABILITY */
	{ 0x002c, 0x33 },	/* QPHY_V6_PCS_UFS_PLL_CNTL */
	{ 0x012c, 0x4f },	/* QPHY_V6_PCS_UFS_RX_HS_G5_SYNC_LENGTH_CAPABILITY */
	{ 0x0220, 0x9e },	/* QPHY_V6_PCS_UFS_RX_HSG5_SYNC_WAIT_TIME */
};

#define N(t)	(sizeof(t) / sizeof((t)[0]))

/* plain LDR/STR only (see shim.c) */
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

static void apply(uintptr_t base, const struct phy_reg *t, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		wr(base + t[i].off, t[i].val);
}

/* qmp_ufs_power_on(): returns 0 when PCS_READY, else the last READY_STATUS | 0x80000000 */
uint32_t ufsphy_power_on(void)
{
	wr(PCS + PCS_POWER_DOWN_CONTROL, rd(PCS + PCS_POWER_DOWN_CONTROL) | 1);
	apply(SERDES, serdes_tbl, N(serdes_tbl));
	apply(TX1, tx_tbl, N(tx_tbl));
	apply(RX1, rx_tbl, N(rx_tbl));
	apply(TX2, tx_tbl, N(tx_tbl));
	apply(RX2, rx_tbl, N(rx_tbl));
	apply(PCS, pcs_tbl, N(pcs_tbl));
	wr(PCS + PCS_SW_RESET, rd(PCS + PCS_SW_RESET) & ~1u);
	wr(PCS + PCS_PHY_START, rd(PCS + PCS_PHY_START) | 1);

	uint32_t st = 0;
	for (uint64_t t0 = 0, t; ; ) {
		__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
		if (!t0)
			t0 = t;
		st = rd(PCS + PCS_READY_STATUS);
		if (st & 1)
			return 0;
		if (t - t0 > 19200000)		/* 1 s (U-Boot: 2 s) */
			return st | 0x80000000u;
	}
}

uint32_t ufsphy_ready_status(void)
{
	return rd(PCS + PCS_READY_STATUS);
}
