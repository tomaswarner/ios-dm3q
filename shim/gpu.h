#pragma once

void gpu_probe(void);	/* DM3Q Adreno 740 probe 1: CX power, GMU clocks, TCM load (build with DIAG=gpu) */
void gpu2_probe(void);	/* probe 2: kgsl-smmu context for the GMU, fw DDR block, GMU boot (DIAG=gpu2) */
