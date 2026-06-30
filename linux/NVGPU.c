/*
htop - NVGPU.c
(C) 2026 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/NVGPU.h"

#ifdef BUILD_WITH_NVIDIA

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <nvml.h>

#include "CRT.h"
#include "Macros.h"
#include "Object.h"
#include "Platform.h"
#include "RichString.h"
#include "XUtils.h"


#define NVGPU_MAX_GPUS 8


/* ---- Global state ---- */

static nvmlDevice_t nvmlDevices[NVGPU_MAX_GPUS];
static unsigned int nvmlDeviceCount = 0;
static bool nvmlInitialized = false;

typedef struct {
   unsigned long long prevPeriod;
   unsigned int prevUtilization;
} NvmlUtilHistory;

static NvmlUtilHistory nvmlUtilHistory[NVGPU_MAX_GPUS];

NVGPUMeterInfo NVGPUMeter_engineData[NVGPU_MAX_GPUS];
static unsigned int activeMeters = 0;


/* ---- Initialize NVML (called once on first meter access) ---- */

static bool nvmlInitLibrary(void) {
   if (nvmlInitialized)
      return true;

   if (nvmlInit() != 0)
      return false;

   unsigned int count = 0;
   if (nvmlDeviceGetCount(&count) != 0 || count == 0) {
      nvmlShutdown();
      return false;
   }

   if (count > NVGPU_MAX_GPUS)
      count = NVGPU_MAX_GPUS;

   for (unsigned int i = 0; i < count; i++) {
      nvmlDevices[i] = NULL;
      nvmlUtilHistory[i].prevPeriod = 0;
      nvmlUtilHistory[i].prevUtilization = 0;
      NVGPUMeter_engineData[i].utilization = -1.0;
      NVGPUMeter_engineData[i].totalMem = 0;
      NVGPUMeter_engineData[i].usedMem = 0;

      if (nvmlDeviceGetHandleByIndex(i, &nvmlDevices[i]) == 0) {
         char name[80] = {0};
         nvmlDeviceGetName(nvmlDevices[i], name, sizeof(name));
         NVGPUMeter_engineData[i].name = xStrdup(name[0] ? name : "NVIDIA GPU");
      }
   }

   nvmlDeviceCount = count;
   nvmlInitialized = true;
   return true;
}


/* ---- Fetch utilization and memory for all GPUs ---- */

static void nvmlFetchValues(void) {
   if (!nvmlInitialized || nvmlDeviceCount == 0)
      return;

   static bool firstSample = true;
   if (firstSample) {
      for (unsigned int i = 0; i < nvmlDeviceCount; i++)
         nvmlUtilHistory[i].prevPeriod = 0;
      firstSample = false;
   }

   /* Query memory info */
   for (unsigned int i = 0; i < nvmlDeviceCount; i++) {
      if (!nvmlDevices[i])
         continue;

      nvmlMemory_t memInfo;
      if (nvmlDeviceGetMemoryInfo(nvmlDevices[i], &memInfo) == 0) {
         NVGPUMeter_engineData[i].totalMem = memInfo.total;
         NVGPUMeter_engineData[i].usedMem = memInfo.used;
      }
   }

   /* Query utilization */
   for (unsigned int i = 0; i < nvmlDeviceCount; i++) {
      if (!nvmlDevices[i])
         continue;

      unsigned int count = 1;
      nvmlProcessUtilizationSample_t sample;
      memset(&sample, 0, sizeof(sample));

      nvmlReturn_t ret = nvmlDeviceGetProcessUtilization(
         nvmlDevices[i], &sample, &count, 1000);

      if (ret == 0 && count > 0 && sample.lastCounterPeriod > nvmlUtilHistory[i].prevPeriod) {
         unsigned long long periodDelta = sample.lastCounterPeriod - nvmlUtilHistory[i].prevPeriod;
         if (periodDelta > 0) {
            int utilDelta = sample.utilization - nvmlUtilHistory[i].prevUtilization;
            NVGPUMeter_engineData[i].utilization =
               CLAMP(100.0 * utilDelta / periodDelta * 1000.0, 0.0, 100.0);
         }
         nvmlUtilHistory[i].prevPeriod = sample.lastCounterPeriod;
         nvmlUtilHistory[i].prevUtilization = sample.utilization;
      }
   }
}


/* ---- Meter: updateValues ---- */

static void NVGPUMeter_updateValues(Meter* this) {
   unsigned int gpuIndex = this->param - 1; /* param is 1-based (0 = summary) */

   if (!nvmlInitialized || !nvmlInitLibrary() || gpuIndex >= nvmlDeviceCount || !nvmlDevices[gpuIndex]) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      this->values[0] = NAN;
      this->values[1] = NAN;
      return;
   }

   nvmlFetchValues();

   double util = NVGPUMeter_engineData[gpuIndex].utilization;
   if (util < 0.0) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      this->values[0] = NAN;
      this->values[1] = NAN;
      return;
   }

   this->values[0] = util;

   if (NVGPUMeter_engineData[gpuIndex].totalMem > 0) {
      this->values[1] = 100.0 * NVGPUMeter_engineData[gpuIndex].usedMem / NVGPUMeter_engineData[gpuIndex].totalMem;
   } else {
      this->values[1] = NAN;
   }

   /* Build text summary */
   char utilBuf[16];
   xSnprintf(utilBuf, sizeof(utilBuf), "%.1f%%", util);

   if (isNonnegative(this->values[1])) {
      char memUsed[16];
      char memTotal[16];
      Meter_humanUnit(memUsed, NVGPUMeter_engineData[gpuIndex].usedMem / (1024.0 * 1024.0), sizeof(memUsed));
      Meter_humanUnit(memTotal, NVGPUMeter_engineData[gpuIndex].totalMem / (1024.0 * 1024.0), sizeof(memTotal));
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%s Mem %s/%sMiB %.1f%%",
         utilBuf, memUsed, memTotal, this->values[1]);
   } else {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%s Mem N/A", utilBuf);
   }
}


/* ---- Meter: display ---- */

static void NVGPUMeter_display(const Object* cast, RichString* out) {
   const Meter* this = (const Meter*)cast;
   unsigned int gpuIndex = this->param - 1;

   if (!isNonnegative(this->values[0])) {
      RichString_appendAscii(out, CRT_colors[METER_SHADOW], " N/A");
      return;
   }

   char buffer[50];
   int written;

   RichString_appendAscii(out, CRT_colors[METER_TEXT], ":");
   written = xSnprintf(buffer, sizeof(buffer), "%5.1f%%", this->values[0]);
   RichString_appendnAscii(out, CRT_colors[GPU_ENGINE_1], buffer, written);

   if (isNonnegative(this->values[1])) {
      Meter_humanUnit(buffer, NVGPUMeter_engineData[gpuIndex].usedMem / (1024.0 * 1024.0), sizeof(buffer));
      int memLen = strlen(buffer);
      xSnprintf(buffer + memLen, sizeof(buffer) - memLen, "MiB");
      written = xSnprintf(buffer + memLen + 3, sizeof(buffer) - memLen - 3, "%.1f%%", this->values[1]);
      RichString_appendAscii(out, CRT_colors[METER_TEXT], " Mem ");
      RichString_appendnAscii(out, CRT_colors[GPU_ENGINE_2], buffer, memLen + 3 + written);
   } else {
      RichString_appendAscii(out, CRT_colors[METER_TEXT], " Mem N/A");
   }
}


/* ---- Meter: init/done ---- */

static void NVGPUMeter_init(Meter* this) {
   activeMeters++;

   nvmlInitLibrary();

   unsigned int gpuIndex = this->param - 1;
   if (gpuIndex < nvmlDeviceCount && NVGPUMeter_engineData[gpuIndex].name) {
      Meter_setCaption(this, NVGPUMeter_engineData[gpuIndex].name);
   } else {
      char caption[16];
      xSnprintf(caption, sizeof(caption), "GPU %u", gpuIndex);
      Meter_setCaption(this, caption);
   }
}

static void NVGPUMeter_done(Meter* this) {
   (void)this;
   if (activeMeters > 0)
      activeMeters--;
}


/* ---- Meter: getUiName ---- */

static void NVGPUMeter_getUiName(const Meter* this, char* buffer, size_t length) {
   assert(length > 0);
   xSnprintf(buffer, length, "NVGPU %u", this->param - 1);
}


/* ---- Meter class definition ---- */

const MeterClass NVGPUMeter_class = {
   .super = {
      .extends = Class(Meter),
      .delete = Meter_delete,
      .display = NVGPUMeter_display,
   },
   .init = NVGPUMeter_init,
   .done = NVGPUMeter_done,
   .getUiName = NVGPUMeter_getUiName,
   .updateValues = NVGPUMeter_updateValues,
   .defaultMode = BAR_METERMODE,
   .supportedModes = METERMODE_DEFAULT_SUPPORTED,
   .maxItems = 2,
   .isPercentChart = true,
   .total = 100.0,
   .attributes = (const int[]) { GPU_ENGINE_1, GPU_ENGINE_2 },
   .name = "NVGPU",
   .uiName = "NVGPU",
   .caption = "GPU"
};


/* ---- Active meter check ---- */

bool NVGPUMeter_active(void) {
   return activeMeters > 0;
}


/* ---- Shutdown: free GPU name strings and shut down NVML ---- */

void NVGPUMeter_shutdown(void) {
   if (nvmlInitialized) {
      nvmlShutdown();
      nvmlInitialized = false;
      for (unsigned int i = 0; i < nvmlDeviceCount; i++) {
         free((void*)NVGPUMeter_engineData[i].name);
         NVGPUMeter_engineData[i].name = NULL;
      }
   }
}
#else

/* Stub when NVIDIA support is not enabled. */
NVGPUMeterInfo NVGPUMeter_engineData[NVGPU_MAX_GPUS];
const MeterClass NVGPUMeter_class = { 0 };
bool NVGPUMeter_active(void) { return false; }

#endif
