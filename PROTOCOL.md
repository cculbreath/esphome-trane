# Trane ComfortLink II CAN Protocol — Field Notes

This document describes the on-the-wire CAN protocol used by the Trane ComfortLink II SC360 / UX360 system, as reverse-engineered from direct observation. Every claim here is backed by captured frames in [`docs/captures/`](docs/captures/).

Tested against: Trane UX360 thermostat + SC360 controller (model TSYS2C60A2VVUGA) + variable-speed heat pump + air handler.

The [upstream repository](https://github.com/dewbot6/esphome-trane) contained a mostly-working *receive* path but a completely non-functional *transmit* path. This document records what was actually needed to get bidirectional control working, so anyone else decoding this protocol doesn't have to rediscover it.

---

## CAN bus parameters

- **50 kbps** (slow for CAN, but standardized for ComfortLink II)
- Standard 11-bit frame IDs
- No extended frame use observed

## Observed CAN IDs

| ID      | Sender        | Purpose                                             |
|---------|---------------|-----------------------------------------------------|
| `0x649` | SC360         | System broadcast (JSON)                             |
| `0x641` | SC360, UX360  | Command/response channel (JSON, bidirectional)      |
| `0x283` | Air handler   | Indoor air temps 1 & 2 (2 × IEEE-754 float, °C)     |
| `0x308` | Air handler   | Supply air temp, heat exchanger temp (°F)           |
| `0x380` | Heat pump     | (unknown / −99.0 sentinel), outdoor air temp (°F)   |
| `0x381` | Heat pump     | Suction temp (°F)                                   |
| `0x383` | Heat pump     | Discharge temp (°F)                                 |
| `0x387` | Heat pump     | Outdoor coil temp (°F)                              |
| `0x38F` | Heat pump     | Refrigerant pressure (PSI)                          |
| `0x410` | SC360 sensor  | Temp sensor 1 (°F)                                  |
| `0x430` | SC360 sensor  | Temp sensor 2 (°F)                                  |
| `0x450` | SC360 sensor  | Temp sensor 3 (°F)                                  |

Other IDs observed but not decoded yet: `0x5C1`, `0x5C9`, `0x386`, `0x490`.

---

## Multi-frame JSON protocol on `0x641` / `0x649`

JSON payloads are split across multiple 8-byte CAN frames. Each transmission consists of:

```
[HEADER] [CONTINUATION ×N] [LAST] [TRAILER]
```

All four frame types share the same 11-bit CAN ID (e.g. `0x641`).

### Header frame (byte 0 = `0xC2` for data, `0x21` for ack)

```
Byte:   0    1    2    3    4     5  6  7
Value:  C2   0A   30   00   LEN   00 00 00
              └─── constant ──┘   └ padding ┘
```

- **Byte 0**: Message type. `0xC2` = data payload, `0x21` = acknowledgment.
- **Bytes 1–3**: The three-byte constant `0A 30 00`. Same value from every sender observed (SC360, UX360). Probably a protocol version or device-class marker.
- **Byte 4**: Declared message length, **including a trailing null terminator**. E.g. the payload `{"Ack":"200"}` (13 chars) has `LEN = 0x0E = 14`.
- **Bytes 5–7**: Zero padding.

### Continuation frames (byte 0 = `0x01`, `0x02`, …)

```
Byte:   0     1 .. 7
Value:  SEQ   <7 payload bytes>
```

Sequence numbers start at `0x01` and increment by 1 for each continuation. The Ack response is unusual in that its first continuation uses sequence `0x00`; all other messages start at `0x01`.

### Last frame (byte 0 = `0x80 | next_sequence_number`)

```
Byte:   0                 1 ...
Value:  0x80 | NEXT_SEQ   <remaining payload bytes> 0x00 <trailing garbage>
```

- **Byte 0**: `0x80` OR'd with the sequence number that *would come next* if this were another continuation. E.g. after sending continuations 01..05, the last frame is `0x86`.
- **Remaining bytes**: Payload bytes followed by a `0x00` null terminator. Any bytes after the null are padding/garbage and are ignored by the receiver.

This is the ONE framing detail that the upstream repo had wrong. Upstream used `0x80 | (remaining_bytes + 1)` which happens to produce a similar value for short last-frame contents but diverges for longer ones, causing the SC360's parser to fail validation.

### Trailer frame (byte 0 = message-type dependent)

```
Byte:   0     1 .. 7
Value:  XX    00 00 00 00 00 00 00
```

Every multi-frame transmission is followed by a single trailer frame with one non-zero byte at position 0, padded with zeros. The non-zero byte identifies the message type:

| Trailer | Message                              |
|---------|--------------------------------------|
| `0xCD`  | `SpOverride Put` (setpoint command)  |
| `0xC5`  | `ZoneSettings Put` (mode command)    |
| `0xD1`  | `UnitID ModelNumber` (SC360 → bus)   |
| `0xC5`  | `UnitID SerialNumber` (SC360 → bus)  |
| `0xD9`  | `UnitID BuildInfo` (SC360 → bus)     |
| `0xC1`  | Immediately precedes an Ack response |

All observed trailer values fit the bit pattern `11 X0 Y Z 01` (3 free bits → 8 possible types; 7 distinct values seen, `0xDD` never observed). **Omitting the trailer causes the SC360 to enter an error state and ultimately reboot.** Always send one.

---

## Application-layer messages

### `SpOverride Put` — change setpoints

**JSON shape (order matters):**

```json
{"SpOverride":{"Put":{"1":{"Csp":"76","Hsp":"60","HoldType":"1","Source":"1"}}}}
```

- **Key order**: `Csp`, `Hsp`, `HoldType`, `Source`. A lenient JSON parser would accept any order, but the UX360's own transmissions use this exact order, so we match it.
- **Values**: integers as strings, in **Fahrenheit**. Decimals like `"23.33"` are rejected.
- **HoldType**: `"1"` for permanent hold (matches UX360 behavior). `"0"` releases to schedule. Don't use `"2"` — the SC360 silently ignores commands with `HoldType=2`.
- **Source**: `"1"` (observation: value SC360 broadcasts is `"2"`, but commands must use `"1"`).
- **Transmit trailer**: `0xCD`.

**Response:** Within ~20 ms the SC360 emits an Ack (`{"Ack":"200"}`) on `0x641`, then broadcasts a `SpOverride Update` on `0x649` confirming the new state.

### `ZoneSettings Put` — change operating mode

**JSON shape:**

```json
{"ZoneSettings":{"Put":{"1":{"ZoneMode":"1"}}}}
```

**ZoneMode values (verified empirically by cycling modes on the UX360):**

| Value | UX360 display label   | HA equivalent              |
|-------|-----------------------|----------------------------|
| `"0"` | Off                   | `CLIMATE_MODE_OFF`         |
| `"1"` | Heat/Cool (Auto)      | `CLIMATE_MODE_HEAT_COOL`   |
| `"2"` | Heat                  | `CLIMATE_MODE_HEAT`        |
| `"3"` | Cool                  | `CLIMATE_MODE_COOL`        |

**Transmit trailer**: `0xC5`.

> ⚠️ There is no `SystemMode Put` message type on this bus. The upstream `esphome-trane` YAML sent `{"SystemMode":{"Put":{"B":"A"}}}` — that message type is fabricated. Transmitting it caused the SC360 to reboot during testing.

---

## `SystemOpStatus` is operating state, not configured mode

This was a source of significant confusion while debugging. The periodic broadcast `SystemOpStatus.B` reports *what the system is doing right now*, not what the user configured:

- `"A"` — currently heating
- `"B"` — currently cooling
- `"C"` — idle / no demand

A system in `ZoneMode=1` (Auto) will report `B="C"` when no demand is active, `B="A"` when heating, `B="B"` when cooling.

**The configured mode (what the user set) comes from `ZoneSettings.ZoneMode`.** Wire a climate entity's `mode` from `ZoneMode`, and its `action` from `SystemOpStatus.C` (demand stage) plus `IndoorStatus.D` (blower state).

---

## Shared JSON namespace pitfalls

Many top-level objects on `0x649` share field names with `SpOverride` / `ZoneStatus`, and unscoped searches produce wrong values:

| Field       | Correct source (zone 1)                           | Wrong sources that alias       |
|-------------|---------------------------------------------------|--------------------------------|
| `Hsp`/`Csp` | `SpOverride."1"` or `ZoneStatus."1"`              | `PresetSettings.Home/Away/Sleep` (preset values, not zone setpoints) |
| `H`         | `ZoneStatus."1".H` (room temp, °F)                | `IndoorSettings."1".H` (unrelated config value) |
| `E`         | `SystemOpStatus.E` (outdoor temp OR humidity)     | `ZoneStatus."1".E` (battery %) |

Always scope searches to the intended object and to `"1":{` for zone-1 fields.

---

## Capture evidence

See [`docs/captures/`](docs/captures/):

- `trane-ux360-reference-1057.log` — UX360 sending two setpoint changes (Csp 74→75→74), captured at raw-frame level via `can641_raw` debug tag. Shows the complete `SpOverride Put` framing.
- `trane-ux360-mode-reference-1107.log` — UX360 cycling through all four modes. Provides the `ZoneMode` value for each UX360 mode label. This is the capture used to empirically verify the ZoneMode mapping.

Both captures were produced by running the ESPHome firmware in this repo (with raw frame logging enabled) while a user physically changed settings on the UX360 touchscreen.
