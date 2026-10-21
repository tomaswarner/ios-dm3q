#pragma once
#include <stdint.h>

uint32_t rpmh_version(void);		/* RSC_DRV_ID of drv-2; selects the register layout */
uint32_t rpmh_tcs_idle(void);		/* nonzero when the first ACTIVE TCS is free */
int rpmh_cmddb_ok(void);
uint32_t rpmh_cmddb_addr(const char *name);
int rpmh_write(const uint32_t *addr, const uint32_t *data, int n);
int rpmh_vreg_on(const char *name, uint32_t mv, uint32_t mode);

#define PMIC5_LDO_MODE_HPM	7
