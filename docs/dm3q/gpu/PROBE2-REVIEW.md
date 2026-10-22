# Probe 2 (gpu2) pre-flash review

Reviewed: `port/shim/gpu.c` lines 442-762 (gpu2_probe and helpers), `A740-POWERON.md` sections 2-4, phone DT `fw/dts/merged-rev9.dts`
(kgsl-smmu@3da0000 at 28966, gmu at 41152, gpu at 28655). Nothing was flashed, gpu.c was NOT edited.

Primary sources (all fetched 2026-10-21, copies in the session scratchpad):
- `[ARM]`  torvalds/linux master `drivers/iommu/arm/arm-smmu/arm-smmu.c`, `arm-smmu.h`, `arm-smmu-qcom.c`, `arm-smmu-impl.c`; `drivers/iommu/io-pgtable-arm.c`
- `[LGMU]` `drivers/gpu/drm/msm/adreno/a6xx_gmu.c`, `a6xx_gmu.h`, `a6xx_hfi.c`, `a6xx_hfi.h`, `a6xx_catalog.c`; `registers/adreno/a6xx_gmu.xml`, `a6xx.xml`
- `[KGSL]` gitea OnePlus13-Development/android_kernel_qcom_sm8750-modules branch lineage-22.2, `qcom/opensource/graphics-kernel/`:
  `adreno_gen7_gmu.c`, `adreno_gen7_hfi.c`, `adreno_hfi.h`, `adreno_gen7.c`, `adreno.h`, `gen7_reg.h`, `kgsl_gmu_core.{c,h}`.
  This is SM8750-era KGSL, not the Kalama tag; the gen7 GMU boot code is shared but Kalama-specific differences are UNVERIFIED.
- `[GCC]`/`[GPUCC]` `drivers/clk/qcom/gcc-sm8550.c`, `gpucc-sm8550.c`.

Summary: 1 BLOCKER (certain), 4 LIKELY, 8 MINOR. Fix finding 1 before any flash. Findings 2-5 are cheap insurance and each
can turn a "no information" failure into a diagnosable one.

---------------------------------------------------------------------------------------------------------------------------

## Findings

### 1. BLOCKER - `map()` does not return physically consecutive pages; the firmware image overwrites the page tables
gpu.c 527-539 (loop at 530-537), used at 651 (`code = map(VA_CODE, 0x11000)`) and 667 (`copy_words(code + (addr - VA_CODE), ...)`).
The comment on 526 says "pages are consecutive". They are not: each loop iteration calls `carve_page()` for the data page FIRST and
then `l3_for()`, which on first use allocates the L2 page and the L3 page from the same bump allocator. Trace for the code block:
l1 = C+0x0000, VA 0x4000 -> data page C+0x1000, then `l3_for` takes l2[0] = C+0x2000 and the L3 = C+0x3000, so VA 0x5000 -> C+0x4000.
`copy_words()` then writes the 0x10210-byte firmware block linearly from PA `code` = C+0x1000, i.e. it overwrites the L2 table (C+0x2000)
and L3 table (C+0x3000) with firmware bytes, and VA 0x5000..0x14fff do not map to the bytes that were copied for them.
Effect: SMMU walks read garbage descriptors (translation fault / junk), or the GMU executes code from the wrong pages. The probe
cannot reach `(FW_INIT_RESULT & 0x1ff) == 0x100`. (The other maps are harmless: only `code`, `log` and `hfi` PAs are used, and for
log/hfi the L3 already exists when they are mapped, so they happen to be contiguous.)
Correct behaviour: KGSL maps one contiguous kernel allocation per block (`gmu_core_map_memdesc`, kgsl_gmu_core.c:190);
Linux `a6xx_gmu_memory_alloc` allocates one BO per region (a6xx_gmu.c:1535).
Fix (build all table pages first, then allocate the data pages):
```c
static uint64_t map(uint64_t va, uint64_t size)
{
	for (uint64_t o = 0; o < size; o += 0x1000)
		(void)l3_for(va + o);			/* allocates L2/L3 pages only */
	uint64_t pa0 = carve_next;			/* data pages below are now consecutive */
	for (uint64_t o = 0; o < size; o += 0x1000) {
		uint64_t pa = carve_page();
		uint64_t l3 = l3_for(va + o);		/* no allocation any more */
		wr64(l3 + 8 * (((va + o) >> 12) & 0x1ff),
		     pa | (1u << 10) | (3u << 8) | (1u << 6) | 3);
	}
	return pa0;
}
```
Also add a bound in `carve_page()`: `if (p + 0x1000 > CARVE + CARVE_SIZE) { con_str("GPU2","carve overflow"); for(;;); }`
(computed usage today is 95 data pages + 6 table pages = 0x65000, well inside 1 MiB, so this is only a guard).

### 2. LIKELY - sCR0 is never inspected or fixed; if CLIENTPD is set the GMU bypasses translation
gpu.c 696-699 reads `cr0` only for printing. Linux `arm_smmu_device_reset()` (arm-smmu.c ~1688, `reg &= ~ARM_SMMU_sCR0_CLIENTPD`)
always clears CLIENTPD (bit 0) and sets GFRE/GFIE/GCFGFRE/GCFGFIE/VMIDPNE/PTM; `sGFSR` is cleared first. The state this phone's
ABL/TZ/Gunyah leaves kgsl-smmu in is UNVERIFIED (probe 1 never read it; the P1 survey of this probe prints it). With CLIENTPD=1 every
transaction bypasses the SMMU: the GMU fetches VA 0x4000 as PA 0x4000 and the context bank is never used.
Whether Gunyah/TZ lets EL1 write sCR0 on this SMMU is also UNVERIFIED (arm-smmu-qcom.c qcom_smmu_cfg_probe comments that some
hypervisors do not trap/emulate all SMMU registers, which is why the survey loops stop at 128 SMRs, correctly).
Fix: after the SMR write (line 695) and before the GMU config:
```c
	wr(SMMU + SMMU_GFSR, rd(SMMU + SMMU_GFSR));		/* clear stale global faults */
	uint32_t cr0b = rd(SMMU + SMMU_CR0);
	if (cr0b & 1u)						/* CLIENTPD */
		wr(SMMU + SMMU_CR0, cr0b & ~1u);
	con_hex2("P2 sCR0", "before=", cr0b, "after=", rd(SMMU + SMMU_CR0));
```
and add `con_hex2("P4 smmu", "gfsr=", rd(SMMU+SMMU_GFSR), "gfsynr0=", rd(SMMU+0x50)); con_hex2("P4 smmu","gfsynr1=", rd(SMMU+0x54), "gfsynr2=", rd(SMMU+0x58));`
next to the existing CB FSR print at 741 (sGFSR.MULTI = SMR conflict, USF = unmatched stream; the CB FSR does not show those).
If the readback does not change, the write is being swallowed; then the result of this probe is "SMMU not ours to configure".

### 3. LIKELY - nothing re-checks the GCC GPU branches that probe 1 left with CLK_OFF=1 (GMU-to-DDR AXI path), and there is no bus vote
NOTES (probe 1, S4): the five GCC CBCRs keep bit 31 (CLK_OFF) set after enable. For the probe-1 job (register window + TCM) that did
not matter. Probe 2 is the first user of the GMU AXI master -> GBIF -> `gcc_gpu_memnoc_gfx_clk` / `gcc_ddrss_gpu_axi_clk` -> GEMNOC -> DDR
(and the SMMU table walker uses the same path). [GCC] gcc-sm8550.c: `gcc_gpu_memnoc_gfx_clk` is `BRANCH_HALT_VOTED` at 0x71010,
`gcc_ddrss_gpu_axi_clk` is `BRANCH_HALT_SKIP` at 0x71154, `gcc_gpu_snoc_dvm_gfx_clk` is `BRANCH_HALT_DELAY`.
S4 runs in gpu2_probe BEFORE the CX GDSC is on (S6), so a stuck bit 31 may just mean "parent domain was off at that time".
Also KGSL votes DDR bandwidth before releasing the GMU (`gen7_gmu_first_boot`: `icc_set_bw(pwr->icc_path, 0, ddr_table[bus_min])`,
adreno_gen7_gmu.c ~2052) and Linux does `a6xx_gmu_set_initial_bw()` (a6xx_gmu.c a6xx_gmu_resume); the shim does neither (BCM votes
are UNVERIFIED as a requirement, the doc already lists this as an open risk).
Fix (diagnostic, zero risk): after `s7_gmu_clocks()` in gpu2_probe (line 603) re-print the CBCRs now that CX is up:
```c
	con_hex2("P2 GCC", "memnoc=", rd(GCC_GPU_MEMNOC_GFX), "axi=", rd(GCC_DDRSS_GPU_AXI));
	con_hex2("P2 GCC", "dvm=", rd(GCC_GPU_SNOC_DVM_GFX), "cfg=", rd(GCC_GPU_CFG_AHB));
```
Interpretation: if memnoc still reads bit31=1 after CX is on, expect the GMU DDR accesses (code fetch) to time out -> FW_INIT_RESULT
stays 0 and `g2h`/FSR show nothing. In that case the next step is a BCM/ICC vote (cmd-db BCMs SH0/MC0/ACV via rpmh_write), not a GMU fix.

### 4. LIKELY - HFI start is attempted without the RSC/PDC microcode that KGSL loads between GMU start and HFI start
KGSL `gen7_gmu_first_boot` (adreno_gen7_gmu.c 2060-2075): `gen7_gmu_device_start()` -> `gen7_load_pdc_ucode()` + `gen7_load_rsc_ucode()`
-> `gen7_gmu_hfi_start()` (HFI_CTRL_INIT + poll STATUS bit0). Linux loads the same in `a6xx_gmu_rpmh_init()` (a6xx_gmu.c:668) at probe,
before the first boot. gpu.c does neither, so P5 (line 744-749) runs against whatever RSC/PDC state ABL left (probe 1 read PDC
`GPU_ENABLE_PDC` = 0x101, not the 0x80000001 both drivers write, so it is NOT configured). The INIT_RESULT test does not depend on it
(KGSL polls it first, then loads the ucode), so this only affects criterion (c) HFI_CTRL_STATUS and everything after it.
Values (RSCC base 0x03d50000, PDC base 0x0b280000, byte = base + dword*4; dwords from gen7_reg.h / a6xx_gmu.xml, values from
`gen7_load_rsc_ucode` adreno_gen7_gmu.c:276 and `gen7_load_pdc_ucode` :310; the A740 / gen7_2_x variants are used):
```c
static void gpu2_rsc_pdc(void)
{
	const uintptr_t R = 0x03d50000UL, P = 0x0b280000UL;
	wr(R + 0x10, 1u << 24);			/* GPU_RSCC_RSC_STATUS0_DRV0 (dw 0x4): disable SDE clock gating */
	wr(R + 0x2c, 1);			/* RSCC_PDC_SLAVE_ID_DRV0 (dw 0xb) */
	wr(R + 0x38, 0); wr(R + 0x34, 0);	/* HIDDEN_TCS_CMD0_DATA (dw 0xe) / ADDR (dw 0xd) */
	wr(R + 0x40, 0); wr(R + 0x3c, 0);	/* +2 dwords */
	wr(R + 0x48, 0x80000021u); wr(R + 0x44, 0);	/* +4 dwords; 0x80000021 for gen7_2_x / A740 (a740_family in Linux) */
	wr(R + 0x400, 0);			/* RSCC_OVERRIDE_START_ADDR (dw 0x100) */
	wr(R + 0x20, 0x4520);			/* RSCC_PDC_SEQ_START_ADDR (dw 0x8) */
	wr(R + 0x24, 0x4510);			/* RSCC_PDC_MATCH_VALUE_LO (dw 0x9) */
	wr(R + 0x28, 0x4514);			/* RSCC_PDC_MATCH_VALUE_HI (dw 0xa) */
	wr(R + 0x550, 0xeaaae5a0);		/* RSCC_SEQ_MEM_0_DRV0_A740 (dw 0x154) .. +4 */
	wr(R + 0x554, 0xe1a1ebab);
	wr(R + 0x558, 0xa2e0a581);
	wr(R + 0x55c, 0xecac82e2);
	wr(R + 0x560, 0x0020edad);
	wr(P + 0x4520, 0);			/* PDC_GPU_SEQ_START_ADDR (dw 0x1148) */
	wr(P + 0x4500, 0x80000001u);		/* PDC_GPU_ENABLE_PDC (dw 0x1140) */
}
```
Call it after `init_ok` and before `wr(0x03d7e618, 1)` (line 745), with a `con_str("NEXT","P5 rsc/pdc")` first: the PDC window write is
the one new MMIO write in the probe (the window is in the gmu DT node, `gmu_pdc` 0xb280000, probe 1 only read it) so print before it.
Severity is LIKELY only for P5; do it in the same flash because P5 is otherwise guaranteed to be uninformative if it fails.

### 5. LIKELY - init-done loop: no early exit on firmware NMI, and 500 ms / XO clock is unvalidated
gpu.c 726-737. Facts: Linux `a6xx_gmu_start` (a6xx_gmu.c:291) uses `(val & 0x1ff) == 0x100` with a 100 ms limit when the DTCM version word
(`GMU_CM3_DTCM_START + 0xff8` = byte 0x03d74fe0) is > 0x20010004 (ours 0x40010009, read in S9: correct), KGSL `gen7_gmu_device_start`
polls `BIT(8)` under mask `GENMASK(8,0)` for 100 ms - both identical to the probe's test. KGSL also treats `FW_INIT_RESULT & 0xE00`
(bits 11:9, "GMU reset status / NMI") as a firmware assertion (adreno_gen7_hfi.c `poll_gmu_reg`, ~line 286) and bails immediately.
Both drivers run the GMU core at 200 MHz (Linux `clk_set_rate(gmu->core_clk, 200000000)`, a6xx_gmu.c:1309; KGSL `gen7_gmu_enable_clks`
-> `gen7_gmu_clock_set_rate`, freq table 220/550 in gpucc `ftbl_gpu_cc_gmu_clk_src`, PLL1) while probe 2 leaves it on XO 19.2 MHz
(10x slower). Nothing documents a minimum GMU clock for init, so XO is UNVERIFIED but plausible; 500 ms is ample for an init that takes
a few ms at 200 MHz, but the loop wastes it if the fw asserts. The printed `ms=` is correct (19200 ticks/ms at 19.2 MHz; 9.6 M ticks = 500 ms).
Fix: break on NMI and give XO more room:
```c
	while (ticks() - t0 < 19200000ULL * 2) {		/* 2 s */
		res = rd(0x03d7e070);
		...
		if ((res & 0x1ff) == 0x100 || (res & 0xe00))	/* done or fw assertion/NMI */
			break;
	}
```
Escalation path if it times out with res==0 and no fault: GMU clock to PLL1 (gpu_cc_pll1_config l=0x16 alpha=0xeaaa, GMU RCG 0x9318
source PLL1 (parent_map_1 index 3), hub RCG 0x93ec from GPLL0/3) as listed in the doc, and the GCC/BCM items in finding 3.

### 6. MINOR - `CB_ACTLR = 0x32b` (line 688)
Value comes from DT `qcom,actlr = <0x00 0x3ff 0x32b>` (28981), applied by `qcom_smmu_set_actlr_dev()` only as a prefetch/performance hint
(arm-smmu-qcom.c ~line 330); `arm_mmu500_reset()` (arm-smmu-impl.c:110) notes that on MMU-500 r2p0+ ACTLR writes only stick when
ACR.CACHE_LOCK is clear and CPRE (bit 0, next-page prefetch) is the subject of five errata. It is not needed for translation. Recommend
dropping the write (leave reset value) to remove a variable; keep only if you specifically want to mimic Android.

### 7. MINOR - SMR slot selection can leave a second matching SMR (multi-match) and ignores the MASK field
gpu.c 636-641: the compare mask `0x80007fff` discards the SMR MASK bits (31:16, `ARM_SMMU_SMR_MASK`), so an SMR with ID=5 and a non-zero mask
(a wildcard that also matches SID 5 and others) is accepted as "ours" and then overwritten with mask 0. The reverse problem is not
handled at all: any OTHER valid SMR whose (ID,MASK) also matches SID 5 (e.g. a catch-all left by ABL) makes SID 5 a multi-match -> sGFSR.MULTI
(`ARM_SMMU_sGFSR`), transaction fault. Fix: compute, for every valid SMR n != slot, `m = (smr >> 16) & 0x7fff; id = smr & 0x7fff; if (((id ^ GMU_SID) & ~m & 0x7fff) == 0)`
and print "SMR conflict n=..", and for the reuse test require `(smr & 0x7fffffff) == GMU_SID`. Printing is enough for a first probe; the survey
already prints the first 10 valid SMRs.
Verified correct: SMR layout VALID=bit 31, ID=15:0, MASK=31:16; S2CR TYPE=17:16 (0 = translate), CBNDX=7:0, PRIVCFG=25:24 (arm-smmu.h:100-120);
S2CR written before SMR as in `arm_smmu_write_sme()`.

### 8. MINOR - context-bank choice ignores NUMS2CB and the survey does not check capabilities
gpu.c 624-635: Linux allocates stage-1 context banks starting at `smmu->num_s2_context_banks` (arm-smmu.c:744; IDR1[23:16] "stage-2 only").
Picking from the top is normally fine, but add `if (c < ((idr1 >> 16) & 0xff)) break;` (store `idr1`) and print IDR0.SMS (bit 27,
stream matching present), IDR0.S1TS (bit 30), IDR2.PTFS_4K (bit 12), IDR2.OAS (7:4) in `smmu_survey`; bail if any of S1TS/PTFS_4K is 0.
`TCR2.PASIZE=2` (40-bit) is valid only if IDR2.OAS >= 40 bit; Linux derives it from OAS (`arm_smmu_lpae_tcr2`, arm-smmu.h:416). PA 0x9_xxxx_xxxx needs >= 36 bit
(PASIZE 1), so use `min(2, OAS)` if the survey shows OAS < 2 (UNVERIFIED on this SMMU; DT `qcom,iova-width = 0x31` for the QTB means 49-bit IOVA).

### 9. MINOR - add a `dsb sy` before releasing the GMU
The page tables / firmware are written with Device-nGnRnE stores (MMU off), which are ordered and acknowledged by the endpoint, so this is
very likely already safe; but `wr()` (line 65) has only a compiler barrier and the SMMU table walker and GMU read DDR behind the CPU's back.
Cost-free insurance: `__asm__ volatile("dsb sy" ::: "memory");` after `hfi_table(hfi)` (line 672), after the SMR write (695) and
immediately before `wr(0x03d7e000, 0)` (725). KGSL does `wmb()` after the fw load and before SYSRESET (adreno_gen7_gmu.c:560-ish `wmb()` in `gen7_gmu_load_fw`; `gen7_gmu_device_start` also `wmb()`).

### 10. MINOR - `hfi_table()`: `size` unit differs between Linux and KGSL; everything else matches
gpu.c 583: `6 + 3*12` = 42 dwords. KGSL `init_queues()` (adreno_gen7_hfi.c ~line 205): `size = sizeof(struct hfi_queue_table) >> 2` (dwords, but that struct is
bigger: 3 + HFI_QUEUE_DISPATCH_MAX_CNT headers). Linux `a6xx_hfi_init()` (a6xx_hfi.c:1059): `table->size = table_size` where table_size is in BYTES
(24 + 3*48 = 168) although the struct comment says dwords. Both boot their firmware, so the field is evidently not interpreted strictly; the
comment on line 580 ("size(dwords)") matches KGSL and the header doc, so keep 42. If P5 fails and everything else looks right, try 168 (Linux value, same fw file name).
Verified against `a6xx_hfi.h` and `a6xx_hfi_queue_init()`: header order {version,size,qhdr0_offset=6,qhdr_size=12,num_queues=3,active_queues=3};
queue header order {status=1, iova, type=(10<<8)|id, size=0x400 dwords (SZ_4K>>2), msg_size=0, dropped=0, rx_watermark=1, tx_watermark=1, rx_request=1,
tx_request=0, read_index=0, write_index=0} = 12 dwords at table+24+48*q; queue ids 0,1,2 for non-legacy (legacy would be 0,4,5); queues at table_iova + 4K*(q+1);
`HFI_QTBL_ADDR` = table iova, `HFI_QTBL_INFO` = 1 (a6xx_gmu.c:974-975). KGSL's queue header leaves rx/tx watermark and rx_request 0, so those fields are also not critical.
HFI_CTRL_INIT=1 then poll HFI_CTRL_STATUS bit 0 is `a6xx_gmu_hfi_start()` / `gen7_gmu_hfi_start()` (Linux polls 10 ms, KGSL 100 ms with 100 us steps; the probe's 50 ms is fine).

### 11. MINOR - TCM state is not made deterministic; also assert SYSRESET=1 first
The firmware's prealloc list requests zeroed memory at (0x200,0x3adc) in ITCM, (0x10004000,0x2ee0) and (0x10007000,0xf84) in DTCM
(KGSL `gen7_gmu_process_prealloc`, adreno_gen7_gmu.c:1351, returns 0 for TCM-resident ranges, so KGSL does NOT clear them - UNVERIFIED that the
firmware clears them itself; Linux a6xx_gmu_fw_load also only writes the blocks). TCM retention (CX_MISC TCM_RET_CNTL) is only written after the load in probe 2 (line 704)
whereas Linux writes it before `a6xx_gmu_fw_load` (a6xx_gmu.c:950-955). The probe-1 pattern test zeroes only ITCM+0x3ff0 and DTCM+0x3f00. Cheap and harmless:
zero ITCM 0x03d6d000+0x200 .. +0x3cdc and DTCM 0x03d71000+0x0 .. +0x2ee0, +0x3000 .. +0x3f84 (all outside the loaded blocks 0..0x1dc, 0x3f98+0x28, 0x3fdc+0x18) just before the config at 703, and add
`wr(0x03d7e000, 1);` (SYSRESET=1, Linux `a6xx_gmu_start` does it first) at the top of "P3 gmu config".
Verified: the "0x200 size 0x3adc" block lies entirely inside ITCM (0..0x3fff), so nothing needs mapping for it; the probe correctly maps only (0x4000), (0x100000), (0x200000), (0x300000), (0x60000000 x2).

### 12. MINOR - map the Linux a7xx "dummy"/"debug" regions as a hedge
Linux a6xx_gmu.c:2324-2338: a7xx uses a 16 KiB `dummy` at 0x60000000 and a 28 KiB `debug` at 0x60400000 ("allows reusing these BOs which are otherwise unused", comment says
they were guessed from the fw prealloc list). This fw's list (parsed from the file) has only 2 x 0x1000 PERSIST at 0x60000000 and 0x60001000, which the probe maps (line 655, 0x2000), and KGSL maps exactly the
prealloc list (it does NOT map the whole 0x4000..16 MiB cache range; Linux does: `SZ_16M - SZ_16K` at 0x4000). The GMU hardware treats VA 0x60000000-0x7fffffff as the uncached VBIF3 range
(kgsl_gmu_core.h:330), so the log (0x60100000) and HFI (0x60180000) placement is right. To mirror Linux exactly extend line 655 to `map(0x60000000, 0x4000)` and add `(void)map(0x60400000, 0x7000);`.
DT oddity, UNVERIFIED: the phone's gmu node has `qcom,ipc-core = <0x400000 0x100000>` (41146); no such block exists in this firmware's prealloc list and
`GMU_NONCACHED_USER` blocks are skipped by KGSL's prealloc handler, so ignore unless the fault address printed at 741-742 is 0x400000.

### 13. MINOR - carve-out address is unverified against the live memory map
gpu.c 474: `CARVE 0x900000000`. Consistent with this port's map (DRAM upper bank from 0x8_80000000, ~8 GB model up to ~0xa_00000000; mmu.c comment; NOTES "mkadt highmem 0x8c0000000").
The DT `/memory` node is empty (filled by ABL), reserved-memory in the DT is all below 0x8_8c000000, but whether your running image (shim, payload, ADT, framebuffer) or the
bootloader's dynamic `rbin` pool (alloc-ranges include 0x8_80000000.., rbin node in /reserved-memory) overlaps 0x9_0000_0000..0x9_0010_0000 is UNVERIFIED. diag.c already prints the DRAM banks; confirm the carve lies inside them
and does not overlap `_head.._end` + payload (payload may be at 0x8c0000000 + size). If unsure, move CARVE next to the shim image's free tail.
Also note a 12 GB device would have the same base; no change needed.

---------------------------------------------------------------------------------------------------------------------------

## Verified correct (checked against the sources above)

SMMU / MMU-500
- Register offsets: sCR0 0x0, IDR0/1/2/7 0x20/0x24/0x28/0x3c, sGFSR 0x48, SMR(n) 0x800+4n, S2CR(n) 0xc00+4n, GR1 CBAR(n) 0x0+4n, CBA2R(n) 0x800+4n (arm-smmu.h:27-141).
  CB: SCTLR 0x0, ACTLR 0x4, TCR2 0x10, TTBR0 0x20 (64-bit, split lo/hi writes at +0/+4 are fine), TCR 0x30, MAIR0/1 0x38/0x3c, FSR 0x58, FAR 0x60, FSYNR0 0x68,
  TLBIALL 0x618, TLBSYNC 0x7f0, TLBSTATUS 0x7f4 (bit 0 = SACTIVE) (arm-smmu.h:145-250; TLBIALL is architectural, Linux itself uses TLBIASID 0x610 / TLBIVA 0x600).
- Page geometry: pgsz = IDR1[31] ? 64K : 4K; numpage = 1 << (IDR1[30:28]+1); GR1 page = base + pgsz; CB base = base + numpage*pgsz (= half the register space) (arm-smmu.c:1847-1860 and `arm_smmu_page`). NUMSMRG = IDR0[7:0], NUMCB = IDR1[7:0]. Code matches.
- Stage-1 with S2 bypass is CBAR TYPE 1 (`CBAR_TYPE_S1_TRANS_S2_BYPASS`), TYPE = bits 17:16, S1 BPSHCFG=3 (NSH) at 9:8, MEMATTR=0xf at 15:12 (arm_smmu_write_context_bank). The VMID value in CBAR[7:0] (code writes cb+1) is only written by Linux for stage 2; it is harmless for stage 1.
- AArch64 context: CBA2R bit 0 (VA64) first, then CBAR, TCR2, TCR, TTBR0, MAIR, SCTLR - the probe uses the same order (SCTLR M last). `TCR2`: PASIZE bits 3:0 (2 = 40 bit), SEP bits 17:15 = 7 (upstream) as coded; Linux additionally sets TCR2.AS (bit 4, 16-bit ASID), irrelevant with ASID 0.
  `TCR`: T0SZ 5:0 = 25 (64-39), TG0 15:14 = 0 (4K), SH0 13:12 = 0, ORGN0 11:10 = 0, IRGN0 9:8 = 0 (non-cacheable walks, correct for MMU-off table writes), EPD1 bit 23 (`arm_smmu_lpae_tcr`, arm-smmu.h:395); EAE (bit 31) is AArch32-only and correctly absent.
  `qcom,use-3-lvl-tables` == 39-bit VA, 4K granule, start level 1: L1 index = va>>30 & 0x1ff, L2 = va>>21 & 0x1ff, L3 = va>>12 & 0x1ff; TTBR0 = L1 PA (4 KiB aligned, 512 entries). Matches `l3_for()`/`map()`.
- Descriptor bits (io-pgtable-arm.c:67-97, 515-580): table/page type = 3, AF = bit 10, SH inner = 3<<8, AP[7:6]: bit 6 set = unprivileged accessible, bit 7 = read-only, AttrIndx at bit 2, nG bit 11 (Linux sets it; not needed with a single context), XN is bits 54:53 (clear = executable, needed for the code page). Code's `AF | SH_IS | AP_UNPRIV | 3` is a valid RW page for privileged and unprivileged GMU accesses (KGSL/Linux map the cache/prealloc regions `IOMMU_READ|WRITE|PRIV`, i.e. AP priv-only; the probe's mapping is a superset).
- MAIR attr 0 = 0x44 is Linux's `ARM_LPAE_MAIR_ATTR_NC` (io-pgtable-arm.c:130); MAIR1 = 0.
- S2CR TRANSLATE = TYPE 0 with CBNDX, PRIVCFG 0 (default); SMR VALID|ID=5|MASK=0; order S2CR then SMR (`arm_smmu_write_sme`). GMU stream id 5 mask 0: DT `iommus = <kgsl_smmu 0x05 0>`.
- qcom specifics: `qsmmu-v500` -> `qcom_smmu_500_impl`/`qcom_adreno_smmu_500_impl` (arm-smmu-qcom.c:619-663): `arm_mmu500_reset` (ACR tweaks), `write_s2cr` quirk only rewrites BYPASS/FAULT S2CR types (the probe writes TRANSLATE, unaffected), CB0 is reserved for the GPU (`qcom_adreno_smmu_alloc_context_bank`), TBU/QTB nodes (0x3de8000) are only used for ATOS/halt, nothing there blocks plain translation. Under Gunyah, accessibility of SMMU registers from EL1 is UNVERIFIED.
- No pointers in initialised data in the probe 2 code (only `static const uint32_t hdr[6]`, scalar statics); no ABS64 relocation risk. Buffer sizes (`l1s[16]` indexes 6..14, `line[24]` index 17) are in range; `results[12]` indexes 0..11 OK; operator precedence on lines 629/637 is correct.

GMU register map (byte = 0x03d00000 + dword*4; dwords from a6xx_gmu.xml / gen7_reg.h, all addresses in lines 704-740 recomputed and match):
- SYSRESET 0x1f800 (0x03d7e000), BOOT_CONFIG 0x1f801, FW_BUSY 0x1f81a, FW_INIT_RESULT 0x1f81c (0x03d7e070), CM3_CFG 0x1f82d (0x03d7e0b4), FAL_INTF 0x1f8f0 (0x03d7e3c0), FALNEXT_INTF 0x1f8f1 (0x03d7e3c4),
  HFI_CTRL_STATUS 0x1f980 (0x03d7e600), HFI_QTBL_INFO 0x1f984 (0x03d7e610), HFI_QTBL_ADDR 0x1f985 (0x03d7e614), HFI_CTRL_INIT 0x1f986 (0x03d7e618), GMU2HOST_INTR_INFO 0x1f992,
  GENERAL_8/9/10 (A7XX) 0x1f9cd/0x1f9ce/0x1f9cf (0x03d7e734/738/73c), GENERAL_11 0x1f9d0 (KGSL VRB, not used by Linux A7xx, fw v4.1.9 predates it - omission OK),
  ICACHE/DCACHE/SYS_BUS_CONFIG 0x1f400/0x1f401/0x1f40f, AHB_FENCE_RANGE_0 0x23b11 (0x03d8ec44), CX_MISC TCM_RET_CNTL dword 0x39 -> 0x03d9e0e4, GBIF_QSB_SIDE0..3 0x3c03..0x3c06 (0x03d0f00c..018), GBIF_HALT 0x3c45 (0x03d0f114).
- Values: CM3_BOOT_CONFIG=2, FW_INIT_RESULT=0, QTBL_INFO=1, CM3_CFG=0x4052 (a6xx_gmu.c:970-997); FAL/FALNEXT=1 (Linux a650/a7xx and KGSL `gen7_gmu_register_config`);
  AHB_FENCE_RANGE_0 = BIT31 | (0x32 << 18) | 0x8a0 = 0x80c808a0 (a7xx); GBIF sides = 0x00071620 (`a640_gbif`, used by the A740 catalog entry, a6xx_catalog.c:673,1556); GBIF_HALT=0 (KGSL comment: halt may stay asserted);
  SYS_BUS/ICACHE/DCACHE_CONFIG=1 (`a6xx_gmu_power_config`, A7xx returns right after these three, a6xx_gmu.c:802-812); GENERAL_9 = 0 (log write pointer); GENERAL_10 = 0x07020100: Linux A740 catalog `.gmu_chipid = 0x7020100` (a6xx_catalog.c:1557)
  and KGSL `ADRENO_GMU_REV(gpurev 0x070201)` = major<<24 | minor<<16 | patch<<8 = 0x07020100 for the non-chipid-based gen7_2_x: both agree; GENERAL_8 = (log iova & 0xfffff000) | (size/4K - 1) (64 KiB -> 0xf): same formula in both drivers.
- Ordering: Linux writes FAL, TCM_RET, (fw load), INIT_RESULT, BOOT_CONFIG, QTBL, FENCE, CM3_CFG, GENERAL_10/8, GBIF, power_config, then `a6xx_gmu_start` (SYSRESET=1, GENERAL_9=0, SYSRESET=0). The probe's order differs only trivially (GBIF before GENERAL_*, GENERAL_9 after SYSRESET already 1); no functional dependence.
- Init-done test and DTCM version gate verified (finding 5). A740 does not need `qcom_scm_gpu_init_regs` (Linux only for A750+; KGSL tolerates -EOPNOTSUPP, `gen7_scm_gpu_init_cx_regs` adreno_gen7.c:1031) - consistent with probe 1's "GPU_INIT_REGS not available".
- No GMU_AO / CX power counter, VRB, or CX timer init is required for cold boot of gen7_2_x: `gen7_cx_timer_init` is gen7_9_x only (adreno_gen7.c:370); AHB timeout detection is optional (`gen7_enable_ahb_timeout_detection`). The RSC wake sequence is skipped on first boot in both (`a6xx_rpmh_start` / `gen7_rscc_wakeup_sequence` return early without the SLEEP flag).
- Memory regions: firmware block list parsed from the file: PREALLOC (type 1/7, zero size, value = size) = ITCM (0x0,0x1dc), (0x4000,0x10210), (0x200,0x3adc), (0x100000,0x8ffc), (0x200000,0x12c80), (0x300000,0x1b240), DTCM (0x10004000,0x2ee0), (0x10007000,0xf84), (0x10007fdc,0x18), PERSIST (0x60000000,0x1000), (0x60001000,0x1000).
  The probe maps exactly the DDR ones (0x4000: 0x11000; 0x100000: 0x9000; 0x200000: 0x13000; 0x300000: 0x1c000; 0x60000000: 0x2000) with sizes rounded up to 4 KiB as KGSL does; the others are TCM (KGSL: "Nothing to do for TCM blocks"). Block-walk logic at 659-671 (zero-size headers skipped by 16, payload blocks advanced by size) is right; only the DDR block {0x4000,0x10210} is copied.

---------------------------------------------------------------------------------------------------------------------------

## Suggested minimal diff list (apply before flashing)

1. Finding 1 (mandatory): rewrite `map()` as shown; add carve bound check.
2. Finding 2: clear CLIENTPD if set, clear sGFSR, print sGFSR/GFSYNR after the run.
3. Finding 3: print the 4 GCC CBCRs after `s7_gmu_clocks()`.
4. Finding 4: `gpu2_rsc_pdc()` before HFI_CTRL_INIT.
5. Finding 5: break on `res & 0xe00`, 2 s window.
6. Findings 9/11 (one line each): `dsb sy` before SYSRESET=0, `SYSRESET=1` at the top of config, optional TCM zeroing.

Expected photo trail after the fixes: P1 survey (sCR0/IDR/SMRs), `P2 pick`, `P2 carve used=0x65000 code bytes=0x10210`, `P2 code w0=` (first fw code word, non-zero),
`P2 CB sctlr=0x...`, `P4 initres val=0x100` (success) or a non-zero/NMI value with `P4 smmu fsr/far` naming the GMU VA that faulted.
Interpretation guide: FSR!=0 and FAR=a VA in the table -> mapping/attribute bug; FSR!=0 with FAR 0x4000-ish and PTE junk -> finding 1 again; FSR==0, gfsr!=0 -> finding 2/7;
FSR==0, gfsr==0, initres==0 for 2 s -> bus path (finding 3) or GMU clock (finding 5); initres bits 11:9 set -> firmware assertion (read the log buffer words printed at P5, the log is at VA 0x60100000).
