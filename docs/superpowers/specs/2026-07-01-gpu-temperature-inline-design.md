# GPU Temperature — inline display matching CPU temperature

Date: 2026-07-01
Status: Approved

## Goal

Make NVIDIA GPU temperature display and enablement mirror the existing CPU
temperature behavior: an optional inline appendix on the main GPU meter,
toggled by a Display-Options checkbox — while **keeping** the existing
standalone `NVGPUTemp` meter for users who want a dedicated bar/graph.

## Background

CPU temperature is not a separate meter. It is fetched into the CPU meter's
`values[CPU_METER_TEMPERATURE]` and rendered inline, appended to the existing
CPU meter, only when `settings->showCPUTemperature` is set. It respects the
`settings->degreeFahrenheit` toggle. Both settings and their Display-Options
checkboxes are compiled only under `#ifdef BUILD_WITH_CPU_TEMP`.

Our current GPU temperature is a standalone `NVGPUTemp` meter
(`NVGPUTempMeter_class`, registered in `linux/Platform.c`). Temperature is
already fetched for every GPU in `nvmlFetchValues()` and stored in
`NVGPUMeter_engineData[i].temperature` (and `.tempThreshold`).

## Design

### 1. New setting: `showGPUTemperature`

- `Settings.h`: add `bool showGPUTemperature;` under `#ifdef BUILD_WITH_NVIDIA`.
- `Settings.c`: parse `show_gpu_temperature` on read, write it, default to
  `false` — parallel to `showCPUTemperature`.

### 2. Display-Options checkbox

- `DisplayOptionsPanel.c`: add a "Also show GPU temperature" checkbox bound to
  `&settings->showGPUTemperature`, under `#ifdef BUILD_WITH_NVIDIA`, placed
  right after the CPU-temperature checkbox.

### 3. Widen the Fahrenheit guard

`degreeFahrenheit` (setting, read/write/default, and its checkbox) is currently
gated on `BUILD_WITH_CPU_TEMP` only. Widen to
`defined(BUILD_WITH_CPU_TEMP) || defined(BUILD_WITH_NVIDIA)` so GPU temperature
honors Fahrenheit even on a build without libsensors. Affected files:
`Settings.h`, `Settings.c` (read, write, default), `DisplayOptionsPanel.c`.

The existing `NVGPUTemp_useFahrenheit()` helper in `linux/NVGPU.c` has its
`#ifdef BUILD_WITH_CPU_TEMP` guard widened to the same combined condition.

### 4. Inline appendix on the main NVGPU meter

In `linux/NVGPU.c`, the main `NVGPUMeter` (utilization + memory) gains an
optional temperature appendix, gated by
`this->host->settings->showGPUTemperature` and availability of a temperature
reading (`NVGPUMeter_engineData[gpuIndex].temperature >= 0`):

- `NVGPUMeter_updateValues`: append ` NN°C` / ` NN°F` to `txtBuffer` after the
  Mem field.
- `NVGPUMeter_display`: append the temperature after the Mem field using
  `RichString_appendnWide` (so the multibyte `°` decodes), colored with
  `METER_TEXT` label + `METER_VALUE` value, matching `CPUMeter`.

No new `values[]` slot is added; temperature is read directly from
`NVGPUMeter_engineData`. Fetch logic is unchanged. The standalone
`NVGPUTemp` meter and its class are untouched.

## Non-goals

- No change to how temperatures are fetched from NVML.
- No removal of the standalone `NVGPUTemp` meter.
- No AMD/Intel GPU temperature work.

## Testing

htop has no unit-test harness for meters. Verification is manual: build clean,
run htop, add the NVGPU meter, toggle "Also show GPU temperature" and the
Fahrenheit checkbox, and confirm the inline temperature appears/disappears and
switches units on the 4090.
