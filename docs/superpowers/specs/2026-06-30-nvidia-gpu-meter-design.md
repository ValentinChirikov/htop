---
name: nvidia-gpu-meter-design
description: Design for adding NVIDIA GPU utilization and memory monitoring to htop via NVML
date: 2026-06-30
status: approved
---

# NVIDIA GPU Meter (NVGPUMeter) — Design Spec

## Overview

Add a new `NVGPUMeter` class to htop that reads GPU utilization and VRAM usage from NVIDIA GPUs via the NVML (NVIDIA Management Library). Each detected NVIDIA GPU gets its own meter instance, following the pattern of `CPUMeter` (one meter per core).

## Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Meter granularity | One meter per GPU | Best visibility, matches htop patterns for per-device meters |
| Metrics | Utilization % + VRAM usage % | Most useful numbers for process monitoring |
| Library loading | Runtime dlopen | No compile-time dependency; silent no-op when NVML unavailable |
| Display style | Stacked bars + text summary | Matches existing meter conventions |

## Architecture

### New files

- `linux/NVGPU.h` — declarations for NVML backend and meter class
- `linux/NVGPU.c` — NVML dlopen wrapper, data fetching, and meter implementation

No changes to existing files except adding the new meter to `Platform.c`'s meters array.

### Module responsibilities

| Module | Responsibility |
|--------|---------------|
| NVGPU backend functions | dlopen `libnvidia-ml.so.1`, wrap NVML calls, fetch utilization + memory per GPU |
| NVGPUMeter class | Meter interface: `updateValues`, `display`, one instance per detected GPU |
| Platform.c | Register `NVGPUMeter_class` in the meters array; call init/done at platform level to discover GPUs |

### Data flow

```
Platform_init() → NVGPUMeter_detectGPUs() → [dlopen, nvmlInit, nvmlDeviceGetCount]
                                              ↓
Platform_setValues() (every refresh) → NVGPUMeter_fetchValues(gpuIndex) → [nvmlDeviceGetProcessUtilization / nvmlDeviceGetMemoryInfo]
                                              ↓
Meter.updateValues() → fills this->values[] with utilization% and VRAM%
Meter.display()     → draws bars + text
```

## NVML Backend

### Library loading

Runtime dlopen of `libnvidia-ml.so.1`. Function pointers stored in a single static struct:

```c
typedef struct {
   void* handle;
   nvmlInit_t nvmlInit;
   nvmlShutdown_t nvmlShutdown;
   nvmlDeviceGetCount_t nvmlDeviceGetCount;
   nvmlDeviceGetName_t nvmlDeviceGetName;
   nvmlDeviceGetMemoryInfo_t nvmlDeviceGetMemoryInfo;
   nvmlDeviceGetProcessUtilization_t nvmlDeviceGetProcessUtilization;
} NvmlLibrary;
```

### GPU discovery

On first call, `NVGPUMeter_initLibrary()`:

1. dlopen `libnvidia-ml.so.1`, resolve all function pointers
2. Call `nvmlDeviceGetCount()` to get GPU count
3. For each GPU, call `nvmlDeviceGetName()` for the display name
4. Store results in a static array: `static NvmlGpuInfo nvmlGpus[8];` (cap at 8 GPUs)

### Utilization fetching

NVML's `nvmlDeviceGetProcessUtilization()` returns a snapshot of utilizations over a sampling period. Each refresh:

1. Call `nvmlDeviceGetProcessUtilization()` for each GPU with a recent sample period
2. Compare against last sample to get utilization % since last call
3. Call `nvmlDeviceGetMemoryInfo()` for total/used memory

### Shutdown

`nvmlShutdown()` called at `Platform_done()`.

## Meter Class

### Instance creation

One instance per GPU, created with `Meter_new(host, gpuIndex, &NVGPUMeter_class)` where `gpuIndex` is 0-based and stored in `this->param`.

### Values array (`maxItems = 2`)

| Index | Meaning | Range |
|-------|---------|-------|
| `values[0]` | GPU core utilization % | 0–100 |
| `values[1]` | VRAM usage % (used/total × 100) | 0–100 |

### Display format

Bar mode:
```
GPU 0: [████░░░░░░] 42.5%  Mem [███████░░░] 6.2Gi/8.0Gi (77%)
```

- Caption: `"GPU <index>"` (from NVML device name)
- First bar segment: core utilization, colored with `CPU_NORMAL` or new `GPU_UTIL` color
- Second bar segment: VRAM usage, colored with new `GPU_MEMORY` color
- Text summary: `"42.5%  Mem 6.2Gi/8.0Gi (77%)"`

### Meter modes

Support `BAR_METERMODE` (default), `TEXT_METERMODE`, and `CHART_METERMODE`.

### Naming

- `getUiName`: returns `"NVGPU <index>"` (e.g., `"NVGPU 0"`, `"NVGPU 1"`)
- `caption`: set from NVML device name in `init`

## Integration

### Platform registration

`NVGPUMeter_class` added to the Linux `Platform.c` meters array alongside `GPUMeter_class`, `CPUMeter_class`, etc.

### Meter discovery

In `Platform_init()` (linux):

1. Call `NVGPUMeter_detectGPUs()` — dlopen NVML, enumerate GPUs
2. If NVIDIA GPUs found, create one meter per GPU via `Meter_new()` and store in a static array
3. MetersPanel lists "NVGPU 0", "NVGPU 1", etc. as available meters

### Settings persistence

Users add NVGPU meters via the MetersPanel. Configuration persists in `~/.config/htop/htoprc` under `[Meter/ NVGPU 0]` sections. No new config options needed.

### Availability check

Static `NVGPUMeter_active()` returns true if any NVML meters exist, gating whether NVML queries are performed during refresh cycles.

## Error Handling & Edge Cases

| Scenario | Behavior |
|----------|----------|
| libnvidia-ml.so missing | dlopen fails → count = 0, no meters created, silent no-op |
| nvmlInit() fails | Same as above |
| GPU removed at runtime | Error on query → skip meter update for that cycle |
| First utilization call (no prior sample) | Return 0% utilization |
| Abnormal exit | Driver cleans up; no resource leak |
| Thread safety | Single-threaded main loop; no mutex needed |

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `linux/NVGPU.h` | New | NVML backend and meter class declarations |
| `linux/NVGPU.c` | New | NVML dlopen wrapper, data fetching, meter implementation |
| `linux/Platform.c` | Modify | Register `NVGPUMeter_class` in meters array |
| `linux/Platform.h` | Modify | Declare `NVGPUMeter_detectGPUs()` if needed |
