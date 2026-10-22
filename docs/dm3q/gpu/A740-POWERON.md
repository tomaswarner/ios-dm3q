# Adreno 740 (SM8550 / dm3q) power-on and GMU bring-up: design for the first GPU DIAG probes

Status: research only, nothing flashed. Written 2026-10-21 for `port/shim` (EL1, MMU off, Gunyah underneath).
All register/MMIO rules from NOTES apply: plain single-register LDR/STR only (also for TCM/ITCM bulk loads, no
ldp/stp/memcpy-to-IO), no address outside the DT-listed ranges in section 2, keep each probe short (no thermal management).

Source tags used below (all fetched 2026-10-21):
- `[DT]`  /Users/tomaswarner/Desktop/hackios s23/fw/dts/merged-rev9.dts (phone's own DT, line numbers given)
- `[LGMU]` linux a6xx_gmu.c, `[LGPU]` a6xx_gpu.c, `[LHFI]` a6xx_hfi.c, `[LADRENO]` adreno_gpu.c, `[LCAT]` a6xx_catalog.c, `[LST]` a6xx_gpu_state.h,
  all https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/adreno/<file>
- `[LGPUCC]` https://raw.githubusercontent.com/torvalds/linux/master/drivers/clk/qcom/gpucc-sm8550.c, `[LGCC]` .../gcc-sm8550.c, `[LGDSC]` .../gdsc.c
- `[LDTSI]` https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/qcom/sm8550.dtsi
- `[LXML]` https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml (and a6xx.xml)
- `[LRPMHPD]` .../drivers/pmdomain/qcom/rpmhpd.c, `[LCMDDB]` .../drivers/soc/qcom/cmd-db.c, `[LSCM]` .../drivers/firmware/qcom/qcom_scm.{c,h}
- `[KGSL]` Qualcomm KGSL (the driver the phone runs): https://gitea.com/OnePlus13-Development/android_kernel_qcom_sm8750-modules/src/branch/lineage-22.2/qcom/opensource/graphics-kernel/
  files adreno_gen7_gmu.c, gen7_reg.h, adreno_gen7.c. NOTE this is the SM8750-era KGSL (closest public copy I could fetch), not the
  Kalama (SM8550) tag; the GMU boot sequence is common gen7 code but Kalama-specific differences are UNVERIFIED.
- `[FW]`  /Users/tomaswarner/Desktop/hackios s23/firmware/adreno-DYDC/ (my own parse of the files, script in the session, numbers below)

Samsung's public kernel repo https://github.com/mrslezak/S23_Kernel (5.15.78, kernel_platform/common) contains only the upstream
drivers/gpu/drm/msm/adreno (a6xx only, no a7xx); the KGSL driver is in Qualcomm's separate vendor tree and is NOT in that repo.
So `[KGSL]` above is the only KGSL source used.

## 0. Bottom line

1. Everything for probe 1 needs only: GCC GPU branches, RPMh ARC votes (cx/mx/mxc), GPU_CC CX GDSC + a handful of GPU_CC branches,
   and then the GMU register window works. Identifying data we can read WITHOUT the GMU running: GMU TCM read/write-back, GMU
   reset-state registers, CX_MISC, GBIF_CX, RSCC, kgsl-smmu IDR/CR0. There is NO documented HW-revision register in the CX domain
   (A7xx chip id comes from DT/catalog: 0x43050a01 "C510v2" = A740 `[LCAT]`; RBBM registers are GX domain, do NOT read them).
2. Best single "proof of life" for probe 1: load the TCM part of gmu_gen70200.bin (ITCM block 0x1dc B + 3 DTCM blocks), read it back, and
   read the firmware version words at 0x03d74fe0.. which the file defines as 0x40010009 (core v4.1.9). That proves CX GDSC, GMU clocks,
   AHB path and our load code, with no DDR, no SMMU, no GMU execution.
3. Starting the Cortex-M3 (SYSRESET=0) is NOT possible with TCM alone: the file's main code (0x10210 bytes at GMU VA 0x4000) lives in DDR
   and the GMU reaches it through the GPU SMMU (stream ID 5). That needs a minimal stage-1 context bank in kgsl-smmu (0x03da0000) = probe 2.
4. Zap shader: SCM PAS id 13, region 0x9b09a000 (8 KiB), image needs only 0x430 B. Feasible in principle (Samsung-signed zap from this
   same phone), but needs GX up under GMU control and CP running; judge after probe 2/3. Section 6.
5. No prior art for a non-Linux GMU/zap bring-up exists in what I could find (section 1).

## 1. Prior art (web search, 2026-10-21)

Found nothing that does bare-metal / U-Boot / RTOS bring-up of an Adreno 7xx GMU, and nothing about loading the Adreno zap shader from a
non-Android OS on SM8550 under Gunyah. What exists is Linux-only:
- A7xx GMU = A6xx GMU plus a DEMET clock that is "strictly required" (reset lines, OOB requests fail and HFI hangs without it):
  https://lab.nexedi.cn/kirr/linux/-/commit/0247d99d09b7e6f5ccabecbdd7dd671b3168c913 and https://lkml.iu.edu/hypermail/linux/kernel/2309.3/01420.html
- A7xx series (A730/A740, Konrad Dybcio): https://lkml.iu.edu/hypermail/linux/kernel/2309.3/01419.html ; A740 patch https://lkml.rescloud.iu.edu/2309.1/04627.html
- SM8550 A740 DT enablement, zap fw at qcom/sm8550/a740_zap.mbn: https://lkml.iu.edu/hypermail/linux/kernel/2312.0/02933.html
- Zap fw is signed per OEM, so firmware-name must come from DT: https://lkml.iu.edu/hypermail/linux/kernel/2310.2/01336.html ;
  firmware-name design https://lkml.rescloud.iu.edu/hypermail/linux/kernel/2001.1/04514.html
- Another SM8550 OEM-signed zap packaged from stock Android (AYANEO): https://download.copr.fedorainfracloud.org/results/linux4switch/pocketds/fedora-44-aarch64/10432294-pocketds-firmware/pocketds-firmware.spec
- KGSL gen7 GMU source (SM8750 vintage mirror): https://gitea.com/OnePlus13-Development/android_kernel_qcom_sm8750-modules/src/branch/lineage-22.2/qcom/opensource/graphics-kernel/adreno_gen7_gmu.c
- Nothing found about Gunyah mediating GPU/SCM PAS calls; assume the primary VM is allowed (it is what Android's kgsl does), UNVERIFIED for us.

## 2. Address map (all from the phone's DT, plus Linux register offsets)

GPU registers: byte address = 0x03d00000 + (dword offset << 2) for GPU, GMU and GPU_CC registers `[LGMU gmu_read: GMU_BYTE_OFFSET]`.
Linux maps the GMU window at 0x03d6a000, Samsung's DT at 0x03d68000; both end at 0x03d9f000, the dword offsets are relative to 0x03d00000.

| Block | Address | Source |
|---|---|---|
| GPU "kgsl_3d0_reg_memory" | 0x03d00000 size 0x40000 (GX registers! only GBIF_CX 0x03d0f000.. is CX) | [DT] 28667-28670 |
| cx_dbgc | 0x03d61000 size 0x800 | [DT] reg list of msm_gpu |
| RSCC | 0x03d50000 size 0x10000 | [DT] "rscc" |
| isense_cntl | 0x03d8b000 size 0x2000 (not needed) | [DT] |
| GMU ("gmu") | 0x03d68000 size 0x37000 (do not touch 0x03d68000-0x03d69fff: not used by Linux) | [DT] 41167-41169 |
| GMU PDC ("gmu_pdc") | 0x0b280000 size 0x10000 | [DT] |
| gmu_ao_blk_dec0 | 0x03d40000 size 0x10000 (no Linux equivalent, AVOID) | [DT] |
| GPU_CC | 0x03d90000 size 0xa000 | [DT] 22594-22597, [LDTSI] |
| CX_MISC ("cx_misc"/"cx_mem") | 0x03d9e000 size 0x1000 | [DT], [LDTSI] |
| kgsl-smmu (MMU-500 + QTB) | 0x03da0000 size 0x40000, tcu 0x03de6000, qtb 0x03de8000/0x03dec000 | [DT] 28966-28990 |
| GCC | 0x00100000 size 0x1f4200 | [DT] 22581 |
| apps RSC drv2 / cmd-db | 0x17a20000 / 0x81c60000 | shim/rpmh.c |
| zap region | 0x9b09a000 size 0x2000 (no-map reserved) | [DT] 1733-1737 |
| IRQs (not used, we poll) | GPU SPI 300, GMU hfi SPI 304, gmu SPI 305 | [DT] |

Phone GMU/GPU clock list (downstream-numbered indices, so mapped to registers BY NAME using upstream gpucc/gcc; the HW is the same):
gmu node clocks (gpucc 3 "gmu_clk"=CX_GMU, gpucc 5 "cxo_clk"=CXO, gcc 0x0d "axi_clk"=DDRSS_GPU_AXI (index inferred), gcc 0x20 "memnoc_clk",
gpucc 0 "ahb_clk", gpucc 0x12 "hub_clk"=HUB_CX_INT) [DT 41160-41161]. msm_gpu: gcc 0x20 gpu_memnoc_gfx, gcc 0x21 gpu_snoc_dvm_gfx, gpucc 0 ahb [DT 28663-28664].
kgsl-smmu: gpucc 0xf hlos1_vote_gpu_smmu, gcc 0x20, gcc 0x21, gpucc 0 [DT 28980]. gmu-freq-table: 219 MHz (vote 0x40) and 550 MHz (0x80) [DT 41159].
RPMh ARCs (DT regulators, cmd-db names): `cx.lvl` (VDD_CX_LEVEL, phandle 0x26, proxy/init TURBO 0x180) [DT 13334-13345], `mx.lvl` (= VDD_MXA_LEVEL, 0x29),
`mxc.lvl` (0x2a), `gfx.lvl` (VDD_GFX_LEVEL, 0x2b) [DT 13306-13318]. gpucc node supplies: vdd_cx=cx.lvl, vdd_mx=mx.lvl, vdd_mxc=mxc.lvl [DT 22598-22600].
CX GDSC: `gpu_cc_cx_gdsc` reg 0x03d99108 (= GPU_CC+0x9108), hw-ctrl 0x03d9953c, parent supply cx.lvl, "retain-regs", gds-timeout 1500 us [DT 37352-37370].
GX GDSC: 0x03d9905c, parent = gfx voter (VDD_GFX_MXC_VOTER, behind gfx.lvl+mxc.lvl) [DT 37396-37410]; DO NOT TOUCH in these probes.
GMU SMMU stream id: `iommus = <kgsl_smmu 0x05 0>` [DT 41158]. GPU user/lpac/secure streams 0/1/2.

## 3. Probe 1 ("gpu1"): CX power + identification + TCM proof of life (target runtime < 3 s)

Conventions: `G(x)` = 0x03d00000 + x*4. `rd/wr` = shim plain LDR/STR helpers. Poll timeouts 20 ms unless noted. After EVERY step print one
line so a stall is attributable; draw "NEXT: stepname" BEFORE any step marked RISK so a reset photo shows the culprit. Order is chosen so that
nothing unclocked is ever read.

### S1. cmd-db and ARC level tables (RAM reads only)  [LCMDDB, LRPMHPD rpmhpd_update_level_mapping]
- `rpmh_cmddb_ok()` expect 1. For names `"cx.lvl" "mx.lvl" "mxc.lvl" "gfx.lvl" "lcx.lvl"`: `rpmh_cmddb_addr(name)` print (expected nonzero, 0x30000-ish
  addresses; any 0 => name not in this cmd-db, skip the vote).
- Aux table = u16 array at `CMDDB + 144 + rsc_hdr.data_offset + entry.offset`, length entry.len bytes (entry: id[8], prio[2], addr@+16, len@+20, offset@+22; rsc_hdr
  word1 low 16 = data_offset, high 16 = count; this is the same layout rpmh.c already walks). Print up to 12 u16 per resource.
  Expected: ascending levels such as 16, 48, 64, 128, 192, 256, 320, 384, 416 (RPMH_REGULATOR_LEVEL_*; Linux skips 16 = retention).
- Risk if wrong: none (reads). The printed table is needed for S3.

### S2. GCC pre-state (GCC base 0x00100000, offsets from `[LGCC]`)  RISK: first touch of GPU-side CCs
Read and print (value; bit31 = CLK_OFF, bit0 = enable):
| name | addr | [LGCC] |
|---|---|---|
| GCC_GPU_CFG_AHB_CLK | 0x00171004 | probe(): `qcom_branch_set_clk_en(0x71004)`, kept always-on by Linux |
| GCC_QMIP_GPU_AHB_CLK | 0x00171008 | gcc_qmip_gpu_ahb_clk |
| GCC_GPU_MEMNOC_GFX_CLK | 0x00171010 | gcc_gpu_memnoc_gfx_clk (HWCG bit1) |
| GCC_GPU_SNOC_DVM_GFX_CLK | 0x00171018 | gcc_gpu_snoc_dvm_gfx_clk |
| GCC_DDRSS_GPU_AXI_CLK | 0x00171154 | gcc_ddrss_gpu_axi_clk (AON branch, HWCG bit1) |
| GCC apps clk vote reg | 0x00152000 | GPLL0 gating for gpucc: bit15 gpu_gpll0, bit16 gpu_gpll0_div |
| GPLL0 enable vote | 0x00152018 | bit0, normally already 1 (ABL/UFS) |
Expected: unknown which are on; this is the survey. If GPU_CFG_AHB bit31 (OFF) is set, the GPU_CC read in S5 would hang -> do S4 first.

### S3. RPMh ARC votes (active-only via shim rpmh_write)  [LRPMHPD rpmhpd_send_corner / update_level_mapping, shim/rpmh.c]
For each of cx.lvl, mx.lvl, mxc.lvl: `addr = rpmh_cmddb_addr(name)`; vote DATA IS THE INDEX into the aux table, not the level value
(rpmhpd_send_corner: `.data = corner`, corner = hlvl index; rpmhpd_get_performance_state picks first `level <= pd->level[i]`).
Choose the first index whose level >= target and level != 16. Targets: cx.lvl = 256 (NOM), mx.lvl = 128 (SVS), mxc.lvl = 128 (SVS).
Reason: Linux/Samsung proxy-vote TURBO (0x180) on these three during boot [DT 13340-13343], ABL may still hold a high vote that our active-set write
replaces, and other consumers (display scanout, anything ABL left on) share cx; going too low is the only real danger, so vote at or above SVS/NOM.
Write: `rpmh_write({addr},{index},1)`; print rc, addr, index, level. Do NOT vote gfx.lvl: the GMU/RSC owns it (it is the GX supply, GX is off).
Expected rc 0. rc -1 (TCS busy) / -2 (timeout) => abort probe (nothing powered yet). Risk: too-low CX => other blocks glitch (UFS is already dead, display
needs mmcx not cx). Keep the probe < 3 s.

### S4. Enable GCC GPU branches (write bit0, then poll bit31==0)  [LGCC]
Order: 0x00171004 (GPU_CFG_AHB), 0x00171008 (QMIP_GPU_AHB), 0x00171154 (DDRSS_GPU_AXI), 0x00171010 (MEMNOC_GFX), 0x00171018 (SNOC_DVM_GFX).
Do read-modify-write setting bit0 only (do not clear HWCG bit1). 0x00171154 is BRANCH_HALT_SKIP (do not poll). 0x00171018 is BRANCH_HALT_DELAY:
poll but tolerate a stuck bit31 (print). Print each CBCR after. Expected: bit31 = 0 and bit0 = 1 for all but the SKIP/DELAY ones.
Not enabling gcc_gpu_gpll0 votes (0x152000 bits 15/16): in probe 1 every GPU_CC RCG stays on its XO (19.2 MHz) default, see S6.
Risk: a CBCR with a missing parent clock sticks; harmless (we print). A wrong address would be a write into GCC: all five offsets are verified in
`[LGCC]` and GCC is already poked by ufs2.

### S5. GPU_CC read-only baseline BEFORE any GPU_CC write  (GPU_CC base 0x03d90000)
Print: 0x03d99108 (CX GDSCR, expect bit31 PWR_ON=0, bit0 SW_COLLAPSE=1 if off), 0x03d9953c (CX GDSC hw ctrl status), 0x03d9905c (GX GDSCR,
EXPECT off), 0x03d90000.. PLL0 words (+0x0,+0x4,+0x8,+0x10,+0x14,+0x18) and PLL1 (+0x1000..+0x1018: MODE, L_VAL, ALPHA, USER_CTL; are they configured?),
RCGs: GMU_CLK_SRC CMD 0x03d99318 / CFG 0x03d9931c, HUB_CLK_SRC 0x03d993ec / 0x03d993f0, XO_CLK_SRC 0x03d99010 / 0x03d99014 (expect CFG src 0 = XO, CMD ROOT_OFF
perhaps set), CBCRs: AHB 0x03d9911c, CX_GMU 0x03d9913c, CXO 0x03d99144, HUB_CX_INT 0x03d99148, HUB_AON 0x03d993e8, SLEEP 0x03d99134, CXO_AON 0x03d99004,
DEMET 0x03d9900c, HLOS1_VOTE_GPU_SMMU 0x03d97000, MEMNOC_GFX 0x03d99150  [LGPUCC register offsets, base 0x03d90000 + 0x9xxx].
(All addresses here are GPU_CC base 0x03d90000 + the [LGPUCC] offset, e.g. 0x9108 -> 0x03d99108, 0x7000 -> 0x03d97000, PLL1 offset 0x1000 -> 0x03d91000.)
RISK: GPU_CC is on the AHB reached through gcc_gpu_cfg_ahb: if S4 left it OFF the read would hang/reset. Print "NEXT: gpucc read" first.
Expected: GX GDSC off, CX GDSC off (PWR_ON=0), PLLs unconfigured (L_VAL 0) or leftover from ABL.

### S6. GPU_CC always-on branches + CX GDSC power-on  [LGPUCC probe(), gpu_cc_cx_gdsc, LGDSC gdsc_init/gdsc_toggle_logic, DT 37358-37370]
1. `qcom_branch_set_clk_en`: set bit0 of 0x03d99004 (CXO_AON) and 0x03d9900c (DEMET). DEMET is the clock the A7xx GMU needs; Linux marks it always-on right here
   (gpucc-sm8550.c probe: `0x900c /* GPU_CC_DEMET_CLK */`). Poll bit31 of both (they are plain branches; DEMET parent is the always-on XO div).
2. GDSCR (0x03d99108) RMW exactly like gdsc_init for this GDSC (en_rest_wait 2, en_few_wait 2, clk_dis_wait 0xf, flags RETAIN_FF_ENABLE|VOTABLE):
   clear HW_CONTROL (bit1) and SW_OVERRIDE (bit2); set EN_REST_WAIT[23:20]=2, EN_FEW_WAIT[19:16]=2, CLK_DIS_WAIT[15:12]=0xf; set RETAIN_FF_ENABLE (bit11).
3. Clear SW_COLLAPSE (bit0) -> power on. udelay(1) (gds_hw_ctrl present), then poll bit31 (PWR_ON) for up to 1500 us (DT gds-timeout 0x5dc).
4. Print GDSCR and 0x03d9953c. Expected GDSCR ~ 0x80002fx8 with bit31=1 (Samsung additionally writes `qcom,clk-dis-wait-val = 8` [DT 37362], Linux 0xf; either works, use Linux's).
Risk: if PWR_ON never sets, the CX rail is not up (S3 failed) or the GDSC is voted by TZ/another master; stop, print, hold. Do NOT touch GX GDSC (0x03d9905c).

### S7. GMU clocks (all at their XO default; no PLL work in probe 1)  [LGMU a6xx_gmu_resume, LDTSI gmu clocks, LGPUCC]
Set bit0 and poll bit31==0 (timeout 1 ms) in this order: GPU_CC_CXO 0x03d99144, GPU_CC_AHB 0x03d9911c, GPU_CC_HUB_CX_INT 0x03d99148, GPU_CC_CX_GMU 0x03d9913c,
GPU_CC_HUB_AON 0x03d993e8 (AON branch), and for kgsl-smmu access GPU_CC_HLOS1_VOTE_GPU_SMMU 0x03d97000 and GPU_CC_MEMNOC_GFX 0x03d99150 (HALT_VOTED).
Linux sets GMU core clock to 200 MHz and hub to 200 MHz (`clk_set_rate(gmu->core_clk, 200000000)`, hub 200 MHz for A740) and Samsung's table is 219 MHz;
both require PLL1 (cfg l=0x16 alpha=0xeaaa, gpucc-sm8550.c gpu_cc_pll1_config) and GPLL0 (hub src 200 MHz = GPLL0/3). Probe 1 deliberately stays on XO 19.2 MHz:
registers are reachable at any rate and we avoid a PLL-lock hang. Print RCG CFG/CMD again (expect src XO).
UNVERIFIED: that an XO-clocked hub/AHB path is fast enough for the TCM burst (it should be; Linux does `clk_bulk_prepare_enable` right after set_rate, so the RCG sees XO until the PLL is configured).

### S8. CX-domain identification reads (no GMU execution)  (G(x)=0x03d00000+x*4)
Print each: 
| item | address | source / expectation |
|---|---|---|
| GMU_CM3_SYSRESET | 0x03d7e000 | [LXML] 0x1f800; expect 1 (M3 held in reset) or 0 if ABL/KGSL left it running (that would itself be news) |
| GMU_CM3_BOOT_CONFIG | 0x03d7e004 | 0x1f801 |
| GMU_CM3_FW_BUSY | 0x03d7e068 | 0x1f81a |
| GMU_CM3_FW_INIT_RESULT | 0x03d7e070 | 0x1f81c; expect 0 or stale |
| GMU_CM3_CFG | 0x03d7e0b4 | 0x1f82d; boot value 0x4052 is what fw_start writes |
| GMU_ICACHE_CONFIG / DCACHE / SYS_BUS | 0x03d7d000 / 0x03d7d004 / 0x03d7d03c | 0x1f400 0x1f401 0x1f40f |
| GMU_GMU2HOST_INTR_INFO | 0x03d7e648 | 0x1f992 (OOB/fault bits) |
| GMU_HFI_CTRL_STATUS | 0x03d7e600 | 0x1f980 |
| GMU_GENERAL_0/1/6/7/8/9/10/11 | 0x03d7e714 0x03d7e718 0x03d7e72c 0x03d7e730 0x03d7e734 0x03d7e738 0x03d7e73c 0x03d7e740 | 0x1f9c5.. |
| GMU_GPU_CX_BUSY_STATUS (AO) | 0x03d8ec30 | 0x23b0c |
| GMU_AO_HOST_INTERRUPT_STATUS | 0x03d8ec14 | 0x23b05 |
| GMU_AO_AHB_FENCE_CTRL | 0x03d8ec40 | 0x23b10 (0 = allow) |
| GMU_RSCC_CONTROL_REQ / ACK | 0x03d8ec1c / 0x03d8ec20 | 0x23b07 / 0x23b08 |
| GMU_RPMH_POWER_STATE | 0x03d7e3b0 | 0x1f8ec |
| CX_MISC SYSTEM_CACHE_CNTL_0 / _1 | 0x03d9e004 / 0x03d9e008 | [LXML a6xx.xml A6XX_CX_MISC], cx_misc base 0x03d9e000 |
| CX_MISC TCM_RET_CNTL | 0x03d9e0e4 | 0x39*4; Linux warm/cold boot test: ==1 means warm. Expect 0 => cold |
| GBIF_CX_CONFIG | 0x03d0f000 | a6xx.xml 0x3c00 (CX; this is the only part of the 0x03d00000 block we read) |
| GBIF_QSB_SIDE0..3 | 0x03d0f00c 0x03d0f010 0x03d0f014 0x03d0f018 | 0x3c03..0x3c06; catalog sets all to 0x00071620 for A740 (a640_gbif, [LCAT]) |
| GBIF_HALT | 0x03d0f114 | 0x3c45; KGSL clears it before boot; expect 0 |
| RSCC RSC_STATUS0_DRV0 | 0x03d50010 | dword 0x4 in rscc window [KGSL gen7_reg.h GEN7_GPU_RSCC_RSC_STATUS0_DRV0] |
| RSCC SEQ_BUSY_DRV0 | 0x03d50404 | dword 0x101; expect 0 |
| RSCC PDC_SEQ_START / SLAVE_ID | 0x03d50020 / 0x03d5002c | dword 8 / 0xb |
| GMU PDC GPU_ENABLE_PDC | 0x0b284500 | PDC window, dword 0x1140 [KGSL gen7_reg.h]; expect 0 or 0x80000001 |
Danger list: do NOT read G(0x210) RBBM_STATUS (0x03d00840), CP_*, or anything else in 0x03d00000-0x03d3ffff except the GBIF_CX row: it is the
GX domain; a GX-off access is an AHB error that on this platform may reset the SoC (same class as the TLMM UFS_RESET page). RSCC/PDC reads are CX-domain per Linux (it programs
them before GX is ever on), but UNVERIFIED on Samsung; if the PDC window faults it is the first thing to drop. Print "NEXT: rscc/pdc" before them (last in this table).
Risk if the GMU window hangs: S6/S7 incomplete (clocks off). Check the CBCRs printed in S7 first.

### S9. GMU TCM proof of life and fw load from the phone's own gmu_gen70200.bin  [LGMU a6xx_gmu_fw_load, KGSL gen7_gmu_load_fw, FW]
Embed gmu_gen70200.bin (66844 B) via `.incbin`. Format = repeated 16-byte header {addr, size, type, value} + `size` bytes (same as Linux `struct block_header`,
and KGSL `gmu_block_header`). My parse of the file:
| file off | GMU VA addr | size | type | meaning |
|---|---|---|---|---|
| 0x00-0xaf | 11 zero-size headers | | 1 PREALLOC_REQ / 7 PREALLOC_PERSIST_REQ | VA/size to allocate: (0x0,0x1dc) (0x4000,0x10210) (0x200,0x3adc) (0x100000,0x8ffc) (0x200000,0x12c80) (0x300000,0x1b240) (0x10004000,0x2ee0) (0x10007000,0xf84) (0x10007fdc,0x18) all type 1; (0x60000000,0x1000) (0x60001000,0x1000) type 7 |
| 0xb0 | 0x00000000 | 0x1dc | 0 data | ITCM; word0 = 0x10007f80 (initial SP), word1 = 0x105 (reset vector, Thumb) |
| 0x29c | 0x00004000 | 0x10210 | 0 data | main code, beyond ITCM (ITCM is only 16 KiB = 0x0-0x3fff): lives in DDR via SMMU |
| 0x104bc | 0x10007f98 | 0x28 | 0 data | DTCM, all zeros |
| 0x104f4 | 0x10007fdc | 0x18 | 0 data | DTCM: fdc=0xffffffff, fe0=0x40010009 (core fw ver), fe4=0xffffffff, fe8=0x40000011 (pwr ver), fec=0, ff0=0x20050000 (hfi ver) |
TCM mapping (same constants in both drivers): ITCM GMU VA 0x00000000 size 16K -> regs `GMU_CM3_ITCM_START` dword 0x1b400 = **0x03d6d000**; DTCM GMU VA 0x10004000 size 16K
-> `GMU_CM3_DTCM_START` dword 0x1c400 = **0x03d71000** ([LGMU: dtcm_base = 0x10004000 for a7xx]; [KGSL gen7_gmu_vma: GMU_DTCM start SZ_256M+SZ_16K]).
Procedure (single 32-bit STR per word, no memcpy):
1. Optional pattern test first: write 0xa5a5a5a5 / 0x5a5a5a5a to ITCM word 0x03d6d000+0x3ff0 and DTCM 0x03d71000+0x3ff0, read back, restore 0.
2. Write ITCM block: for i in 0..0x1dc/4: STR to 0x03d6d000 + 4*i. Write DTCM blocks: block at VA 0x10007f98 -> 0x03d71000 + 0x3f98, block at 0x10007fdc -> 0x03d71000 + 0x3fdc.
3. Read back and compare ITCM (0x77 words) and the DTCM blocks; print mismatch count and first mismatch.
4. Print firmware version words (DTCM + 0xff8..0xffc dwords = byte 0x03d74fe0, 0x03d74fe4, 0x03d74fe8, 0x03d74fec, 0x03d74ff0).
   This is exactly `GMU_CORE_FW_VERSION` (dword 0x1d3f8) = 0x03d74fe0. **Expected 0x40010009, 0xffffffff, 0x40000011, 0x00000000, 0x20050000**
   ([LGMU] prints it as v%u.%u.%u: MAJOR bits 31:28=4, MINOR 27:16=1, STEP 15:0=9 -> "v4.1.9"; with core > 0x20010004 Linux uses the new init-done test (val & 0x1ff)==0x100).
Risk: leaving TCM written is harmless. If readback == 0xdeadbeef-ish or bus error => clocks/GDSC (S6/S7). The GMU stays in reset, nothing executes.

### S10. SCM / TZ reachability for the later zap load (read-only SMC)  [LSCM qcom_scm.h 101-108, 114-115, __get_convention, qcom_scm_pas_supported]
Do this LAST (draw "NEXT: SCM" first; an unexpected SMC reaching TZ is the only non-MMIO risk). Convention: SMC64 standard call, fn = (1<<30)|(2<<24 SIP owner)|(svc<<8)|cmd,
x1 = arginfo (QCOM_SCM_ARGS(n) with VAL args = n), x2.. = args, results in x1.. (the shim's `smccc(0, fn, a1)` returns res.a0 as status; need a variant returning a1-a3).
- IS_CALL_AVAIL: svc 0x06 cmd 0x01 -> fn 0x42000601, x1 = 1, x2 = (2<<24)|(svc<<8)|cmd of the probed call. Probe: PIL AUTH_AND_RESET (svc 2 cmd 5) -> x2 = 0x02000205, PAS_INIT_IMAGE 0x02000201,
  MEM_SETUP 0x02000202, GPU INIT_REGS (svc 0x28 cmd 1) 0x02002801. Expected result[0] = 1 for the PIL ones.
- PAS_IS_SUPPORTED: svc 2 cmd 7 -> fn 0x42000207, x1 = 1, x2 = 13 (GPU_PAS_ID, [LGPU] `#define GPU_PAS_ID 13`). Expected result[0] = 1.
  (EOPNOTSUPP/0 would mean this TZ build has no zap-shader PAS for the GPU.)
Risk: low (this is how Linux detects it). If Gunyah does not forward SIP SMCs, expect a return code / "smc unknown" value; print x0..x3 raw.

### S11. Teardown
Leave the GMU in reset, leave clocks/GDSC on (probe ends in a hold loop, and the user power-cycles). Optionally collapse the GDSC (set bit0) after printing to leave a clean state; not required.
Print a final results summary line per step as `S<n> OK/FAIL` so one photo shows the whole result.

## 4. Probe 2 ("gpu2"): kgsl-smmu context for the GMU, start the M3, HFI. Only after probe 1 passes S9 and S10.

### P2.1 GPU SMMU read-only survey (needs S7's smmu clocks + CX GDSC, already on)  [DT 28966; arm-smmu MMU-500 register layout]
Print sCR0 (0x03da0000), sCR1/sCR2 (+4/+8), IDR0 (+0x20), IDR1 (+0x24), IDR2 (+0x28), IDR7 (+0x3c), sGFSR (+0x48), sGFSYNR0 (+0x50); from IDR1: NUMPAGENDXB, PAGESIZE (64K?), NUMCB, NUMSMRG.
SMR[n] at +0x800+4n, S2CR[n] at +0xc00+4n for n < NUMSMRG (dump all valid ones; the downstream DT lists no handoff SMRs for this SMMU), CB base = 0x03da0000 + (NUMPAGE/2)*PAGESIZE (+ cb*PAGESIZE):
dump CB0..CB3 SCTLR/TTBR0/TCR/FSR. Expected: sCR0 ~ ABL/KGSL leftovers or reset (CLIENTPD may be set). Needs: ABL may have left stage-1 for SID 0-2 only.
Risk: a protected SMMU page => SoC reset. Print "NEXT: smmu" first; this is the single most likely reset in probe 2.

### P2.2 Minimal translation for SID 5 (GMU)  [KGSL gen7_gmu_vma, Linux a6xx_gmu_memory_probe/alloc: gmu VM range 0..0x80000000]
GMU VAs the firmware needs (KGSL gen7_gmu_process_prealloc over the fw header blocks; Linux a6xx_gmu_init for a7xx hard-codes similar): 
code at 0x4000-0x15000 (copy fw block 2), zero-filled 0x100000+0x8ffc, 0x200000+0x12c80, 0x300000+0x1b240, persist 0x60000000+0x2000 (dummy page; Linux dummy is 16 KiB at 0x60000000
and "debug" 28 KiB at 0x60400000 for a660/a7xx), GMU log 64 KiB (KGSL GMU_LOG_SIZE; Linux 16 KiB, anywhere in 0x60000000+), HFI table 16 KiB, VRB 4 KiB (KGSL GENERAL_11).
Place all backing pages in a carve-out well away from reserved memory (e.g. a 1 MiB block at the HIGH_PHYS area the shim already uses, NOT below 4 GiB reserved pools); physical addresses are >= 0x80000000 (so the GMU can NOT
use identity/bypass: its VAs 0x4000.. are below DDR).
Stage-1 tables (AArch64, 4 KiB granule, 39-bit): L1 (512 x 8) -> L2 -> L3 covering the ranges above; descriptors normal WB-or-NC, `IOMMU_PRIV` for icache/dcache ranges (Linux uses MSM_BO_MAP_PRIV for 0x4000 region). Program one context bank (index c): CBA2R
(bit0 VA64 = 1), CBAR (TYPE=stage-1 only, VMID 0, BPSHCFG as dma-coherent?), TTBR0 = L1 PA, TCR (T0SZ=25, TG0=4K, SH0/IRGN/ORGN WB, EPD1=1, IPS from IDR2), MAIR0, SCTLR (M=1, AFE=0, TRE=0, CFRE...), then SMR[k] = VALID | ID=0x5 | MASK=0 and S2CR[k] = TYPE 0 (translate), CBNDX = c, and
sCR0 CLIENTPD=0 / GFRE etc. only if currently set otherwise. UNVERIFIED: how ABL/Gunyah/TZ left this SMMU (S2/S1 split, who owns SMRs), whether sCR0 accepts our write; this is the largest unknown of probe 2 and the reason it is separate.

### P2.3 Load, configure, release  [KGSL gen7_gmu_first_boot / gen7_gmu_register_config / gen7_gmu_device_start; LGMU a6xx_gmu_fw_start/a6xx_gmu_start]
Order (KGSL first_boot, then register_config values cross-checked with Linux):
1. CX_MISC TCM retention: write 1 to 0x03d9e0e4 (Linux a6xx_gmu_fw_start: `REG_A7XX_CX_MISC_TCM_RET_CNTL, 1`).
2. TCM load (S9) + copy fw block {0x4000, 0x10210} into its DDR pages; zero the prealloc ranges.
3. HFI table at VA H (16 KiB, zeroed): table header {version 0, size dwords of (24 B header + 3*48 B queue headers)/4 = 42, qhdr0_offset 6, qhdr_size 12, num_queues 3, active_queues 3} then queue headers at +24 each
   {status 1, iova = H+4K*(i+1), type (10<<8)|id with id 0/1/2 (non-legacy), size 0x400 dwords, msg_size 0, dropped 0, rx_watermark 1, tx_watermark 1, rx_request 1, tx_request 0, read_index 0, write_index 0}
   ([LHFI a6xx_hfi_init/a6xx_hfi_queue_init], structs in a6xx_hfi.h).
4. Register writes (GMU regs, addresses in S8 table): FALNEXT_INTF 0x03d7e3c4 <- 1; FAL_INTF 0x03d7e3c0 <- 1; FW_INIT_RESULT 0x03d7e070 <- 0; BOOT_CONFIG 0x03d7e004 <- 2;
   HFI_QTBL_ADDR 0x03d7e614 <- H; HFI_QTBL_INFO 0x03d7e610 <- 1; AHB_FENCE_RANGE_0 0x03d8ec44 <- BIT31 | (0x32<<18) | 0x8a0 = 0x80c808a0; CM3_CFG 0x03d7e0b4 <- 0x4052;
   GBIF CX: GBIF_QSB_SIDE0..3 (0x03d0f00c..0x03d0f018) <- 0x00071620; GBIF_HALT 0x03d0f114 <- 0; GENERAL_9 0x03d7e738 <- 0 (log wptr); GENERAL_10 0x03d7e73c <- 0x07020100 (Linux gmu_chipid for 0x43050a01 [LCAT]; KGSL uses a GMU_REV value, UNVERIFIED which the DYDC fw prefers);
   GENERAL_8 0x03d7e734 <- (log_VA & 0xfffff000) | (log_size/4K - 1); GENERAL_11 0x03d7e740 <- VRB VA (KGSL only, harmless to set); SYS_BUS_CONFIG 0x03d7d03c <- 1; ICACHE_CONFIG 0x03d7d000 <- 1; DCACHE_CONFIG 0x03d7d004 <- 1.
5. Release: write 0 to GMU_CM3_SYSRESET 0x03d7e000; poll GMU_CM3_FW_INIT_RESULT 0x03d7e070 until (val & 0x1ff) == 0x100 (100 ms; KGSL: `BIT(8)` with mask 0x1ff; Linux uses 0x100 when fw ver > 0x20010004, which is our case). Print the value every 10 ms.
6. HFI start: write 1 to HFI_CTRL_INIT 0x03d7e618, poll HFI_CTRL_STATUS 0x03d7e600 bit0 (10 ms). This is the "GMU alive and ready for HFI" signal in both drivers.
7. Dump: FW_INIT_RESULT, FW_BUSY 0x03d7e068, GMU2HOST_INTR_INFO 0x03d7e648, AO_HOST_INTERRUPT_STATUS 0x03d8ec14, GENERAL_0/1/6/7/9, CM3_BOOT_CONFIG, HFI_CTRL_STATUS, and the first 256 bytes (hex) of the GMU log buffer at its PA (the log is how the fw reports why init failed).
Success criteria in order: (a) SYSRESET reads 0 and does not reset the SoC, (b) FW_INIT_RESULT low 9 bits == 0x100, (c) HFI_CTRL_STATUS bit0 == 1. After (c) real HFI messages (GMU_FW_START etc. [LHFI a6xx_hfi_start]) and PDC/RSC ucode ([KGSL gen7_load_rsc_ucode: RSCC regs 0x3d50000 + {0x4,0xb,0xd,0xe,0x10,0x8,0x9,0xa,0x154..}, values 0xeaaae5a0 0xe1a1ebab 0xa2e0a581 0xecac82e2 0x0020edad; PDC 0xb284520 <- 0, 0xb284500 <- 0x80000001; note KGSL loads these AFTER device_start on first boot]) are later stages.
Not needed for (a)-(c): AOP QMP ACD message (KGSL gen7_gmu_aop_send_acd_state via aoss_qmp 0x0c300000), interconnect/DDR BW vote (KGSL votes bus_min for GMU init; the DDR path is already alive, but a missing BCM vote is a plausible reason for AXI timeouts: UNVERIFIED).

## 5. What to print format (shim style)
`con_hex2("S6 CXGDSC", "gdscr=", v, "hw=", h)` per step plus a final table. For polls print iterations used. Never loop without a timeout (use cntvct like rpmh.c `ticks()`), every wait <= 100 ms.

## 6. Zap shader (PAS id 13) requirements, for later feasibility  [LADRENO zap_shader_load_mdt, LGPU a6xx_zap_shader_init, LSCM qcom_scm.c/h]
- Files (from the phone): a740_zap.mdt (4044 B) = ELF32 header + 3 phdrs (0x94 B) + hash segment (0xf38 B); a740_zap.b00 (148 B, ELF header+phdrs), b01 (1072 B = 0x430 code/data segment), b02 (3896 B = 0xf38 hash). Segment table: phdr1 PT_LOAD va=pa=0x1000 filesz=memsz=0x430 flags 0x8000007; phdr2 hash 0xf38 at file off 0x2000 (my parse).
  Image to load = bytes of b01 (0x430) at region base + (p_paddr - min_paddr) = +0x0, size span = 0x430. Region from DT: `zap-shader { memory-region = <&gpu_micro_code_mem> }` -> 0x9b09a000..0x9b09c000 (8 KiB, no-map, reserved) [DT 1733-1737, 28673-28676]. Samsung DT has no firmware-name (their kgsl names the file internally): UNVERIFIED which name.
- Sequence in Linux (qcom_mdt_load -> qcom_scm PAS), executed from the primary VM:
  1. PAS_INIT_IMAGE: svc 2 cmd 1, fn 0x42000201, x1 arginfo = QCOM_SCM_ARGS(2, VAL, RW) = 2 | (0<<4) | (2<<6) = 0x82 (qcom_scm.h: num | a<<4 | b<<6 ...), x2 = 13, x3 = metadata PA. Metadata = ELF header + phdrs + hash segment = the whole 4044 B mdt, copied to a 4 KiB-aligned physically contiguous NON-cacheable buffer (MMU-off shim satisfies this); TZ makes the buffer secure during the call, so do not reuse it afterwards; returns result[0].
  2. PAS_MEM_SETUP: svc 2 cmd 2, fn 0x42000202, x1 arginfo = 3, x2 = 13, x3 = 0x9b09a000, x4 = size (0x430 span as qcom_mdt_get_size computes).
  3. Load segment bytes to 0x9b09a000 (plain stores; region is mapped for HLOS in Android, whether Gunyah maps it for our EL1 is UNVERIFIED -> probe write/readback first).
  4. PAS_AUTH_AND_RESET: svc 2 cmd 5, fn 0x42000205, x1 = 1, x2 = 13 (what Linux calls last; -EOPNOTSUPP => "this target needs no zap").
  5. Only then the CP executes `CP_SET_SECURE_MODE 0` through the ring [LGPU a6xx_hw_init] (needs GX up, SQE fw (a740_sqe.fw, 76724 B) loaded, RB set up), i.e. NOT before probe 3+.
- Dependencies: Linux calls it from hw_init after the GMU has powered GX (OOB GPU_SET, HFI) and the CP is running; whether TZ's auth_and_reset needs GX/CX powered or only the memory is UNVERIFIED.
- Preliminary S10 above (IS_CALL_AVAIL + PAS_IS_SUPPORTED(13)) answers "does this TZ expose it" for free.
- The zap is signed with the OEM key of this device (it comes from the same firmware DYDC); a Samsung-signed image on a Samsung-fused phone should authenticate; key/anti-rollback issues UNVERIFIED.

## 7. UNVERIFIED / could not confirm
- Exact KGSL (Kalama) gen7_2_1 boot differences; I used SM8750-vintage KGSL + upstream Linux a740 support (Linux treats 0x43050a01 = our DT's "gen7-2-1"? only by name: DT compatible "qcom,adreno-gpu-gen7-2-1", [DT 28670]).
- That the apps VM may touch: kgsl-smmu (0x03da0000), GMU PDC (0x0b280000), RSCC (0x03d50000), GBIF_CX, cx_misc. All are in the phone's kgsl/gmu DT nodes (Android's VM does it), but any of them may be XPU-protected after ABL. Reads were ordered so protected pages show up in the photo trail.
- Samsung DT clock index to register mapping is by name (indices are downstream numbering); `axi_clk`=gcc 0x0d is inferred as GCC_DDRSS_GPU_AXI_CLK.
- Whether ABL/TZ initialised GPU_CC PLLs or left any of the S2/S5 branches on (probe 1 prints it).
- ARC level targets (cx NOM, mx/mxc SVS) are my choice; their sufficiency for UFS/display coexisting is unverified.
- GMU chip-id value (GENERAL_10) for the KGSL firmware; Linux value 0x07020100 comes from the upstream catalog entry for this exact fw name (gmu_gen70200.bin), not from Samsung.
- DDR bandwidth (BCM) votes and the AOP ACD message may be needed for stable GMU AXI traffic.
- Whether A740 (this phone) requires `qcom_scm_gpu_init_regs` (Linux only calls it for A750+ [LGMU a6xx_gmu_secure_init]; KGSL calls it generally and tolerates EOPNOTSUPP). S10 probes availability.

## 8. Risks and recommended split

Biggest risks, in order:
1. A protected/unclocked MMIO access resets the SoC (as the TLMM page did). Mitigations: only DT ranges, GCC_GPU_CFG_AHB before GPU_CC, CX GDSC + GMU clocks before the GMU window, "NEXT:" lines before S5, S8 (rscc/pdc block last), P2.1.
2. GX-domain access (anything in 0x03d00000-0x03d3ffff except GBIF_CX) with GX off => AHB error/reset. We never power GX.
3. RPMh ARC vote too low or index/level mix-up (data is the aux-table INDEX). Mitigation: print tables first (S1), vote >= SVS.
4. kgsl-smmu programming (probe 2): unknown ownership; wrong CB/SMR could fault the GMU or other GPU streams, but not other devices. Separate probe.
5. Thermal: GMU + clocks at XO are cold; keep PLLs off in probes 1-2.

Recommended split:
- **Probe 1 (gpu1)**: S1-S10 as above. Output proves: cmd-db ARC names/tables, GCC GPU branches, RPMh ARC votes ok, GPU_CC CX GDSC on, DEMET/AHB/CX_GMU/CXO/HUB branches on, GMU register window alive, TCM write/readback, fw version 0x40010009 readable, CX_MISC/GBIF/RSCC/PDC visible, TZ exposes PAS 13. No SMMU writes, no GMU execution. If S9 passes the GMU side is "powered and loadable".
- **Probe 2 (gpu2)**: P2.1-P2.3: SMMU survey, minimal CB for SID 5, DDR carve-out, M3 release, FW_INIT_RESULT / HFI_CTRL_STATUS, GMU log dump. Add PLL1/GPLL0 + 219 MHz GMU clock only if XO-clocked boot is flaky (PLL1: gpucc-sm8550.c gpu_cc_pll1_config; RCG GMU 0x9318 src PLL1/2).
- Later: PDC/RSC ucode, HFI messages (GMU_FW_START/perf/bw tables), OOB + GX via GMU, SQE load, zap (section 6).
