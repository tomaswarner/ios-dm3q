# Reboot to Samsung Download Mode (Odin) from our own EL1 kernel, SM-S918B (dm3q)

Status: research only, nothing flashed. Evidence = fw/dts/merged-rev9.dts (cited "DTS:line") and
disassembly of the stock vendor_boot modules in /private/tmp/claude-501/gpu-research/vramdisk/x/lib/modules
(llvm-objdump -dr; disassembly copies in the session scratchpad d/*.S). Items marked UNVERIFIED are inference.

## 0. Answer in one paragraph

`adb reboot download` does NOT use Qualcomm dload/TCSR/IMEM cookies. Samsung's `sec_qc_rbcmd` maps the command string
"download" to PON restart reason 0x15 (PON_RESTART_REASON_DOWNLOAD) and writes that single byte into a PMIC SDAM
register (PMK8550 SID0, SDAM at PID 0x71, offset 0x48, bit-field [7:1]). The IMEM Samsung cookie is skipped for
"download" (value -1). Then a plain cold reset (PSCI SYSTEM_RESET) is issued. Samsung's bootloader (sboot, not in
our hands; UNVERIFIED but implied by the behavior) reads that SDAM byte after reset and enters Download Mode.
Before resetting, Android also clears the Qualcomm download-mode (dump) cookie with an SCM call; we should too.

## 1. How the stock Samsung kernel does it

### 1a. IMEM (qcom,msm-imem @ 0x146aa000, DTS:15121-15180)
Children (offset from 0x146aa000): restart_reason@0x65c (DTS:15143), upload_cause@0x66c (15133), dload_type@0x1c (15148),
diag_dload@0xc8, mem_dump_table@0x10, boot_stats@0x6b0, kaslr_offset@0x6d0.
- `sec_qc_qcom_reboot_reason.ko` (str "qcom,msm-imem-restart_reason", "unable to map imem restart reason offset";
  `__qc_reboot_reason_ioremap_qcom_restart_reason` @0x7b0 does of_find_compatible_node + of_iomap, stores ptr at drv+0x20).
  `sec_qc_reboot_reason_write_sec_rr` @0xaa0: `if (val == -1) return 0; *(u32*)imem_restart_reason = val;`
  So IMEM[0x146aa65c] is the Samsung "sec_rr" (magic 0x7766 55xx) and -1 means "do not write".
- sec_rr magic values seen in `sec_qc_rbcmd.ko` command table (.rodata @0x718, 0x28-byte entries {pad,pad,name,handler,{u32 pon_rr,u32 sec_rr}}):
  `recovery` 0x77665502, `rtc` 0x77665503, `dm-verity device corrupted` 0x77665508, `dm-verity enforcing` 0x77665509,
  `keys clear` 0x7766550a, default/normal reboot with a cmd string 0x77665501 (sec_qc_rbcmd_default_reason @0xca0:
  `mov w8,#0x5501; movk w8,#0x7766,lsl16`), and `sec_debug_hw_reset`/panic = 0x776655ee (pon_rr 0x80 = skip).
- **"download" has sec_rr = 0xffffffff (skipped).** Table entry offset 0x1b8: name "download", handler
  `sec_qc_rbcmd_simple_strict` (.text+0x17c4), value qword 0xffffffff_00000015 -> pon_rr=0x15, sec_rr=-1.
  (Layout confirmed by `sec_qc_rbcmd_simple` @0x1714: `ldp w22,w21,[x0,#0x20]` ; pon chain = notifier at qc_rbcmd+0x38,
  sec chain = qc_rbcmd+0x40; `qc_pre_defined` @0x18a0 uses `[x21,#0x24]`=sec for "recovery".)
  Handler requires strlen(cmd)==strlen("download") (exact match).

### 1b. PMIC PON restart reason = Samsung's real download trigger
- DTS:5154-5162 `samsung,qcom-qcom_reboot_reason`: nvmem-cell-names "restart_reason","pon_reason", cells <0x52c 0x5b3>.
- DTS:27680 (inside qcom,spmi@c42d000 / qcom,pmk8550@0, DTS:27372; sdam node at DTS:27670 `sdam@7100`, compatible qcom,spmi-sdam):
  `restart_reason: restart@48 { bits = <1 7>; reg = <0x48 1>; }` and `pon_reason@69` (read-only, report only).
  => SPMI address: **SID 0 (pmk8550@0), PID 0x71, register 0x48** = SPMI addr 0x7148. Field = bit_offset 1, nbits 7,
  i.e. stored byte = (value << 1) | (old & 1). For value 0x15 the byte is **0x2A** (bit0 preserved).
  Also exposed as stock DT node `reboot_reason` (DTS:13105, qcom,reboot-reason, same cell) used by qcom-reboot-reason.ko
  (recovery -> 0x01, bootloader -> 0x02 via `reasons` table in .data+0xf0; no "download" there).
- `sec_qc_reboot_reason_write_pon_rr` @0xad0 (sec_qc_qcom_reboot_reason.ko): `if (val == 0x80) return 0 (skip)`; also skipped if
  param->[8]==3 (meaning unknown, UNVERIFIED); else `nvmem_cell_write(cell, &val_byte, 1)` up to 4 attempts, each
  verified by `nvmem_cell_read` == written byte (log "0x%02hhX is written successfully. (retry = %zu)"). On total failure
  it warns "pon reason was not written properly!" and drops the cell.
- Enum names (BTF strings, sec_qc_qcom_reboot_reason.ko) in order: ... NORMALBOOT, DOWNLOAD, NVBACKUP, NVRESTORE, NVERASE,
  NVRECOVERY ...; command table gives NORMALBOOT=0x14 (GlobalActions restart/userrequested/silent.sec/default),
  **DOWNLOAD=0x15**, NVBACKUP 0x16, NVRESTORE 0x17, NVERASE 0x18, NVRECOVERY 0x19, recovery-update 0x60, cpmem_on/off 0x35/0x36,
  mbsmem_on/off 0x3e/0x3f, dram 0x2d, peripheral_hw_reset 0x1a, cross_fail 0x2c, user_dram_test 0x40, quest 0x51-0x5b,
  panic/hw_reset 0x80 (skip). Default for any unknown cmd: pon_rr=0x14 (sec_qc_rbcmd_default_reason @0xd60).
- That the *bootloader* maps SDAM 0x15 to Download Mode is UNVERIFIED (sboot is not available); it is the only
  `download`-specific value the kernel writes, so it must be it.

### 1c. Notifier plumbing (why this runs on `reboot download`)
`sec_reboot_cmd.ko` registers a reboot notifier (DTS:5250 `samsung,reboot_cmd`, sec,reboot_notifier-priority 0xfa) and a restart
handler (priority 0xdc, panic-path). `samsung,qcom-reboot_cmd` (DTS:5243, `sec,use-on_reboot`) registers the QC table above at the
reboot-notifier stage, so LINUX_REBOOT_CMD_RESTART2 "download" -> pon writer (SDAM) only. Nothing else is needed from the AP side.

### 1d. Qualcomm dload mode (what it is, why it is NOT the download trigger)
- DTS:2073 `qcom_scm { qcom,dload-mode = <0x02 0x19000>; }`, phandle 0x02 = `tcsr: syscon@1fc0000` (DTS:23156, size 0x30000).
  => **dload cookie register = TCSR_BOOT_MISC_DETECT-class register at PA 0x1fc0000 + 0x19000 = 0x01FD9000.**
- qcom-dload-mode.ko: `.data+0x0 = 0x10` default dump_mode (FULL dump), `.data+0x4 = 1` default download_mode=enabled, so Android arms
  dump mode (mode 0x10; minidump bit 0x20; 0x30 both) at boot (`qcom_dload_probe` -> `msm_enable_dump_mode`->`qcom_scm_set_download_mode(0x10,0)`).
  `qcom_dload_reboot` @0x660: cmd "edl" (rodata+0xa3) -> mode 1 (Emergency DownLoad, 9008) + reboot_mode=WARM;
  "qcom_dload" -> mode=dump_mode; "rtc" -> qcom_scm_custom_reset_type=0x80000005; "twm" -> 0x80000006 (SYSTEM_RESET2 vendor type);
  anything else (incl. "download") -> `qcom_scm_set_download_mode(0)` first (when `[nb+0x78]==0`), i.e. **dump mode is disarmed on a
  normal/download reboot**. `sec_qc_reboot_reason_reboot_call` @0xa74 also calls `qcom_set_dload_mode(drv->flag@+0x88)`; that flag is set
  to 1 only by `sec_qc_reboot_reason_panic_call` @0xa00 (which also writes sec_rr=0x776655ee and arms dload) -> normal reboot = 0.
- `qcom_scm_set_download_mode(mode, tcsr)` (qcom-scm.ko @0x1948): if `scm->dload_mode_addr` (struct+0xe0, filled from the DT property) is set ->
  `qcom_scm_io_writel(dload_mode_addr, mode)`; else if SVC_BOOT cmd 0x10 is available -> legacy call svc=1 cmd=0x10 arginfo=2 args[0]=mode.
  Since the DT has the property, the Android path is the **io-write variant**.

### 1e. Reset call
Android reboot_mode stays COLD for "download" (the WARM override exists only in the "edl" branch), so arm64 psci_sys_reset takes the
`else` branch: `PSCI_0_2_FN_SYSTEM_RESET` (0x84000009) (Linux v6.1 drivers/firmware/psci/psci.c: warm+RESET2 only if reboot_mode is WARM/SOFT).
SYSTEM_RESET2 (0xC4000012) is not needed; vendor reset types 0x80000005/6 are rtc/twm only.

## 2. Minimal sequence for our kernel

1. (Recommended, cheap) disarm Qualcomm dump/EDL cookie exactly like Android's reboot path:
   SCM call, SMC64 fast, SIP owner. Function id = `0xC2000000 | (svc<<8) | cmd`.
   - io-write variant (what this DT selects): svc=0x05 (QCOM_SCM_SVC_IO) cmd=0x02 (IO_WRITE), **fid 0xC2000502**,
     x1 = arginfo 0x2 (QCOM_SCM_ARGS(2)), x2 = 0x01FD9000, x3 = 0x00000000, x4..x6 = 0. (qcom_scm_io_writel @qcom-scm.ko 0x1af0:
     desc svc=5 cmd=2 arginfo=2 args={addr,val}; SMC64 per qcom_scm-smc.c `__scm_smc_call`.) Retry while x0 == QCOM_SCM_INTERRUPTED (1).
   - boot variant (fallback): svc=0x01 cmd=0x10 (QCOM_SCM_BOOT_SET_DLOAD_MODE), **fid 0xC2000110**, x1=2, x2=mode (0 = clear;
     0x10 full dump; 0x1 EDL), rest 0. (qcom-scm.ko 0x1948 sets only args[0]=mode; mainline passes args={0x10, enable?0x10:0}.)
   Result in x0 (0 = success). Failure is non-fatal for our goal; see risks.
2. Write PMIC restart reason through the SPMI arbiter (write channel, EE0): SID0, PID 0x71 (PPID 0x071), reg 0x48.
   read old byte, `new = (old & 0x01) | (0x15 << 1)` = 0x2A when old bit0 = 0, write it, read back and compare (Samsung retries 4x).
   Spec for SM8550 (Linux spmi-pmic-arb v7, v6.6, fetched): channel offset `0x1000*apid` in "chnls" (DTS:27274 reg: chnls = 0xc500000, size 0x400000);
   registers CMD 0x00, CONFIG 0x04, STATUS 0x08, WDATA0 0x10, RDATA0 0x18; cmd word `(opc<<27)|((addr&0xff)<<4)|(bc&7)`,
   EXT_WRITEL opc=0 (bc=0 for 1 byte), EXT_READL opc=1; STATUS bit0 DONE, bit1 FAILURE, bit2 DENIED, bit3 DROPPED;
   write order: WDATA0 first then CMD. APID lookup: PPID->APID map at core (0xc400000) + 0x2000 + 4*n (v7);
   owner/write_ee at cnfg (0xc42d000) + 0x700 + 4*n, must equal our EE (DTS: qcom,ee = 0, qcom,channel 0). 
   **UNVERIFIED / caution:** the existing `sweet_spmi.c` (read-only, observer) uses different bases (core 0xc440000, cfg 0xc40a000,
   observer 0xe600000, 0x80*apid, accepts arb version 0x5xxxxxxx-0x6xxxxxxx) than the DT (core 0xc400000, cnfg 0xc42d000,
   obsrvr 0xc440000, chnls 0xc500000) and than v7 (version >= 0x70000000). Reconcile before writing: read PMIC_ARB_VERSION (reg 0 of
   the true core) first, and only extend sweet_spmi with a write path after confirming APID mapping reads the right PPID
   (e.g. the existing key/battery reads). The write channel base for v5/v6 would be `0x10000*apid` (v5 style), v7 is `0x1000*apid`.
   Gunyah note: HLOS (primary VM) is the EE0 owner in Android and programs this arbiter directly (SDAM is accessed by Linux via nvmem); no SCM needed.
3. (Optional hygiene, UNVERIFIED necessity) leave IMEM 0x146aa65c / 0x146aa66c untouched. Android does not write them for "download".
   Log them beforehand; if 0x146aa65c reads 0x776655ee or 0x146aa66c is nonzero from an earlier crash, see Risks.
4. Reset: PSCI SYSTEM_RESET, function id **0x84000009** via SMC (already working for us). Do not use SYSTEM_RESET2.
   (Reset type/cookie irrelevant; if RESET2 were ever used it would be fid 0xC4000012 with x1=0 arch warm reset - do not.)

Gunyah / access summary: IMEM, SPMI arbiter and the PSCI/SCM SMCs are all things the Android HLOS does as the primary VM (IMEM is a normal DT node
the stock kernel ioremaps; SMCs are forwarded to TZ), so none is expected to be blocked. TCSR is XPU-protected: do NOT write 0x1FD9000 directly,
use the SCM IO-write (secure world performs it). UNVERIFIED: that Gunyah forwards SIP-owner SCM calls to TZ for our VM exactly as for Android;
PSCI already works, the SCM path is the same SMC conduit.

## 3. Risks (Upload mode / RAM dump instead of Download)

- Armed dump cookie at reset: if TCSR 0x1FD9000 holds 0x10/0x20/0x30 (set by Android at boot - default dump_mode 0x10, download_mode=1 -
  or left by the bootloader) and the watchdog/warm reset path hits, the next boot goes to RAM-dump ("Upload mode"). Mitigation: step 1 (write 0).
  Value 0x1 selects Qualcomm EDL (9008), also not Odin - never write 1.
- Warm resets: PSCI SYSTEM_RESET2 warm / reboot_mode WARM is paired with dump mode in the stock code; use cold SYSTEM_RESET only.
- Stale Samsung cookies: sec_rr 0x776655ee (panic/sec_debug_hw_reset) and a nonzero IMEM upload_cause (0x146aa66c, written by sec_qc_upload_cause on
  panic) mark an upload event. We never write them; do not write PON 0x80/0x15 mix-ups. The panic notifier path (0xa00) writes 0x776655ee AND
  arms dload - do not mimic panic.
- Wrong SDAM value: other PON reasons (e.g. WATCH_DOG, KERNEL_PANIC, CP_CRASH, MANUAL_RESET, WTSR ...) are bootloader-interpreted crash causes;
  only write 0x15 (stored 0x2A). Mind bit0 preservation (bits <1 7>): writing a raw 0x15 would be interpreted as 0x0A. Verify read-back.
- A kernel watchdog bite or hang between the SDAM write and the reset would also leave the 0x15 in SDAM, causing Download Mode on the next reset
  (harmless but surprising); clear it (write 0x14<<1 = 0x28) after a successful boot if you stage the write earlier.
- SDI/secure watchdog: `qcom_scm_disable_sdi` is invoked by stock when dump is disabled; not required here since dload is zeroed. UNVERIFIED whether TZ has SDI armed
  on our boot path; if a reset ends in upload despite dload=0, add SCM boot-svc call disable_sdi (svc 0x01, cmd 0x09 - UNVERIFIED id).
- Hardware keys: the physical fallback (power off, hold Vol Up + Vol Down, connect USB) enters Download Mode independently of all of the above.
- Persistence: SDAM survives a PS_HOLD cold reset on the always-on PMK8550 (UNVERIFIED; Android relies on it).

## 4. Files / locations cited
- DTS: fw/dts/merged-rev9.dts lines 2073, 5154, 5230-5262, 13105, 15121-15180, 22383, 23156, 27272-27290, 27372, 27670-27688.
- Modules (/private/tmp/claude-501/gpu-research/vramdisk/x/lib/modules): sec_qc_rbcmd.ko, sec_qc_qcom_reboot_reason.ko, sec_reboot_cmd.ko,
  qcom-dload-mode.ko, qcom-reboot-reason.ko, qcom-scm.ko.
- Our SPMI code: /private/tmp/claude-501/-Users-tomaswarner-Desktop-hackios-s23/a874681b-7d77-46bb-a8e0-74641f66ed4d/scratchpad/bt012/xnu/bsd/dev/sweet_spmi.c (read-only today).
- Upstream reference: Linux v6.6 drivers/spmi/spmi-pmic-arb.c; v6.1 drivers/firmware/qcom_scm.c, qcom_scm-smc.c, psci/psci.c.
