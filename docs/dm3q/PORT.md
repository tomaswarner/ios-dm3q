# ios-sweet → Galaxy S23 Ultra (SM-S918B, "dm3q")

Fuente: firmware oficial `S918BXXSAFZI1` (CHO, Android 16), sin root.
DT base `fw/dts/vendor_boot-2.dtb` (KalamaP v2, msm-id 0x218/0x20000) + overlay de placa
`fw/dts/dtbo-9-id0-rev0.dtb` (DM3Q board-id 13) → `fw/dts/merged-rev9.dts`.
**Confirmado en el teléfono** (`ro.boot.revision` = 13). Datos en vivo (adb, sin root):
- Pantalla física **1440×3088** (`wm size`), densidad 450.
- **RAM 8 GB** (MemTotal 7 243 816 kB), no 12 GB.
- CPUs: 3× A510 (0xd46), **2× A710 (0xd47) + 2× A715 (0xd4d)**, 1× X3 (0xd4e). Features incluyen `paca pacg bti`.
- Bootloader actual **S918BXXS8DYDC** (binario 8), Knox `warranty_bit` 0, bloqueado, `green`.

> ⚠️ **NO ACTUALIZAR EL TELÉFONO.** One UI 8 (Android 16) quitó el desbloqueo del bootloader, y Samsung
> no deja volver atrás un binario. El teléfono está en **S918BXXS8DYDC (One UI 7, binario 8, CSC TPA)**
> y todavía se puede desbloquear. El firmware `firmware/NO-FLASHEAR_binA_OneUI8_*` (binario A) sirve
> **solo para análisis**: no se flashea nunca. Para volver a stock usar `firmware/actual-DYDC/`.

## Constantes para `tools/mkadt.py`, `shim/*.h`, `shim/shim.c`

| Valor | sweet (SM7150) | **S23 Ultra (SM8550-AC)** | Origen en el DT |
|---|---|---|---|
| `GICD`, size | 0x17a00000, 0x10000 | **0x17100000, 0x10000** | `interrupt-controller@17100000` |
| `GICR`, size | 0x17a60000, 0x100000 | **0x17180000, 0x200000**, `redistributor-stride` 0x40000 (8 × 256 KiB, no 128 KiB como sweet) | mismo nodo, 2ª `reg` |
| `TIMEBASE` | 19.2 MHz | **19.2 MHz** (0x124f800) | `arm,armv8-timer` |
| `VTIMER_INTID` | 19 | **27** (PPI 11) | interrupts = sec 13, **phys 14**, **virt 11**, hyp 12 |
| `PTIMER_INTID` | 18 | **30** (PPI 14) | ídem |
| `FB` / `FB_PHYS` | 0x9c000000 | **0xb8000000** | `splash_region` "cont_splash_region" |
| `FB_SIZE` | 0x1700000 | **0x2b00000** (43 MiB) | ídem |
| `FB_W × FB_H` | 1080×2400 | **1440×3088 (probable)** o 1080×2316: *confirmar en el 1er boot* | panel S6E3HAE (modos 720×1544, 1080×2316, 1440×3088, DSC, dual-DSI) |
| `FB_STRIDE` | 1080×4 | **1440×4** (o 1080×4) | — |
| `UFS`, size | 0x01d84000, 0x3000 | **0x01d84000, 0x3000** (igual) | `ufshc@1d84000` `ufs_mem` |
| `IMEM_RESTART_REASON` | 0x146aa65c | **0x146aa65c** (igual) | `qcom,msm-imem@146aa000` + `restart_reason@65c` |
| `PSHOLD` | 0x0c264000 | **no existe en el DT** → usar PSCI `SYSTEM_RESET` (0x84000009) | sin nodo `qcom,pshold` |
| Watchdog | msm-watchdog MMIO | **Gunyah virtual watchdog (`hypervisor:qcom,gh-watchdog`)**, sin MMIO: se maneja por hypercall | ver “Riesgos” |
| CPUs / MPIDR | 8 (6×A55 + 2×A76) | **8: 0x000,0x100,0x200 (A510) · 0x300–0x600 (A715/A710) · 0x700 (X3)** | `cpus`, capacity 0x334/0x708/0x766 |
| `clock-frequency` | 1.8/2.3 GHz | A510 ~2.0 GHz · A715/A710 ~2.8 GHz · X3 ~3.36 GHz | valores públicos del SM8550-AC, no salen del DT (solo informativos en el ADT) |
| RAM (`HIGH_PHYS_BASE/SIZE`) | 0x1_00000000 / 0xfcc00000 | **por confirmar**: el nodo `memory` va vacío (lo llena el ABL) | ver abajo |

### Mapa de memoria reservada (fijo)
```
0x080000000-0x080a00000   gunyah_hyp_region  (hipervisor, no tocar)
0x080a00000-0x082800000   cpusys_vm, xbl logs, aop, smem, tz_stat ...
0x08a800000-0x0a43b4000   mpss (264M), dsps, camera, video, cvp, cdsp, adsp/slpi
0x0b8000000-0x0bab00000   splash_region  ← framebuffer
0x0d4d00000-0x0d8800000   mpss_dsm, xbl_sc, cpucp_fw, qtee
0x0e6440000-0x0e671d000   hwfence
0x0f3800000-0x0fa200000   trust_ui_vm (65M), oem_vm (40M)  ← VMs de Gunyah
0x0ff800000-0x0ffe00000   llcc_lpi
0x880100000-0x880b00000   sec_debug / sec_log_buf / sec_pmsg (Samsung)
```
Hay pools dinámicos (rbin 400M en 0x8_80000000–0x9_00000000, display 164M, qseecom, etc.).
DDR del SM8550: banco bajo 0x80000000–0xffffffff y banco alto desde 0x8_80000000. Con 8 GB el banco alto
llegaría hasta ~0xA_00000000 (6 GB).
**Propuesta inicial** (igual que sweet: lejos de todo lo fijo y de los pools < 4G):
`HIGH_PHYS_BASE = 0x9_00000000`, `HIGH_MEM_SIZE = 0xf0000000` (~3.75 GiB).
Verificar en el primer boot: el shim recibe en `x0` el DTB que arma el ABL, con el nodo
`memory` ya lleno → imprimirlo en pantalla antes de fijar estos valores.

### Hardware de entrada
- **Touch: STMicro FTS por SPI** (`stm,stm_ts_spi`), *no* Goodix por I²C.
  QUP v3 SE en **0x00a90000** (`spi@a90000`, wrapper `qupv3_1` @ 0xac0000), IRQ = TLMM GPIO **25**,
  CS = GPIO **47**, SPI máx 10 MHz, coords 4096×4096.
  Alimentación: **S2DOS05** (PMIC Samsung por I²C) `l3` (tsp_io) y `panel_vci` (tsp_avdd), no RPMh.
  → los parches 0060 (motor GENI I²C), 0062 (RPMh L11C) y 0063 (Goodix GT9896) hay que **reescribirlos**:
  driver GENI-SPI + protocolo STM FTS (referencia: `drivers/input/sec_input/stm_spi/` del kernel de Samsung).
- **Botones** (SPMI arb `0x0c42d000`, core `0x0c400000`, chnls `0x0c500000`, obsrvr `0x0c440000`, intr `0x0c4c0000`):
  PMK8550 SID 0 PON `0x1300`: KPDPWR (power, irq 0x13/7) y RESIN (vol −, irq 0x13/6).
  **Vol +** = PM8550 GPIO 6 (`pm8550_gpios`, `pinctrl@8800`).

## Imagen de arranque (`shim/build.sh`)
El S23 Ultra usa **boot header v4** (GKI). El DT **no va en `boot.img`**: el ABL lo saca de
`vendor_boot`. Para el shim:
```
mkbootimg --header_version 4 --os_version 13.0.0 --os_patch_level 2026-09 \
          --kernel out/Image --cmdline "" -o out/shim-boot.img
```
y se deja el `vendor_boot` **stock** (pagesize 0x1000, kernel_offset 0x8000, ramdisk 0x2000000,
tags 0x1e00000, dtb 0x1f00000). `init_boot` (ramdisk de Android) no se usa.
Si el shim necesita datos propios (payload/ADT), meterlos al final del `Image` en vez del ramdisk.

## Flasheo (Samsung no tiene `fastboot boot`)
1. Desbloqueo OEM (Ajustes → Opciones de desarrollador → Desbloqueo OEM; luego Download Mode,
   mantener Vol + → desbloquear). **Borra todo y quema Knox para siempre.**
2. `vbmeta` con verificación desactivada (`avbtool make_vbmeta_image --flags 3 --padding_size 4096`),
   empaquetado en `.tar` como hace Magisk.
3. Cada prueba = flashear `boot` (tar con `boot.img`) con `tools/samloader-rs/samloader flash`
   (o Odin/Heimdall). Volver a Android = flashear el `boot.img` stock (`fw/img/boot.img`).
4. **Respaldar primero** (con TWRP/root, o al menos `dd` desde recovery): `efs`, `sec_efs`, `persist`,
   `modemst1/2`, `fsg`, `fsc`, `param`, `steady`. `efs` tiene el IMEI: perderlo no se recupera.

## Riesgos y diferencias grandes respecto a sweet/V60
1. **Gunyah**: todo Linux corre como VM bajo el hipervisor Gunyah de Qualcomm (EL2). La memoria que
   no le asigna a la VM principal da *stage-2 fault* = reinicio instantáneo. El watchdog es virtual
   (hypercall de Gunyah, driver `gh_virt_wdt` en el kernel de Samsung): si el ABL lo deja armado,
   el shim tiene que acariciarlo o apagarlo, igual que el V60 tuvo que apagar el WDT MMIO.
2. **Pantalla**: panel con DSC y dual-DSI. Mientras el ABL deje el pipeline vivo y escaneando
   `splash_region`, alcanza con escribir ahí (es lo que hace el shim). Resolución por confirmar.
3. **Touch por SPI + PMIC Samsung**: es el trabajo más grande de drivers.
4. **UFS 4.0**: misma dirección y registros UFSHCI estándar; el patch 0034 (DMA > 4G) debe aceptar
   CAP.64AS. Además el UFS **está detrás del SMMU** (`iommus = <&apps_smmu 0x60 0>`, `dma-coherent`),
   cosa que en sweet no pasa: si el SMMU no deja el SID 0x60 en bypass, el DMA del UFS falla
   y hace falta programar el SMMU (o pedirle a Gunyah un mapeo identidad).
5. Los núcleos ARMv9 (X3/A715/A510) **sí tienen PAC**, a diferencia del 732G, pero este proyecto
   sigue en iOS 16 / iPhone X.

## Próximos pasos
1. `adb shell getprop ro.boot.revision` → elegir el overlay correcto.
2. Fork de `ref/ios-sweet`, aplicar la tabla de arriba (guiarse por `ref/ios-v60`, que cambió lo mismo).
3. `shim/build.sh` → header v4; primer shim **de diagnóstico**: sin XNU, pinta la pantalla,
   imprime los bancos de `memory` del DTB, el EL actual y prueba GIC/timers (como `tools/build-v60-diag.py`).
4. Recién con eso: desbloqueo + flash del shim de diagnóstico.
