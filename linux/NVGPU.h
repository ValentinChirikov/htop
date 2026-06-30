#ifndef HEADER_NVGPU
#define HEADER_NVGPU
/*
htop - NVGPU.h
(C) 2026 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include <stddef.h>
#include <stdbool.h>

#include "Meter.h"


#define NVGPU_MAX_GPUS 8

typedef struct {
   char* name;
   unsigned long long totalMem;
   unsigned long long usedMem;
   double utilization;
} NVGPUMeterInfo;

extern NVGPUMeterInfo NVGPUMeter_engineData[NVGPU_MAX_GPUS];

extern const MeterClass NVGPUMeter_class;

bool NVGPUMeter_active(void);

#ifdef BUILD_WITH_NVIDIA
void NVGPUMeter_shutdown(void);
#endif /* BUILD_WITH_NVIDIA */

#endif /* HEADER_NVGPU */
