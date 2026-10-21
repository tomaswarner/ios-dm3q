/*
 * Minimal RPMh for the DM3Q (S23 Ultra) shim: cmd-db lookup and active-only
 * votes through the first ACTIVE TCS of the apps RSC (DRV 2). Follows U-Boot's
 * drivers/soc/qcom/{cmd-db,rpmh-rsc}.c and drivers/power/regulator/
 * qcom-rpmh-regulator.c (GPL-2.0, Linaro); single threaded, polled, MMU off.
 *
 * Phone DT: apps_rsc drv@2 at 0x17a20000, qcom,tcs-offset 0xd00, tcs-config
 * ACTIVE 3, SLEEP 2, WAKE 2, CONTROL 0, FAST 1 -> ACTIVE TCS ids 0..2.
 * cmd-db: reserved-memory aop_cmd_db_region@81c60000.
 */
#include <stdint.h>
#include "rpmh.h"

#define CMDDB_BASE	0x81c60000UL
#define RSC_BASE	0x17a20000UL	/* drv-2 */
#define TCS_BASE	(RSC_BASE + 0xd00)
#define TCS_ID		0		/* first ACTIVE TCS */

/* rpmh_rsc_reg_offset_ver_3_0 (ver_2_7 when the RSC major version is < 3) */
static uint32_t tcs_stride = 672, cmd_stride = 24;
static uint32_t r_wait = 0x20, r_ctrl = 0x24, r_status = 0x28, r_enable = 0x2c;
static uint32_t r_msgid = 0x34, r_addr = 0x38, r_data = 0x3c, r_cstatus = 0x40;
#define R_IRQ_CLEAR	0x08

#define CMD_MSGID_LEN		8
#define CMD_MSGID_RESP_REQ	(1u << 8)
#define CMD_MSGID_WRITE		(1u << 16)
#define CMD_STATUS_COMPL	(1u << 16)
#define TCS_AMC_MODE_ENABLE	(1u << 16)
#define TCS_AMC_MODE_TRIGGER	(1u << 24)

/* plain LDR/STR only (Gunyah emulation needs ISV; see shim.c) */
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

static uintptr_t tcs_reg(uint32_t reg)
{
	return TCS_BASE + tcs_stride * TCS_ID + reg;
}

static uintptr_t tcs_cmd(uint32_t reg, int cmd)
{
	return tcs_reg(reg) + cmd_stride * cmd;
}

static void wr_sync(uintptr_t a, uint32_t v)
{
	wr(a, v);
	uint64_t start = ticks();
	while (rd(a) != v && ticks() - start < 19200000)
		;
}

uint32_t rpmh_version(void)
{
	uint32_t id = rd(RSC_BASE);
	if (((id >> 16) & 0xff) < 3) {
		tcs_stride = 672;
		cmd_stride = 20;
		r_wait = 0x10, r_ctrl = 0x14, r_status = 0x18, r_enable = 0x1c;
		r_msgid = 0x30, r_addr = 0x34, r_data = 0x38, r_cstatus = 0x3c;
	}
	return id;
}

uint32_t rpmh_tcs_idle(void)
{
	return rd(tcs_reg(r_status));
}

int rpmh_cmddb_ok(void)
{
	volatile uint8_t *m = (volatile uint8_t *)(CMDDB_BASE + 4);
	return m[0] == 0xdb && m[1] == 0x30 && m[2] == 0x03 && m[3] == 0x0c;
}

/* struct cmd_db_header: version, magic, 8 x rsc_hdr (16 bytes), checksum, reserved, data[] */
uint32_t rpmh_cmddb_addr(const char *name)
{
	const uintptr_t data = CMDDB_BASE + 8 + 8 * 16 + 8;
	for (int i = 0; i < 8; i++) {
		uintptr_t h = CMDDB_BASE + 8 + 16 * i;
		uint32_t w0 = rd(h), w1 = rd(h + 4);
		uint32_t slv = w0 & 0xffff, hoff = w0 >> 16, cnt = w1 >> 16;
		if (!slv)
			continue;
		for (uint32_t j = 0; j < cnt; j++) {
			uintptr_t e = data + hoff + 24 * j;	/* entry_header: id[8], prio[2], addr, len, offset */
			volatile const char *id = (volatile const char *)e;
			int match = 1, end = 0;
			for (int k = 0; k < 8 && match; k++) {	/* strncmp(id, name, 8) == 0 */
				char c = end ? 0 : name[k];
				end |= !c;
				match = id[k] == c;
			}
			if (match)
				return rd(e + 16);
		}
	}
	return 0;
}

/* Write n (<= 16) addr/data pairs as one active-only request and wait for completion. */
int rpmh_write(const uint32_t *addr, const uint32_t *data, int n)
{
	if (!rd(tcs_reg(r_status)))
		return -1;			/* TCS busy */
	uint32_t mask = (1u << n) - 1;
	for (int j = 0; j < n; j++) {
		wr(tcs_cmd(r_msgid, j), CMD_MSGID_LEN | CMD_MSGID_WRITE | CMD_MSGID_RESP_REQ);
		wr(tcs_cmd(r_addr, j), addr[j]);
		wr(tcs_cmd(r_data, j), data[j]);
	}
	wr(tcs_reg(r_enable), mask);
	wr(tcs_reg(r_wait), mask);

	/* __tcs_set_trigger(true): clear trigger, clear enable, enable, trigger */
	uint32_t ctrl = rd(tcs_reg(r_ctrl));
	ctrl &= ~TCS_AMC_MODE_TRIGGER;
	wr_sync(tcs_reg(r_ctrl), ctrl);
	ctrl &= ~TCS_AMC_MODE_ENABLE;
	wr_sync(tcs_reg(r_ctrl), ctrl);
	wr_sync(tcs_reg(r_ctrl), TCS_AMC_MODE_ENABLE);
	wr(tcs_reg(r_ctrl), TCS_AMC_MODE_ENABLE | TCS_AMC_MODE_TRIGGER);

	int rc = -2;				/* timeout */
	uint64_t start = ticks();
	while (ticks() - start < 19200000) {
		int done = 1;
		for (int j = 0; j < n; j++)
			done &= !!(rd(tcs_cmd(r_cstatus, j)) & CMD_STATUS_COMPL);
		if (done) {
			rc = 0;
			break;
		}
	}

	/* __tcs_set_trigger(false) and reclaim the TCS */
	ctrl = rd(tcs_reg(r_ctrl)) & ~TCS_AMC_MODE_TRIGGER;
	wr_sync(tcs_reg(r_ctrl), ctrl);
	wr_sync(tcs_reg(r_ctrl), ctrl & ~TCS_AMC_MODE_ENABLE);
	wr(tcs_reg(r_enable), 0);
	wr(TCS_BASE + R_IRQ_CLEAR, 1u << TCS_ID);
	return rc;
}

/* VRM regulator: voltage (mV), mode (PMIC5 LDO HPM 7, 0 = leave), enable. Returns 0 or <0. */
int rpmh_vreg_on(const char *name, uint32_t mv, uint32_t mode)
{
	uint32_t base = rpmh_cmddb_addr(name);
	if (!base)
		return -3;
	uint32_t a[3], d[3];
	int n = 0;
	a[n] = base + 0x0, d[n++] = mv;		/* RPMH_REGULATOR_REG_VRM_VOLTAGE */
	if (mode)
		a[n] = base + 0x8, d[n++] = mode;	/* RPMH_REGULATOR_REG_VRM_MODE */
	a[n] = base + 0x4, d[n++] = 1;		/* RPMH_REGULATOR_REG_ENABLE */
	return rpmh_write(a, d, n);
}
