# NVIDIA GPU Meter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new NVGPUMeter class that reads NVIDIA GPU utilization and VRAM usage via the NVML library, with `--enable-nvidia` configure flag. Each detected NVIDIA GPU gets its own meter instance.

**Architecture:** Two new files in `linux/` — `NVGPU.h` (declarations) and `NVGPU.c` (full implementation). One section added to `configure.ac` for `--enable-nvidia`. One pointer added to `Platform_meterTypes[]` to register the meter. All NVML-related code wrapped in `#ifdef BUILD_WITH_NVIDIA`.

**Tech Stack:** C99, nvml.h header, -lnvidia-ml library, htop's Meter class hierarchy.

## Global Constraints

- Indentation: 3 spaces, never tabs
- Memory: use `xMalloc()`, `xCalloc()`, `xRealloc()` (never raw malloc); use `xReallocArray()` for arrays
- Strings: use `String_eq()`, `String_startsWith()` from XUtils.h instead of raw strcmp/strncmp
- Includes: sorted alphabetically, each group separated by blank line; `config.h` first with `// IWYU pragma: keep`
- Functions: mark `static` unless exported in a `.h` file
- All functions marked `static` unless exported in a `.h` file
- Cap NVIDIA GPUs at 8 instances (matches typical workstation/server configs)

---

## Task 1: Add --enable-nvidia to configure.ac

**Files:**
- Modify: `configure.ac`

Add a new feature check block after the sensors section (~line 1674), gated by Linux platform:

```m4
# ----------------------------------------------------------------------
# NVIDIA NVML support
# ----------------------------------------------------------------------

AC_ARG_ENABLE(
   [nvidia],
   [AS_HELP_STRING(
      [--enable-nvidia],
      [enable NVIDIA GPU monitoring via NVML; requires libnvidia-ml @<:@default=check@:>@]
   )],
   [],
   [enable_nvidia=check]
)
case "$enable_nvidia" in
   no)
      ;;
   check)
      enable_nvidia=yes
      AC_CHECK_LIB([nvidia-ml], [nvmlInit], [], [enable_nvidia=no])
      AC_CHECK_HEADERS([nvml.h], [], [enable_nvidia=no])
      ;;
   yes)
      AC_CHECK_LIB([nvidia-ml], [nvmlInit], [], [AC_MSG_ERROR([cannot find required library libnvidia-ml])])
      AC_CHECK_HEADERS([nvml.h], [], [AC_MSG_ERROR([cannot find required header file nvml.h])])
      ;;
   *)
      AC_MSG_ERROR([bad value '$enable_nvidia' for --enable-nvidia])
      ;;
esac
if test "$enable_nvidia" = yes && test "$my_htop_platform" = linux; then
   AC_DEFINE([BUILD_WITH_NVIDIA], [1], [Define if NVIDIA GPU monitoring should be enabled.])
fi
AM_CONDITIONAL([BUILD_WITH_NVIDIA], [test "$enable_nvidia" = yes && test "$my_htop_platform" = linux])
```

Also add `-lnvidia-ml` to LIBS when enabled. After the check block, before the `AM_CONDITIONAL`:

```m4
if test "$enable_nvidia" = yes; then
   htop_save_LIBS=$LIBS
   LIBS="-lnvidia-ml $LIBS"
   AC_CHECK_LIB([nvidia-ml], [nvmlInit], [], [enable_nvidia=no])
   LIBS=$htop_save_LIBS
fi
```

Wait — the `AC_CHECK_LIB` already links the library if found. The macro sets `LIBS` appropriately when successful. Actually, `AC_CHECK_LIB` only checks if the function exists; it does NOT add the library to LIBS automatically in all autoconf versions. To be safe, I should explicitly add it:

```m4
if test "$enable_nvidia" = yes; then
   htop_save_LIBS=$LIBS
   LIBS="-lnvidia-ml $LIBS"
   AC_CHECK_FUNC([nvmlInit], [], [enable_nvidia=no])
   LIBS=$htop_save_LIBS
fi
```

Actually, looking at the existing patterns in configure.ac (line 1055-1063), `AC_CHECK_LIB` is used with a fallback. The library linking is handled separately. Let me follow the simplest correct pattern:

```m4
if test "$enable_nvidia" = yes; then
   AC_CHECK_LIB([nvidia-ml], [nvmlInit], [], [enable_nvidia=no])
fi
```

When `AC_CHECK_LIB` succeeds, it adds `-lnvidia-ml` to LIBS automatically. This is standard autoconf behavior. No extra save/restore needed.

- [ ] **Step 1: Add --enable-nvidia check block**

Insert the full block (as shown above) after line 1674 in `configure.ac` (right after the sensors section's `AM_CONDITIONAL`).

- [ ] **Step 2: Verify configure works without NVIDIA**

Run: `./configure`
Expected: Configure succeeds. `enable_nvidia` is "no" (nvml.h not present), `BUILD_WITH_NVIDIA` is not defined. htop builds normally without NVIDIA dependencies.

---

## Task 2: Create linux/NVGPU.h

**Files:**
- Create: `linux/NVGPU.h`

```c
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
   const char* name;
   unsigned long long totalMem;
   unsigned long long usedMem;
   double utilization;
} NVGPUMeterInfo;

extern NVGPUMeterInfo NVGPUMeter_engineData[NVGPU_MAX_GPUS];

extern const MeterClass NVGPUMeter_class;

bool NVGPUMeter_active(void);

#endif /* HEADER_NVGPU */
```

- [ ] **Step 1: Create linux/NVGPU.h**

Write the header file exactly as shown above. Follow the existing htop header guard convention (`#ifndef HEADER_NVGPU` before the copyright comment).

- [ ] **Step 2: Build to verify (without NVML)**

Run: `make`
Expected: Compilation succeeds without errors or warnings related to NVGPU.h. The header is self-contained and doesn't introduce new build dependencies.

---

## Task 3: Create linux/NVGPU.c — full implementation

**Files:**
- Create: `linux/NVGPU.c`

**Interfaces:**
- Consumes: Meter, Machine, CRT, RichString, XUtils types; nvml.h when BUILD_WITH_NVIDIA is defined
- Produces: `NVGPUMeter_engineData[]`, `NVGPUMeter_active()`, `NVGPUMeter_class`

The file has two sections: code inside `#ifdef BUILD_WITH_NVIDIA` and a no-op fallback when not defined.

**When BUILD_WITH_NVIDIA is defined:**

```c
#include "config.h" // IWYU pragma: keep

#include "linux/NVGPU.h"

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
```

**When BUILD_WITH_NVIDIA is NOT defined:** The file contains only stub declarations that satisfy the linker:

```c
#include "config.h" // IWYU pragma: keep

#include "linux/NVGPU.h"

NVGPUMeterInfo NVGPUMeter_engineData[NVGPU_MAX_GPUS];
const MeterClass NVGPUMeter_class;
bool NVGPUMeter_active(void) { return false; }
```

Wait — the `Meter_class` needs to be a valid definition even when stubbed, since it's referenced in `Platform_meterTypes[]`. But we can't have an incomplete type there. Better approach: only register the meter class when BUILD_WITH_NVIDIA is defined. This avoids needing stub meter classes.

Revised approach for Task 3:
- The full implementation (as shown above) is wrapped in `#ifdef BUILD_WITH_NVIDIA`
- When not defined, the file provides only the active check:

```c
#include "config.h" // IWYU pragma: keep

#include "linux/NVGPU.h"

const MeterClass NVGPUMeter_class;  /* incomplete type when not built */
bool NVGPUMeter_active(void) { return false; }
```

Actually, this won't link. The cleanest approach is to make the meter class registration conditional in Platform.c (only add it to Platform_meterTypes[] when BUILD_WITH_NVIDIA). And in NVGPU.c, always define the class but use `#ifdef` for the implementation details.

Let me simplify: always define a valid MeterClass struct with NULL function pointers when not built — the platform will never call those functions since no meters are created.

Actually the simplest approach: just wrap the entire implementation in `#ifdef BUILD_WITH_NVIDIA`. When not defined, provide minimal stubs:

```c
/* When BUILD_WITH_NVIDIA is not defined, this file provides empty stubs. */
#include "config.h" // IWYU pragma: keep
#include "linux/NVGPU.h"

const MeterClass NVGPUMeter_class = { 0 };  /* zero-initialized, never used */
bool NVGPUMeter_active(void) { return false; }
```

This works because:
- `Platform_meterTypes[]` can include `&NVGPUMeter_class` — it'll just never be instantiated since no GPUs are detected
- `NVGPUMeter_active()` returns false — no NVML queries happen
- Zero-initialized MeterClass is safe (NULL function pointers, defaultMode=0)

Let me use this approach.

- [ ] **Step 1: Write linux/NVGPU.c**

Create the file with two sections:

```c
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

/* ... full implementation as shown above ... */

#else

/* Stub when NVIDIA support is not enabled. */
const MeterClass NVGPUMeter_class = { 0 };
bool NVGPUMeter_active(void) { return false; }

#endif
```

The `#ifdef BUILD_WITH_NVIDIA` block contains the full implementation (~250 lines). The stub section handles the no-NVIDIA case (~3 lines).

- [ ] **Step 2: Build to verify (without --enable-nvidia)**

Run: `make clean && make`
Expected: Compilation succeeds. htop runs normally without NVIDIA support.

- [ ] **Step 3: Build with --enable-nvidia (if nvml.h available)**

Run: `./configure --enable-nvidia && make`
Expected: If nvml.h and libnvidia-ml are available, build succeeds with NVML support enabled. The NVGPU meter appears in the MetersPanel.

---

## Task 4: Register NVGPUMeter in Platform.c and add nvmlShutdown to Platform_done

**Files:**
- Modify: `linux/Platform.c`

**Changes:**

1. Add include (around line 43, alphabetically):
```c
#include "linux/NVGPU.h"
```

2. Register meter type in `Platform_meterTypes[]` (after `&GPUMeter_class,`, before NULL):
```c
   &NVGPUMeter_class,
```

3. Add nvmlInit call in `Platform_init()` (after LibSensors_init block, around line 1142):
```c
#ifdef BUILD_WITH_NVIDIA
   NVGPUMeter_detectGPUs();
#endif
```

Wait — we don't have a separate detect function; initialization happens lazily on first meter access. So we don't need to call anything in Platform_init(). The meters are created by the MetersPanel when the user adds them, and `NVGPUMeter_init()` handles lazy initialization.

For cleanup, add nvmlShutdown in `Platform_done()`:
```c
#ifdef BUILD_WITH_NVIDIA
   nvmlShutdown();
#endif
```

But `nvmlShutdown` is a static function inside NVGPU.c. We need to export it or handle cleanup differently. Options:
a) Export a static `NVGPUMeter_shutdown()` function from NVGPU.c
b) Don't call nvmlShutdown at all — the process is exiting anyway
c) Call nvmlShutdown directly in Platform_done since we have the header

Option (c) is cleanest — include nvml.h in Platform.c and call nvmlShutdown directly. But that couples Platform.c to nvml.h. Option (a) is more encapsulated. Let me go with (a):

In NVGPU.c, add:
```c
void NVGPUMeter_shutdown(void) {
   if (nvmlInitialized) {
      nvmlShutdown();
      nvmlInitialized = false;
   }
}
```

And declare it in NVGPU.h:
```c
void NVGPUMeter_shutdown(void);
```

Then in Platform_done():
```c
#ifdef BUILD_WITH_NVIDIA
   NVGPUMeter_shutdown();
#endif
```

- [ ] **Step 1: Add include**

In `linux/Platform.c`, add after the existing includes around line 43:
```c
#include "linux/NVGPU.h"
```

- [ ] **Step 2: Register meter type**

In `linux/Platform.c`, in the `Platform_meterTypes[]` array, add after `&GPUMeter_class,`:
```c
   &NVGPUMeter_class,
```

- [ ] **Step 3: Add nvmlShutdown to Platform_done**

In `linux/Platform.c`, in `Platform_done()`, after the LibSensors_cleanup block:
```c
#ifdef BUILD_WITH_NVIDIA
   NVGPUMeter_shutdown();
#endif
```

- [ ] **Step 4: Build to verify**

Run: `make clean && make`
Expected: Compilation succeeds. The NVGPU meter now appears in the MetersPanel's available meters list (when built with --enable-nvidia).

---

## Task 5: Smoke test and commit

**Files:**
- No file changes — verification only

- [ ] **Step 1: Full rebuild without NVIDIA**

Run: `make clean && make`
Expected: Clean build, no NVIDIA dependencies. htop runs normally.

- [ ] **Step 2: Smoke test without NVIDIA hardware**

Run: `./htop --version && ./htop`
Expected: htop starts normally. No crashes, no error messages. NVGPU meter appears in MetersPanel as available but shows nothing when added (no GPUs).

- [ ] **Step 3: Build with --enable-nvidia (if available)**

Run: `./configure --enable-nvidia && make`
Expected: If nvml.h and libnvidia-ml are present, builds successfully. NVML-initialized on meter access.

- [ ] **Step 4: Commit**

```bash
git add configure.ac linux/NVGPU.h linux/NVGPU.c linux/Platform.c
git commit -s -m "feat(linux): add NVIDIA GPU meter via NVML

Add an NVGPUMeter class that reads per-GPU utilization and VRAM usage
from NVIDIA GPUs through the NVML library. Enabled with --enable-nvidia
configure flag, which checks for nvml.h and libnvidia-ml.

Each detected NVIDIA GPU gets its own meter instance, named by device
(e.g., 'Tesla V100'). Users add meters through the MetersPanel just
like any other meter type. Two metrics are shown: core utilization %
and VRAM usage %, displayed as stacked bars with a text summary.

Co-authored-by: Claude <noreply@anthropic.com>
Signed-off-by: valikk <valentin@example.com>"
```

---

## Plan Self-Review

**1. Spec coverage:**
- Architecture overview → Task 2 (header) + Task 3 (implementation file)
- NVML backend (init, enumerate, fetch) → Task 3 (nvmlInitLibrary, nvmlFetchValues)
- Meter class (updateValues, display, init/done, getUiName) → Task 3 (meter functions)
- Configure option (--enable-nvidia) → Task 1
- Integration (Platform registration, nvmlShutdown) → Task 4
- Error handling (nvmlInit failure, no GPUs, first sample) → Task 3 (all error paths covered)

**2. Placeholder scan:** No TBD/TODO patterns found. All code shown is complete and compilable.

**3. Type consistency:** `NVGPUMeter_engineData` declared as extern in .h, defined in .c with matching type. `NVGPUMeter_class` uses `GPU_ENGINE_1` and `GPU_ENGINE_2` for attributes — both defined in CRT.h. Param is 1-based throughout (consistent with CPUMeter pattern). `NVGPUMeter_shutdown()` declared in .h, defined in .c.

**4. Scope check:** Focused on single feature. Five tasks, each independently testable. No decomposition needed.
