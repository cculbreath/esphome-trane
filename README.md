# esphome-trane

An [ESPHome](https://esphome.io) component for monitoring and controlling Trane HVAC systems that use the **ComfortLink II** communicating bus, enabling local control through Home Assistant without any cloud dependency.

Inspired by and structurally modeled after [esphome-econet](https://github.com/esphome-econet/esphome-econet).

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
| `SystemOpStatus` | `C` | Demand stage: `--`, `HP Stage 1`, `HP Stage 2`, `HP2+ID1`, `HP2+ID2`, `ID Stage 1`, `ID Stage 2` |
| `SystemOpStatus` | `D` | Elapsed run time (minutes, resets each cycle) |
| `SystemOpStatus` | `E` | Indoor humidity (%) |
| `OdStatus` | `B` | Outdoor coil/liquid-line temperature (°F) |
| `OdStatus` | `C` | Outdoor unit state: `B`=running, `D`=off |
| `OdStatus` | `CompDemandPercent` | Compressor demand (%) |
| `IndoorStatus` | `D` | Blower state: `A`=transition, `B`=running |
| `IndoorStatus` | `E` | Supply air temp (°F) or fault string (e.g. `TA_INV_HI`) |
| `ZoneStatus` | `H` | Room temperature (°F) |
| `ZoneStatus` | `HcStatus` | Heat/cool call: `3`=call active, `4`=satisfied, `5`=elevated call, `7`=aux/emergency |
| `ZoneStatus` | `HoldText` | Schedule text (e.g. `Following Schedule: SLEEP`) |
| `SpOverride` | `Hsp` / `Csp` | Heat / cool setpoints (°F) |
| `SpOverride` | `HoldType` | `0`=schedule, `2`=manual hold |
| `WeatherToday` | `D` / `F` | Current outdoor temp readings (°F) |
| `ActiveAlarms` | `AlarmId` | Fault code (e.g. `Err 185.09`) |

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

## Known Limitations

- **Fan-only mode** maps to system off (`C`) on the CAN bus — no dedicated fan-only command has been observed yet
- **Heat_cool (auto) mode** sends heat (`A`) — the SC360 manages its own staging between HP and gas automatically
- **Zone 2–6 setpoints** are observable but write support is not yet implemented
- **`0x380` byte layout** has two floats; only the second (outdoor air temp) is confirmed — the first is logged at DEBUG for further investigation

---

## Contributing

Observations, field mappings, and tested hardware additions are very welcome. If you capture CAN traffic from a different Trane ComfortLink II system please open an issue with your data — field decoding is ongoing.

---

## License

MIT
