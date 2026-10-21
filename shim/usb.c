/*
 * DM3Q USB state probe (DIAG=usb): what does Samsung's ABL leave for the USB
 * controller?  sweet's xnu driver (bsd/dev/sweet_usb.c) assumes ABL leaves the
 * USB30_PRIM GDSC, the clocks and the PHY on and only restarts the DWC3.
 * READ ONLY: no register is written.  DWC3/QSCRATCH/PHY are read only when the
 * GDSC and the master/AHB clocks report on (a read of a powered-down block can
 * hang the bus).  Offsets: Linux gcc-sm8550.c, tcsrcc-sm8550.c, dwc3 core.h,
 * dwc3-qcom.c, phy-qcom-snps-eusb2.c; addresses from the S23's DT (ssusb@a600000,
 * hsphy@88e3000, iommus <&apps_smmu 0x40 0>).
 */
#include <stdint.h>
#include "console.h"
#include "usb.h"

#define GCC		0x00100000UL
#define TCSR		0x01fc0000UL
#define DWC3		0x0a600000UL
#define QSCRATCH	0x0a6f8800UL
#define EUSB2		0x088e3000UL
#define SMMU		0x15000000UL

static inline uint32_t rd(uintptr_t a)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
	return v;
}

static int on(uint32_t cbcr)
{
	return !(cbcr >> 31);		/* CLK_OFF clear */
}

void usb_probe(void)
{
	con_str("USB", "dm3q USB state left by ABL (read only)");

	uint32_t gdsc = rd(GCC + 0x39004);
	uint32_t master = rd(GCC + 0x39018), sleep = rd(GCC + 0x39024), utmi = rd(GCC + 0x39028);
	uint32_t cfgaxi = rd(GCC + 0x39088), aggaxi = rd(GCC + 0x3908c);
	uint32_t aux = rd(GCC + 0x39060), comaux = rd(GCC + 0x39064), pipe = rd(GCC + 0x39068);
	uint32_t clkref = rd(TCSR + 0x15118);
	con_hex2("USB GDSC", "usb30_prim=", gdsc, "on=", gdsc >> 31);
	con_hex2("USB CLK", "master=", master, "sleep=", sleep);
	con_hex2("USB CLK", "utmi=", utmi, "cfg/agg axi=", (cfgaxi >> 31) << 4 | (aggaxi >> 31));
	con_hex2("USB CLK", "phy aux/com/pipe=", (aux >> 31) << 8 | (comaux >> 31) << 4 | (pipe >> 31), "usb2 clkref=", clkref);

	/* SMMU: is the USB stream (0x40) routed? */
	uint32_t nsmr = rd(SMMU + 0x20) & 0xff, found = 0;
	for (uint32_t n = 0; n < nsmr && n < 256; n++) {
		uint32_t smr = rd(SMMU + 0x800 + 4 * n);
		if ((smr >> 31) && !((smr ^ 0x40) & ~(smr >> 16) & 0x7fff)) {
			con_hex2("USB SMMU", "smr=", n << 24 | (smr & 0xffffff), "s2cr=", rd(SMMU + 0xc00 + 4 * n));
			found++;
		}
	}
	if (!found)
		con_str("USB SMMU", "sid 0x40 unmatched (DMA would fault)");

	if (!(gdsc >> 31) || !on(master) || !on(cfgaxi)) {
		con_str("USB", "GDSC or master/AXI clock off: controller not read");
		return;
	}
	/* DWC3 core (dwc3 core.h) */
	con_hex2("DWC3", "gsnpsid=", rd(DWC3 + 0xc120), "gctl=", rd(DWC3 + 0xc110));
	con_hex2("DWC3", "dcfg=", rd(DWC3 + 0xc700), "dctl=", rd(DWC3 + 0xc704));
	con_hex2("DWC3", "dsts=", rd(DWC3 + 0xc70c), "gusb2phycfg=", rd(DWC3 + 0xc200));
	con_hex2("DWC3", "gevnt lo=", rd(DWC3 + 0xc400), "hi/siz=", rd(DWC3 + 0xc404) << 16 | (rd(DWC3 + 0xc408) & 0xffff));
	con_hex2("DWC3", "gevntcount=", rd(DWC3 + 0xc40c), "dalepena=", rd(DWC3 + 0xc720));
	/* Qualcomm wrapper (dwc3-qcom.c): VBUS session override bits */
	con_hex2("QSCRATCH", "hs_phy_ctrl=", rd(QSCRATCH + 0x10), "ss_phy_ctrl=", rd(QSCRATCH + 0x30));
	con_hex2("QSCRATCH", "pwr_evnt=", rd(QSCRATCH + 0x58), "gen_cfg=", rd(QSCRATCH + 0x08));
	if (!on(clkref)) {
		con_str("EUSB2", "usb2 clkref off: PHY not read");
		return;
	}
	/* eUSB2 PHY (phy-qcom-snps-eusb2.c) */
	con_hex2("EUSB2", "utmi_ctrl0=", rd(EUSB2 + 0x3c), "utmi_ctrl5=", rd(EUSB2 + 0x50));
	con_hex2("EUSB2", "hs_ctrl_common0=", rd(EUSB2 + 0x54), "hs_ctrl2=", rd(EUSB2 + 0x64));
	con_hex2("EUSB2", "cfg_ctrl1..2=", rd(EUSB2 + 0x58) << 16 | (rd(EUSB2 + 0x5c) & 0xffff), "cfg3=", rd(EUSB2 + 0x60));
}
