# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESPHome external component + config that decodes Trane ComfortLink II CAN bus traffic (50kbps) and exposes a Home Assistant climate entity. Runs on an ESP32-S3 (M5Stack AtomS3 + CAN module). See `README.md` for hardware, wiring, and the full CAN field dictionary — do not duplicate that information elsewhere.

## Build / flash / validate

```bash
esphome config   esphome-trane.yaml   # validate YAML + codegen, no compile
esphome compile  esphome-trane.yaml   # full compile (slow, ESP-IDF)
esphome run      esphome-trane.yaml   # compile + upload + logs
esphome logs     esphome-trane.yaml   # attach to a running device
```

There are no unit tests and no Python test runner — this is an ESPHome config repo, not a Python package. `esphome config` is the fastest way to catch codegen / schema errors after touching `components/trane_hvac/*.py` or the YAML. Reserve `compile`/`run` for C++ changes or full verification.

Requires a `secrets.yaml` (copy from `secrets.yaml.example`) with `wifi_ssid` / `wifi_password`.

## Architecture

The system has **two cooperating layers** — almost all CAN parsing lives in YAML, not C++.

### Layer 1 — YAML (`esphome-trane.yaml`): the decoder

All sensor/text_sensor entities are declared as `platform: template` with `lambda: "return NAN;"` placeholders. They are written by `canbus: on_frame:` lambda handlers — one per CAN ID. The template `lambda` is effectively a no-op; the real data path is `publish_state()` calls from the frame handlers.

Two frame formats coexist on this bus:

- **`0x649` (system broadcast) and `0x641` (command/response)** carry JSON spread across multiple CAN frames. Reassembly protocol:
  - Header frame: `x[0] & 0xF0 == 0xC0`, payload starts at byte 5
  - Continuation frames: `x[0]` is a sequence number, 7 payload bytes follow
  - Last frame: `x[0] & 0x80` set, low 7 bits = `(remaining_bytes + 1)`
  - Buffers are held in `globals: buf_649` / `buf_641` and parsed with naïve `std::string::find` — there is no JSON library on-device.
- **Float frames** (`0x283`, `0x308`, `0x380`, `0x381`, `0x383`, `0x387`, `0x38F`, `0x410/0x430/0x450`) carry 1–2 IEEE-754 floats via `memcpy`. Each handler applies a plausibility range check before publishing (e.g. outdoor coil `-40..130°F`) — **keep those range checks when adding new sensors**, they are the only thing preventing garbage frames from poisoning HA history.

The reverse path is `script: can_transmit_json`, which segments a JSON string using the same framing protocol and sends it on `0x641`. Both `api.services.set_*` and the climate `set_*_action` automations dispatch through this single script.

Field quirks to remember (not obvious from the code alone):
- **CAN pin assignment is swapped from the README.** The YAML uses `tx_pin: GPIO6, rx_pin: GPIO5` — the README shows the opposite (`tx_pin: GPIO5, rx_pin: GPIO6`). The YAML is correct for this hardware (SN65HVD230 transceiver wiring). Do not "fix" the YAML to match the README.
- `SystemOpStatus.E` is overloaded: decimal string = SC360 outdoor temp (°F), integer string = indoor humidity (%). The handler branches on `estr.find('.')`.
- `IndoorStatus.E` is either a blower-speed number (0–100) *or* a fault string (e.g. `TA_INV_HI`); non-digit first char routes to `supply_air_fault`.
- `canbus.can_id` on the `esp32_can` platform is the *controller bus ID*, not a filter — `on_frame.can_id` with mask `0x7FF` is the per-ID filter.
- The catchall `can_id: 0x000, can_id_mask: 0x000` handler must list every already-handled ID in its `switch` to avoid duplicate logging. Update it when adding a new handler.

### Layer 2 — C++ component (`components/trane_hvac/`): the climate entity

`TraneClimate` (`trane_climate.h` / `.cpp`) is a thin `climate::Climate` + `Component` that does **not** touch CAN directly. It subscribes to the template sensors via `add_on_state_callback` and derives `this->mode` / `this->action` / target temperatures from them. Fahrenheit arrives from the bus and is converted to Celsius at the subscription boundary (ESPHome climate is always °C internally); the `set_temperature_action` trigger converts back to °F before feeding `can_transmit_json`.

Action derivation is a two-signal join: `demand_stage_sensor` (`SystemOpStatus.C`) sets `HEATING` / `COOLING` / `IDLE`, then `indoor_unit_state_sensor` (`IndoorStatus.D`) upgrades `IDLE` → `FAN` when the blower is running post-cycle. This is why both sensors are wired into the climate component — neither alone captures the full state.

User control flows: Home Assistant → `TraneClimate::control()` → `Trigger<>` fires → YAML automation (`set_mode_action` / `set_temperature_action` / `set_preset_action`) → `can_transmit_json` → CAN bus. The Python binding (`climate.py`) uses `automation.build_automation` on those triggers, which is what lets the YAML author write the JSON payload in a lambda instead of hardcoding it in C++.

## When adding a new CAN field

1. Add a `template` sensor or `text_sensor` in the YAML with the right `device_id` (`dev_thermostat` / `dev_sc360` / `dev_air_handler` / `dev_heat_pump` — sub-device grouping requires ESPHome 2025.7.0+).
2. Add parsing inside the appropriate `on_frame` handler. For JSON fields, scope the `find()` to the object (e.g. `find(..., sos)` after locating `"SystemOpStatus"`) so identical keys on different objects don't collide.
3. Apply a plausibility range check before `publish_state`.
4. If it's a new CAN ID, also add it to the catchall's `switch` to silence the verbose logger.
5. Update the README's field tables — they are the authoritative field dictionary, not code comments.
