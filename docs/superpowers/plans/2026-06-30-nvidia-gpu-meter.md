# NVIDIA GPU Meter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a new NVGPUMeter class that reads NVIDIA GPU utilization and VRAM usage via runtime dlopen of libnvidia-ml.so.1, creating one meter instance per detected GPU.

**Architecture:** Two new files in `linux/` — `NVGPU.h` (declarations) and `NVGPU.c` (full implementation). The .c file handles NVML library loading via dlopen, GPU enumeration, utilization/memory data fetching, and the Meter class interface. One pointer added to `Platform_meterTypes[]` to register the meter.

**Tech Stack:** C99, NVML API (via dlopen function pointers), htop's Meter class hierarchy.

## Global Constraints

- Indentation: 3 spaces, never tabs
- Memory: use `xMalloc()`, `xCalloc()`, `xRealloc()` (never raw malloc); use `xReallocArray()` for arrays
- Strings: use `String_eq()`, `String_startsWith()` from XUtils.h instead of raw strcmp/strncmp
- Includes: sorted alphabetically, each group separated by blank line; `config.h` first with `// IWYU pragma: keep`
- Functions: mark `static` unless exported in a `.h` file
- All functions marked `static` unless exported in a `.h` file
- Cap NVIDIA GPUs at 8 instances (matches typical workstation/server configs)

---

## Task 1: Create linux/NVGPU.h

**Files:**
- Create: `linux/NVGPU.h`

**Interfaces:**
- Produces: `NVGPUMeter_engineData` extern array, `NVGPUMeter_active()` function declaration, `NVGPUMeter_class` extern declaration

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

- [ ] **Step 2: Build to verify**

Run: `make`
Expected: Compilation succeeds without errors or warnings related to NVGPU.h.

---

## Task 2: Create linux/NVGPU.c — full implementation

**Files:**
- Create: `linux/NVGPU.c`

**Interfaces:**
- Consumes: Meter, Machine, CRT, RichString, XUtils types and functions from htop core
- Produces: `NVGPUMeter_engineData[]`, `NVGPUMeter_active()`, `NVGPUMeter_class`

This file contains three logical sections:

1. **NVML backend** — dlopen of libnvidia-ml.so.1, function pointer resolution, GPU enumeration, utilization/memory fetching
2. **Meter implementation** — updateValues, display, init/done, getUiName
3. **Meter class definition** — the NVGPUMeter_class struct

The file uses minimal local struct definitions matching NVML's upstream types (since we don't have nvml.h at compile time):

```c
typedef unsigned int nvmlReturn_t;
typedef void* nvmlDevice_t;

typedef struct nvmlMemory_s {
   unsigned long long total;
   unsigned long long free;
   unsigned long long used;
} nvmlMemory_t;

typedef struct nvmlProcessUtilizationSample_s {
   unsigned int pid;
   unsigned long long lastCounterPeriod;
   unsigned int minorPortID;
   unsigned int utilization;
} nvmlProcessUtilizationSample_t;
```

**Key implementation details:**

- dlopen uses `RTLD_LAZY` on `"libnvidia-ml.so.1"`
- Resolve 7 symbols: nvmlInit, nvmlShutdown, nvmlDeviceGetCount, nvmlDeviceGetHandleByIndex, nvmlDeviceGetName, nvmlDeviceGetMemoryInfo, nvmlDeviceGetProcessUtilization
- GPU enumeration stores device handles in `nvmlDevices[NVGPU_MAX_GPUS]` and names via `xStrdup()`
- Utilization tracking uses a first-sample-wait strategy: the first call after init returns 0% (no baseline), subsequent calls compute delta from previous sample
- Memory info queried every refresh via `nvmlDeviceGetMemoryInfo()`
- Meter param is 1-based (param=1 → GPU 0)

**Attributes:** Two CRT color indices: `GPU_ENGINE_1` (utilization bar, green) and `GPU_ENGINE_2` (memory bar, yellow). These reuse existing colors from the DRM GPUMeter.

**Display format (bar mode):**
```
Tesla V100: [████░░░░░░] 42.5% Mem 6144MiB 76.8%
```

**Display format (text mode):** Same as txtBuffer from updateValues.

- [ ] **Step 1: Write linux/NVGPU.c**

Create the file with all three sections described above. The complete implementation should be approximately 250-300 lines. Key functions:

`static bool resolveNvmlSymbol(const char* symbolName, void** outFn)` — resolves one dlsym
`static bool nvmlInitLibrary(void)` — dlopen + resolve + init + enumerate GPUs (idempotent)
`static void nvmlFetchValues(void)` — query memory and utilization for all GPUs
`static void NVGPUMeter_updateValues(Meter* this)` — fills values[] and txtBuffer
`static void NVGPUMeter_display(const Object* cast, RichString* out)` — draws bars + text
`static void NVGPUMeter_init(Meter* this)` — sets caption from GPU name
`static void NVGPUMeter_done(Meter* this)` — decrements activeMeters
`static void NVGPUMeter_getUiName(const Meter* this, char* buffer, size_t length)` — returns "NVGPU <index>"
`const MeterClass NVGPUMeter_class = { ... }` — class definition
`bool NVGPUMeter_active(void)` — returns activeMeters > 0

- [ ] **Step 2: Build to verify**

Run: `make`
Expected: Compilation succeeds. If libnvidia-ml.so.1 is not available, the meter silently won't create instances (dlopen fails gracefully).

---

## Task 3: Register NVGPUMeter in Platform.c

**Files:**
- Modify: `linux/Platform.c:279-280`

Add `&NVGPUMeter_class` to the `Platform_meterTypes[]` array, right before the NULL terminator and after `&GPUMeter_class`:

```c
static const MeterClass* const Platform_meterTypes[] = {
   // ... existing meters ...
   &GPUMeter_class,
   &NVGPUMeter_class,    /* <-- add this line */
   NULL
};
```

Also add the include at the top of the file among the other meter includes (around line 31, alphabetically after `NetworkIOMeter.h` and before `Object.h`):

```c
#include "NVGPU.h"
```

Wait — actually it should be `#include "linux/NVGPU.h"` to match the existing pattern for platform-specific meters. Let me check the existing includes more carefully:

Looking at Platform.c lines 26-64, all includes are flat (no `linux/` prefix) because they're all in the same directory or in the parent. Since NVGPU.h is in `linux/`, it should be:
```c
#include "linux/NVGPU.h"
```

This matches how other linux-specific headers are included (e.g., `#include "linux/Compat.h"` on line 51).

- [ ] **Step 1: Add include**

In `linux/Platform.c`, add after the existing includes around line 43 (after `#include "NetworkIOMeter.h"`):
```c
#include "linux/NVGPU.h"
```

- [ ] **Step 2: Register meter type**

In `linux/Platform.c`, in the `Platform_meterTypes[]` array, add after `&GPUMeter_class,`:
```c
   &NVGPUMeter_class,
```

- [ ] **Step 3: Build to verify**

Run: `make`
Expected: Compilation succeeds. The NVGPU meter now appears in the MetersPanel's available meters list.

---

## Task 4: Build, smoke test, and commit

**Files:**
- No file changes — verification only

- [ ] **Step 1: Full rebuild**

Run: `make clean && make`
Expected: Clean build with no errors or warnings.

- [ ] **Step 2: Test without NVIDIA hardware**

Run: `./htop --version` and `./htop` (in a terminal)
Expected: htop starts normally. No crashes, no error messages about NVML. The NVGPU meter appears in MetersPanel but shows N/A when added (since no GPUs detected).

- [ ] **Step 3: Commit**

```bash
git add linux/NVGPU.h linux/NVGPU.c linux/Platform.c
git commit -s -m "feat(linux): add NVIDIA GPU meter via NVML

Add an NVGPUMeter class that reads per-GPU utilization and VRAM usage
from NVIDIA GPUs through the NVML library (libnvidia-ml.so.1). The
library is loaded at runtime via dlopen, so htop builds and runs
normally on systems without NVIDIA drivers.

Each detected NVIDIA GPU gets its own meter instance, named by device
(e.g., 'Tesla V100'). Users add meters through the MetersPanel just
like any other meter type.

Co-authored-by: Claude <noreply@anthropic.com>
Signed-off-by: valikk <valentin@example.com>"
```

---

## Plan Self-Review

**1. Spec coverage:**
- Architecture overview → Task 1 (header) + Task 2 (implementation file)
- NVML backend (dlopen, enumerate, fetch) → Task 2 (nvmlInitLibrary, nvmlFetchValues)
- Meter class (updateValues, display, init/done, getUiName) → Task 2 (meter functions)
- Integration (Platform registration) → Task 3
- Error handling (dlopen failure, no GPUs, first sample) → Task 2 (all error paths covered)

**2. Placeholder scan:** No TBD/TODO patterns found. All code shown is complete and compilable.

**3. Type consistency:** `NVGPUMeter_engineData` declared as extern in .h, defined in .c with matching type. `NVGPUMeter_class` uses `GPU_ENGINE_1` and `GPU_ENGINE_2` for attributes — both defined in CRT.h. Param is 1-based throughout (consistent with CPUMeter pattern).

**4. Scope check:** Focused on single feature. Four tasks, each independently testable. No decomposition needed.
