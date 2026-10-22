# Adreno 740 (SM8550 / dm3q): stages after "GMU answers HFI" up to "CP executes a command buffer"

Status: research only, nothing flashed. Written 2026-10-21. Continues `A740-POWERON.md` (probes 1 and 2). Target: `port/shim` (EL1, MMU off,
Gunyah underneath, plain single-register LDR/STR for MMIO, poll everything, no interrupts, no thermal driver: keep every probe short and at low GPU clocks).

Source tags (all fetched 2026-10-21):
- `[LHFI]` linux drivers/gpu/drm/msm/adreno/a6xx_hfi.c + a6xx_hfi.h; `[LGMU]` a6xx_gmu.c; `[LGPU]` a6xx_gpu.c; `[LCAT]` a6xx_catalog.c; `[LADRENO]` adreno_gpu.c;
  `[LPRE]` a6xx_preempt.c; `[LXML]` drivers/gpu/drm/msm/registers/adreno/{a6xx.xml,a6xx_gmu.xml,adreno_pm4.xml,adreno_common.xml};
  `[LSMMU]` drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c; `[LMSMIOMMU]` drivers/gpu/drm/msm/msm_iommu.c; `[LSCM]` drivers/firmware/qcom/qcom_scm.{c,h};
  `[LGPUCC]` drivers/clk/qcom/gpucc-sm8550.c; `[LALPHA]` drivers/clk/qcom/clk-alpha-pll.c; `[LDTSI]` arch/arm64/boot/dts/qcom/sm8550.dtsi
  (all https://raw.githubusercontent.com/torvalds/linux/master/<path>).
- `[KGSL]` Qualcomm KGSL (SM8750-vintage public mirror, same gen7 code and same firmware names as the phone):
  https://gitea.com/OnePlus13-Development/android_kernel_qcom_sm8750-modules/src/branch/lineage-22.2/qcom/opensource/graphics-kernel/ files
  `adreno_gen7.c` (gen7_start, gen7_rb_start, gen7_cp_init_cmds), `adreno_gen7_gmu.c` (gen7_gmu_first_boot, gen7_gpu_boot, oob, rsc/pdc),
  `adreno_gen7_hfi.c` (gen7_hfi_start, queue code), `adreno_gen7_rpmh.c` + `adreno_rpmh.c` (vote tables), `adreno_gen7_ringbuffer.c`, `adreno_hfi.h`,
  `gen7_reg.h`, `adreno_pm4types.h`, `adreno-gpulist.h` (adreno_gpu_core_gen7_2_1 = our DT compatible `qcom,adreno-gpu-gen7-2-1`), `kgsl_util.c` (zap), `kgsl_iommu.c`.
  NOTE: KGSL for gen7_2_1 runs "hwsched" (GMU schedules submissions over HFI context queues). Linux runs the SAME firmware (gmu_gen70200.bin,
  a740_sqe.fw) in plain ringbuffer mode on SM8550 (a740 catalog entry `[LCAT]` ~l.1537), so THIS DOCUMENT FOLLOWS LINUX for the HFI/ring flow and uses KGSL as a cross-check.
- `[DT]` /Users/tomaswarner/Desktop/hackios s23/fw/dts/merged-rev9.dts (gpu node `qcom,kgsl-3d0@3d00000` ~l.28659, gmu node ~l.41156, kgsl-smmu ~l.28966).
- `[FW]` my own parse of /Users/tomaswarner/Desktop/hackios s23/firmware/adreno-DYDC/.
- `[P1]`/`[P2]` A740-POWERON.md section 3 / 4, `[gpu.c]` port/shim/gpu.c (gpu2_probe).
- Local copies of every fetched source are in the session scratchpad `/private/tmp/claude-501/-Users-tomaswarner-Desktop-hackios-s23/a874681b-7d77-46bb-a8e0-74641f66ed4d/scratchpad/src/`
  (k_*.c/h = KGSL).

Conventions: `G(dw)` = 0x03d00000 + 4*dw (GPU/GMU/GPU_CC dword offsets as in the XML/gen7_reg.h). `RSCC(dw)` = 0x03d50000 + 4*dw. `PDC(dw)` = 0x0b280000 + 4*dw.
"CX" regs = GMU window / CX_MISC / GBIF_CX / RSCC / PDC / kgsl-smmu (readable with GX off). "GX" regs = every other register in 0x03d00000-0x03d3ffff (RBBM, CP, UCHE, SP...,
fault/reset if GX is off, `[P1]` danger list).

---------------------------------------------------------------------------------------------------------------------------------------

## 0. Bottom line and corrections to the existing probe-2 design

1. After the GMU is released (SYSRESET=0, FW_INIT_RESULT&0x1ff==0x100) and HFI_CTRL_STATUS bit0 is set, the A7xx GMU is **not** driven with "GMU_FW_START / H2F_MSG_INIT /
   FW_VERSION / TEST". Those (ids 0,1,5) are the a630 "legacy HFI v1" flow (`[LHFI] a6xx_hfi_start_v1`, only when `gmu->legacy`, which `[LGMU] a6xx_gmu_init` sets only for a630 family).
   The gen7 flow is exactly: **PERF_TABLE(4) -> BW_TABLE(3) -> [ACD(7) skip] -> [IFPC feature(11) skip] -> CORE_FW_START(14) -> START(10)** (`[LHFI] a6xx_hfi_start`,
   `[KGSL] gen7_hfi_start` + `gen7_gpu_boot`), then freq vote GX_BW_PERF_VOTE(30), then OOB_GPU_SET (a doorbell, not an HFI message) powers GX.
2. **RSC + PDC programming is required before GX can power** (GX/gfx.lvl is voted by the GMU through the RSC sequencer; Linux loads it once at probe `[LGMU] a6xx_gmu_rpmh_init`,
   KGSL loads it right after SYSRESET release and before HFI_CTRL_INIT `[KGSL] gen7_gmu_first_boot`: `gen7_load_pdc_ucode` + `gen7_load_rsc_ucode`). `[gpu.c]` probe 2 does not do this yet:
   add it to probe 3 (values in section 2). It is also needed so that RSC sleep/wake is sane.
3. **GPU_CC PLL0 (the GX PLL) must be configured by the host** (Linux does it at gpucc probe, `[LGPUCC] gpu_cc_sm8550_probe: clk_lucid_ole_pll_configure(pll0/pll1)`), the GX clock
   itself is owned by the GMU (Linux has no gfx3d RCG clock, only `gpu_cc_mnd1x_*_gfx3d_clk` branches). Section 3.0. UNVERIFIED that ABL/Android left PLL0 configured; probe 1 S5 printed it, check the log first.
4. GPU pagetable: **no CP-driven pagetable switching is needed** for one global table. KGSL itself does not emit CP_SMMU_TABLE_UPDATE for the default pagetable
   (`[KGSL] adreno_gen7_ringbuffer.c gen7_rb_pagetable_switch`: `if (pagetable == device->mmu.defaultpagetable) return 0;`). We program the CB TTBR0 from the CPU. Section 5.
5. Zap: no non-zap path is documented for a *locked* retail phone; the only fallback is Linux's `RBBM_SECVID_TRUST_CNTL = 0` write, which only works when TZ leaves the register accessible
   (Chromebook-style). Plan: try CP work first WITHOUT zap, then fallback, then the SCM PAS flow. Section 8. Everything is UNVERIFIED on this phone (Gunyah).
6. Small corrections to `[P2]`/`[gpu.c]`: (a) `[P2]` writes the HFI table `size` field as dwords (6+3*12=42), Linux writes **bytes** (`table_size = sizeof(table)+3*sizeof(qhdr)` = 168, `[LHFI] a6xx_hfi_init`)
   while KGSL writes dwords of a 17-queue-header struct (`[KGSL] init_queues`: sizeof(hfi_queue_table)>>2). The firmware evidently reads only qhdr0_offset/qhdr_size/num_queues; keep what is there,
   but if HFI misbehaves flip it. (b) the gmu-freq-table in `[DT]` is 0xd1cef00 = **220 MHz** (vote level 0x40) and 0x20c85580 = 550 MHz (0x80), not 219. (c) Linux also maps a 28 KiB "debug" BO at GMU VA 0x60400000
   and a 16 KiB dummy at 0x60000000 for a7xx (`[LGMU] a6xx_gmu_init`); `[gpu.c]` maps only 0x60000000+0x2000: also map 0x60000000+0x4000 and 0x60400000+0x7000 (cheap insurance, GMU fw may touch them).
7. `PAS_IS_SUPPORTED==0` is irrelevant: `[LADRENO] zap_shader_load_mdt` never calls it; it goes straight to init_image/mem_setup/auth_and_reset. IS_CALL_AVAIL(init/auth)=1 (seen in probe 1) is the useful bit.

---------------------------------------------------------------------------------------------------------------------------------------

## 1. State assumed at the start of stage 3 (what probe 2 must have achieved)

- CX GDSC on, GMU clocks (XO), kgsl-smmu context for SID 5 with GMU code/data mapped, GMU M3 released, `FW_INIT_RESULT & 0x1ff == 0x100`, HFI_CTRL_INIT=1 -> HFI_CTRL_STATUS bit0 = 1.
- HFI memory (16 KiB, GMU VA `VA_HFI`, CPU-visible at the carve-out PA): table header + 3 queue headers at +0, queue0 (host->GMU "command") at +0x1000, queue1 (GMU->host "response/msg") at +0x2000,
  queue2 (GMU->host "debug") at +0x3000, each 0x400 dwords (4 KiB), queue header `type = (10<<8)|id` id=0,1,2, `status=1`, `iova` as above (`[gpu.c] hfi_table`, `[LHFI] a6xx_hfi_queue_init`, `[KGSL] init_queues`).
- GMU register map in `[P1]` S8 / `[P2]` P2.3 (SYSRESET 0x03d7e000, FW_INIT_RESULT 0x03d7e070, HFI_CTRL_INIT 0x03d7e618, HFI_CTRL_STATUS 0x03d7e600, HFI_QTBL_ADDR 0x03d7e614 ...).
- The probe-2 dump of the GMU log buffer (GENERAL_8 region) is the first diagnostic for every failure below.

---------------------------------------------------------------------------------------------------------------------------------------

## 2. Stage 3a: RSC + PDC sequencer programming (required before GX)

When: KGSL order = after SYSRESET release and FW_INIT_RESULT ok, **before** HFI_CTRL_INIT (`[KGSL] gen7_gmu_first_boot`: device_start -> `gen7_load_pdc_ucode` -> `gen7_load_rsc_ucode` -> `gen7_gmu_hfi_start`).
Linux order = at probe, i.e. before the first GMU release (`[LGMU] a6xx_gmu_init -> a6xx_gmu_rpmh_init`). Either works on real devices; since probe 2 will already have released the M3, do it KGSL's way
(load between FW_INIT_RESULT and HFI_CTRL_INIT; if the GMU is already past that, load now anyway before the first OOB). UNVERIFIED which ordering the Samsung-signed GMU image is picky about.
Gunyah/XPU risk: RSCC and PDC windows are in the phone DT (`[DT]` msm_gpu reg "rscc" 0x03d50000, gmu reg "gmu_pdc" 0x0b280000); probe 1 already read them without a fault.

Values are the gen7_2_x ("A740 family") variants: `[LGMU] a6xx_gmu_rpmh_init` (`pdc_in_aop=true` for a7xx: the PDC sequence/TCS microcode is loaded by AOP, host only enables) and `[KGSL] gen7_load_rsc_ucode/gen7_load_pdc_ucode`
(`adreno_is_gen7_2_x_family`). Register dword offsets `[LXML a6xx_gmu.xml l.244-268]` / `[KGSL gen7_reg.h l.1261-1301]`:

RSCC (byte = 0x03d50000 + 4*dw), 32-bit stores in this order:
| step | dw | byte | value |
|---|---|---|---|
| 1 GPU_RSCC_RSC_STATUS0_DRV0 ("disable SDE clock gating") | 0x004 | 0x03d50010 | 0x01000000 (BIT24) |
| 2 RSCC_PDC_SLAVE_ID_DRV0 | 0x00b | 0x03d5002c | 1 |
| 3 HIDDEN_TCS_CMD0_DATA | 0x00e | 0x03d50038 | 0 |
| 4 HIDDEN_TCS_CMD0_ADDR | 0x00d | 0x03d50034 | 0 |
| 5 HIDDEN_TCS_CMD0_DATA+2 | 0x010 | 0x03d50040 | 0 |
| 6 HIDDEN_TCS_CMD0_ADDR+2 | 0x00f | 0x03d5003c | 0 |
| 7 HIDDEN_TCS_CMD0_DATA+4 | 0x012 | 0x03d50048 | **0x80000021** (A740/gen7_2_x; 0x80000000 on other a7xx) |
| 8 HIDDEN_TCS_CMD0_ADDR+4 | 0x011 | 0x03d50044 | 0 |
| 9 RSCC_OVERRIDE_START_ADDR | 0x100 | 0x03d50400 | 0 |
| 10 RSCC_PDC_SEQ_START_ADDR | 0x008 | 0x03d50020 | 0x4520 |
| 11 RSCC_PDC_MATCH_VALUE_LO | 0x009 | 0x03d50024 | 0x4510 |
| 12 RSCC_PDC_MATCH_VALUE_HI | 0x00a | 0x03d50028 | 0x4514 |
| 13-17 RSCC_SEQ_MEM_0_DRV0 (A740 variant base **dw 0x154**, `REG_A7XX_RSCC_SEQ_MEM_0_DRV0_A740` / `GEN7_2_0_RSCC_SEQ_MEM_0_DRV0`) | 0x154..0x158 | 0x03d50550..0x03d50560 | 0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad |
PDC (AOSS, `gmu_pdc` window):
| 18 PDC_GPU_SEQ_START_ADDR | 0x1148 | 0x0b284520 | 0 |
| 19 PDC_GPU_ENABLE_PDC | 0x1140 | 0x0b284500 | 0x80000001 |
Then a DSB (Linux `wmb()`). Read back steps 13-17 and 19 and print them (probe 1 saw PDC 0x0b284500 = 0x101 before, i.e. ABL/Android left something there; 0x80000001 is the enabled value).
Do NOT write the a6xx (non-aop) PDC TCS microcode (`pdc_write(seqptr...)`), a7xx has `pdc_in_aop`.

RSC wake/sleep handshakes (not needed for first boot): `a6xx_rpmh_start` is skipped on first boot (`GMU_STATUS_PDC_SLEEP` clear); at teardown see section 10.
Sanity reads after the load: RSCC_SEQ_BUSY_DRV0 (0x03d50404) == 0, RSC_STATUS0_DRV0 (0x03d50010), GMU_RSCC_CONTROL_ACK (0x03d8ec20).

---------------------------------------------------------------------------------------------------------------------------------------

## 3. Stage 3b: HFI messages (poll-only)

### 3.1 Queue protocol (all `[LHFI]`, cross-checked with `[KGSL] adreno_gen7_hfi.c`)
- Header dword: `seq<<20 | type<<16 | size_dwords<<8 | id` (`[LHFI] a6xx_hfi_send_msg`: `(seqnum << 20) | (HFI_MSG_CMD << 16) | (dwords << 8) | id`; `[KGSL] MSG_HDR_SET_SEQNUM_SIZE`, `CREATE_MSG_HDR`).
  type 0 = command (HFI_MSG_CMD), 1 = ACK (HFI_MSG_ACK), 2 = ACK_V1. `size` field is 8 bits => max 255 dwords per message. seq: `atomic_inc_return(&queue->seqnum) % 0xfff`, first message seq=1
  (KGSL uses a global counter, no constraint beyond uniqueness among in-flight messages).
- Host writes to queue 0 (command): data starts at `hfi_va + 0x1000`, indices `write_index`/`read_index` are **dword** indices into the 0x400-dword queue (qhdr dwords 11 / 10: `{status, iova, type, size, msg_size, dropped,
  rx_wm, tx_wm, rx_req, tx_req, read_index, write_index}` so read_index is header dword 10, write_index dword 11). Copy the message, then **pad with 0xfafafafa up to a 4-dword boundary** (`!gmu->legacy`
  branch of `a6xx_hfi_queue_write`, `[KGSL] gen7_hfi_queue_write align_size = ALIGN(size,4)`), DSB, store new write_index (wrapping mod 0x400), DSB, then ring the doorbell:
  **write 0x1 to GMU_HOST2GMU_INTR_SET (dw 0x1f994 = 0x03d7e650)**. Free-space check: `CIRC_SPACE(w, r, 0x400) >= dwords` (Linux) / `empty_space > align_size` (KGSL).
- GMU responds in queue 1 (response): data at `hfi_va + 0x2000`. Reader: while `read_index != write_index`: read header, take `size` dwords (index wraps mod 0x400), then **round read_index up to a multiple of 4**
  (`index = ALIGN(index,4) % size`, `a6xx_hfi_queue_read` non-legacy), DSB, store read_index. GMU also asserts **GMU_GMU2HOST_INTR_INFO bit0 (MSGQ, dw 0x1f992 = 0x03d7e648)**; clear it with
  **GMU_GMU2HOST_INTR_CLR (dw 0x1f991 = 0x03d7e644) <- 1** (`a6xx_hfi_wait_for_msg_interrupt`: poll `val & MSGQ` 100 us step / 1 s timeout; KGSL poll_gmu_reg HFI_RSP_TIMEOUT 1000 ms).
  Without interrupts: clear bit0 before sending, then poll `INTR_INFO bit0` OR `qhdr1.write_index != qhdr1.read_index`; you may simply poll the index (memory) and still clear bit0.
  Also watch `INTR_INFO bit23` = CM3_FAULT (GMU crashed) and `FW_INIT_RESULT & 0xE00` (NMI/assert) (`[KGSL] poll_gmu_reg`), and dump the GMU log on timeout.
- ACK layout (`[LHFI] struct a6xx_hfi_msg_response`, `[KGSL] gen7_receive_ack_cmd/CMP_HFI_ACK_HDR`): `dw0 = ack header (id 126 F2H_MSG_ACK, type 1 or 2, size n)`,
  `dw1 = ret_header = the header you sent (seq in bits 31:20)`, `dw2 = error (0 = ok)`, `dw3.. = payload (up to 16 dwords)`. Match `seq(dw1)`; ignore (but log) unrelated messages:
  `F2H_MSG_ERR=100` (`{hdr, code, payload[2]}`), `F2H_MSG_DEBUG=101`, `F2H_MSG_LOG_BLOCK=102`. For KGSL's v5 handling (HFI minor >4) payload word0 is a `gmu_ret_type` (0 ok, 6 timeout, 7 not supported, 8 no entry) -
  our GMU reports HFI 0x20050000 (v2.5.0) in DTCM 0xffc [P1], so treat nonzero dw2 as failure and print dw2/dw3.
- Queue 2 (debug) is written by the GMU only for debug messages; Linux leaves its IRQ masked and never reads it. Drain it for diagnostics only.
- Nothing in these flows needs interrupts unmasked: Linux enables only CM3_FAULT (`A6XX_HFI_IRQ_MASK`) after boot; keep GMU2HOST_INTR_MASK / AO_HOST_INTERRUPT_MASK at ~0 (all masked), which is the probe-2 state.
- Memory: HFI queue memory is DDR seen by the GMU through SID 5; CPU stores with MMU off are device-ordered, but put a DSB SY between payload, index and doorbell.

### 3.2 Message list in order (sizes in dwords; every message after the table messages is tiny)
HFI version from the DTCM version words is v2.5.0 (<2.6.1) so the perf table goes in the legacy-format message id 4 (`[KGSL] gen7_hfi_send_gpu_perf_table`: `gmu->ver.hfi >= HFI_VERSION(2,6,1)` selects H2F_MSG_TABLE(15), else H2F_MSG_PERF_TBL(4);
Linux sends id 4 always except a8xx).

| # | message | id | dwords | payload (source) |
|---|---|---|---|---|
| 1 | PERF_TABLE | 4 | 59 | `{hdr, num_gpu_levels, num_gmu_levels, gx_votes[16]{vote, acd, freq_kHz}, cx_votes[4]{vote, freq_kHz}}` = 1+2+48+8; unused slots 0; (`[LHFI] a6xx_hfi_send_perf_table`, `struct a6xx_hfi_msg_perf_table`, `[KGSL] struct hfi_dcvstable_cmd`) |
| 2 | BW_TABLE | 3 | 160 | `{hdr, bw_level_num, cnoc_cmds_num, ddr_cmds_num, cnoc_wait_bitmask, ddr_wait_bitmask, cnoc_addrs[6], cnoc_data[2][6], ddr_addrs[8], ddr_data[16][8]}` = 1+5+6+12+8+128 (`struct a6xx_hfi_msg_bw_table`) |
| - | ACD table (feature 12 + id 7) | 11 / 7 | | SKIP (only `if acd_table->enable_by_level`; needs the AOP QMP ACD message first, `[KGSL] gen7_gmu_aop_send_acd_state`). Not required at low clocks. UNVERIFIED |
| - | IFPC feature | 11 | | SKIP: A740 catalog has no ADRENO_QUIRK_IFPC so `idle_level != IFPC` (`[LGMU] a6xx_gmu_init`, `[LHFI] a6xx_hfi_enable_ifpc`). (KGSL enables IFPC; we do not.) |
| 3 | CORE_FW_START | 14 | 2 | `{hdr, handle=0}` (`a6xx_hfi_send_core_fw_start`) |
| 4 | START | 10 | 1 | `{hdr}` (`a6xx_hfi_send_start`; Linux sends it here, KGSL sends it after OOB_GPU_SET inside gen7_gpu_boot. Follow Linux) |
| 5 | GX_BW_PERF_VOTE | 30 | 4 | `{hdr, ack_type=1 (blocking), freq=perf_index, bw=0}`, perf_index 1 = lowest real OPP, 0 = "off" (`a6xx_hfi_set_freq`, `a6xx_gmu_set_freq`; `[KGSL] gen7_gmu_dcvs_set`: `req.freq = gpu_level_num - gpu_pwrlevel - 1`) |
Each message is acked individually; do not pipeline, wait for the ack of message N before sending N+1 (Linux/KGSL both do).

### 3.3 Building the perf table from the phone's DT (`[DT]` gpu node `qcom,gpu-pwrlevel-bins`, gmu node)
The vote word for a level is `ARC_VOTE_SET(pri_idx, sec_idx, vlvl) = vlvl<<16 | sec_idx<<8 | pri_idx` (`[KGSL] adreno_rpmh.h ARC_VOTE_SET`, `adreno_rpmh_setup_volt_dependency_tbl`;
`[LGMU] a6xx_gmu_rpmh_arc_votes_init` is the same: `((pri[pindex] & 0xffff) << 16) | (sindex << 8) | pindex`), where
- `pri_idx` = first index k in the cmd-db aux table of the primary rail (`gfx.lvl` for GX, `cx.lvl` for the GMU/CX levels) with `val[k] >= vlvl`; `pri val` = `val[k]`;
- `sec_idx` = first index j of the secondary table with `val[j] >= pri val` (else the last non-zero entry; KGSL stops at `j+1==num`). Secondary: `mx.lvl` (KGSL always; Linux prefers `gmxc.lvl` if cmd-db has it: `[LGMU] a6xx_gmu_rpmh_votes_init`); print whether `gmxc.lvl` exists (probe-1 S1 read `mx.lvl`, `mxc.lvl`, `gfx.lvl`, not `gmxc.lvl`). UNVERIFIED which the Samsung firmware wants; use `mx.lvl` (KGSL = the phone's driver).
- the aux table is read with the same code as probe 1 (`rpmh_cmddb_*`), 16-bit entries, count = entry len/2 up to the zero padding (`[KGSL] adreno_rpmh_arc_cmds`).
Level 0 is always the "off" entry: `{vote 0, acd/dep 0xffffffff, freq 0}` (`[KGSL] setup_gx_arc_votes`: `gx_votes[0].freq=0; dep_vote=0xffffffff when DT has no cx-level`; Linux: `acd = 0xffffffff` for all, `gpu_freqs[0]=0` from `a6xx_gmu_build_freq_table`).
Entries are **ascending in frequency**, freq in kHz, `acd` field = 0xffffffff (our DT has no `qcom,cx-level`, so KGSL also sends 0xffffffff).

GPU levels from `[DT]` (bin 0 sku-codes list has 9 real levels, bin 1 "sku 0" has the same minus the 719 MHz; `qcom,level` = RPMh vlvl; same numbers as upstream opp-level RPMH_REGULATOR_LEVEL_*, `[LDTSI]`):
| ascending idx | freq (kHz) | DT qcom,level (vlvl) |
|---|---|---|
| 0 (off) | 0 | 0 |
| 1 | 220000 (0x0d1cef00 Hz) | 0x34 = 52 |
| 2 | 295000 (0x119557c0) | 0x38 = 56 |
| 3 | 348000 (0x14be0f00) | 0x3c = 60 |
| 4 | 401000 (0x17e6c640) | 0x40 = 64 |
| 5 | 475000 (0x1c4fecc0) | 0x50 = 80 |
| 6 | 550000 (0x20c85580) | 0x80 = 128 |
| 7 | 615000 (0x24a827c0) | 0x90 = 144 |
| 8 | 680000 (0x2887fa00) | 0xc0 = 192 |
| 9 | 719000 (0x2adb11c0) bin 0 only | 0xe0 = 224 |
(A 10th DT entry with freq 0x0761704c = 123826252 Hz at vlvl 0x34 is a floor entry, ignore.) Linux upstream deliberately keeps only <=680 MHz on A740 ("Speedbin needs more work", `[LDTSI]`).
**Recommended table for probe 3: num_gpu_levels = 4 (off, 220, 295, 348 MHz)**: the GMU cannot pick anything faster than 348 MHz, which also bounds heat (no thermal driver). Expand later.
GMU (CX) levels: KGSL `setup_cx_arc_votes`: `{vote 0, freq 0}`, `{vote(cx.lvl,mx.lvl, vlvl 0x40=64), 220000}`, `{vote(.., vlvl 0x80=128), 550000}`, `num_gmu_levels = 3`
(from `qcom,gmu-freq-table = <0xd1cef00 0x40 0x20c85580 0x80>`). With the probe-1 cmd-db table cx.lvl = {0,16,64,128,192,256,320,384,416,480} and mx.lvl identical: vlvl 64 -> pri_idx 2, sec_idx 2 ->
**0x00400202**; vlvl 128 -> pri_idx 3, sec_idx 3 -> **0x00800303** (if mx.lvl really equals cx.lvl, recompute from the live tables).
GX example: vlvl 52 must be found in the live `gfx.lvl` aux table (probe 1 says gfx differs from cx): compute at run time, print the table and every vote word before sending. If a vlvl is missing,
the table build fails exactly like Linux ("Level %u not found in the RPMh list"): then choose the next listed level.

### 3.4 BW table (`[LHFI] a740_build_bw_table`, `a6xx_hfi_send_bw_table`; no OPP bw table on our side so the "single entry" path)
`bw_level_num=1; ddr_cmds_num=3; ddr_wait_bitmask=0x7; ddr_cmds_addrs[0..2] = cmd_db_read_addr("SH0"), ("MC0"), ("ACV"); ddr_cmds_data[0][0..2] = 0x40000000;`
`cnoc_cmds_num=1; cnoc_wait_bitmask=0x1; cnoc_cmds_addrs[0]=cmd_db_read_addr("CN0"); cnoc_cmds_data[0][0]=0x40000000; cnoc_cmds_data[1][0]=0x60000001;` everything else 0.
0x40000000 = BCM_TCS_CMD(commit, valid=0) "off", 0x60000001 = commit|valid|vote BIT0 (`[LHFI] a6xx_generate_bw_table` comment). The four addresses come from the shim's cmd-db walker (`rpmh_cmddb_addr("SH0"|"MC0"|"ACV"|"CN0")`,
nonzero expected like the ARC ones; a 0 means the name is absent -> send 0 and expect GMU error). Consequence: the GMU never votes DDR/NOC bandwidth for the GPU. DDR/NOC are alive (CPU, display), so a handful
of GPU stores works; for heavier traffic a real BCM vote may be needed (KGSL builds levels from `qcom,bus-table-ddr`: `[DT]` list 0x0 0x209a8e ... , and `icc_set_bw` for GMU init). UNVERIFIED.

### 3.5 Sequence, timeouts and failure triage for stage 3b
1. clear `GMU2HOST_INTR_INFO` (write ~0 to INTR_CLR) and DSB; read qhdr1 indices (both 0).
2. send #1..#4 (timeout 1 s each, poll step 100 us). After each: print id, seq, ack dw2 and the first payload dw. Print GMU log buffer head after the last.
3. send #5 only after #4 acked. Expect the ack quickly (GMU just stores the perf level; GX is not up yet).
4. Failure triage: no ack and `INTR_INFO bit23` -> GMU fault, dump log. Ack with dw2!=0 -> wrong table (print votes). No ack, no fault -> M3 stuck waiting (RSC/PDC missing? DEMET clock off? [gotcha 12.1]) or GMU cannot read the queue (SMMU fault: check CB FSR/FAR of GMU's CB).

---------------------------------------------------------------------------------------------------------------------------------------

## 4. Stage 4: power GX (GMU does it), OOB protocol, polls, frequency

### 4.0 GPU_CC PLL0/PLL1 configuration (host, before the first OOB)  `[LGPUCC] gpu_cc_sm8550_probe`, `[LALPHA] clk_lucid_ole_pll_configure`, `[P1]` S5
Read first (print): PLL0 at GPU_CC+0x0 (MODE 0x03d90000, OPMODE +4, STATE +8, STATUS +0xc, L +0x10, ALPHA +0x14), PLL1 at GPU_CC+0x1000. If L_VAL is 0 (unconfigured) configure both, **without enabling them**
(the GMU enables PLL0 and switches the GX RCG; the host never enables PLL0/PLL1 in Linux, only the GMU/CX RCG users do). Sequence per PLL (offsets from `clk_alpha_pll_regs[LUCID_OLE]`: OPMODE 0x04, L 0x10, ALPHA 0x14, USER_CTL 0x18, USER_CTL_U 0x1c, CONFIG_CTL 0x20, CONFIG_CTL_U 0x24, CONFIG_CTL_U1 0x28, TEST_CTL 0x2c, TEST_CTL_U 0x30, TEST_CTL_U1 0x34, TEST_CTL_U2 0x38; MODE at +0x0):
| reg | PLL0 (base 0x03d90000) | PLL1 (base 0x03d91000) |
|---|---|---|
| L_VAL (`l | 0x44<<16 | 0x44<<24`, TRION_PLL_CAL_VAL=0x44, shifts 16 and 24) | 0x4444001e | 0x44440016 |
| ALPHA_VAL | 0xbaaa | 0xeaaa |
| CONFIG_CTL | 0x20485699 | same |
| CONFIG_CTL_U | 0x00182261 | same |
| CONFIG_CTL_U1 | 0x82aa299c | same |
| USER_CTL | 0x00000000 | same |
| USER_CTL_U | 0x00000005 | same |
| TEST_CTL / U / U1 / U2 | 0x0 / 0x3 / 0x9000 / 0x34 | same |
Then: MODE.OUTCTRL (bit0) <- 0 (RMW clear), OPMODE <- 0 (STANDBY), MODE.RESET_N (bit2) <- 1 (RMW set). PLL0 = L 0x1e => 19.2 MHz x (30 + 0xbaaa/2^16) ~ 600 MHz VCO; GMU picks the divide. UNVERIFIED: whether ABL/Android already did this (then skip) and
whether the GMU firmware also does it itself. This is cheap and idempotent, do it only if L_VAL reads 0.
GX GDSC (0x03d9905c): do **not** toggle SW_COLLAPSE (the GMU does, `[LGPUCC] gpu_cc_gx_gdsc .power_on = gdsc_gx_do_nothing_enable`: host side is a no-op by design). Optional first-attempt-B only: apply the `gdsc_init` config fields
(EN_REST_WAIT[23:20]=2, EN_FEW_WAIT[19:16]=2, CLK_DIS_WAIT[15:12]=0xf, RETAIN_FF_ENABLE bit11=1) as an RMW without touching bit0; `[LGPUCC]` lists `.en_rest_wait_val=2 .en_few_wait_val=2 .clk_dis_wait_val=0xf .flags=CLAMP_IO|POLL_CFG_GDSCR|RETAIN_FF_ENABLE, .clamp_io_ctrl=0x9504`. UNVERIFIED if needed.

### 4.1 The OOB request (host -> GMU doorbell; `[LGMU] a6xx_gmu_set_oob/a6xx_gmu_clear_oob`, `a6xx_gmu_oob_bits[GMU_OOB_GPU_SET]`, `[KGSL] gen7_gmu_oob_set/clear`)
- SET GPU: **write (1<<30) to HOST2GMU_INTR_SET (G(0x1f994) = 0x03d7e650)**; poll **GMU2HOST_INTR_INFO (0x03d7e648) bit 31** (Linux 10 ms, KGSL 100 ms; use 300 ms: first GX power-up includes the RSC/rail sequence); then **write (1<<31) to GMU2HOST_INTR_CLR (0x03d7e644)**.
  (KGSL formula: `set = BIT(30 - req*2)`, `check = BIT(31 - req)` with oob_gpu=0.) Legacy bit numbers (set 16 / ack 24) are for a630 only (`gmu->legacy`).
- CLEAR GPU: **write (1<<31) to HOST2GMU_INTR_SET** (`clear_new = 31` / KGSL `BIT(31 - req*2)`), no ack. Keep OOB_GPU_SET asserted for the whole experiment (it holds GX on); clear it only at the end (section 10). The A740 has no IFPC here, GX would stay on anyway.
- Other OOBs (not needed): PERFCOUNTER_SET set 28/ack 30/clear 29 (Linux), BOOT_SLUMBER/DCVS are legacy-only.

### 4.2 Sequence
1. (done) HFI messages 1-5 of section 3.2 acked (the freq vote #5 must precede the OOB: Linux sets it at the end of `a6xx_gmu_resume`, before `hw_init` issues the OOB).
2. Set `GMU_AO_AHB_FENCE_CTRL` (G(0x23b10) = 0x03d8ec40) to **0** (allow) if reading shows non-zero (Linux writes 0 in `a6xx_gmu_notify_slumber`; `[P1]` S8 reads it). AHB_FENCE_RANGE_0 already set by P2 (0x80c808a0).
3. OOB_GPU_SET as above. On timeout print: GMU2HOST_INTR_INFO, GMU_GENERAL_0/1, FW_BUSY (0x03d7e068), RPMH_POWER_STATE (0x03d7e3b0), RSCC_STATUS0_DRV0 (0x03d50010), RSCC SEQ_BUSY (0x03d50404), RSC TCS0..9 DRV0 status words (gen7_2_x offsets dw 0x34a,0x3f2,0x49a,0x542,0x5ea,0x692,0x73a,0x7e2,0x88a,0x932 -> bit0 = "complete", `[KGSL] gen7_complete_rpmh_votes`), GX GDSCR (0x03d9905c bit31 PWR_ON), the GMU log.
4. **Poll GX power: `GMU_GFX_PWR_CLK_STATUS` (G(0x1f8d0) = 0x03d7e340) must read bits[1:0] == 0** (bit0 GX_HM_GDSC_POWER_OFF, bit1 GX_HM_CLK_OFF; `[LGMU] a7xx_gmu_gx_is_on`, `[KGSL] gen7_gmu_gx_is_on`/`is_on`). Also `GPU_GMU_CX_GMU_RPMH_POWER_STATE` (0x03d7e3b0) was 0xF (slumber) in probe 1; it leaves 0xF once GX is up (value meanings: ACTIVE/ SPTP / IFPC / slumber 0xf, `[LGMU] a6xx_gmu_notify_slumber`). UNVERIFIED active value (expect 0).
5. Only after step 4 passes: first GX read (`RBBM_STATUS` G(0x210)=0x03d00840, `RBBM_INT_0_STATUS` G(0x201)=0x03d00804, `CP_STATUS_1`). Print them. Reading any GX register before step 4 is the reset hazard of `[P1]`.
6. Optionally read GX_GDSCR and the GPU_CC GX CBCR/GFX3D branch to see the real state; the GMU programs PLL0/RCG for the voted level.
Frequency: HFI #5 `GX_BW_PERF_VOTE{ack_type=1, freq=idx, bw=0}`; to change later: send another #5 (blocking ack) with a new index; `bw` index 0 means "no bandwidth vote". Linux defers the OPP/ICC part (`dev_pm_opp_set_opp`), we have none.

### 4.3 What power-collapses CX/GX behind our back (gotchas for this section: 12.3)
- Never write GPU_CC GX GDSC or collapse CX GDSC while the GMU runs; GX collapse is a GMU job (prepare-slumber) `[LGMU] a6xx_gmu_gxpd_get` comments.
- GBIF must be un-halted before GX use: `[LGPU] hw_init` clears CX `GBIF_HALT` (G(0x3c45)=0x03d0f114) and GX `RBBM_GBIF_HALT` (G(0x16)=0x03d00058) = 0 right after the OOB.

---------------------------------------------------------------------------------------------------------------------------------------

## 5. Stage 5: GPU SMMU (SIDs 0/1/2) and the memory the GPU needs

`[DT]`: kgsl-smmu stream ids: `gfx3d_user` SID 0, `gfx3d_lpac` SID 1, `gfx3d_secure` SID 2 (iommus <&kgsl_smmu N 0>); GMU = SID 5. Linux upstream lists only SIDs 0 and 1 for the gpu node (`[LDTSI]`).
- Linux: the GPU (SID 0) always gets **context bank 0** (`[LSMMU] qcom_adreno_smmu_alloc_context_bank`: `start = 0; count = 1`, "so the GPU hardware can switch pagetables"); the CP switches TTBR0 of CB0 itself
  (`CP_SMMU_TABLE_UPDATE`, `[LGPU] a6xx_set_pagetable`, packet words `{TTBR0_LO, TTBR0_HI|ASID<<48, CONTEXTIDR, CONTEXTBANK=0}`), which only works after the SCM call `qcom_scm_set_gpu_smmu_aperture(0)`
  (`[LADRENO] adreno_hw_init`: SVC_MP(0x0c) cmd CP_SMMU_APERTURE_ID(0x1b), arginfo ARGS(4), args = {0xffff0000|cb, 0xffffffff, 0xffffffff, 0xffffffff}, `[LSCM]`; KGSL `kgsl_set_smmu_aperture` likewise).
  Linux sets TTBR1 for the kernel global mappings and uses TTBR0 for per-process tables; KGSL's "default pagetable" is a TTBR0 table too and the CP does not switch for it.
- **Minimum for one global table (our plan):** one context bank (use CB0 if it is free, otherwise any free CB; CB0 is only special for CP table switching which we do not use), AArch64 stage 1, 4 KiB granule, TTBR0 -> our table, EPD1=1, T0SZ=25 (39-bit VA) exactly as the working
  GMU context in `[gpu.c] gpu2_probe` (CBA2R VA64, CBAR type S1/VMID, TCR2 PASize, TCR, MAIR0=0x44 normal non-cacheable, ACTLR 0x32b from DT `qcom,actlr`), **SCTLR = M|TRE|AFE|CFRE (0x27) | HUPCF (bit8)**
  (`[LSMMU] qcom_adreno_smmu_write_sctlr` always ORs HUPCF so a fault does not wedge the GPU; also leave CFIE off, no stall (CFCFG) while polling).
  Route the stream: `SMR[n] = VALID | ID 0x0 | MASK 0` (`0x80000000`), `S2CR[n] = TYPE 0 (translate), CBNDX = cb`; same for SID 1/2 only if you ever use LPAC/secure (do not). Program SMR/S2CR **last** (as P2 does), TLBIALL+TLBSYNC before enabling.
  After programming the GPU CB, `RBBM_STATUS3 bit24 (SMMU_STALLED_ON_FAULT)` stays 0 when no stall; read CB FSR/FAR/FSYNR0 on any hang.
- Page attributes: every page AP[1]=1 ("unprivileged accessible", what `io-pgtable-arm` produces without IOMMU_PRIV), read/write, normal NC (MAIR attr0), so both privileged (APRIV, section 6) and unprivileged CP accesses succeed. Linux marks privileged BOs with MSM_BO_MAP_PRIV (shadow, pwrup reglist, GMU code) = AP[1]=0; we avoid that. UNVERIFIED but only privileged-vs-unprivileged *mismatch* faults, and the superset mapping has none.
- Gunyah/TZ risk (the same unknown as GMU SID 5): who owns SMRs/S2CRs and whether a stage-2 sits behind the CB. Probe 2's result for SID 5 decides this; reuse its helper unchanged for SID 0.
- Suggested layout (GPU VA = identity of nothing; pick VAs, back with carve-out pages; all < 2^39, CP packet addresses are 64-bit lo/hi):
| VA | size | use |
|---|---|---|
| 0x10000000 | 0x8000 | ringbuffer rb[0] (32 KiB = `MSM_GPU_RINGBUFFER_SZ`) |
| 0x10008000 | 0x1000 | memptrs: +0 rptr shadow, +4 fence, +8 bv_rptr, +0xc bv_fence, +0x100.. test words (`struct msm_rbmemptrs`, `[LGPU] shadowptr/rbmemptr`) |
| 0x10009000 | 0x2000 | pwrup reglist (`PWRUP_REGLIST_SIZE = 2*PAGE_SIZE`, a6xx_gpu.h) |
| 0x1000b000 | 0x13000 (19 pages for 76720 B) | SQE microcode = file `a740_sqe.fw` bytes **from offset 4** (`[LADRENO] adreno_fw_create_bo`: `fw->size - 4`, `memcpy(ptr, &fw->data[4], ...)`; `[KGSL] adreno_get_firmware`); file dw0 is 0, dw1 = 0x01740181 version word [FW] |
| 0x10020000 | 0x1000 | scratch / test target (zeroed, filled with 0xdddddddd) |
Zero the ring and memptrs; KGSL fills the ring with 0xaa (`gen7_rb_start`) to detect stale fetches (fine).

---------------------------------------------------------------------------------------------------------------------------------------

## 6. Stage 6: GX register init after GX is up (`a6xx_hw_init`->`hw_init` for a7xx, `[LGPU] hw_init` l.1179-1500, cross-checked with `[KGSL] gen7_start`)

Order below is the Linux order; "R" = required for the first CP test, "S" = recommended/security or perf (skippable for a first memory write), "X" = skip. All writes are plain 32-bit stores after the OOB ack. Byte address = 0x03d00000+4*dw.
| # | reg (dw) byte | value | tag | source |
|---|---|---|---|---|
| 1 | CX GBIF_HALT (0x3c45) 0x03d0f114 | 0 (read back, then RBBM_GBIF_HALT) | R | hw_init `a6xx_has_gbif` block |
| 2 | RBBM_GBIF_HALT (0x16) 0x03d00058 | 0 | R | same |
| 3 | RBBM_SECVID_TSB_CNTL (0xf803) 0x03d3e00c | 0 | R | `gpu_write(RBBM_SECVID_TSB_CNTL, 0)` |
| 4 | RBBM_SECVID_TSB_TRUSTED_BASE lo/hi (0xf800/0xf801) 0x03d3e000/4 | 0 / 0 ; TRUSTED_SIZE (0xf802) 0x03d3e008 = 0 | R | hw_init (KGSL instead programs a secure range: `_set_secvid`, unneeded without secure memory) |
| 5 | (A6xx-only ADDR_MODE_CNTL regs) | not written on a7xx | X | `if (!adreno_is_a7xx())` |
| 6 | HWCG | `a6xx_set_hwcg(gpu,true)` GMU AO CGC regs + A740 hwcg table (`a740_hwcg`, `[LCAT] l.1252`, 66 pairs, RBBM_CLOCK_CNTL=0x8aa8aa82) | X (first test) / S later | clocks stay always-on without it. UNVERIFIED that skipping is harmless |
| 7 | RBBM_GBIF_CLIENT_QOS_CNTL (0x11) 0x03d00044 | 0x02120212 | R | `adreno_is_a7xx` branch; KGSL `0x2120212` |
| 8 | UCHE_GBIF_GX_CONFIG (0xe3a) 0x03d038e8 | 0x010240e0 | R | both |
| 9 | RBBM_PERFCTR_GPU_BUSY_MASKED (0x50b) 0x03d0142c | 0xffffffff | S | both |
| 10 | UCHE_TRAP_BASE lo/hi (0xe09/0xe0a) 0x03d03824/8 | 0xfffff000 / 0x0001ffff (=0x1fffffffff000) | R | `adreno_gpu->uche_trap_base = 0x1fffffffff000ull` (`a6xx_gpu_init`), KGSL `GEN7_UCHE_TRAP_BASE` |
| 11 | UCHE_WRITE_THRU_BASE lo/hi (0xe07/0xe08) 0x03d0381c/20 | same as TRAP_BASE | R | "disable L2 bypass" |
| 12 | UCHE_GMEM_RANGE_MIN lo/hi (0xe0b/0xe0c) 0x03d0382c/30 | 0x01000000 / 0 (SZ_16M for a740 family) | R | `gmem_range_min = SZ_16M`, KGSL `uche_gmem_alignment = SZ_16M` |
| 13 | UCHE_GMEM_RANGE_MAX lo/hi (0xe0d/0xe0e) 0x03d03834/38 | 0x012fffff / 0 (= 0x1000000 + 3 MiB - 1; gmem 3 MiB `[LCAT]`/`[KGSL] gmem_size`) | R | same |
| 14 | UCHE_CACHE_WAYS (0xe17) 0x03d0385c | 0x00800000 (BIT23) | R | Linux a7xx `BIT(23)`, KGSL 0x800000 |
| 15 | CP_ROQ_THRESHOLDS / CP_MEM_POOL_SIZE / UCHE_FILTER_CNTL | not for a7xx | X | `!adreno_is_a7xx` |
| 16 | PC_DBG_ECO_CNTL (prim_fifo_threshold) | none set for a740 in catalog | X | |
| 17 | CP_AHB_CNTL (0x98d) 0x03d02634 | 1 ("AHB default slave response ERROR") | S | Linux hw_init |
| 18 | RBBM_PERFCTR_CNTL (0x500) 0x03d01400 | 1 | S | |
| 19 | GMU_CX_GMU_POWER_COUNTER_SELECT_1 | 0x4 (IFPC counter) | X | |
| 20 | CP_PERFCTR_CP_SEL(0) (0x8d0) 0x03d02340 | PERF_CP_ALWAYS_COUNT | X | |
| 21 | UBWC: RB_NC_MODE_CNTL (0x8e08) 0x03d23820 / TPL1_NC_MODE_CNTL (0xb604) 0x03d2d810 / SP_NC_MODE_CNTL (0xae02) 0x03d2b808 / GRAS_NC_MODE_CNTL (0x8602, per pipe) / UCHE_MODE_CNTL (0xe01) 0x03d03804 | KGSL values for our DT (ubwc-mode 4, min-access-length 32, highest_bank_bit 16 => hbb_lo=3, hbb_hi=0): RB 0x00000816, TPL1 0x00000006, SP 0x00000026, GRAS (BR and BV, via CP_APERTURE_CNTL_HOST=pipe<<12) 0x00000060, UCHE_MODE 0x00600000 | X for the memory-write test (S for any rendering) | `[KGSL] gen7_start`; Linux derives the same from `qcom_ubwc_cfg` (`a6xx_set_ubwc_config`; its extra bits (level2_swizzling, ubwc_mode) UNVERIFIED vs KGSL) |
| 22 | RBBM_INTERFACE_HANG_INT_CNTL (0x1f) 0x03d0007c | (1<<30) \| 0xcfffff = 0x40cfffff | S | A740 in the `adreno_is_a740_family` list; KGSL `hang_detect_cycles = 0xcfffff` |
| 23 | UCHE_CLIENT_PF (0xe19) 0x03d03864 | 0x00000081 (BIT7 \| 1) | S | |
| 24 | GMU AO busy counter: GPU_GMU_AO_GPU_CX_BUSY_MASK = 0xff000000; POWER_COUNTER_SELECT_0 rmw; ENABLE=1 | | X | |
| 25 | CP_PROTECT_CNTL (0x84f) 0x03d0213c = 0x0000000b (ACCESS_PROT_EN \| ACCESS_FAULT_ON_VIOL_EN \| LAST_SPAN_INF_RANGE); CP_PROTECT(i) = 0x850+i table in Appendix B; last entry index 47 = 0x8001f8c0 | | S (R for security parity; first test may skip. UNVERIFIED that CP runs unprotected) | `a6xx_set_cp_protect`, `a730_protect_regs` |
| 26 | UCHE_CMDQ_CONFIG (0xe3c) 0x03d038f0 | `6<<16 \| 6<<12 \| 9<<8 \| BIT3 \| BIT2 \| 2` = **0x0006690e** | R | Linux a7xx branch, KGSL identical |
| 27 | APRIV: CP_APRIV_CNTL (0x844) 0x03d02110 = 0x6f (BR: ICACHE\|RBFETCH\|RBPRIVLEVEL\|RBRPWB\|CDREAD\|CDWRITE = bits 0,1,2,3,5,6); CP_BV_APRIV_CNTL (0xad0) 0x03d02b40 = 0x0f; CP_LPAC_APRIV_CNTL (0xb31) not used | | R if hw_apriv (A740 has `ADRENO_QUIRK_HAS_HW_APRIV`) | Linux `A7XX_BR_APRIVMASK` / `A7XX_APRIV_MASK`; KGSL `GEN7_BR_APRIV_DEFAULT = BIT3\|BIT2\|BIT1\|BIT0\|BIT6\|BIT5` |
| 28 | CP_CHICKEN_DBG (0x841) 0x03d02104 = 1; CP_BV_CHICKEN_DBG (0xada) 0x03d02b68 = 1 | | S (icache prefetch off, KGSL for gen7_2_1) | `[KGSL] gen7_start` |
| 29 | RB_CMP_DBG_ECO_CNTL (0x8e28) 0x03d238a0 \|= BIT(11) | | S | both |
| 30 | RBBM_INT_0_MASK (0x38) 0x03d000e0 | 0 (keep masked, we poll RBBM_INT_0_STATUS 0x03d00804). Linux A7XX_INT_MASK if you ever want the raw bits identical: irrelevant for status polling | R | |
After row 30: `ret = adreno_hw_init()` = `qcom_scm_set_gpu_smmu_aperture(0)` (skip, we do not use CP table switching) + ring bookkeeping, then section 7.

Also for the pwrup-reglist (section 7.4) the registers in `a7xx_pwrup_reglist` that Linux reads back are: UCHE_TRAP_BASE(2), UCHE_WRITE_THRU_BASE(2), UCHE_GMEM_RANGE_MIN(2), MAX(2), UCHE_CACHE_WAYS, UCHE_MODE_CNTL, RB_NC_MODE_CNTL,
RB_CMP_DBG_ECO_CNTL, RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE (0x8e50, `[KGSL]` writes 1 only with preemption), UCHE_GBIF_GX_CONFIG, UCHE_CLIENT_PF, TPL1_DBG_ECO_CNTL1 (0xb602) = 16 regs.

---------------------------------------------------------------------------------------------------------------------------------------

## 7. Stage 7: SQE microcode, ringbuffer, CP_ME_INIT

### 7.1 Order (`[LGPU] hw_init` l.1425-1480; `[KGSL] gen7_rb_start`)
1. Copy `a740_sqe.fw` bytes [4:] to the SQE buffer (section 5).
2. `CP_SQE_INSTR_BASE_LO/HI` (0x830/0x831 -> 0x03d020c0/0x03d020c4) <- GPU VA of the SQE buffer (0x1000b000, 0). Note: it is the VA of the byte AFTER the 4-byte header.
3. `CP_RB_BASE_LO/HI` (0x800/0x801 -> 0x03d02000/4) <- ring VA (0x10000000, 0).
4. `CP_RB_CNTL` (0x802 -> 0x03d02008) <- **0x0000020c** (`MSM_GPU_RB_CNTL_DEFAULT`: BUFSZ = ilog2(32768/8)=12 in bits[5:0], BLKSZ = ilog2(32/8)=2 in bits[13:8]; KGSL `GEN7_CP_RB_CNTL_DEFAULT` = ilog2(KGSL_RB_DWORDS>>1) \| ilog2(4)<<8 gives the same). Do NOT set NO_UPDATE (bit27) on a7xx: hw_apriv targets use the RPTR shadow.
5. `CP_RB_RPTR_ADDR_LO/HI` (0x804/0x805 -> 0x03d02010/14) <- VA of memptrs.rptr (0x10008000, 0) (Linux `shadowptr`/KGSL `SCRATCH_RB_GPU_ADDR rptr`); `CP_BV_RB_RPTR_ADDR_LO/HI` (0xa98/0xa99 -> 0x03d02a60/64) <- VA of memptrs.bv_rptr (0x10008008, 0) ("always on A7xx", Linux l.1458).
6. Preemption init: **skip** (single ring => `a6xx_preempt_hw_init` returns at `nr_rings == 1`, `[LPRE]` l.136; do not write CP_CONTEXT_SWITCH_*).
7. `CP_SQE_CNTL` (0x808 -> 0x03d02020) <- 1 (starts the SQE; KGSL "Clear the SQE_HALT"). Read back `CP_STATUS_1` (0x03d02094) and `CP_SQE_STAT_ADDR/DATA` (0x908/0x909) for a pulse check; CP_ALWAYS_ON_COUNTER (0x980 -> 0x03d02600) must advance.
8. Build the pwrup reglist buffer (7.4), then send ME_INIT (7.2). Then idle-wait (7.3).

### 7.2 CP_ME_INIT stream for gen7 (exactly 10 dwords; `[LGPU] a7xx_cp_init`, `[KGSL] gen7_cp_init_cmds` GEN7_CP_INIT_DWORDS=10)
Packet header (PKT7) = `0x70000000 | cnt | parity(cnt)<<15 | opcode<<16 | parity(opcode)<<23` with odd-parity bit (`pm4_calc_odd_parity_bit` = `(0x9669 >> (0xf & (val ^ val>>4 ^ ... ^ val>>28))) & 1`, `[KGSL] adreno_pm4types.h`); computed headers in Appendix C.
| dw | value |
|---|---|
| 0 | 0x70970001 (CP_THREAD_CONTROL, 1) |
| 1 | 0x08000000 (BIT27: disable concurrent binning) |
| 2 | 0x70c80007 (CP_ME_INIT, 7) |
| 3 | 0x0000014b (mask = BIT0 multiple HW contexts \| BIT1 error detection \| BIT3 default reset state \| BIT6 no perfcounter save/restore \| BIT8 register init list with spinlock; KGSL additionally ORs BIT11 when DMS is off) |
| 4 | 0x00000003 (number of HW contexts) |
| 5 | 0x20000000 (error detection) |
| 6 | 0x00000002 (operation mode mask) |
| 7 | lower_32(pwrup_reglist VA) = 0x10009000 |
| 8 | upper_32 = 0 |
| 9 | 0x80000000 (BIT31: list contains dynamic triplets) |
### 7.3 Submit and wait (`[LGPU] a6xx_flush`, `a6xx_idle`, `[KGSL] gen7_ringbuffer_submit`, `gen7_send_cp_init`)
- Write the dwords into the ring at wptr (start 0), DSB; **write CP_RB_WPTR (0x807 -> 0x03d0201c) = new wptr in dwords** (wrap at 8192 dwords). Linux uses a "fenced write" (write, mb, poll `GMU_AHB_FENCE_STATUS` G(0x23b13)=0x03d8ec4c until `!(status & BIT0)` (WRITEDROPPED0), retry the write up to 2 ms) because the GMU may be collapsing GX; with OOB_GPU_SET held and fence ctrl 0 it should pass first time; still do the status check.
- Idle/complete: `rptr == wptr` where rptr = `memptrs.rptr` (shadow, CP writes it via RPTR_ADDR) or the register CP_RB_RPTR (0x806 -> 0x03d02018) (reliable here, no IFPC), AND `RBBM_STATUS & ~BIT0 == 0` (0x03d00840; CP_AHB_BUSY_CX_MASTER bit0 is ignored), AND `RBBM_INT_0_STATUS bit23 (RBBM_HANG_DETECT)` clear (`_a6xx_check_idle`), and `GMU_AO_GPU_CX_BUSY_STATUS` (0x03d8ec30) bit23 (GPUBUSYIGNAHB) clear. KGSL waits up to 2000 ms (`adreno_spin_idle(2000)`).
- If rptr does not move: read `CP_HW_FAULT` (0x821 -> 0x03d02084), `CP_INTERRUPT_STATUS` (0x823 -> 0x03d0208c), `CP_PROTECT_STATUS` (0x824 -> 0x03d02090), `RBBM_INT_0_STATUS`, the GPU SMMU CB FSR/FAR/FSYNR0 (translation fault on ring or SQE fetch), `RBBM_STATUS3 bit24`. Typical causes: SQE VA wrong (+4 offset), SMMU not routing SID 0, APRIV/AP mismatch, SQE not started, GX clocks (no freq vote).
### 7.4 pwrup reglist buffer (needed because mask BIT8/BIT31 tell the CP to read it)
Layout `struct cpu_gpu_lock` (`[LGPU]/a6xx_gpu.h l.19-37`): dw0 gpu_req=0, dw1 cpu_req=0, dw2 turn=0, dw3 = `ifpc_list_len(u8)=0 | preemption_list_len(u8)=16<<8 | dynamic_list_len(u16)=2<<16`, then from byte 16 the list as u32 pairs
`{reg_dw_offset, value}` for the 16 registers of 6, in `a7xx_pwrup_reglist_regs` order, values READ BACK from the GPU after section 6 (`a7xx_patch_pwrup_reglist`), then the dynamic triplets `{aperture=PIPE(id)<<12, reg 0x8602 (GRAS_NC_MODE_CNTL), value}` for pipe BR (aperture 0x1000) and BV (0x2000) (write CP_APERTURE_CNTL_HOST (0x03d02800) = pipe<<12 before reading, restore 0).
Fallback if the CP rejects it (CP_HW_FAULT / hang on ME_INIT): (A) zero the whole page (lists 0), keep the packet; (B) send mask `0x4b` (no BIT8) and dw9 = 0, dw7/8 = 0. UNVERIFIED which the SQE tolerates; (A) and (B) only drop power-up register restore (preemption/IFPC), irrelevant for us.

---------------------------------------------------------------------------------------------------------------------------------------

## 8. Stage 8: zap shader, CP_SET_SECURE_MODE, and what if zap cannot be loaded

### 8.1 What the sources say
- Linux (`[LGPU] hw_init` l.1494-1530): after `a7xx_cp_init` it calls `a6xx_zap_shader_init -> adreno_zap_shader_load(gpu, GPU_PAS_ID=13)`. On success it emits `CP_SET_SECURE_MODE(1 dword = 0)` and idle-waits.
  If the DT has **no `zap-shader` node** (`-ENODEV`): "Zap shader not enabled - using SECVID_TRUST_CNTL instead" and `gpu_write(RBBM_SECVID_TRUST_CNTL, 0x0)`; comment: "If we guessed wrong then access to the RBBM_SECVID_TRUST_CNTL register will be blocked and a permissions violation will soon follow." Any other zap error aborts hw_init (`return ret`).
- KGSL (`gen7_rb_start`): `if (!adreno_dev->zap_loaded) kgsl_regwrite(RBBM_SECVID_TRUST_CNTL, 0); else CP_SET_SECURE_MODE`; but `adreno_zap_shader_load` failing returns the error and boot fails (`kgsl_zap_shader_load` -> `qcom_mdt_load` -> `qcom_scm_pas_auth_and_reset(13)`), `zap_name = "a740_zap.mbn"` for gen7_2_1.
- Meaning: the GPU comes out of reset in **secure mode** (TZ-owned) on fused/locked devices; the zap shader is a TZ-authenticated blob that, executed by the CP on `CP_SET_SECURE_MODE 0`, flips the GPU to non-secure mode (it writes RBBM_SECVID_TRUST_CNTL from the secure side).
  The register write fallback only works where TZ leaves that register writable by the non-secure side (coreboot/Chromebook sc7180 style).
  What the GPU can do while still secure is **not documented in any source I could fetch**: Linux/KGSL never run user work in that state. UNVERIFIED what a CP_MEM_WRITE to non-secure memory does in secure mode (candidates: works, is dropped, or faults at the SMMU/NS check).
- Phone data: `[DT]` has `zap-shader { memory-region = <&gpu_micro_code_mem> }` = 0x9b09a000..0x9b09c000 (8 KiB, no-map). Firmware name is OEM-specific; our `a740_zap.mbn` is the single-file ELF form: ELF header+3 phdrs 0x94 B, **segment 1 (PT_LOAD, vaddr=paddr=0x1000, filesz=memsz=0x430, flags 0x8000007) at file offset 0x1000**, hash segment (0xf38) at file offset 0x2000 (phdr2). [FW] verified: `.mdt == mbn[0:0x94] + mbn[0x2000:0x2f38]`, `.b01 == mbn[0x1000:0x1430]`, `.b00 == mbn[0:0x94]`, `.b02 == mbn[0x2000:0x2f38]`.

### 8.2 SCM PAS sequence (`[LADRENO] zap_shader_load_mdt` + `qcom_mdt_load`, `[LSCM]` qcom_scm.c/h, `[KGSL] kgsl_zap_shader_load`; P1 S10 convention: SMC64 standard call, fn = (1<<30)|(2<<24)|(svc<<8)|cmd, x1 = arginfo)
Do this ONLY with GX up, SQE running and ME_INIT idle (Linux order); whether TZ needs GX on for auth_and_reset is UNVERIFIED (Linux order says yes: the CP is already running).
1. Metadata buffer (4044 B = `.mdt`, 4 KiB-aligned, physically contiguous, non-cacheable (MMU-off shim ok), not reused afterwards: TZ secures it).
   `PAS_INIT_IMAGE`: svc 0x02 cmd 0x01 -> fn **0x42000201**, x1 = arginfo `ARGS(2, VAL, RW)` = 2 | (0<<4) | (2<<6) = **0x82**, x2 = **13**, x3 = metadata PA. Result x1.. = status; 0 = ok.
2. `PAS_MEM_SETUP`: svc 0x02 cmd 0x02 -> fn **0x42000202**, x1 = ARGS(3) = **3**, x2 = 13, x3 = **0x9b09a000**, x4 = **0x430** (max_addr - min_addr of the PT_LOAD segments, `qcom_mdt_get_size`).
3. Copy the 0x430 segment bytes (`mbn[0x1000:0x1430]`) to PA 0x9b09a000 (offset = p_paddr - min_paddr = 0). Whether Gunyah maps this no-map region writable for our EL1: probe write+readback first (UNVERIFIED).
4. `PAS_AUTH_AND_RESET`: svc 0x02 cmd 0x05 -> fn **0x42000205**, x1 = ARGS(1) = **1**, x2 = 13. Result 0 = ok; `-EOPNOTSUPP` (Linux `zap_available=false`) means "this target needs no zap".
5. Emit `0x70e60001, 0x00000000` (CP_SET_SECURE_MODE 0) into the ring, wptr, idle-wait (rptr==wptr, RBBM_STATUS idle). KGSL `gen7_rb_start`.
Risk notes: (i) newer TZ builds expect the PAS_INIT_IMAGE metadata inside an SHM-bridge ("qtee shmbridge") region: SVC_MP(0x0c) cmds ENABLE 0x1c / DELETE 0x1d / CREATE 0x1e (`[LSCM] qcom_scm.h l.125-127`), used by Linux `qcom_tzmem` and by Samsung's Android kernel; if INIT_IMAGE returns an error (not 0), look for that. UNVERIFIED on this TZ, the failure is a clean SCM error.
(ii) `PAS_IS_SUPPORTED` answers 0 for every id under Gunyah (probe 1) and is not part of the Linux/KGSL zap path: ignore it. (iii) `QCOM_SCM_GPU_INIT_REGS` (svc 0x28 cmd 1) is "not available" on this TZ (probe 1) and Linux only needs it for A750+ (`[LGMU] a6xx_gmu_secure_init`); A740: "Raytracing is always enabled", nothing to do. KGSL calls it for gen7_2_1 and ignores `-EOPNOTSUPP` (`gen7_scm_gpu_init_cx_regs`).
(iv) anti-rollback/OEM key: the image is the phone's own Samsung-signed zap, expected to authenticate. UNVERIFIED.

### 8.3 Decision tree for the probes
- P-A (no secure-mode exit): run stage 7 and the first test (section 9) as is. If the CP executes and the memory write lands, the GPU is usable without zap for this purpose.
- P-B: if the CP runs but the write/event does not land or faults, read `RBBM_SECVID_TRUST_CNTL` (G(0xf400) = 0x03d3d000) first (print), then write 0 (Linux fallback) and re-test; if the write is blocked you get an AHB error/CP protect fault: expect `RBBM_INT_0_STATUS` AHB error (bit1) and `CP_PROTECT_STATUS`.
- P-C: SCM PAS flow of 8.2.
- If none work, the GPU cannot leave secure mode on this retail device from this VM: there is no source-documented non-zap path.

---------------------------------------------------------------------------------------------------------------------------------------

## 9. Stage 9: the first test command buffer (a value written to memory)

All packets are PKT7 (type 7). Opcodes from `[KGSL] adreno_pm4types.h` / `[LXML] adreno_pm4.xml`: CP_NOP 0x10, CP_WAIT_MEM_WRITES 0x12, CP_WAIT_FOR_ME 0x13, CP_THREAD_CONTROL 0x17, CP_WAIT_FOR_IDLE 0x26, CP_MEM_WRITE 0x3d, CP_EVENT_WRITE 0x46, CP_ME_INIT 0x48, CP_SET_PROTECTED_MODE 0x5f, CP_SET_SECURE_MODE 0x66.
Suppose test VA T = 0x10020000 (lo 0x10020000, hi 0), fence VA F = 0x10008004 (memptrs.fence). Pre-fill T with 0xdddddddd, F with 0.
Stream A (register-only; independent of SMMU/data path, do it first):
| dw | value | meaning |
|---|---|---|
| 0 | 0x48088501 | PKT4: write 1 dword to CP_SCRATCH(2) (dw reg 0x885) (`[LGPU] a7xx_submit` writes the fence this way) |
| 1 | 0xc0de0001 | value; host reads `G(0x885)` = 0x03d02214 afterwards |
Stream B (memory write, `CP_MEM_WRITE` = 2 address dwords + N data dwords; `[LGPU] a6xx_set_pagetable`, `[KGSL] gen7_rb_context_switch`):
| dw | value | meaning |
|---|---|---|
| 2 | 0x703d8003 | CP_MEM_WRITE, cnt 3 |
| 3 | 0x10020000 | ADDR_LO |
| 4 | 0x00000000 | ADDR_HI |
| 5 | 0xcafef00d | data |
| 6 | 0x70928000 | CP_WAIT_MEM_WRITES |
| 7 | 0x70460004 | CP_EVENT_WRITE, 4 dwords (a7xx timestamp form, `[LGPU] a7xx_submit`) |
| 8 | 0x08000031 | CACHE_CLEAN (0x31) \| BIT27 (write timestamp); add BIT31 (`CP_EVENT_WRITE_0_IRQ`) to also raise RBBM_INT_0 CP_CACHE_FLUSH_TS (bit20) |
| 9 | 0x10008004 | fence ADDR_LO |
| 10 | 0x00000000 | fence ADDR_HI |
| 11 | 0x00000001 | fence value |
| 12 | 0x70268000 | CP_WAIT_FOR_IDLE |
13 dwords total; wptr = 13 (continue from wptr after the ME_INIT stream: ring offset = 10 dwords there, so these dwords land at ring dwords 10..22 and the new wptr is 23). Header values per Appendix C.
Submit: write dwords, DSB, `CP_RB_WPTR` <- wptr (fenced-write check). 
Completion, from cheapest to richest: (1) `CP_RB_RPTR` reg == wptr, (2) host reads CP_SCRATCH(2) == 0xc0de0001 (proves the CP executed PM4 + registers), (3) T == 0xcafef00d in DDR (CPU reads with MMU off; if the GPU's UCHE holds the write, the CACHE_CLEAN event + fence at F forces it out: F == 1 means the clean completed; if T is still 0xdddddddd while F == 1, suspect a write-back in UCHE/ non-coherence (re-run with the event before the read) or a secure-mode drop (section 8)), (4) `RBBM_INT_0_STATUS` (0x03d00804) bit20/bit17 if the IRQ flag was set (clear via `RBBM_INT_CLEAR_CMD` (0x37 -> 0x03d000dc) <- the same bits).
Timeout 2 s. On hang: dump CP_HW_FAULT, CP_INTERRUPT_STATUS, CP_PROTECT_STATUS, RBBM_STATUS, RBBM_INT_0_STATUS, CP_STATUS_1, SMMU CB FSR/FAR; then OOB clear + slumber (section 10).
Variations if A works but B does not: use `CP_REG_TO_MEM` (0x3e, 3 dwords `{reg|CNT<<?, lo, hi}` as `get_stats_counter` in a6xx_gpu.c l.222: `CP_REG_TO_MEM_0_REG(CP_ALWAYS_ON_COUNTER) | CNT(2) | 64B`) to store a counter to T, which shows the write path works even if CP_MEM_WRITE data gets stuck.

---------------------------------------------------------------------------------------------------------------------------------------

## 10. Stage 10: teardown (leave the device clean; also how to power GX off)

`[LGMU] a6xx_gmu_shutdown/a6xx_gmu_notify_slumber/a6xx_rpmh_stop`, `[KGSL] gen7_gmu_notify_slumber/gen7_rscc_sleep_sequence`:
1. If you never cleared it: OOB clear = write (1<<31) to HOST2GMU_INTR_SET. (Linux does a dummy set/clear OOB first when needs_hw_init; irrelevant.)
2. Wait GMU idle: `GMU_AO_GPU_CX_BUSY_STATUS` (0x03d8ec30) bit30 `CXGXCPUBUSYIGNAHB` clear (KGSL) / RPMH_POWER_STATE == idle level.
3. GBIF halt (`bus_halt`): optional for a clean power-down; skip for short probes.
4. HFI `PREPARE_SLUMBER` (id 33, 3 dwords `{hdr, bw=0, freq=0}`), wait ack, then poll `GPU_GMU_AO_GPU_CX_BUSY_STATUS` bit23 clear (10 ms).
5. `GMU_AO_AHB_FENCE_CTRL` <- 0; `FALNEXT_INTF` (0x03d7e3c4) <- 1 (gemnoc workaround, `a6xx_gemnoc_workaround`).
6. `GMU_CM3_SYSRESET` <- 1; `GMU_RSCC_CONTROL_REQ` (0x03d8ec1c) <- 1; poll `RSCC_RSC_STATUS0_DRV0` (0x03d50010) bit16; REQ <- 0 (sleep sequence, needs the RSC ucode of section 2).
7. Leave CX GDSC/clocks on (as probes 1-2) or collapse CX last; **never collapse CX with GX on** (`a6xx_gmu_gxpd_put` comment, `cxpd_notifier_cb`: GX must be off first).

---------------------------------------------------------------------------------------------------------------------------------------

## 11. Proposed probes 3..N (each ends with a printed one-line verdict; "NEXT:" line before every RISK step)

- **Probe 3 (rsc + hfi)**: 1-9 of probe 2, then section 2 (RSC+PDC), HFI_CTRL_INIT, section 3 messages #1..#5 (4-level perf table, single-entry bw table). Success = 5 clean acks and `GMU2HOST_INTR_INFO` quiet. No OOB, no GX.
- **Probe 4 (gx on)**: probe 3 + GPU_CC PLL0/1 config (4.0) + OOB_GPU_SET + GX status poll + first GX reads + GBIF unhalt + a handful of safe GX register writes from section 6 (rows 1-14,26) and read-back. Success = `GMU_GFX_PWR_CLK_STATUS[1:0]==0` and `RBBM_STATUS` readable. End with OOB clear + slumber (or hold).
- **Probe 5 (gpu smmu + ring + SQE)**: probe 4 + GPU CB/SMR for SID 0, carve-out, SQE load, rows 7-30, ring regs, `CP_SQE_CNTL=1`, ME_INIT, idle poll. Success = rptr==wptr after ME_INIT.
- **Probe 6 (test write)**: probe 5 + section 9 streams A then B, in P-A/P-B/P-C order of 8.3 as separate runs.
Keep the run time < 5 s each; first GX power-up at the 220 MHz entry (perf index 1).

---------------------------------------------------------------------------------------------------------------------------------------

## 12. Known gotchas (Linux lists, commits, code comments)

1. **DEMET clock is mandatory** for the A7xx GMU: "now-required ... strictly required for things like asserting reset lines"; without it OOB requests fail and HFI hangs after the first OOB (lkml 2309.3/01420, https://lkml.iu.edu/hypermail/linux/kernel/2309.3/01420.html; `[LGPUCC]` `qcom_branch_set_clk_en(0x900c /* GPU_CC_DEMET_CLK */)`, `[P1]` S6 enabled it). If probe 4 OOB times out with all else fine, re-check GPU_CC 0x03d9900c.
2. Cover letter of the a7xx series (https://lkml.iu.edu/hypermail/linux/kernel/2309.3/01419.html): A7xx = A6xx GMU + new flows; patch "send ACD state to QMP at GMU resume" (not required to run); "poll for GBIF unhalt status in hw_init" (Linux today just clears `GBIF_HALT`/`RBBM_GBIF_HALT`); a "hack" patch whose necessity depends on the board; firmware is not redistributable (we have the phone's).
3. CX/GX collapse: GEMNoC can power collapse during GPU power-down so CX_GDSC will not collapse -> assert Qactive via `GPU_GMU_CX_GMU_CX_FALNEXT_INTF = BIT(0)` (`a6xx_gemnoc_workaround`, a7xx); GX must be refcounted ON before the GMU initialises or "GMU OOB errors during resume" (Akhil P Oommen 2020, https://lkml.iu.edu/hypermail/linux/kernel/2007.2/02186.html); GX power domain is "do nothing" on the host; KMD must do a "handshake (fenced_write, OOB signalling)" before touching GX-domain registers (IFPC series, https://www.mail-archive.com/freedreno@lists.freedesktop.org/msg38972.html).
4. "GMU firmware's internal power state gets messed up if we send prepare_slumber HFI when oob_gpu handshake wasn't done after the last wake up" (`a6xx_gmu_shutdown` comment): always do OOB set/clear before PREPARE_SLUMBER.
5. TCM retention and warm/cold boot: `CX_MISC_TCM_RET_CNTL` (0x03d9e0e4) = 1 each boot; warm boot (==1 at entry) skips the firmware reload (`a6xx_gmu_resume`); "warm boot path does not work on newer A6xx" for non-a7xx, so we always cold-load.
6. The GMU fw "Vote veto for FAL10": `CX_FALNEXT_INTF = 1`, `CX_FAL_INTF = 1` before release (`a6xx_gmu_fw_start`, probe 2 does this).
7. `UCHE_GMEM_RANGE_MIN` is 16 MiB on A740/A722 but 1 MiB on A6xx/A730; **A730 and A650 skip the register** (`hw_init`: `!(a650 || a702 || a730)`); do not copy a730 behaviour.
8. A740 differs from A730 in: RSC seq mem base (dw 0x154), HIDDEN_TCS_CMD0_DATA+4 = 0x80000021 (A740/A722 only), GMU hub clock 200 MHz (`a6xx_gmu_resume`: `adreno_is_a740_family ? 200000000 : 150000000`), `gmu_chipid` 0x7020100, `gmu_cgc_mode 0x00020202`, bw table single entry, A740 needs no GPU_INIT_REGS SCM, no IFPC quirk.
9. a7xx HW_APRIV: RPTR shadow and RB fetch are privileged accesses; `CP_RB_CNTL` must not have NO_UPDATE; `a6xx_get_rptr` reads the shadow. With no shadow (`CP_RB_RPTR_ADDR` unset) the CP writes RPTR to address 0 -> SMMU fault.
10. The GMU must be released with `CM3_CFG = 0x4052` (snapshot NMI bit) and HFI table iova in HFI_QTBL_ADDR; `FW_INIT_RESULT & 0xE00` non-zero = NMI/assert (KGSL `poll_gmu_reg`).
11. GMU and GPU SMMU: set HUPCF so that a faulting transaction does not stall the GMU/GPU forever (`qcom_adreno_smmu_write_sctlr`); a pagefault on the GMU log/dbg buffer will look like an HFI hang.
12. No thermal management on our side: single-digit seconds, <= 348 MHz, no sustained CP loops. The phone already got hot from lavapipe (NOTES 2026-10-21).
13. KGSL hwsched vs Linux: do NOT send HFI feature HWSCHED (id 11/feature 1) or context-queue messages; in hwsched mode the GMU may send `F2H_MSG_MEM_ALLOC (20)` requests (KGSL adreno_hfi.h); if one shows up in queue 1 unexpectedly, the GMU thinks it is in hwsched mode: print and stop.

---------------------------------------------------------------------------------------------------------------------------------------

## 13. UNVERIFIED / open (collected)

- Whether the Samsung gmu_gen70200.bin (v4.1.9) behaves exactly like the linux-firmware one in ring mode (same file name and version words as Linux's catalog entry; high confidence).
- RSC/PDC load order (before vs after SYSRESET release); whether RSCC/PDC are writable from the primary VM (read OK in probe 1).
- GPU_CC PLL0/PLL1 pre-configuration state and whether the GMU needs the host to configure them; GX GDSC config fields.
- `gmxc.lvl` presence, exact `gfx.lvl` aux table (the DT vlvl values 52..224 must exist in it).
- Whether HFI table `size` dwords vs bytes matters; whether GX comes up with a 4-level table; whether the GMU wants ACD/QMP messages or DDR BCM votes for the GX power-up.
- GPU SMMU ownership/stage-2 for SID 0 (follows probe 2 SID 5 result); AP[1]=1 everywhere being accepted by APRIV transactions; UCHE write-back vs non-cacheable memory visibility to the CPU.
- Whether the CP runs and writes memory while the GPU is still in secure mode; whether `RBBM_SECVID_TRUST_CNTL` is writable; whether PAS auth needs GX up and metadata in an SHM bridge.
- Skipping HWCG, CP_PROTECT, UBWC, pwrup-reglist contents (listed as S/X above).
- A740-specific differences between this SM8750-vintage KGSL and the Kalama-tagged one.

---------------------------------------------------------------------------------------------------------------------------------------

## Appendix A: register byte addresses used in stages 3-9 (0x03d00000 + 4*dw; sources gen7_reg.h / a6xx.xml / a6xx_gmu.xml)

GMU/CX: HOST2GMU_INTR_SET 0x03d7e650 (dw 0x1f994) | GMU2HOST_INTR_INFO 0x03d7e648 | GMU2HOST_INTR_CLR 0x03d7e644 | GMU2HOST_INTR_MASK 0x03d7e64c | GMU_GFX_PWR_CLK_STATUS 0x03d7e340 | RPMH_POWER_STATE 0x03d7e3b0 |
GMU_GMU_PWR_COL_KEEPALIVE 0x03d7e30c | AHB_FENCE_CTRL 0x03d8ec40 | AHB_FENCE_STATUS 0x03d8ec4c (WRITEDROPPED0 = BIT0) | GPU_CX_BUSY_STATUS 0x03d8ec30 (bit23 GPUBUSYIGNAHB, bit30 CXGXCPUBUSYIGNAHB) |
RSCC_CONTROL_REQ 0x03d8ec1c / ACK 0x03d8ec20 | GENERAL_8/9/10 0x03d7e734/738/73c | GBIF_CX_CONFIG 0x03d0f000, GBIF_QSB_SIDE0..3 0x03d0f00c.. | GBIF_HALT 0x03d0f114, HALT_ACK 0x03d0f118.
GX: RBBM_GBIF_HALT 0x03d00058 | RBBM_GBIF_CLIENT_QOS_CNTL 0x03d00044 | RBBM_INTERFACE_HANG_INT_CNTL 0x03d0007c | RBBM_INT_0_MASK 0x03d000e0 | INT_CLEAR_CMD 0x03d000dc | INT_0_STATUS 0x03d00804 | RBBM_STATUS 0x03d00840 | RBBM_STATUS3 0x03d0084c |
RBBM_PERFCTR_CNTL 0x03d01400 | RBBM_PERFCTR_GPU_BUSY_MASKED 0x03d0142c | CP_RB_BASE 0x03d02000/4 | CP_RB_CNTL 0x03d02008 | CP_RB_RPTR_ADDR 0x03d02010/14 | CP_RB_RPTR 0x03d02018 | CP_RB_WPTR 0x03d0201c | CP_SQE_CNTL 0x03d02020 |
CP_SQE_INSTR_BASE 0x03d020c0/c4 | CP_HW_FAULT 0x03d02084 | CP_INTERRUPT_STATUS 0x03d0208c | CP_PROTECT_STATUS 0x03d02090 | CP_STATUS_1 0x03d02094 | CP_CHICKEN_DBG 0x03d02104 | CP_APRIV_CNTL 0x03d02110 | CP_PROTECT_CNTL 0x03d0213c |
CP_PROTECT(0) 0x03d02140 ... (47) 0x03d021fc | CP_SCRATCH(2) 0x03d02214 | CP_SQE_STAT_ADDR/DATA 0x03d02420/24 | CP_ALWAYS_ON_COUNTER 0x03d02600 | CP_AHB_CNTL 0x03d02634 | CP_APERTURE_CNTL_HOST 0x03d02800 | CP_BV_RB_RPTR_ADDR 0x03d02a60/64 |
CP_BV_APRIV_CNTL 0x03d02b40 | CP_BV_CHICKEN_DBG 0x03d02b68 | UCHE_MODE_CNTL 0x03d03804 | UCHE_WRITE_THRU_BASE 0x03d0381c/20 | UCHE_TRAP_BASE 0x03d03824/28 | UCHE_GMEM_RANGE_MIN 0x03d0382c/30 | MAX 0x03d03834/38 |
UCHE_CACHE_WAYS 0x03d0385c | UCHE_CLIENT_PF 0x03d03864 | UCHE_GBIF_GX_CONFIG 0x03d038e8 | UCHE_CMDQ_CONFIG 0x03d038f0 | RB_NC_MODE_CNTL 0x03d23820 | RB_CMP_DBG_ECO_CNTL 0x03d238a0 | GRAS_NC_MODE_CNTL 0x03d21808 |
SP_NC_MODE_CNTL 0x03d2b808 | TPL1_NC_MODE_CNTL 0x03d2d810 | RBBM_SECVID_TRUST_CNTL 0x03d3d000 | RBBM_SECVID_TSB_TRUSTED_BASE 0x03d3e000/04, SIZE 0x03d3e008, TSB_CNTL 0x03d3e00c.
RSCC: base 0x03d50000 (dw table in section 2); PDC: ENABLE_PDC 0x0b284500, SEQ_START_ADDR 0x0b284520.
GPU_CC: PLL0 0x03d90000, PLL1 0x03d91000, GX GDSCR 0x03d9905c, GX clamp_io_ctrl 0x03d99504, DEMET 0x03d9900c.

## Appendix B: CP_PROTECT values for a740 (`a730_protect_regs`, `A6XX_PROTECT_NORDWR(r,l) = BIT31|l<<18|r`, `A6XX_PROTECT_RDONLY = l<<18|r`; write CP_PROTECT(i) at dw 0x850+i; entries 45 and 46 are skipped; index 47 last)
0:0x13fc0000 1:0x0160050b 2:0x8000050e 3:0x80000510 4:0x80000534 5:0x027405fb 6:0x87a40699 7:0x802008a0 8:0x809008ab 9:0x800408de 10:0x052c08e7 11:0x81340900 12:0x82c8098d 13:0x86f80a41 14:0x80040df0 15:0x80000e01
16:0x80200e07 17:0x830c3c00 18:0x7ffc3cc4 19:0x873c8630 20:0x80008e00 21:0x80008e08 22:0x807c8e50 23:0x8a008e80 24:0x876c9624 25:0x80009e40 26:0x80349e64 27:0x861c9e78 28:0x873ca630 29:0x8000ae02 30:0x803cae50 31:0x800cae66
32:0x800cae6f 33:0x800cb604 34:0xbffcec00 35:0x7ffcfc00 36:0x814d8400 37:0x00118454 38:0xfffd8459 39:0xfffda459 40:0xfffdc459 41:0x910df400 42:0x01edf844 43:0x8001f860 44:0x80a9f878 47:0x8001f8c0. Then CP_PROTECT_CNTL = 0xb.
(KGSL's equivalent table is `gen7_0_0_protected_regs` in adreno-gpulist.h used by gen7_2_1, written by `gen7_protect_init` with `GEN7_PROTECT_DEFAULT = BIT0|BIT1|BIT3`.)

## Appendix C: PKT7/PKT4 header constants (computed with the odd-parity formula above)
CP_THREAD_CONTROL(1) 0x70970001 | CP_ME_INIT(7) 0x70c80007 | CP_MEM_WRITE(3) 0x703d8003, (4) 0x703d0004 | CP_EVENT_WRITE(4) 0x70460004, (1) 0x70460001 | CP_SET_SECURE_MODE(1) 0x70e60001 |
CP_NOP(0) 0x70108000, (1) 0x70100001 | CP_WAIT_FOR_IDLE(0) 0x70268000 | CP_WAIT_MEM_WRITES(0) 0x70928000 | CP_WAIT_FOR_ME(0) 0x70138000 | CP_REG_TO_MEM(3) 0x703e8003 | CP_SET_MARKER(1) 0x70e50001 | CP_SET_PROTECTED_MODE(1) 0x70df0001 |
PKT4 (reg dw 0x885 = CP_SCRATCH(2), 1 value) 0x48088501.
Event ids: CACHE_FLUSH_TS 4, CACHE_CLEAN 0x31, LRZ_CACHE_FLUSH 0x26 (adreno_pm4types.h).
HFI header examples (seq n): PERF_TABLE `n<<20 | 59<<8 | 4`, BW_TABLE `n<<20 | 160<<8 | 3`, CORE_FW_START `n<<20 | 2<<8 | 14`, START `n<<20 | 1<<8 | 10`, GX_BW_PERF_VOTE `n<<20 | 4<<8 | 30`, PREPARE_SLUMBER `n<<20 | 3<<8 | 33`.
