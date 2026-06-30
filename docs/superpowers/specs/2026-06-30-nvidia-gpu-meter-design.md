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
| Library loading | Compile-time via `--enable-nvidia` | Simpler code, direct nvml.h headers, standard autoconf pattern like --enable-sensors |
| Display style | Stacked bars + text summary | Matches existing meter conventions |

## Architecture

### New files

- `linux/NVGPU.h` — declarations for NVML backend and meter class
- `linux/NVGPU.c` — NVML data fetching and meter implementation

### Modified files

- `configure.ac` — add `--enable-nvidia` option (pattern: sensors check, Linux-only)
- `linux/Platform.c` — register `NVGPUMeter_class` in meters array, include header, call nvmlShutdown at Platform_done

### Module responsibilities

| Module | Responsibility |
|--------|---------------|
| NVGPU backend functions | Direct NVML API calls (via nvml.h), fetch utilization + memory per GPU |
| NVGPUMeter class | Meter interface: `updateValues`, `display`, one instance per detected GPU |
| Platform.c | Register `NVGPUMeter_class` in the meters array; call nvmlInit/done at platform level |

### Data flow

```
configure.ac → --enable-nvidia → AC_CHECK_LIB + AC_CHECK_HEADERS → HAVE_NVML in config.h
                                                          ↓
Platform_init() → NVGPUMeter_detectGPUs() → [nvmlInit, nvmlDeviceGetCount]
                                              ↓
Platform_setValues() (every refresh) → NVGPUMeter_fetchValues(gpuIndex) → [nvmlDeviceGetProcessUtilization / nvmlDeviceGetMemoryInfo]
                                              ↓
Meter.updateValues() → fills this->values[] with utilization% and VRAM%
Meter.display()     → draws bars + text
```

## NVML Backend

### Library linkage

Linked at build time via `-lnvidia-ml` (when `--enable-nvidia` is set). Include `<nvml.h>` directly. Standard autoconf check pattern:

```
AC_CHECK_LIB([nvidia-ml], [nvmlInit], [], [enable_nvidia=no])
AC_CHECK_HEADERS([nvml.h], [], [enable_nvidia=no])
```

Gated by `my_htop_platform = linux` (NVML is Linux-only).

Define `BUILD_WITH_NVIDIA` preprocessor macro when enabled.

### GPU discovery

On first call, `NVGPUMeter_initLibrary()`:

1. Call `nvmlInit()` to initialize NVML
2. Call `nvmlDeviceGetCount()` to get GPU count
3. For each GPU, call `nvmlDeviceGetHandleByIndex(i, &handle)` and `nvmlDeviceGetName(handle, name, sizeof(name))` for the display name
4. Store results in a static array: `static nvmlDevice_t nvmlDevices[8];` (cap at 8 GPUs)

### Utilization fetching

NVML's `nvmlDeviceGetProcessUtilization()` returns a snapshot of utilizations over a sampling period. Each refresh:

1. Call `nvmlDeviceGetProcessUtilization()` for each GPU with a recent sample period
2. Compare against last sample to get utilization % since last call
3. Call `nvmlDeviceGetMemoryInfo(handle, &memInfo)` for total/used memory

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
Tesla V100: [████░░░░░░] 42.5% Mem 6144MiB 76.8%
```

- Caption: NVML device name (e.g., `"Tesla V100-SXM2-16GB"`)
- First bar segment: core utilization, colored with `GPU_ENGINE_1` (green)
- Second bar segment: VRAM usage, colored with `GPU_ENGINE_2` (yellow)
- Text summary: `"42.5% Mem 6144MiB 76.8%"`

### Meter modes

Support `BAR_METERMODE` (default), `TEXT_METERMODE`, and `CHART_METERMODE`.

### Naming

- `getUiName`: returns `"NVGPU <index>"` (e.g., `"NVGPU 0"`, `"NVGPU 1"`)
- `caption`: set from NVML device name in `init`

## Integration

### Platform registration

`NVGPUMeter_class` added to the Linux `Platform.c` meters array alongside `GPUMeter_class`, `CPUMeter_class`, etc. Wrapped in `#ifdef BUILD_WITH_NVIDIA`.

### Meter discovery

In `Platform_init()` (linux):

1. Call `NVGPUMeter_detectGPUs()` — nvmlInit, enumerate GPUs
2. If NVIDIA GPUs found, create one meter per GPU via `Meter_new()` and store in a static array
3. MetersPanel lists "NVGPU 0", "NVGPU 1", etc. as available meters

### Settings persistence

Users add NVGPU meters via the MetersPanel. Configuration persists in `~/.config/htop/htoprc` under `[Meter/ NVGPU 0]` sections. No new config options needed.

### Availability check

Static `NVGPUMeter_active()` returns true if any NVML meters exist, gating whether NVML queries are performed during refresh cycles.

## Error Handling & Edge Cases

| Scenario | Behavior |
|----------|----------|
| nvmlInit() fails | No GPUs detected, no meters created, silent no-op |
| GPU removed at runtime | NVML error on query → skip meter update for that cycle |
| First utilization call (no prior sample) | Return 0% utilization |
| Abnormal exit | Driver cleans up; no resource leak |
| Thread safety | Single-threaded main loop; no mutex needed |

## Files Changed

| File | Action | Description |
|------|--------|-------------|
| `linux/NVGPU.h` | New | NVML backend and meter class declarations |
| `linux/NVGPU.c` | New | NVML data fetching, meter implementation |
| `configure.ac` | Modify | Add `--enable-nvidia` option (Linux-only) |
| `linux/Platform.c` | Modify | Register `NVGPUMeter_class`, nvmlInit/done calls |
