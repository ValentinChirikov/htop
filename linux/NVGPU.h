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
   double powerUsage; /* current draw in Watts, or -1.0 if unavailable */
   double powerLimit; /* enforced power cap in Watts, or -1.0 if unavailable */
   double temperature; /* GPU die temperature in °C, or -1.0 if unavailable */
   double tempThreshold; /* slowdown threshold in °C, or -1.0 if unavailable */
} NVGPUMeterInfo;

extern NVGPUMeterInfo NVGPUMeter_engineData[NVGPU_MAX_GPUS];

extern const MeterClass NVGPUMeter_class;

extern const MeterClass NVGPUPowerMeter_class;

extern const MeterClass NVGPUTempMeter_class;

bool NVGPUMeter_active(void);

/* Detect NVIDIA GPUs via NVML (lazy NVML init on first call).
 * Returns the number of detected GPUs (capped at NVGPU_MAX_GPUS), or 0 if
 * NVML is unavailable or no NVIDIA GPUs are present. */
unsigned int NVGPUMeter_detectGPUs(void);

/* Returns the product name of the GPU at the given 0-based index, or NULL if
 * the index is out of range or the GPU has no name. */
const char* NVGPUMeter_gpuName(unsigned int gpuIndex);

#ifdef BUILD_WITH_NVIDIA
void NVGPUMeter_shutdown(void);
#endif /* BUILD_WITH_NVIDIA */

#endif /* HEADER_NVGPU */
