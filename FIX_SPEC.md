# esphome-trane — Fix Spec (v2)

Four bugs observed on a Trane SC360 / UX360 / variable-speed heat pump installation.
Log evidence is in `docs/captures/`. Fix each in a separate commit. Keep changes
minimal and consistent with the existing code style of the repo.

This spec assumes the maintainer has committed at least one 30+ minute capture with
representative activity (setpoint changes, mode changes, at least one cooling cycle)
to `docs/captures/`.

---

## Context

- Hardware: M5Stack AtomS3 Lite + KLAYERS SN65HVD230 transceiver (not the M5 CAN
  module). Wiring is **reversed** from the upstream dewbot6 default: `tx_pin:
  GPIO6, rx_pin: GPIO5`. G6 was easier to route to CTX and G5 to CRX. Not in
  scope for this spec — but watch the YAML: the reversal has historically
  regressed to upstream default when the file was reshuffled, so keep an eye on
  it across rebases.
- System: Trane ComfortLink II, SC360 controller, UX360 thermostat, variable-speed
  HP, upstairs zone, single-zone.
- ESPHome version at capture: 2026.4.1.
- Owner-observed symptoms:
  - Setpoint values get smashed into Celsius fields in Home Assistant.
  - Setpoint changes pushed from HA briefly show the new value in the UI, then
    revert to the previous value.
  - Indoor temperature sensors show values in the 20s and 30s during normal
    indoor conditions.
  - Indoor humidity never leaves `nan` even though the American Standard cloud
    shows live humidity values (65–74%) from the same system over the same
    interval.

---

## Bug 1 — Indoor humidity never publishes

### Symptom

`Indoor Humidity` template sensor emits `nan %` every update interval. No humidity
values reach Home Assistant. Meanwhile, the Trane/American Standard cloud receives
humidity events once per minute.

### Evidence

From the short capture in `docs/captures/capture-2026-04-21-00h51m.log`:

```
00:51:49.678  [S][sensor]: 'Indoor Humidity' >> nan %
00:52:49.708  [S][sensor]: 'Indoor Humidity' >> nan %
00:53:49.661  [S][sensor]: 'Indoor Humidity' >> nan %
00:54:49.680  [S][sensor]: 'Indoor Humidity' >> nan %
```

### Root cause hypothesis

The project's own README documents the field:

> `SystemOpStatus.E` — Indoor humidity (%)

on CAN ID `0x649`. Either:

1. The `SystemOpStatus` JSON handler never extracts `E`, or
2. It extracts it but doesn't publish to `Indoor Humidity`, or
3. It publishes to a different entity than the YAML wires up.

The short capture did not contain a `SystemOpStatus` frame at all (only `ZoneStatus`
and `SpOverride` fired during the capture window). The 30-minute capture must
confirm the frame is present and note the exact JSON shape.

### Fix

1. Inspect the 0x649 handler in `components/trane_hvac/` (likely in
   `trane_climate.cpp` or a related parser). Locate where `SystemOpStatus` is
   processed.
2. If `SystemOpStatus.E` is not being parsed, add extraction. Publish the parsed
   float as a percentage to the `Indoor Humidity` sensor exposed in
   `esphome-trane.yaml`.
3. If `SystemOpStatus.E` is already being parsed to a different sensor, wire it
   to `Indoor Humidity` without breaking any existing consumer.
4. Handle missing/non-numeric values by leaving the sensor unchanged (do not
   publish `nan` explicitly; skip the publish).

### Acceptance criteria

- Within one `SystemOpStatus` broadcast window after flash, the HA
  `sensor.indoor_humidity` entity shows a realistic 0–100 value.
- Values track (within ±1%) what the American Standard mobile app reports for the
  same system over the same interval.
- If humidity is unavailable, the sensor holds its previous value rather than
  publishing `nan`.

---

## Bug 2 — Inbound setpoints written to climate as if Fahrenheit values were Celsius

### Symptom

On any setpoint change observed via 0x649 broadcast (`SpOverride` or
`ZoneStatus`), the climate entity's target temperature low/high values are
replaced with the raw Fahrenheit integer interpreted as Celsius. E.g., setting
73°F cool / 58°F heat on the UX360 causes the HA climate card to show target
range 58–73°C (= 136–163°F).

### Evidence

At 00:52:03 in the capture, the user changed setpoints:

```
00:52:03.767  Target Temperature: Low: 13.89°C  High: 22.78°C   (correct: 57°F/73°F)
00:52:03.818  [D][can649]: JSON: {"ZoneStatus":{"Update":{"1":{"Hsp":"58"}}}}
00:52:03.972  [D][can649]: JSON: {"SpOverride":{"Update":{"1":{"Csp":"73","Hsp":"58","Source":"2","HoldType":"1"}}}}
00:52:04.075  Target Temperature: Low: 58.00°C  High: 73.00°C   (bug)
```

The `Heat Setpoint Zone 1` and `Cool Setpoint Zone 1` template sensors (unit °F)
report 58 and 73 correctly. Only the climate entity's target temperature fields
are written in the wrong unit.

### Root cause hypothesis

ESPHome climate entities expect all temperatures in Celsius internally. The
`SpOverride` and `ZoneStatus` JSON handlers call
`set_target_temperature_low()` / `set_target_temperature_high()` with the raw
Fahrenheit values from the JSON strings. No conversion is applied.

The `ZoneStatus.H` → `current_temperature` path DOES work correctly (the log
shows 21.67°C = 71°F, matching the UX360 display). So the correct pattern exists
elsewhere in the codebase — the setpoint handlers are missing the same
conversion.

### Fix

1. In `components/trane_hvac/trane_climate.cpp` (or wherever the 0x649 handler
   lives), locate the assignments to climate target temperature low/high from
   `SpOverride.Csp`, `SpOverride.Hsp`, and any corresponding `ZoneStatus`
   setpoint fields.
2. Convert °F → °C before calling the setters:
   `c = (f - 32.0f) * 5.0f / 9.0f;`
3. Reuse any existing helper function if the codebase already has an F↔C
   converter (check `trane_climate.h` and `__init__.py`). Do not duplicate the
   conversion inline.
4. Verify the `ZoneStatus.H` → `current_temperature` path is unaffected — it's
   already correct and should stay correct.

### Acceptance criteria

- Changing the setpoint on the UX360 updates the HA climate target range in °F
  (default HA unit) or °C (if user-configured) to match the physical reading
  within ≤1° of rounding error.
- `Heat Setpoint Zone 1` and `Cool Setpoint Zone 1` template sensors continue to
  report correct °F values.

---

## Bug 3 — Outbound setpoints serialized in Celsius to an SC360 expecting Fahrenheit

### Symptom — CONFIRMED by user observation

Setpoint change pushed from Home Assistant: UI briefly shows the new value, then
reverts to the previous value. E.g., user sets 74°F cool target in HA; the
climate card shows 74°F momentarily, then snaps back to 73°F.

### Root cause hypothesis

Exact inverse of bug 2. The sequence:

1. HA commands a setpoint change; ESPHome climate entity passes a °C value
   internally (e.g., `23.33` for 74°F) to the outbound command handler.
2. The 0x641 `SpOverride Put` serializer writes the °C value into the JSON
   string (e.g., `"Csp":"23.33"` or `"Csp":"23"`) with no C→F conversion.
3. SC360 interprets the JSON string as Fahrenheit. Either:
   - Rejects the command as out of range (a 23°F cool setpoint is
     nonsensical), or
   - Accepts a 23°F cool setpoint briefly, then normalizes or ignores it.
4. SC360's next 0x649 broadcast carries the actual unchanged current state.
5. The inbound handler (buggy per Bug 2) writes that back to the climate
   entity, causing the UI revert.

Bugs 2 and 3 are two symptoms of the same missing C↔F conversion in opposite
directions. Fixing only one will leave the UI still apparently broken — bug 2
will mask the bug 3 fix until both are in place.

### Fix

1. Locate the 0x641 `SpOverride Put` serializer — likely in
   `components/trane_hvac/trane_climate.cpp`, in whatever method is called when
   `control()` or `write_state()` runs on a setpoint change.
2. Apply C→F conversion before writing the value into the JSON:
   `f = c * 9.0f / 5.0f + 32.0f;`
3. Match the SC360's expected format. From the observed inbound JSON, setpoints
   are integer strings (e.g. `"73"`, `"58"`). Round to nearest integer before
   serializing; do not send `"23.33"`. Reference shape:
   `{"SpOverride":{"Put":{"1":{"Csp":"74","Hsp":"58"}}}}` or similar — confirm
   the exact envelope by inspecting the repo's existing command-side code or by
   capturing traffic while the UX360 issues a change.
4. Reuse the same F↔C helper introduced for bug 2. One conversion utility,
   used in both directions.
5. If the repo also sends a mode change command (`SystemMode Put`) that contains
   no temperature, leave it alone.

### Acceptance criteria

- Setpoint change pushed from HA takes effect on the UX360 (physical display
  updates to match).
- Climate card in HA stays at the commanded value; no revert.
- Round-trip test: set 74°F in HA → UX360 shows 74°F → change on UX360 to 76°F
  → HA updates to 76°F. No drift, no revert.

---

## Bug 4 — Indoor Temp 1 / Indoor Temp 2 emitted in °C but labeled °F

### Symptom

```
'Indoor Temp 1' >> 24.2 °F    (impossible: 24°F indoors in April Austin)
'Indoor Temp 2' >> 35.3 °F    (impossible: 35°F indoors)
```

Values are stable and track over time but never make physical sense in
Fahrenheit. Reinterpreted as Celsius: 75.6°F and 95.5°F — the first plausible as
return air, the second plausible as a supply-plenum or attic-adjacent reading.

### Evidence

See capture above. Values held steady at 24.2 / 35.3 for the duration of the
short capture, during which the system was IDLE and ambient indoor conditions
were normal (~75°F).

### Root cause

Documented in the README, not a parser bug — a YAML bug:

> `0x283` — Indoor temps 1 & 2 **(°C, two floats)**

The C++ parser correctly emits Celsius. The template sensors in
`esphome-trane.yaml` declare `unit_of_measurement: '°F'` with no conversion
lambda. HA displays a Celsius value with an °F label.

### Fix

Preferred approach: keep the user-facing unit consistent with the rest of the
dashboard (°F), convert in the YAML lambda.

1. In `esphome-trane.yaml`, locate the `Indoor Temp 1` and `Indoor Temp 2`
   template sensors.
2. Leave `unit_of_measurement: '°F'`.
3. Add `x * 9.0 / 5.0 + 32.0` in the publish lambda.
4. Ensure `accuracy_decimals` is at least 1 to avoid precision loss on the
   conversion.

Before committing, confirm the C++ parser is indeed emitting °C (per README)
and not doing its own conversion — otherwise the YAML fix would double-convert.

### Acceptance criteria

- `Indoor Temp 1` and `Indoor Temp 2` publish plausible indoor Fahrenheit values
  (typically 65–95°F depending on which physical point in the AHU they
  represent).
- Values move appropriately during cooling cycles.
- Unit label in HA is `°F`.

---

## Evidence files

1. `docs/captures/capture-2026-04-21-00h51m.log` — original 4-minute capture
   where bugs 1, 2, and 4 were identified. Bug 3 was diagnosed from owner's
   UI-revert observation; a fresh 30+ minute capture should contain a 0x641
   `SpOverride Put` frame for reference.
2. `docs/captures/capture-2026-04-21-longrun.log` — 30+ minute capture with
   varied activity. Used to confirm `SystemOpStatus` JSON shape for bug 1 and
   `SpOverride Put` shape for bug 3.

---

## Out of scope

- ETP11318 duct probe integration. Probe is installed; verification is pending
  an SC360 installer-menu enable check. If `Supply Air Temp` still doesn't
  populate from `0x308` second float or `IndoorStatus.E` after the probe is
  enabled in the installer menu and bugs 1–4 are fixed, a fifth bug may be
  needed — but that's a separate spec once evidence is in.
- Zone 2–6 setpoint write support (README already lists as limitation).
- Fan-only mode mapping (README limitation).
- AtomS3 / SN65HVD230 CAN pin assignment changes — already customized locally,
  not upstream.

---

## Testing

After flashing the full fix build:

1. Idle for 5 minutes. Confirm `Indoor Humidity` is a finite number close to the
   American Standard cloud value.
2. Confirm `Indoor Temp 1` and `Indoor Temp 2` are plausible °F values.
3. Change a setpoint on the UX360 by 2°F. Confirm HA shows the new targets in
   °F (not °C misinterpreted).
4. Change a setpoint in HA by 2°F. Confirm the UX360 physically updates. Confirm
   the HA climate card stays at the new value (no revert).
5. Round-trip a setpoint: HA → UX360 → HA. No drift.
6. Run one full cooling cycle. Confirm `IndoorStatus.E` behavior (fault string
   if probe disabled in installer menu, real SAT if enabled).

If any acceptance criterion fails, capture fresh logs and attach to the PR.
