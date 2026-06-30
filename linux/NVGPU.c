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


/* ---- Global state ---- */

static nvmlDevice_t nvmlDevices[NVGPU_MAX_GPUS];
static unsigned int nvmlDeviceCount = 0;
static bool nvmlInitialized = false;

/* No history state needed — nvmlDeviceGetUtilizationRates returns instantaneous %. */

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
      nvmlInitialized = false;
      return false;
   }

   if (count > NVGPU_MAX_GPUS)
      count = NVGPU_MAX_GPUS;

   for (unsigned int i = 0; i < count; i++) {
      nvmlDevices[i] = NULL;
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

   /* Query memory info and utilization rates */
   for (unsigned int i = 0; i < nvmlDeviceCount; i++) {
      nvmlDevice_t dev = nvmlDevices[i];
      if (!dev)
         continue;

      nvmlMemory_t memInfo;
      if (nvmlDeviceGetMemoryInfo(dev, &memInfo) == 0) {
         NVGPUMeter_engineData[i].totalMem = memInfo.total;
         NVGPUMeter_engineData[i].usedMem = memInfo.used;
      }

      /* nvmlDeviceGetUtilizationRates returns % of time during which GPU
       * cores/memory were active over the past sample period. */
      nvmlUtilization_t rates;
      if (nvmlDeviceGetUtilizationRates(dev, &rates) == 0) {
         NVGPUMeter_engineData[i].utilization = (double)rates.gpu;
      }
   }
}


/* ---- Meter: updateValues ---- */

static void NVGPUMeter_updateValues(Meter* this) {
   unsigned int gpuIndex = this->param - 1; /* param is 1-based (param 0 is legacy/never created by the UI) */

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
      /* Meter_humanUnit takes a value in KiB and appends its own unit suffix. */
      Meter_humanUnit(memUsed, NVGPUMeter_engineData[gpuIndex].usedMem / 1024.0, sizeof(memUsed));
      Meter_humanUnit(memTotal, NVGPUMeter_engineData[gpuIndex].totalMem / 1024.0, sizeof(memTotal));
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%s Mem %s/%s %.1f%%",
         utilBuf, memUsed, memTotal, this->values[1]);
   } else {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%s Mem N/A", utilBuf);
   }
}


/* ---- Meter: display ---- */

static void NVGPUMeter_display(const Object* cast, RichString* out) {
   const Meter* this = (const Meter*)cast;

   /* param is 1-based (param 0 is reserved/legacy and never created by the UI). */
   unsigned int gpuIndex = this->param - 1;
   if (this->param == 0 || gpuIndex >= nvmlDeviceCount || !isNonnegative(this->values[0])) {
      RichString_appendAscii(out, CRT_colors[METER_SHADOW], " N/A");
      return;
   }

   char buffer[50];
   int written;

   RichString_appendAscii(out, CRT_colors[METER_TEXT], ":");
   written = xSnprintf(buffer, sizeof(buffer), "%5.1f%%", this->values[0]);
   RichString_appendnAscii(out, CRT_colors[GPU_ENGINE_1], buffer, written);

   if (isNonnegative(this->values[1])) {
      /* Meter_humanUnit takes a value in KiB and appends its own unit suffix. */
      Meter_humanUnit(buffer, NVGPUMeter_engineData[gpuIndex].usedMem / 1024.0, sizeof(buffer));
      int memLen = strlen(buffer);
      written = xSnprintf(buffer + memLen, sizeof(buffer) - memLen, " %.1f%%", this->values[1]);
      RichString_appendAscii(out, CRT_colors[METER_TEXT], " Mem ");
      RichString_appendnAscii(out, CRT_colors[BAR_SHADOW], buffer, memLen + written);
   } else {
      RichString_appendAscii(out, CRT_colors[METER_TEXT], " Mem N/A");
   }
}


/* ---- Meter: init/done ---- */

static void NVGPUMeter_init(Meter* this) {
   activeMeters++;

   nvmlInitLibrary();

   /* param is 1-based (param 0 is reserved/legacy and never created by the UI). */
   if (this->param > 0) {
      unsigned int gpuIndex = this->param - 1;
      if (gpuIndex < nvmlDeviceCount && NVGPUMeter_engineData[gpuIndex].name) {
         Meter_setCaption(this, NVGPUMeter_engineData[gpuIndex].name);
         return;
      }
   }
   Meter_setCaption(this, "GPU");
}

static void NVGPUMeter_done(Meter* this) {
   (void)this;
   if (activeMeters > 0)
      activeMeters--;
}


/* ---- Meter: getUiName ---- */

static void NVGPUMeter_getUiName(const Meter* this, char* buffer, size_t length) {
   assert(length > 0);
   if (this->param > 0)
      xSnprintf(buffer, length, "NVGPU %u", this->param - 1);
   else
      xSnprintf(buffer, length, "NVGPU");
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
   .attributes = (const int[]) { GPU_ENGINE_1, BAR_SHADOW },
   .name = "NVGPU",
   .uiName = "NVGPU",
   .caption = "GPU"
};


/* ---- Active meter check ---- */

bool NVGPUMeter_active(void) {
   return activeMeters > 0;
}


/* ---- GPU discovery (used by AvailableMetersPanel to list per-GPU meters) ---- */

unsigned int NVGPUMeter_detectGPUs(void) {
   nvmlInitLibrary();
   return nvmlDeviceCount;
}

const char* NVGPUMeter_gpuName(unsigned int gpuIndex) {
   if (gpuIndex >= nvmlDeviceCount)
      return NULL;
   return NVGPUMeter_engineData[gpuIndex].name;
}


/* ---- Shutdown: free GPU name strings and shut down NVML ---- */

void NVGPUMeter_shutdown(void) {
   if (nvmlInitialized) {
      nvmlShutdown();
      nvmlInitialized = false;
      for (unsigned int i = 0; i < nvmlDeviceCount; i++) {
         free(NVGPUMeter_engineData[i].name);
         NVGPUMeter_engineData[i].name = NULL;
      }
   }
}
#else

/* Stub when NVIDIA support is not enabled. The meter class is not registered
 * (see Platform_meterTypes[]), so these symbols are never used at runtime. */
NVGPUMeterInfo NVGPUMeter_engineData[NVGPU_MAX_GPUS];
const MeterClass NVGPUMeter_class = { 0 };
bool NVGPUMeter_active(void) { return false; }
unsigned int NVGPUMeter_detectGPUs(void) { return 0; }
const char* NVGPUMeter_gpuName(unsigned int gpuIndex) { (void)gpuIndex; return NULL; }

#endif
