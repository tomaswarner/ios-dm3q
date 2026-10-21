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
#include "rpmh.h"
#include "ufsphy.h"

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

static void read_gpt(void);

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

	read_gpt();
}

/* READ(10) LBA 0, 2 blocks: protective MBR and GPT header, then the big partitions. */
static void read_gpt(void)
{
	uint32_t ocs = 0, st = 0, rc;
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
	while (!(ufs_rd(REG_HCS) & HCS_UCRDY)) {
		/* dm3q ufs7: a pending UE (PA LINE-RESET indication after the device
		 * reset) kept UCRDY low; acknowledge it as ufshcd does */
		uint32_t is = ufs_rd(REG_IS);
		if (is & (1u << 2)) {
			(void)ufs_rd(REG_UECPA);
			ufs_wr(REG_IS, 1u << 2);
		}
		if (ticks() - start > 19200000)
			return 0xfe;
	}
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

#define GCC_BASE	0x00100000UL	/* gcc: clock-controller@100000 (Linux gcc-sm8550.c offsets) */
#define CBCR_CLK_OFF	(1u << 31)
#define CBCR_CLK_EN	(1u << 0)

static inline void mmio_wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]" : : "r"(v), "r"(a) : "memory");
}

/* UFS clock branches and power domains; returns how many branches were off */
static int ufs_clocks(const char *label)
{
	static const uint32_t off[] = { 0x77018, 0x77074, 0x77068, 0x77024, 0x770b0, 0x7702c, 0x770cc, 0x77028 };
	uint32_t v[8];
	int n_off = 0;
	for (int i = 0; i < 8; i++) {
		v[i] = mmio_rd(GCC_BASE + off[i]);
		n_off += !!(v[i] & CBCR_CLK_OFF);
	}
	con_hex2(label, "axi=", v[0], "ice=", v[1]);
	con_hex2(label, "unipro=", v[2], "ahb=", v[3]);
	con_hex2(label, "aux=", v[4], "rx0=", v[5]);
	con_hex2(label, "rx1=", v[6], "tx0=", v[7]);
	con_hex2(label, "gdsc=", mmio_rd(GCC_BASE + 0x77004), "phygdsc=", mmio_rd(GCC_BASE + 0x9e000));
	return n_off;
}

/* Turn on AXI (DMA master), ICE (inline crypto, in the data path), UniPro and PHY aux, as Linux does */
static void ufs_clocks_on(void)
{
	static const uint32_t off[] = { 0x77018, 0x77074, 0x77068, 0x770b0 };
	for (int i = 0; i < 4; i++) {
		uintptr_t r = GCC_BASE + off[i];
		uint32_t v = mmio_rd(r);
		if (v & CBCR_CLK_OFF) {
			mmio_wr(r, v | CBCR_CLK_EN);
			uint64_t start = ticks();
			while ((mmio_rd(r) & CBCR_CLK_OFF) && ticks() - start < 1920000)
				;
		}
	}
}

/* DME_PEER_GET: asks the UFS device end of the link (fails if it is asleep/off) */
static uint32_t dme_peer_get(const char *label, uint32_t attr)
{
	uint32_t v = 0, rc = uic(0x03, attr << 16, 0, &v);
	con_hex2(label, "rc=", rc, "val=", v);
	return rc;
}

/* What ABL last did: its transfer list is still in RAM. Show the used slots. */
static void abl_list_dump(uintptr_t list)
{
	int shown = 0;
	for (int i = 0; i < 32 && shown < 4; i++) {
		volatile uint32_t *d = (volatile uint32_t *)(list + 32 * i);
		uint32_t ucdba = d[4];
		if (!ucdba && !d[5])
			continue;
		shown++;
		con_hex2("ABL UTRD", "dw0=", d[0], "ocs=", d[2]);
		con_hex2("", "ucd=", ucdba, "resp/prdt=", (d[6] >> 16) << 16 | (d[7] & 0xffff));
		/* dm3q ufs3: ABL frees its list at exit; UEFI fills freed pool with 0xAF */
		if (ucdba == 0xafafafafu || d[5] || ucdba < 0x80000000u || (ucdba & 0x7f)) {
			con_str("", ucdba == 0xafafafafu ? "freed by ABL (0xAF fill)" : "ucd pointer invalid");
			continue;
		}
		volatile uint8_t *c = (volatile uint8_t *)(uintptr_t)ucdba;
		con_hex2("", "upiu=", c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3],
		    "cdb=", c[16] << 24 | c[17] << 16 | c[18] << 8 | c[19]);
	}
	if (!shown)
		con_str("ABL UTRD", "list empty");
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
	/* dm3q ufs1/ufs2: local link HS-G5 x2 FAST, UEC clean, GCC UFS clocks on, SMMU bypass */
	con_hex2("UFS", "ahit=", ufs_rd(0x18), "cfg1=", ufs_rd(0xdc));
	con_hex2("UFS", "cfg2=", ufs_rd(0xe0), "ah8cfg=", ufs_rd(0xfc));
	dme_get("PWRMODE", 0x1571);		/* local PA_PWRMode */
	stage_str("dme peer");
	dme_peer_get("PEER PWR", 0x1571);	/* device-side PA_PWRMode */
	dme_peer_get("PEER GEAR", 0x1583);	/* device-side PA_RxGear */
	dme_peer_get("PEER MID", 0x5002);	/* device-side DME_DDBL1_ManufacturerID */
	stage_str("abl list");
	if (abl)
		abl_list_dump(abl);
	(void)ufs_clocks;
	(void)ufs_clocks_on;
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
	/* smmu_probe(): done (dm3q ufs1: SID 0x60 -> CB0, SCTLR.M=0 bypass, no faults) */
	(void)smmu_probe;
}

/* ---- DM3Q: wake the UFS device that ABL powered down at exit ----------- */

/* TLMM UFS_RESET io_reg 0x0f0de004 (pinctrl-sm8550): DO NOT access, it resets the SoC (dm3q ufs5) */
#define IS_ULSS			(1u << 8)

static void udelay(uint32_t us)
{
	uint64_t end = ticks() + (uint64_t)us * 192 / 10;
	while (ticks() < end)
		;
}

/* Query request: function 0x81 (write) / 0x01 (read), opcode, flag IDN. Returns flag value or 0x100+rc. */
static uint32_t query_flag(uint8_t func, uint8_t opcode, uint8_t idn)
{
	zero(&utrl[0], sizeof(utrl[0]));
	zero(ucdp, sizeof(*ucdp));
	static uint8_t qtag;
	ucdp->cmd[0] = 0x16;		/* QUERY REQUEST */
	ucdp->cmd[3] = ++qtag;
	ucdp->cmd[5] = func;
	ucdp->cmd[12] = opcode;
	ucdp->cmd[13] = idn;
	uintptr_t cd = (uintptr_t)ucdp;
	utrl[0].dw0 = (1u << 28) | (1u << 24);
	utrl[0].dw2 = 0x0f;
	utrl[0].ucdba = (uint32_t)cd;
	utrl[0].ucdbau = (uint32_t)(cd >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1)
		if (ticks() - start > 19200000) {
			ufs_wr(REG_UTRLCLR, ~1u);
			return 0x101;
		}
	if (utrl[0].dw2 & 0xff)
		return 0x102;
	if (ucdp->resp[0] != 0x36)	/* QUERY RESPONSE */
		return 0x103;
	if (ucdp->resp[6])		/* query response code */
		return 0x180 | ucdp->resp[6];
	return ucdp->resp[23];		/* flag value: LSB of the BE32 value at UPIU bytes 20-23 (Linux upiu_res.value) */
}

static void ufs_bringup_device(void);

void test_ufs_wake(void)
{
	utrl = utrl_own;
	ucdp = &ucd_own;
	con_str("UFS", "dm3q wake (read-only data)");
	uintptr_t abl = (uintptr_t)ufs_rd(REG_UTRLBA) | (uintptr_t)ufs_rd(REG_UTRLBAU) << 32;
	if (abl) {
		/* ABL's last command (dm3q ufs4): SSU to the UFS Device W-LUN 0xD0 */
		volatile uint32_t *d = (volatile uint32_t *)abl;
		uint32_t cd = d[4];
		if (cd != 0xafafafafu && !d[5] && cd >= 0x80000000u && !(cd & 0x7f)) {
			volatile uint8_t *c = (volatile uint8_t *)(uintptr_t)cd;
			con_hex2("ABL LAST", "upiu=", c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3],
			    "cdb0-4=", c[16] << 24 | c[19] << 8 | c[20]);	/* opcode .. byte4 = power cond<<4 | start */
		}
	}
	con_hex2("UFS", "hcs=", ufs_rd(REG_HCS), "is=", ufs_rd(REG_IS));

	/*
	 * 1. No RST_n pulse: dm3q ufs5 showed that touching the TLMM UFS_RESET
	 * register (0xF0DE004) from the shim resets the whole SoC (no exception).
	 * Wake the device in software instead: link startup, then SSU Active.
	 */

	/* 2. host controller reset: HCE 0 -> 1 */
	stage_str("ufs hce");
	ufs_wr(REG_HCE, 0);
	uint64_t t0 = ticks();
	while ((ufs_rd(REG_HCE) & 1) && ticks() - t0 < 1920000)
		;
	udelay(1000);
	ufs_wr(REG_HCE, 1);
	t0 = ticks();
	while (!(ufs_rd(REG_HCE) & 1) && ticks() - t0 < 19200000)
		;
	con_hex2("HCE", "hce=", ufs_rd(REG_HCE), "hcs=", ufs_rd(REG_HCS));

	/* 3. DME_LINKSTARTUP, up to 3 tries */
	stage_str("ufs linkstartup");
	uint32_t rc = 0xff;
	for (int i = 0; i < 3 && rc; i++) {
		rc = uic(0x16, 0, 0, 0);
		con_hex2("LINKUP", "rc=", rc, "hcs=", ufs_rd(REG_HCS));
	}
	con_hex2("LINKUP", "is=", ufs_rd(REG_IS), "uecpa=", ufs_rd(REG_UECPA));
	if (rc || !(ufs_rd(REG_HCS) & HCS_DP)) {
		con_str("UFS", "link startup failed / no device");
		return;
	}
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	ufs_bringup_device();
}

static int ufs_power_hs(uint32_t gear);
static void ufs_pick_gear(void);

/* Link is up: our list, NOP OUT, SSU Active, fDeviceInit, HS gear, TUR, capacity, GPT (read-only). */
static void ufs_bringup_device(void)
{
	uint32_t rc;
	int tries = 0;

	/* 4. our transfer list, NOP OUT until the device answers */
	stage_str("ufs nop");
	ufs_wr(REG_UTRLBA, (uint32_t)(uintptr_t)utrl);
	ufs_wr(REG_UTRLBAU, (uint32_t)((uintptr_t)utrl >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	uint32_t ocs = 0, st = 0, nop = 1;
	for (; tries < 10 && nop; tries++)
		nop = scsi(0, 0, 0, 0, 0, &ocs, &st);
	con_hex2("NOP", "rc=", nop << 8 | ocs, "tries=", tries);
	if (nop)
		return;

	/* 4b. leave UFS-PowerDown: START STOP UNIT, power condition Active (1), START=1, to W-LUN 0xD0 */
	stage_str("ufs ssu active");
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x11, 0 };
	for (int i = 0; i < 3; i++) {
		rc = scsi(0xd0, ssu, sizeof(ssu), 0, 0, &ocs, &st);
		if (!rc)
			break;
	}
	con_hex2("SSU", "rc=", rc, "ocs/st=", ocs << 8 | st);

	/* 5. fDeviceInit: set, then poll until the device clears it */
	stage_str("ufs fdeviceinit");
	uint32_t set = query_flag(0x81, 0x06, 0x01);	/* SET_FLAG fDeviceInit */
	uint32_t val = 1;
	for (tries = 0; tries < 100 && val == 1; tries++) {
		udelay(10000);
		val = query_flag(0x01, 0x05, 0x01);	/* READ_FLAG fDeviceInit */
	}
	con_hex2("FDEVINIT", "set=", set, "now=", val << 8 | tries);

	/* link_startup leaves PWM-G1 (< 1 MB/s). dm3q ufs10: HS-G4 is accepted (UPMCRS 1, pwrmode
	 * 0x11, 4/4) but then every command times out (G4 needs adaptation/calibration we lack);
	 * keep the highest gear that still works. */
	ufs_pick_gear();

	/* 6. SCSI on LUN 0: TUR (unit attention first), capacity, GPT */
	stage_str("ufs scsi");
	static const uint8_t tur[6] = { 0x00 };
	for (int i = 0; i < 4; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		if (!rc)
			break;
	}
	con_hex2("TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
	static const uint8_t cap10[10] = { 0x25 };
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));
	stage_str("ufs gpt");
	read_gpt();
}

/* ---- DM3Q: power the UFS device and PHY back up through RPMh (U-Boot style) ---- */

#define TCSR_UFS_CLKREF_EN	0x01fd5110UL	/* tcsr 0x1fc0000 + 0x15110 (tcsrcc-sm8550), bit 0 */

/* sm8550-samsung-q5q.dts / phone DT: VCC l17b 2.504 V, VCCQ l1g 1.2 V, HBA l3g 1.2 V,
 * PHY vdda l1d 0.88 V, PLL l3e 1.2 V, qref l1e 0.88 V */
static void ufs_vote_rails(void)
{
	stage_str("rpmh votes");
	rpmh_version();
	uint32_t v0 = (uint32_t)rpmh_vreg_on("ldob17", 2504, PMIC5_LDO_MODE_HPM);
	uint32_t v1 = (uint32_t)rpmh_vreg_on("ldog1", 1200, PMIC5_LDO_MODE_HPM);
	uint32_t v2 = (uint32_t)rpmh_vreg_on("ldog3", 1200, 0);
	uint32_t v3 = (uint32_t)rpmh_vreg_on("ldod1", 880, PMIC5_LDO_MODE_HPM);
	uint32_t v4 = (uint32_t)rpmh_vreg_on("ldoe3", 1200, 0);
	uint32_t v5 = (uint32_t)rpmh_vreg_on("ldoe1", 880, 0);
	con_hex2("VOTE", "vcc/vccq=", v0 | v1, "hba/phy=", v2 | v3);
	con_hex2("VOTE", "pll/qref=", v4 | v5, 0, 0);
}

void test_ufs_power(void)
{
	utrl = utrl_own;
	ucdp = &ucd_own;
	con_str("UFS", "dm3q rpmh power-up (read-only data)");
	stage_str("rpmh probe");
	uint32_t ver = rpmh_version();
	con_hex2("RPMH", "ver=", ver, "tcs0 idle=", rpmh_tcs_idle());
	if (!rpmh_cmddb_ok()) {
		con_str("RPMH", "cmd-db magic bad: stop");
		return;
	}
	con_hex2("CMDDB", "ldob17=", rpmh_cmddb_addr("ldob17"), "ldog1=", rpmh_cmddb_addr("ldog1"));
	con_hex2("CMDDB", "ldog3=", rpmh_cmddb_addr("ldog3"), "ldod1=", rpmh_cmddb_addr("ldod1"));
	con_hex2("CMDDB", "ldoe3=", rpmh_cmddb_addr("ldoe3"), "ldoe1=", rpmh_cmddb_addr("ldoe1"));

	/* sm8550-samsung-q5q.dts / phone DT: VCC l17b 2.504 V, VCCQ l1g 1.2 V, HBA l3g 1.2 V,
	 * PHY vdda l1d 0.88 V, PLL l3e 1.2 V, qref l1e 0.88 V */
	ufs_vote_rails();
	(void)0;
	if (0) {
	uint32_t v0 = (uint32_t)rpmh_vreg_on("ldob17", 2504, PMIC5_LDO_MODE_HPM);
	uint32_t v1 = (uint32_t)rpmh_vreg_on("ldog1", 1200, PMIC5_LDO_MODE_HPM);
	uint32_t v2 = (uint32_t)rpmh_vreg_on("ldog3", 1200, 0);
	uint32_t v3 = (uint32_t)rpmh_vreg_on("ldod1", 880, PMIC5_LDO_MODE_HPM);
	uint32_t v4 = (uint32_t)rpmh_vreg_on("ldoe3", 1200, 0);
	uint32_t v5 = (uint32_t)rpmh_vreg_on("ldoe1", 880, 0);
	con_hex2("VOTE", "vcc=", v0, "vccq=", v1);
	con_hex2("VOTE", "hba=", v2, "phy=", v3);
	con_hex2("VOTE", "pll=", v4, "qref=", v5);
	}

	stage_str("tcsr clkref");
	uint32_t ref = mmio_rd(TCSR_UFS_CLKREF_EN);
	mmio_wr(TCSR_UFS_CLKREF_EN, ref | 1);
	udelay(5000);
	con_hex2("CLKREF", "was=", ref, "now=", mmio_rd(TCSR_UFS_CLKREF_EN));

	/* gentle path first: keep ABL's PHY calibration, just leave Hibern8 */
	stage_str("ufs h8exit");
	uint32_t h8 = uic(0x18, 0, 0, 0);
	con_hex2("H8EXIT", "rc=", h8, "is=", ufs_rd(REG_IS));
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	if (!dme_peer_get("PEER PWR", 0x1571)) {
		ufs_bringup_device();
		return;
	}
	/* otherwise host reset + link startup (ABL's PHY setup may not survive) */
	test_ufs_wake();
}

/* ---- DM3Q: full UFS bring-up in U-Boot's order (ufs-qcom + qmp-ufs + ufs-uclass) ---- */

#define TLMM_UFS_RESET_IO	0x0f1de004UL	/* tlmm 0xf100000 + UFS_RESET 0xde000 + 4 (out bit 0, active low) */
#define GCC_UFS_PHY_BCR		0x00177000UL	/* gcc 0x100000 + 0x77000 (clock-sm8550 resets) */
#define REG_UFS_CFG0		0xd8		/* QUNIPRO_G4_SEL bit 5 */
#define REG_UFS_CFG1		0xdc		/* QUNIPRO_SEL bit 0 */
#define REG_UFS_HW_VERSION	0xe4		/* major [31:28] */

static uint32_t dme_set(uint32_t attr, uint32_t val)
{
	return uic(0x02, attr << 16, val, 0);	/* DME_SET, ARG3 = value */
}

static uint32_t dme_get_val(uint32_t attr, uint32_t *val)
{
	return uic(0x01, attr << 16, 0, val);
}

void test_ufs_linux(void)
{
	utrl = utrl_own;
	ucdp = &ucd_own;
	con_str("UFS", "dm3q full bring-up (U-Boot order)");
	ufs_vote_rails();

	/* ufs_qcom_device_reset: RST_n low >= 1 us */
	stage_str("ufs device reset");
	uint32_t io = mmio_rd(TLMM_UFS_RESET_IO);
	mmio_wr(TLMM_UFS_RESET_IO, 0);
	udelay(10);
	mmio_wr(TLMM_UFS_RESET_IO, 1);
	udelay(10);
	con_hex2("RST_N", "was=", io, "now=", mmio_rd(TLMM_UFS_RESET_IO));

	/* ufshcd_hba_enable: stop, then hce_enable_notify(PRE) = power_up_sequence */
	stage_str("ufs hba stop");
	ufs_wr(REG_HCE, 0);
	uint64_t t0 = ticks();
	while ((ufs_rd(REG_HCE) & 1) && ticks() - t0 < 1920000)
		;
	stage_str("ufs core reset");
	mmio_wr(GCC_UFS_PHY_BCR, 1);		/* ufs_qcom_reset: assert, >= 125 us */
	udelay(210);
	mmio_wr(GCC_UFS_PHY_BCR, 0);
	udelay(1100);

	stage_str("ufs phy");
	uint32_t phy = ufsphy_power_on();
	uint32_t hwv = ufs_rd(REG_UFS_HW_VERSION);
	con_hex2("PHY", "rc=", phy, "hwver=", hwv);
	/*
	 * Lane symbol clocks come from the PHY: point the GCC muxes at it (Linux
	 * gcc-sm8550 parent maps 10/11/12: 0 = PHY symbol clk, 2 = TCXO) and enable
	 * the branches. ABL's UFS power-down may leave them on TCXO (dm3q ufs8).
	 */
	stage_str("ufs symbol clk");
	uint32_t mrx0 = mmio_rd(GCC_BASE + 0x77064), mrx1 = mmio_rd(GCC_BASE + 0x770e0);
	uint32_t mtx0 = mmio_rd(GCC_BASE + 0x77054);
	mmio_wr(GCC_BASE + 0x77064, mrx0 & ~3u);
	mmio_wr(GCC_BASE + 0x770e0, mrx1 & ~3u);
	mmio_wr(GCC_BASE + 0x77054, mtx0 & ~3u);
	static const uint32_t sym[] = { 0x7702c, 0x770cc, 0x77028 };	/* rx0, rx1, tx0 CBCR */
	for (int i = 0; i < 3; i++)
		mmio_wr(GCC_BASE + sym[i], mmio_rd(GCC_BASE + sym[i]) | CBCR_CLK_EN);
	udelay(100);
	con_hex2("SYMMUX", "rx0/rx1=", mrx0 << 16 | (mrx1 & 0xffff), "tx0=", mtx0);
	con_hex2("SYMCLK", "rx0=", mmio_rd(GCC_BASE + 0x7702c), "tx0=", mmio_rd(GCC_BASE + 0x77028));
	ufs_wr(REG_UFS_CFG1, ufs_rd(REG_UFS_CFG1) | 1);			/* QUNIPRO_SEL */
	if ((hwv >> 28) >= 5)
		ufs_wr(REG_UFS_CFG0, ufs_rd(REG_UFS_CFG0) & ~(1u << 5));	/* QUNIPRO_G4_SEL off */

	/* ufshcd_hba_start + wait for HCE */
	stage_str("ufs hce");
	ufs_wr(REG_HCE, 1);
	udelay(1000);
	t0 = ticks();
	while (!(ufs_rd(REG_HCE) & 1) && ticks() - t0 < 19200000)
		;
	uint32_t fsm = 0xff;
	for (int i = 0; i < 50; i++) {			/* POST_CHANGE: TX FSM must reach HIBERN8 (1) */
		if (!dme_get_val(0x41, &fsm) && fsm == 1)
			break;
		udelay(200);
	}
	con_hex2("HCE", "hce=", ufs_rd(REG_HCE), "txfsm=", fsm);

	/* link_startup_notify(PRE): unipro core clk 300 MHz -> 1 us = 300, 40 ns = 12; LCC off */
	stage_str("ufs clk ctrl");
	uint32_t cc = 0, c40 = 0;
	uint32_t e1 = dme_get_val(0xd002, &cc);			/* DME_VS_CORE_CLK_CTRL */
	cc = (cc & ~(0xfffu << 16) & ~(1u << 8)) | (300u << 16);
	uint32_t e2 = dme_set(0xd002, cc);
	uint32_t e3 = dme_get_val(0x9007, &c40);		/* PA_VS_CORE_CLK_40NS_CYCLES */
	uint32_t e4 = dme_set(0x9007, (c40 & ~0x7fu) | 12);
	uint32_t e5 = dme_set(0x155e, 0);			/* PA_LOCAL_TX_LCC_ENABLE */
	con_hex2("CLKCTL", "cc=", cc, "err=", e1 | e2 << 4 | e3 << 8 | e4 << 12 | e5 << 16);

	stage_str("ufs linkstartup");
	con_hex2("PRELINK", "hcs=", ufs_rd(REG_HCS), "is=", ufs_rd(REG_IS));
	con_hex2("PRELINK", "uecpa=", ufs_rd(REG_UECPA), "uecdl=", ufs_rd(REG_UECDL));
	ufs_wr(REG_IS, 0xffffffffu);			/* ack everything pending */
	uint32_t rc = 0xff;
	for (int i = 0; i < 5 && rc; i++) {
		rc = uic(0x16, 0, 0, 0);
		con_hex2("LINKUP", "rc/try=", rc << 8 | i, "hcs/is=", ufs_rd(REG_HCS) << 16 | (ufs_rd(REG_IS) & 0xffff));
		if (rc) {
			(void)ufs_rd(REG_UECPA);
			ufs_wr(REG_IS, 0xffffffffu);
			udelay(10000);
		}
	}
	con_hex2("LINKUP", "is=", ufs_rd(REG_IS), "uecpa=", ufs_rd(REG_UECPA));
	con_hex2("LINKUP", "hce=", ufs_rd(REG_HCE), "pcsrdy=", ufsphy_ready_status());
	if (rc || !(ufs_rd(REG_HCS) & HCS_DP)) {
		con_str("UFS", "link startup failed / no device");
		return;
	}
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	ufs_bringup_device();
}

/* ufshcd_change_power_mode (U-Boot): HS gear, 2 lanes, termination, rate B, FAST/FAST */
#define IS_UPMS		(1u << 4)

static int ufs_power_hs(uint32_t gear)
{
	stage_str("ufs pwrmode");
	uint32_t e = 0;
	e |= dme_set(0x1583, gear);		/* PA_RXGEAR */
	e |= dme_set(0x1580, 2);		/* PA_ACTIVERXDATALANES */
	e |= dme_set(0x1584, 1);		/* PA_RXTERMINATION */
	e |= dme_set(0x1568, gear);		/* PA_TXGEAR */
	e |= dme_set(0x1560, 2);		/* PA_ACTIVETXDATALANES */
	e |= dme_set(0x1569, 1);		/* PA_TXTERMINATION */
	e |= dme_set(0x156a, 2);		/* PA_HSSERIES = rate B */
	if ((ufs_rd(REG_UFS_HW_VERSION) >> 28) >= 4 && gear > 3)
		e |= dme_set(0x15d4, 1);	/* PA_TXHSADAPTTYPE = PA_INITIAL_ADAPT */
	ufs_wr(REG_IS, IS_UPMS);
	uint32_t rc = dme_set(0x1571, 0x11);	/* PA_PWRMODE: FAST rx << 4 | FAST tx */
	uint64_t t0 = ticks();
	while (!(ufs_rd(REG_IS) & IS_UPMS) && ticks() - t0 < 19200000)
		;
	uint32_t upmcrs = (ufs_rd(REG_HCS) >> 8) & 7;	/* PWR_LOCAL = 1 */
	ufs_wr(REG_IS, IS_UPMS);
	uint32_t pm = 0, tg = 0, rg = 0;
	dme_get_val(0x1571, &pm);
	dme_get_val(0x1568, &tg);
	dme_get_val(0x1583, &rg);
	con_hex2("HSGEAR", "g/rc/upm=", gear << 16 | rc << 8 | upmcrs, "pm/tx/rx=", pm << 16 | tg << 8 | rg | e << 24);
	return !rc && upmcrs == 1;
}

/* Walk HS-G1..G4 (rate B): keep the highest gear where TEST UNIT READY still completes. */
static void ufs_pick_gear(void)
{
	static const uint8_t tur[6] = { 0x00 };
	uint32_t ocs = 0, st = 0, best = 0;
	for (uint32_t g = 1; g <= 4; g++) {
		if (!ufs_power_hs(g))
			break;
		uint32_t rc = 1;
		for (int i = 0; i < 3 && rc; i++)	/* first one may be a unit attention */
			rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("GEARTUR", "gear=", g, "rc/ocs/st=", rc << 16 | ocs << 8 | st);
		if (rc)
			break;
		best = g;
	}
	if (best && best < 4) {
		ufs_power_hs(best);		/* step back to the last gear that worked */
	} else if (!best) {
		dme_set(0x1583, 1);		/* back to PWM-G1 (SLOW_MODE 2) */
		dme_set(0x1568, 1);
		dme_set(0x1584, 0);
		dme_set(0x1569, 0);
		ufs_wr(REG_IS, IS_UPMS);
		dme_set(0x1571, 0x22);
		uint64_t t0 = ticks();
		while (!(ufs_rd(REG_IS) & IS_UPMS) && ticks() - t0 < 19200000)
			;
		ufs_wr(REG_IS, IS_UPMS);
	}
	con_hex2("GEAR", "best HS=", best, "(0 = PWM-G1)", 0);
}
