/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
 * is enabled and the link is up, points the transfer request list at our own
 * buffers and issues READ-ONLY SCSI commands on LUN 0. Nothing is ever
 * written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x01d84000UL	/* ufshc@1d84000, sweet live FDT */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

/* plain LDR/STR only (see shim.c: Gunyah cannot emulate writeback/pair MMIO) */
static inline uint32_t ufs_rd(uint32_t off)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(UFS_BASE + off) : "memory");
	return v;
}
static inline void ufs_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]" : : "r"(v), "r"((uintptr_t)UFS_BASE + off) : "memory");
}

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl_own[32] __attribute__((aligned(1024)));
static struct ucd ucd_own __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));
/* DM3Q: the probe can move the list + command descriptor into ABL's DMA area.
 * Set at runtime: an initialized pointer would be an absolute relocation. */
static struct utrd *utrl;
static struct ucd *ucdp;

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(ucdp, sizeof(*ucdp));

	/* Command UPIU: header (12 bytes), expected length, CDB. cdb == 0: NOP OUT. */
	ucdp->cmd[0] = cdb ? 0x01 : 0x00;		/* transaction type: COMMAND / NOP OUT */
	ucdp->cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucdp->cmd[2] = lun;
	ucdp->cmd[3] = ++tag;
	ucdp->cmd[12] = len >> 24;
	ucdp->cmd[13] = len >> 16;
	ucdp->cmd[14] = len >> 8;
	ucdp->cmd[15] = len;
	for (unsigned i = 0; cdb && i < cdb_len; i++)
		ucdp->cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)ucdp;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucdp->prdt[0].addr_lo = (uint32_t)buf_pa;
		ucdp->prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucdp->prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucdp->resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucdp->resp[0] != (cdb ? 0x21 : 0x20))	/* RESPONSE / NOP IN UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
}

void test_ufs(void)
{
	utrl = utrl_own;
	ucdp = &ucd_own;
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	/* A first command may report a unit attention; retry a few times. */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}

	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* READ(10) LBA 0, 2 blocks: protective MBR and the GPT header. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = scsi(0, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (rc)
		return;
	con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;
	con_str("UFS GPT", sig);
	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16, esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 128)
		return;
	list_partitions(elba, nent);
}

/* ---- DM3Q (S23 Ultra) bring-up probe ---------------------------------- */

#define REG_IE		0x24
#define REG_UECPA	0x38
#define REG_UECDL	0x3c
#define REG_UECN	0x40
#define REG_UECT	0x44
#define REG_UECDME	0x48
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98
#define REG_UCMDARG3	0x9c
#define IS_UHXS		(1u << 5)
#define IS_UCCS		(1u << 10)

#define SMMU_BASE	0x15000000UL	/* apps-smmu@15000000, qcom,qsmmu-v500 (MMU-500) */
#define UFS_SID		0x60		/* ufshc iommus = <&apps_smmu 0x60 0> */

static inline uint32_t mmio_rd(uintptr_t a)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}

/* UIC command; returns ARG2[7:0] result (0 = success), 0xff on timeout */
static uint32_t uic(uint32_t cmd, uint32_t arg1, uint32_t arg3, uint32_t *val)
{
	uint64_t start = ticks();
	while (!(ufs_rd(REG_HCS) & HCS_UCRDY))
		if (ticks() - start > 19200000)
			return 0xfe;
	ufs_wr(REG_IS, IS_UCCS | IS_UHXS);
	ufs_wr(REG_UCMDARG1, arg1);
	ufs_wr(REG_UCMDARG2, 0);
	ufs_wr(REG_UCMDARG3, arg3);
	ufs_wr(REG_UICCMD, cmd);
	start = ticks();
	while (!(ufs_rd(REG_IS) & IS_UCCS))
		if (ticks() - start > 19200000)
			return 0xff;
	if (val)
		*val = ufs_rd(REG_UCMDARG3);
	ufs_wr(REG_IS, IS_UCCS);
	return ufs_rd(REG_UCMDARG2) & 0xff;
}

static void dme_get(const char *label, uint32_t attr)
{
	uint32_t v = 0, rc = uic(0x01, attr << 16, 0, &v);	/* DME_GET */
	con_hex2(label, "rc=", rc, "val=", v);
}

/* Take over the transfer list at `list`, then NOP OUT + TEST UNIT READY. */
static void try_cmds(const char *label, uintptr_t list, uintptr_t cd)
{
	utrl = (struct utrd *)list;
	ucdp = (struct ucd *)cd;
	ufs_wr(REG_UTRLRSR, 0);
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str(label, "list base not taken");
		return;
	}
	uint32_t ocs = 0, st = 0;
	uint32_t nop = scsi(0, 0, 0, 0, 0, &ocs, &st);
	con_hex2(label, "nop=", nop << 8 | ocs, "is=", ufs_rd(REG_IS));
	static const uint8_t tur[6] = { 0x00 };
	uint32_t rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
	con_hex2(label, "tur=", rc << 16 | ocs << 8 | st, "hcs=", ufs_rd(REG_HCS));
}

static void smmu_probe(void)
{
	uint32_t idr0 = mmio_rd(SMMU_BASE + 0x20), idr1 = mmio_rd(SMMU_BASE + 0x24);
	con_hex2("SMMU", "cr0=", mmio_rd(SMMU_BASE + 0x0), "idr0=", idr0);
	con_hex2("SMMU", "idr1=", idr1, "gfsr=", mmio_rd(SMMU_BASE + 0x48));
	uint32_t nsmr = idr0 & 0xff, found = 0;
	for (uint32_t n = 0; n < nsmr && n < 256; n++) {
		uint32_t smr = mmio_rd(SMMU_BASE + 0x800 + 4 * n);
		uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
		if (!(smr >> 31) || ((id ^ UFS_SID) & ~mask & 0x7fff))
			continue;
		uint32_t s2cr = mmio_rd(SMMU_BASE + 0xc00 + 4 * n);
		con_hex2("SMR", "n=", n, "smr=", smr);
		con_hex2("S2CR", "type=", (s2cr >> 16) & 3, "cb=", s2cr & 0xff);
		found++;
		if (((s2cr >> 16) & 3) == 0) {		/* translation context */
			uint32_t pgsz = (idr1 >> 31) ? 0x10000 : 0x1000;
			uint32_t npages = 1u << (((idr1 >> 28) & 7) + 1);
			uintptr_t cb = SMMU_BASE + (uintptr_t)npages * pgsz + (s2cr & 0xff) * pgsz;
			con_hex2("CB", "sctlr=", mmio_rd(cb + 0x0), "fsr=", mmio_rd(cb + 0x58));
			con_hex2("CB", "ttbr0=", mmio_rd(cb + 0x20), "hi=", mmio_rd(cb + 0x24));
			con_hex2("CB", "far=", mmio_rd(cb + 0x60), "fsynr0=", mmio_rd(cb + 0x68));
		}
	}
	con_hex2("SMMU", "nsmr=", nsmr, "ufs_smr=", found);
}

void test_ufs_dm3q(void)
{
	con_str("UFS", "dm3q probe (read-only data)");
	uint32_t hcs = ufs_rd(REG_HCS);
	uintptr_t abl = (uintptr_t)ufs_rd(REG_UTRLBA) | (uintptr_t)ufs_rd(REG_UTRLBAU) << 32;
	con_hex2("UFS", "hcs=", hcs, "is=", ufs_rd(REG_IS));
	con_hex2("UFS", "ie=", ufs_rd(REG_IE), "abl utrl=", (uint32_t)abl);
	con_hex2("UEC", "pa=", ufs_rd(REG_UECPA), "dl=", ufs_rd(REG_UECDL));
	con_hex2("UEC", "n=", ufs_rd(REG_UECN), "t/dme=", ufs_rd(REG_UECT) << 16 | (ufs_rd(REG_UECDME) & 0xffff));
	if (!(ufs_rd(REG_HCE) & 1) || ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "host off or requests outstanding: stop");
		return;
	}
	dme_get("PWRMODE", 0x1571);	/* PA_PWRMode: 0x11 fast, 0x22 slow, 0x44/0x55 auto */
	dme_get("TXGEAR", 0x1568);	/* PA_TxGear */
	dme_get("RXGEAR", 0x1583);	/* PA_RxGear */
	dme_get("TXLANES", 0x1561);	/* PA_ConnectedTxDataLanes */

	stage_str("ufs own");
	try_cmds("OWN", (uintptr_t)utrl_own, (uintptr_t)&ucd_own);
	/* DME_HIBERNATE_EXIT: harmless if the link is not in Hibern8 (error result) */
	uint32_t h8 = uic(0x18, 0, 0, 0);
	con_hex2("H8EXIT", "rc=", h8, "hcs=", ufs_rd(REG_HCS));
	try_cmds("OWN2", (uintptr_t)utrl_own, (uintptr_t)&ucd_own);
	/* ABL's list page (ABL is done with it): list at its base, descriptor +2K */
	if (abl) {
		stage_str("ufs abl");
		try_cmds("ABL", abl & ~0xfffUL, (abl & ~0xfffUL) + 0x800);
	}
	stage_str("smmu");
	smmu_probe();
}
