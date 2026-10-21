#pragma once

void touch_preboot(void);	/* QUP1 BCM vote + S2DOS05 LDO2/LDO3 before xnu (sweettouch=2) */
void touch_probe(void);		/* DM3Q STM touch over QUP1 SE4 SPI: reset, id, scan, show touches */
