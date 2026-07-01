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
#include "Machine.h"
#include "Macros.h"
#include "Object.h"
#include "Platform.h"
#include "RichString.h"
#include "Settings.h"
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
      NVGPUMeter_engineData[i].powerUsage = -1.0;
      NVGPUMeter_engineData[i].powerLimit = -1.0;
      NVGPUMeter_engineData[i].temperature = -1.0;
      NVGPUMeter_engineData[i].tempThreshold = -1.0;

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

      /* nvmlDeviceGetPowerUsage / power limits report milliwatts. */
      unsigned int powerMilliW = 0;
      if (nvmlDeviceGetPowerUsage(dev, &powerMilliW) == 0) {
         NVGPUMeter_engineData[i].powerUsage = powerMilliW / 1000.0;
      }

      unsigned int limitMilliW = 0;
      if (nvmlDeviceGetEnforcedPowerLimit(dev, &limitMilliW) == 0 ||
          nvmlDeviceGetPowerManagementLimit(dev, &limitMilliW) == 0) {
         NVGPUMeter_engineData[i].powerLimit = limitMilliW / 1000.0;
      }

      /* nvmlDeviceGetTemperature reports the GPU die temperature in °C. */
      unsigned int tempC = 0;
      if (nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &tempC) == 0)
         NVGPUMeter_engineData[i].temperature = (double)tempC;

      unsigned int thresholdC = 0;
      if (nvmlDeviceGetTemperatureThreshold(dev, NVML_TEMPERATURE_THRESHOLD_SLOWDOWN, &thresholdC) == 0)
         NVGPUMeter_engineData[i].tempThreshold = (double)thresholdC;
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


/* ---- Power meter: default bar total (Watts) when the GPU reports no limit ---- */

#define NVGPU_POWER_DEFAULT_TOTAL 300.0


/* ---- Power meter: updateValues ---- */

static void NVGPUPowerMeter_updateValues(Meter* this) {
   unsigned int gpuIndex = this->param - 1; /* param is 1-based (param 0 is legacy/never created by the UI) */

   if (this->param == 0 || !nvmlInitLibrary() || gpuIndex >= nvmlDeviceCount || !nvmlDevices[gpuIndex]) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      this->values[0] = NAN;
      return;
   }

   nvmlFetchValues();

   double power = NVGPUMeter_engineData[gpuIndex].powerUsage;
   if (power < 0.0) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      this->values[0] = NAN;
      return;
   }

   double limit = NVGPUMeter_engineData[gpuIndex].powerLimit;
   this->total = (limit > 0.0) ? limit : NVGPU_POWER_DEFAULT_TOTAL;
   this->values[0] = power;

   if (limit > 0.0)
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0fW / %.0fW", power, limit);
   else
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0fW", power);
}


/* ---- Power meter: display ---- */

static void NVGPUPowerMeter_display(const Object* cast, RichString* out) {
   const Meter* this = (const Meter*)cast;

   /* param is 1-based (param 0 is reserved/legacy and never created by the UI). */
   unsigned int gpuIndex = this->param - 1;
   if (this->param == 0 || gpuIndex >= nvmlDeviceCount || !isNonnegative(this->values[0])) {
      RichString_appendAscii(out, CRT_colors[METER_SHADOW], " N/A");
      return;
   }

   char buffer[32];
   int written;

   RichString_appendAscii(out, CRT_colors[METER_TEXT], ":");
   written = xSnprintf(buffer, sizeof(buffer), "%.0fW", this->values[0]);
   RichString_appendnAscii(out, CRT_colors[GPU_ENGINE_1], buffer, written);

   double limit = NVGPUMeter_engineData[gpuIndex].powerLimit;
   if (limit > 0.0) {
      written = xSnprintf(buffer, sizeof(buffer), " / %.0fW", limit);
      RichString_appendnAscii(out, CRT_colors[METER_TEXT], buffer, written);
   }
}


/* ---- Power meter: init ---- */

static void NVGPUPowerMeter_init(Meter* this) {
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
   Meter_setCaption(this, "GPU Power");
}


/* ---- Power meter: getUiName ---- */

static void NVGPUPowerMeter_getUiName(const Meter* this, char* buffer, size_t length) {
   assert(length > 0);
   if (this->param > 0)
      xSnprintf(buffer, length, "NVGPU Power %u", this->param - 1);
   else
      xSnprintf(buffer, length, "NVGPU Power");
}


/* ---- Power meter class definition ---- */

const MeterClass NVGPUPowerMeter_class = {
   .super = {
      .extends = Class(Meter),
      .delete = Meter_delete,
      .display = NVGPUPowerMeter_display,
   },
   .init = NVGPUPowerMeter_init,
   .done = NVGPUMeter_done,
   .getUiName = NVGPUPowerMeter_getUiName,
   .updateValues = NVGPUPowerMeter_updateValues,
   .defaultMode = BAR_METERMODE,
   .supportedModes = METERMODE_DEFAULT_SUPPORTED,
   .maxItems = 1,
   .total = NVGPU_POWER_DEFAULT_TOTAL,
   .attributes = (const int[]) { GPU_ENGINE_1 },
   .name = "NVGPUPower",
   .uiName = "NVGPU Power",
   .caption = "GPU Power"
};


/* ---- Temperature meter: default bar total (°C) when the GPU reports no slowdown threshold ---- */

#define NVGPU_TEMP_DEFAULT_TOTAL 100.0


/* Respect the global degree-Fahrenheit setting when CPU temperature support is compiled in
 * (the setting only exists then); otherwise report temperatures in Celsius. */
static bool NVGPUTemp_useFahrenheit(const Meter* this) {
#ifdef BUILD_WITH_CPU_TEMP
   return this->host->settings->degreeFahrenheit;
#else
   (void)this;
   return false;
#endif
}


/* ---- Temperature meter: updateValues ---- */

static void NVGPUTempMeter_updateValues(Meter* this) {
   unsigned int gpuIndex = this->param - 1; /* param is 1-based (param 0 is legacy/never created by the UI) */

   if (this->param == 0 || !nvmlInitLibrary() || gpuIndex >= nvmlDeviceCount || !nvmlDevices[gpuIndex]) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      this->values[0] = NAN;
      return;
   }

   nvmlFetchValues();

   double temp = NVGPUMeter_engineData[gpuIndex].temperature;
   if (temp < 0.0) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      this->values[0] = NAN;
      return;
   }

   double threshold = NVGPUMeter_engineData[gpuIndex].tempThreshold;
   /* Scale the bar against the slowdown threshold so a fuller bar warns of impending throttling. */
   this->total = (threshold > 0.0) ? threshold : NVGPU_TEMP_DEFAULT_TOTAL;
   this->values[0] = temp;

   bool fahrenheit = NVGPUTemp_useFahrenheit(this);
   if (threshold > 0.0) {
      if (fahrenheit)
         xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0f%sF / %.0f%sF", temp * 9 / 5 + 32, CRT_degreeSign, threshold * 9 / 5 + 32, CRT_degreeSign);
      else
         xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0f%sC / %.0f%sC", temp, CRT_degreeSign, threshold, CRT_degreeSign);
   } else if (fahrenheit) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0f%sF", temp * 9 / 5 + 32, CRT_degreeSign);
   } else {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0f%sC", temp, CRT_degreeSign);
   }
}


/* ---- Temperature meter: display ---- */

static void NVGPUTempMeter_display(const Object* cast, RichString* out) {
   const Meter* this = (const Meter*)cast;

   /* param is 1-based (param 0 is reserved/legacy and never created by the UI). */
   unsigned int gpuIndex = this->param - 1;
   if (this->param == 0 || gpuIndex >= nvmlDeviceCount || !isNonnegative(this->values[0])) {
      RichString_appendAscii(out, CRT_colors[METER_SHADOW], " N/A");
      return;
   }

   char buffer[32];
   int written;
   double temp = this->values[0];
   bool fahrenheit = NVGPUTemp_useFahrenheit(this);

   RichString_appendAscii(out, CRT_colors[METER_TEXT], ":");
   if (fahrenheit)
      written = xSnprintf(buffer, sizeof(buffer), "%.0f%sF", temp * 9 / 5 + 32, CRT_degreeSign);
   else
      written = xSnprintf(buffer, sizeof(buffer), "%.0f%sC", temp, CRT_degreeSign);
   /* CRT_degreeSign is multibyte (°); use the wide append so the glyph decodes, as CPUMeter does. */
   RichString_appendnWide(out, CRT_colors[METER_VALUE], buffer, written);

   double threshold = NVGPUMeter_engineData[gpuIndex].tempThreshold;
   if (threshold > 0.0) {
      if (fahrenheit)
         written = xSnprintf(buffer, sizeof(buffer), " / %.0f%sF", threshold * 9 / 5 + 32, CRT_degreeSign);
      else
         written = xSnprintf(buffer, sizeof(buffer), " / %.0f%sC", threshold, CRT_degreeSign);
      RichString_appendnWide(out, CRT_colors[METER_TEXT], buffer, written);
   }
}


/* ---- Temperature meter: init ---- */

static void NVGPUTempMeter_init(Meter* this) {
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
   Meter_setCaption(this, "GPU Temp");
}


/* ---- Temperature meter: getUiName ---- */

static void NVGPUTempMeter_getUiName(const Meter* this, char* buffer, size_t length) {
   assert(length > 0);
   if (this->param > 0)
      xSnprintf(buffer, length, "NVGPU Temp %u", this->param - 1);
   else
      xSnprintf(buffer, length, "NVGPU Temp");
}


/* ---- Temperature meter class definition ---- */

const MeterClass NVGPUTempMeter_class = {
   .super = {
      .extends = Class(Meter),
      .delete = Meter_delete,
      .display = NVGPUTempMeter_display,
   },
   .init = NVGPUTempMeter_init,
   .done = NVGPUMeter_done,
   .getUiName = NVGPUTempMeter_getUiName,
   .updateValues = NVGPUTempMeter_updateValues,
   .defaultMode = BAR_METERMODE,
   .supportedModes = METERMODE_DEFAULT_SUPPORTED,
   .maxItems = 1,
   .total = NVGPU_TEMP_DEFAULT_TOTAL,
   .attributes = (const int[]) { GPU_ENGINE_1 },
   .name = "NVGPUTemp",
   .uiName = "NVGPU Temp",
   .caption = "GPU Temp"
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
const MeterClass NVGPUPowerMeter_class = { 0 };
const MeterClass NVGPUTempMeter_class = { 0 };
bool NVGPUMeter_active(void) { return false; }
unsigned int NVGPUMeter_detectGPUs(void) { return 0; }
const char* NVGPUMeter_gpuName(unsigned int gpuIndex) { (void)gpuIndex; return NULL; }

#endif
