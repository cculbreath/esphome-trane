# esphome-trane — Conversation Handoff (2026-04-21)

Scope: ESPHome firmware for the AtomS3-based Trane Link bus bridge. Everything
else (home automation infra, other HVAC projects, networking) is out of scope
for this thread.

## Who / what

Christopher Culbreath, physicist/engineer. Upstairs HVAC is a Trane ComfortLink II
variable-speed heat pump with SC360 controller and UX360 thermostat. Goal is local
HA control of setpoints/modes via CAN bus, bypassing the American Standard cloud.

## Hardware state

- **MCU:** M5Stack AtomS3 Lite (ESP32-S3).
- **Transceiver:** KLAYERS SN65HVD230 3.3V CAN transceiver. R120 termination
  resistor has been removed (0603, marked "121" on PCB) — the Trane bus is already
  terminated at both ends; a third termination drops impedance.
- **Wiring (REVERSED from upstream dewbot6 defaults):** AtomS3 G6 → transceiver
  CTX, G5 → CRX. YAML reflects this: `tx_pin: GPIO6, rx_pin: GPIO5`. Swapped
  from upstream because G6/G5 was easier to route physically on this board —
  not for any electrical reason. Confirmed with Christopher 2026-04-22 after a
  commit had regressed the pins to upstream default and this handoff briefly
  matched the regression. **If you see the YAML at `tx_pin: GPIO5, rx_pin:
  GPIO6`, that is the regression, not the correct state.**
- **Power:** USB from the HA host. Not 24VAC. Cleaner, gives galvanic isolation
  from the 24VAC transformer.
- **Ground:** Common between AtomS3, transceiver, and Rs pin tied to GND
  (slope-control mode).
- **Bus tap:** SC360 mounting plate's R/DH/DL/B screw terminals.
  - DH = CANH
  - DL = CANL
  - B  = GND
  - R  = 24VAC (unused — power is USB)

## Topology note (correcting earlier handoff confusion)

The SC360 and UX360 are distinct devices and were previously conflated in Claude's
notes:

- **SC360 (model TSYS2C60A2VVUGA)** — system controller in the 2nd floor AHU
  closet. Smooth white plastic face, no user UI. Runs the Link bus, handles
  staging.
- **UX360** — user-facing touchscreen in the 2nd floor hallway. Link bus node.

The AtomS3 is tapped at the SC360, not the UX360.

## Firmware state

- Repo: `dewbot6/esphome-trane` (MIT licensed). Christopher has a fork pending.
- ESPHome: 2026.4.1.
- Built and flashed from Christopher's Mac M4 via `esphome` CLI. The HA ESPHome
  add-on has a known idf_tools.py regression (esphome/esphome#12407) that blocks
  ESP32 builds in the add-on environment; Mac CLI is the workaround until
  upstream is fixed. HA discovers the device via mDNS regardless of where it was
  built.
- Device online at 192.168.68.59, hostname `esphome-trane`. Parsing frames
  successfully (outdoor temp, indoor temps, coil temps, pressure, discharge,
  suction, setpoints — all publishing).

## ETP11318 duct temperature probe

- **Installed.** Physical install complete: probe mounted in supply plenum
  (downstream of AHU, after evap coil), wires routed to SC360.
- **Not yet verified populating.** Before the install, `IndoorStatus.E` on 0x649
  showed `TA_INV_HI` fault string, and the float-frame path (`0x380: -99.0 / 57.0`
  pattern) showed sentinel values.
- **SC360 installer menu may need to be checked.** Trane ComfortLink II systems
  typically require the Supply Air Sensor to be explicitly enabled in the
  installer/technician menu before the SC360 will use probe data. Hold the
  settings area on the UX360 for ~5-10 seconds to enter installer mode, look for
  a "Supply Air Sensor" or "Discharge Air Sensor" option, set it to
  Installed/Enabled.
- **Verification is part of post-fix testing** (see §Testing).

## Known bugs and fix workflow

Four bugs identified from log analysis and user observation. Spec committed as
`FIX_SPEC.md` in Christopher's fork. Summary:

1. **Indoor Humidity never publishes** — `nan %` every interval. `SystemOpStatus.E`
   on 0x649 (documented in repo README as humidity source) not wired to the
   template sensor.
2. **Inbound setpoints written as °C when they're actually °F** — 0x649
   `SpOverride` and `ZoneStatus` handlers pass raw Fahrenheit JSON strings
   directly to the climate entity's target temperature setters without F→C
   conversion. HA shows target range in what looks like °C.
3. **Outbound setpoints serialized as °C to an SC360 expecting °F** — CONFIRMED
   by user observation (UI briefly shows new value, reverts to prior). Inverse
   of bug 2. The 0x641 `SpOverride Put` serializer writes the climate entity's
   internal °C value into the JSON without C→F conversion. SC360 rejects or
   normalizes, then echoes the unchanged state back, which bug 2 writes into
   the climate entity, producing the revert.
4. **Indoor Temp 1 / Indoor Temp 2 emitted in °C, labeled °F** — YAML bug, not
   parser bug. Frame 0x283 documented as °C in repo README; template sensors
   declare °F with no conversion lambda.

Bugs 2 and 3 are two symptoms of the same missing conversion in opposite
directions. They must be fixed together; fixing only one masks the other.

Possible fifth bug, not yet confirmed: `Supply Air Temp` never published from
0x308 in the pre-probe capture, even though `Heat Exchanger Temp` (same frame)
did. Defer until probe is enabled in installer menu + bugs 1-4 are fixed. If
SAT is still missing after that, it's a parser bug to add to the spec.

### Workflow

1. Capture 30+ min of ESPHome logs with varied activity (setpoint changes,
   mode changes, at least one cooling cycle):
   ```
   esphome logs esphome-trane.yaml 2>&1 | tee trane-capture-$(date +%Y%m%d-%H%M).log
   ```
2. Fork `dewbot6/esphome-trane` if not already (`gh repo fork dewbot6/esphome-trane --clone`).
3. Commit both captures to `docs/captures/` and `FIX_SPEC.md` to `docs/`.
4. Kick off Claude Code session: "Read `docs/FIX_SPEC.md` and implement the four
   fixes. One commit per bug. Reference the capture files in commit messages."
5. Flash updated firmware from Mac CLI. Verify each acceptance criterion.
6. After fork works cleanly in production, open upstream PRs to dewbot6.

## Testing

After flashing the full fix build:

1. Idle 5 minutes. Confirm `Indoor Humidity` is a finite number close to what
   the American Standard cloud shows for the same system.
2. Confirm `Indoor Temp 1` and `Indoor Temp 2` are plausible °F values.
3. Change a setpoint on the UX360 by 2°F. Confirm HA shows new targets in °F
   (not °C misinterpreted).
4. Change a setpoint in HA by 2°F. Confirm UX360 physically updates. Confirm HA
   climate card stays at the new value — no revert.
5. Round-trip: HA → UX360 → HA. No drift.
6. Run one full cooling cycle. Check `IndoorStatus.E` and `Supply Air Temp`:
   - If Supply Air Sensor is disabled in SC360 installer menu → expect fault
     string (`TA_INV_HI` or similar).
   - If enabled and everything else is working → expect real SAT value (likely
     55-65°F during cooling, ambient during idle).
   - If enabled but `Supply Air Temp` still `nan`, file the fifth bug.

If any acceptance criterion fails, capture fresh logs and attach to the PR.

## Christopher's preferences (relevant to this work)

- CLI over GUI, text over diagrams.
- Honest direct analysis, no hedging.
- Will push back when something's wrong. Listen.
- Comfortable with CLI, git, C++, ESPHome, soldering, HVAC mechanical work.

## Things Claude got wrong earlier, worth remembering

- Initially conflated SC360 and UX360 model numbers. SC360 is TSYS2C60A2VVUGA and
  lives in the AHU cabinet with no user UI; UX360 is the hallway touchscreen.
  All the photos Christopher shared of the mounting plate / bus connector are
  of the SC360.
- Initially suggested building firmware in the HA ESPHome add-on. The add-on has
  an upstream idf_tools.py bug that blocks ESP32 builds. Build from Mac CLI.
- Described the ESPHome add-on bug as if it might be user error. It's not —
  esphome/esphome#12407, upstream regression.
