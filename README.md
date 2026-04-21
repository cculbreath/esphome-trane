# esphome-trane

An [ESPHome](https://esphome.io) component for monitoring and controlling Trane HVAC systems that use the **ComfortLink II** communicating bus, enabling local control through Home Assistant without any cloud dependency.

Inspired by and structurally modeled after [esphome-econet](https://github.com/esphome-econet/esphome-econet).

> **Fork status (2026-04):** This fork rewrites the outbound CAN protocol against captured UX360 traffic. The upstream's transmit path (SpOverride Put, SystemMode Put) was non-functional — malformed framing caused the SC360 to silently discard commands and in some cases crash-reboot. This fork sends the correct wire format and accepts commands end-to-end (verified: UX360 display updates within 20 ms of an HA mode or setpoint change). See [`PROTOCOL.md`](PROTOCOL.md) for the full frame-level protocol description and [`docs/captures/`](docs/captures/) for reference traffic.

---

## Overview

Trane ComfortLink II systems communicate over a proprietary 50kbps CAN bus connecting the thermostat, SC360 zone controller, air handler, and outdoor unit. This project taps that bus to parse the JSON-encoded messages on CAN IDs `0x649` (system broadcast) and `0x641` (command/response), as well as raw float frames from the outdoor unit, exposing full system state to Home Assistant.

Confirmed working on:
- **Thermostat**: Trane UX360
- **Controller**: Trane SC360
- **System**: Trane ComfortLink II variable-speed heat pump with gas auxiliary heat

Other ComfortLink II systems using the SC360 controller are likely compatible — field reports welcome.

---

## Features

- **Climate entity** with heat, cool, heat_cool, fan_only, and off modes
- **Presets**: Home, Away, Sleep, Boost
- **Real-time sensors**: outdoor coil temp, compressor demand %, supply air temp, room temp, humidity, setpoints
- **System status**: operating mode, demand stage (HP Stage 1/2, ID Stage 1/2, HP2+ID1/2), run timer, active alarms
- **Fault detection**: captures `IndoorStatus.E` fault strings (e.g. `TA_INV_HI`) as a dedicated text sensor
- **Bidirectional control**: setpoint and mode commands transmitted back on `0x641`
- **Sub-device grouping** (ESPHome 2025.7.0+): entities appear under four logical devices in HA
  - Trane Thermostat UX360
  - Trane SC360 Controller
  - Trane Air Handler
  - Trane Heat Pump

---

## Hardware

### Required

| Component | Notes |
|---|---|
| M5Stack AtomS3 | ESP32-S3 based, tested and confirmed |
| M5Stack CAN bus module | Plugs directly onto the AtomS3 |
| 24VAC to 5VDC converter | Powers the ESP from the system's 24VAC supply |

Other ESP32-S3 boards with a CAN transceiver should work — adjust `tx_pin` and `rx_pin` in the YAML to match your wiring.

### CAN Bus Connection

Tap the four wires from the ComfortLink II bus at the SC360 or any accessible point in the daisy chain:

| Wire | Signal |
|---|---|
| 24VAC | System power (to 24VAC→5VDC converter) |
| H | CAN High |
| L | CAN Low |
| GND | Ground (common to 24VAC→5VDC converter) |

> ⚠️ The bus runs at **50kbps**. This is configured in the YAML — do not change it.

> ⚠️ Use a dedicated 24VAC→5VDC converter to power the ESP. Do not attempt to power it directly from the 24VAC supply without conversion.

### ESP32 CAN Pin Assignment (M5Stack AtomS3 + CAN module)

```yaml
tx_pin: GPIO5
rx_pin: GPIO6
bit_rate: 50kbps
```

---

## Installation

### 1. Clone or download this repo

```bash
git clone https://github.com/YOUR_USERNAME/esphome-trane.git
cd esphome-trane
```

### 2. Create your secrets file

```bash
cp secrets.yaml.example secrets.yaml
# Edit secrets.yaml with your Wi-Fi credentials
```

### 3. Flash

```bash
esphome run esphome-trane.yaml
```

Or adopt the device in the ESPHome dashboard and flash from there.

---

## Repository Structure

```
esphome-trane/
├── esphome-trane.yaml          # Main ESPHome configuration
├── secrets.yaml.example        # Template for Wi-Fi credentials
├── components/
│   └── trane_hvac/
│       ├── __init__.py         # Component registration
│       ├── climate.py          # ESPHome codegen for climate entity
│       ├── trane_climate.h     # Climate component header
│       └── trane_climate.cpp   # Climate component implementation
└── README.md
```

---

## Decoded CAN Bus Fields

### `0x649` — System Broadcast (SC360 → all)

| Object | Field | Decoded meaning |
|---|---|---|
| `SystemOpStatus` | `A` | System state: `A`=active, `E`=error/standby, `G`=coast-down |
| `SystemOpStatus` | `B` | System mode: `A`=heat, `B`=cool, `C`=off |
| `SystemOpStatus` | `C` | Demand stage: `--`, `HP Stage 1`, `HP Stage 2`, `HP1+ID1`, `HP1+ID2`, `HP2+ID1`, `HP2+ID2`, `ID Stage 1`, `ID Stage 2` |
| `SystemOpStatus` | `D` | Elapsed run time (minutes, resets each cycle) |
| `SystemOpStatus` | `E` (integer) | Indoor humidity (%) |
| `SystemOpStatus` | `E` (float, e.g. `28.00`) | SC360 outdoor ambient temp (°F) |
| `OdStatus` | `A` | Outdoor unit active flag (always `"A"` when running) |
| `OdStatus` | `B` | Compressor speed % (0–100, tracks CompDemandPercent at high load) |
| `OdStatus` | `C` | Outdoor unit state: `B`=running, `D`=off |
| `OdStatus` | `D` | Outdoor unit fault code (`"0"` = no fault) |
| `OdStatus` | `CompDemandPercent` | Compressor demand (%) |
| `OutdoorSettings` | `OdTempUserOffset` | Outdoor temp user calibration offset |
| `OutdoorSettings` | `A` | Outdoor settings flag |
| `IndoorStatus` | `D` | Blower state: `A`=transition, `B`=running |
| `IndoorStatus` | `E` | Supply air temp (°F) or fault string (e.g. `TA_INV_HI`) |
| `ZoneStatus` | `H` | Room temperature (°F) |
| `ZoneStatus` | `HcStatus` | Heat/cool call: `3`=call active, `4`=satisfied, `5`=elevated call, `7`=aux/emergency |
| `ZoneStatus` | `HoldText` | Schedule/hold text. Schedule names: `WAKE`, `DAY`, `EVENING`, `SLEEP`. Timed hold format: `"Holding Until Today HH:MM"` |
| `SpOverride` | `Hsp` / `Csp` | Heat / cool setpoints (°F) |
| `SpOverride` | `HoldType` | `0`=schedule, `2`=manual hold |
| `WeatherToday` | `D` | Current outdoor temp (°F) |
| `WeatherToday` | `E` | Current outdoor humidity (%) |
| `WeatherToday` | `F` | Feels-like temp (°F) |
| `WeatherToday` | `G` | Forecast high temp (°F) |
| `WeatherToday` | `H` | Weather condition code (observed values: `P`, `Q`, `AL`, `AX`, `AZ`, `BB`) |
| `WeatherToday` | `I` | Forecast low temp (°F) |
| `ActiveAlarms` | `AlarmId` | Fault code (e.g. `Err 185.09`) |
| `TechAppAccess` | `A` | Technician/app access flag (observed only) |
| `ScheduleCopy` | — | Schedule sync notification (observed only, payload empty) |

### `0x641` — Command/Response

| Object | Meaning |
|---|---|
| `SpOverride Put` | Set zone 1 heat/cool setpoints |
| `SystemMode Put` | Set system mode (A/B/C) |
| `GetProfile` / `RemoveProfile` | SC360 profile management (observed, not commanded) |
| `DebugUI` | SC360 heap diagnostics (observed, not commanded) |

### Float Frames

| CAN ID | Content |
|---|---|
| `0x283` | Indoor temps 1 & 2 (°C, two floats) |
| `0x308` | Supply air temp, heat exchanger temp (°F, two floats) |
| `0x380` | Outdoor air temp (°F, second float) |
| `0x381` | Suction temp (°F) |
| `0x383` | Discharge temp (°F) |
| `0x387` | Outdoor coil temp (°F) |
| `0x38F` | Refrigerant pressure (PSI) |
| `0x410` / `0x430` / `0x450` | SC360 temp sensors 1–3 (°F) |

---

## Operating State Mapping

Cross-referenced against the Trane Home app activity log:

| App state | CAN bus equivalent |
|---|---|
| heat | `HcStatus=3`, `SystemOpStatus.C` = `HP Stage 1/2` or `ID Stage 1/2` |
| cool | `HcStatus=3`, `SystemOpStatus.C` = `Cool Stage` (not yet observed) |
| defrost | `SystemOpStatus.C` = `HP2+ID1` or `HP2+ID2` |
| emergency heating | `HcStatus=7`, `SystemOpStatus.C` = `ID Stage 1/2` |
| fan | `SystemOpStatus.C` = `--`, `IndoorStatus.D` = `B` (blower running post-cycle) |
| idle | `HcStatus=4`, `SystemOpStatus.C` = `--` |
| waiting | `SystemOpStatus.A` = `E` (post-alarm standby/lockout) |

App "Run mode" maps to `SpOverride.HoldType`: permanent hold = `1` (what the UX360 emits when you tap a setpoint), schedule = `0`. (Upstream README said `2` for hold — empirically the SC360 ignores commands with `HoldType=2`.)

App "Compressor speed" maps directly to `OdStatus.B` (0–100%).

App "Outdoor temperature" maps to `SystemOpStatus.E` (SC360 ambient sensor, float).

---

## Climate Presets

Setpoints are derived from observed schedule data on a Trane UX360 / SC360 system:

| Preset | Hsp | Csp | Notes |
|---|---|---|---|
| Home | 70°F | 75°F | Typical occupied setpoints |
| Away | 62°F | 85°F | Matches observed zone 2–6 unoccupied values |
| Sleep | 67°F | 72°F | Matches UX360 SLEEP schedule transition |
| Boost | 74°F | 72°F | Raises heat target to encourage aux/gas staging |

Adjust these in `esphome-trane.yaml` under `set_preset_action` to match your schedule.

---

## Mode command details

Mode changes use `ZoneSettings Put` (per-zone), not `SystemMode Put` (which does not exist on this bus). `ZoneMode` values — verified empirically by physically cycling modes on the UX360 while capturing `0x641` traffic:

| `ZoneMode` | UX360 label         | HA ClimateMode          |
|-----------:|---------------------|-------------------------|
| `0`        | Off                 | `OFF`                   |
| `1`        | Heat/Cool (Auto)    | `HEAT_COOL`             |
| `2`        | Heat                | `HEAT`                  |
| `3`        | Cool                | `COOL`                  |

Full frame format in [`PROTOCOL.md`](PROTOCOL.md#zonesettings-put--change-operating-mode).

## Known Limitations

- **Fan-only mode** is not mapped — no dedicated fan-only `ZoneMode` value observed. Blower-only behavior is inferred from `IndoorStatus.D` during idle periods.
- **Zone 2–6 setpoints** are observable but write support is only implemented for zone 1.
- **`0x380` byte layout** has two floats; only the second (outdoor air temp) is confirmed — the first is always `-99.0` (sentinel).

---

## Contributing

Observations, field mappings, and tested hardware additions are very welcome. If you capture CAN traffic from a different Trane ComfortLink II system please open an issue with your data — field decoding is ongoing.

---

## License

MIT
