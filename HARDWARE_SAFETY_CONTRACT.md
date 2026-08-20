# Hardware Safety Contract — Timer Digital Relay v4.3.8

> Implements the hardware-software contract requirements from the
> Industrial-Grade Implementation Directive §69, §70, §71, §102.

---

## 1. Hardware Platform

| Component | Spec | Notes |
|---|---|---|
| MCU | ESP32-WROOM-32 (38-pin, 4 MB flash) | Dual-core Xtensa LX6 @ 240 MHz |
| Flash | 4 MB (default.csv partitions) | 2 MB app + 2 MB OTA + LittleFS |
| RAM | 520 KB SRAM | 320 KB available to app after WiFi/BLE stack |
| WiFi | 802.11 b/g/n @ 2.4 GHz | CGNAT-friendly via MQTT outbound |
| Bluetooth | BLE 4.2 (unused) | |
| Operating voltage | 3.3 V logic | **NOT 5 V-tolerant** — ESP32 GPIOs are 3.3 V only. Applying 5 V to any GPIO (including the so-called "input-only" pins 34/35/36/39) will damage the SoC. The earlier "5 V-tolerant digital inputs" claim in this document was INCORRECT and has been removed (Phase 19 reconciliation). All 5 V signals (PIR, PZEM RX, external triggers) MUST pass through a level shifter or resistor divider before reaching any ESP32 GPIO. |
| Boot strapping | GPIO0, GPIO2, GPIO12, GPIO15 | Boot mode + flash voltage — DO NOT repurpose |

---

## 2. GPIO Allocation (firmware v4.3.8 — DO NOT REPURPOSE WITHOUT AUDIT)

### 2.1 Relay Outputs (12 channels, active-LOW module)

| Channel | GPIO | Mode | Polarity | Notes |
|---|---|---|---|---|
| CH1 | 13 | OUTPUT | LOW=ON, HIGH=OFF | Standard active-low relay module |
| CH2 | 14 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH3 | 16 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH4 | 17 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH5 | 18 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH6 | 19 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH7 | 21 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH8 | 22 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH9 (PIR1) | 23 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH10 (PIR2) | 25 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH11 (PIR3) | 26 | OUTPUT | LOW=ON, HIGH=OFF | |
| CH12 (PIR4) | 27 | OUTPUT | LOW=ON, HIGH=OFF | |

**Boot glitch prevention**: `RelayDriver::begin()` sets all GPIO levels BEFORE
setting pinMode OUTPUT. This prevents momentary relay activation during boot.

### 2.2 PIR Inputs (4 channels, input-only)

| PIR | GPIO | Notes |
|---|---|---|
| PIR1 | 34 | Input-only, no internal pull. HC-SR501 3.3 V compatible |
| PIR2 | 35 | Input-only |
| PIR3 | 36 (VP) | Input-only, ADC1_CH0 |
| PIR4 | 39 (VN) | Input-only, ADC1_CH3 |

**Important**: GPIO34/35/36/39 are input-only — they have no output drivers.
This is why they're used for PIR.

### 2.3 I²C Bus (GPIO32/33)

| Pin | GPIO | Function | Devices on bus |
|---|---|---|---|
| SDA | 32 | I²C data | DS3231 RTC (0x68), SHT31 (0x44), INA219 #1 (0x40), INA219 #2 (0x41), ADS1115 #1 (0x48), ADS1115 #2 (0x49) |
| SCL | 33 | I²C clock | Same |

I²C bus speed: 400 kHz (Fast Mode). All devices support this rate.

### 2.4 PZEM UART (GPIO4/5)

| Pin | GPIO | Function | Notes |
|---|---|---|---|
| PZEM TX → ESP RX | 5 | UART1 RX | 5 V → 3.3 V via 1 K series resistor |
| PZEM RX ← ESP TX | 4 | UART1 TX | 3.3 V → 5 V via level shifter (optional) |

PZEM-004T v3.0 communicates via Modbus-RTU at 9600 baud.

### 2.5 ADC for Battery Pack Voltage (v4.1 deviation, audit-documented)

| Source | Configuration | Status |
|---|---|---|
| **Default** (v4.1+) | ADS1115 #2 AIN3 (B+ node) | Active |
| **Optional** | ESP32 ADC1 GPIO (assignable in BatteryConfig.h) | Inactive (no free ADC1 pin available on standard WROOM-32 — see audit deviation note below) |

**Audit deviation note**: Brief §10/§51/§52 explicitly forbid moving PIR
GPIO without owner approval. The standard WROOM-32 module exposes only
GPIO32-39 for ADC1, all of which are used (32/33 = I²C, 34/35/36/39 = PIR).
GPIO37/38 are NOT broken out. The resolution is to use the existing ADS1115
#2 AIN3 (B+ node already allocated per brief §16) as the authoritative pack
voltage source. This avoids breaking the PIR GPIO contract.

---

## 3. Relay Module Polarity & Driver

The firmware is designed for **active-LOW** relay modules (the most common
type sold on the market — e.g., the "8-channel 5V relay module with optocoupler").

| Relay state | GPIO output | Indicator LED |
|---|---|---|
| OFF (relay coil de-energized) | HIGH (3.3 V) | Off |
| ON (relay coil energized) | LOW (0 V) | On |

**For active-HIGH relay modules**: change `Core::RELAY_ON` and
`Core::RELAY_OFF` in `Config.h`.

### 3.1 Relay Contact Ratings (typical 5V relay module)

| Contact type | Max rating | Notes |
|---|---|---|
| NO (Normally Open) | 10 A @ 250 VAC | Use for loads energized on command |
| NC (Normally Closed) | 10 A @ 250 VAC | Use for loads de-energized on command (fail-safe) |
| NO/NC @ 30 VDC | 10 A | DC loads (battery, solar) |

**For inductive loads (motors, contactors, solenoids)**: install flyback
diodes or RC snubbers across the load. Relay contact arcing degrades the
contact surface and reduces lifespan.

---

## 4. Power Supply

### 4.1 ESP32 Power

| Source | Voltage | Current | Notes |
|---|---|---|---|
| USB bus power | 5 V ± 0.25 | 500 mA min | For development + flash |
| External 5 V (via VIN/5V pin) | 5 V ± 0.5 | 1 A min | For production (relay module current) |
| External 3.3 V (via 3V3 pin) | 3.3 V ± 0.1 | 500 mA | Not recommended — bypasses onboard regulator |

**Total current budget**:
- ESP32 (WiFi active): ~250 mA peak
- 12 relays (all ON): ~360 mA (30 mA per relay coil)
- DS3231 + SHT31 + INA219 ×2 + ADS1115 ×2: ~10 mA
- PZEM-004T: ~10 mA
- **Total worst case**: ~630 mA → use ≥1 A power supply

### 4.2 Brownout Detection

ESP32 has built-in brownout detector (default threshold ~2.43 V).
When triggered, the chip resets with reason `RTCWDT_BROWN_OUT_RESET`
(code 11). The firmware v4.3.8 Health Supervisor tracks this in NVS
(`brn_cnt`) and raises a `BROWNOUT_RESET` alarm on next boot.

If brownouts are frequent:
- Upgrade power supply (current capacity insufficient)
- Add bulk capacitance at relay module (1000 µF electrolytic)
- Separate ESP32 power from relay power (use diode OR + capacitor)

---

## 5. Grounding & Isolation

### 5.1 Single Ground Reference

All grounds MUST be tied together at ONE point (star ground):
- ESP32 GND
- Relay module GND (high-current return)
- PZEM GND
- DS3231/SHT31/INA219/ADS1115 GND (I²C bus ground)
- Power supply GND

**Forbidden**: ground loops (multiple ground paths create circulating
currents that corrupt sensor readings).

### 5.2 Galvanic Isolation

| Interface | Isolation | Reason |
|---|---|---|
| Relay contacts (AC side) | Mechanical (air gap) | 250 VAC mains isolation from 3.3 V logic |
| PZEM voltage input | Internal CT/PT | AC mains isolated from logic |
| WiFi/Bluetooth | RF (air) | Inherent |
| I²C sensors (DS3231/SHT31/INA219/ADS1115) | **NONE** | 3.3 V logic shared with MCU — if sensor circuit touches AC, FATAL |

**Critical safety rule**: I²C sensor circuits MUST be galvanically isolated
from AC mains. Use opto-isolated sensors if measuring AC; the INA219/ADS1115
in this design measure **DC only** (battery pack + cell voltages), so they're
safe IF the battery pack is isolated from AC mains (typical in solar/battery
systems with isolated charge controller).

---

## 6. Fusing & Circuit Protection

### 6.1 Per-Channel Fuse (Recommended)

Each relay's NO/NC contact should have a fuse sized to 1.25 × max load current:

| Load type | Fuse rating | Fuse type |
|---|---|---|
| Lighting (LED) | 3 A | Slow-blow (inrush) |
| Motor (inductive) | 1.5 × FLA | Slow-blow (start surge) |
| Heater (resistive) | 1.25 × max | Fast-blow |

### 6.2 MCB (Miniature Circuit Breaker) — AC Mains

For AC loads: install a Type C MCB at the relay module's AC input.
- Type C trips at 5–10× rated current (handles motor inrush, trips on fault)
- Rating: equal to or less than relay contact rating (10 A → use 10 A MCB)

### 6.3 SPD (Surge Protective Device)

For installations in lightning-prone areas: install a Type 2 SPD at the
main distribution board. ESP32 + relay module are downstream of MCB + SPD.

---

## 7. Thermal

| Component | Max operating temperature | Failure mode if exceeded |
|---|---|---|
| ESP32 SoC | 85 °C | Throttles, then crashes |
| Relay coil (continuous ON) | 70 °C (typical) | Coil insulation degrades, lifetime reduced |
| Electrolytic capacitor (in PSU) | 105 °C (typical) | Bulging, venting, eventual short |

**For installations in hot environments** (>40 °C ambient):
- Use vented enclosure
- Add small fan (5 V, 40 mm) controlled by an extra GPIO
- Derate relay current (use 8 A max per channel instead of 10 A)

The SHT31 ambient sensor measures environment temperature — use it to
detect thermal stress and raise alarms when ambient > 50 °C.

---

## 8. Boot State & Safe Defaults

### 8.1 GPIO Boot State

ESP32 GPIO pins are HIGH-Z (floating input) at reset, until pinMode is
called. The firmware calls `RelayDriver::begin()` early in setup() to
set GPIO levels BEFORE pinMode OUTPUT — this prevents relay glitches.

### 8.2 Per-Channel Boot Policy (brief §13)

| Policy | Default use | Behavior |
|---|---|---|
| `BOOT_OFF` | Hazardous loads (heaters, motors) | GPIO set to OFF (HIGH for active-low) |
| `BOOT_ON` | Critical loads that must auto-resume | GPIO set to ON (LOW for active-low) |
| `RESTORE_LAST` | Convenience loads (lighting) | Restore last known state from NVS |
| `SAFE_STATE` | Mixed (default) | OFF unless explicitly overridden |

Configure per-channel via `Channel.bootPolicy` in `Types.h`.

### 8.3 Safe State Definition

"Safe state" for this system means:
- All relays OFF (default for hazardous loads)
- WiFi disconnected (no remote control until local state is stable)
- MQTT not connected (no remote commands until local state is stable)
- RTC validity checked (if invalid, scheduler inhibited)
- maxOnTimeForced flags cleared
- All alarms cleared (fresh start)

Safe state is entered:
- On factory reset
- On config corruption recovery
- On OTA rollback

---

## 9. Physical Feedback Disclosure (brief §70, §102)

The current hardware uses **GPIO output only** — there is NO auxiliary
contact feedback to confirm physical relay state.

**This means**: firmware sets GPIO=LOW (intended: relay ON), but cannot
detect whether the relay contact actually closed. If the relay coil fails
open, or the contact welds shut, or the contact doesn't close due to
insufficient drive current — the firmware will report ON but the load
may be OFF (or vice versa).

**PWA behavior**: PWA shows the GPIO-commanded state, not the confirmed
physical state. This is documented in the UI as "commanded state" rather
than "confirmed state".

**For mission-critical loads**: add an auxiliary contact feedback (e.g.,
current sensor on the load side, or a NO auxiliary contact on the relay
wired to a spare GPIO). Then firmware can verify:
1. GPIO commanded ON
2. Auxiliary contact closed (current flows)
3. State confirmed

This is left as a **future hardware revision** per brief §70.

---

## 10. Hardware Interlock (brief §71)

Software interlocks are NOT a substitute for hardware interlocks on
critical loads (motors, generators, transfer switches, contactors).

For loads where simultaneous operation of two channels is physically
dangerous (e.g., motor forward + reverse), the safe approach is:

```
Interlock group: [CH1 (forward), CH2 (reverse)]
Mutual exclusion: CH1 ON → CH2 cannot be ON (and vice versa)
Dead time: 500 ms between OFF of one and ON of other
```

In v4.3.8, this is implemented in software (SafetySupervisor) per brief §9.
For mission-critical applications:

- **Add a hardware interlock** (mechanical, electrical, or solid-state)
- Document the interlock in this contract
- Software interlock complements (does not replace) hardware interlock

---

## 11. Software Cannot Replace Electrical Protection

The firmware provides:
- ✅ Logical safety (maxOnTime, anti-chatter, priority order)
- ✅ Fail-safe behavior (boot policy, safe state)
- ✅ Detection (alarms, audit log, crash forensics)
- ✅ Recovery (OTA rollback, transaction journal)

The firmware does NOT provide:
- ❌ Overcurrent protection (use fuses / MCB)
- ❌ Short-circuit protection (use MCB + RCD/GFCI)
- ❌ Overvoltage protection (use SPD + varistors)
- ❌ Overheating protection (use thermal cutouts)
- ❌ Ground fault protection (use RCD/GFCI)
- ❌ Arc fault protection (use AFCI)
- ❌ Lightning protection (use SPD Type 1 + Type 2)

**Always install proper electrical protection per local electrical code.
Software failure MUST NOT result in uncontrolled physical output —
but software success does not guarantee physical safety.**

---

## 12. Enclosure & Environment

| Parameter | Min | Max | Notes |
|---|---|---|---|
| Operating temperature | -10 °C | +50 °C | ESP32 rated -40 to +85, derate above 50 |
| Operating humidity | 10 % RH | 90 % RH (non-condensing) | SHT31 measures this |
| Enclosure rating | IP54 (indoor) or IP65 (outdoor) | | Vented for heat dissipation |
| Vibration | IEC 60721-3-3 Class 3M2 (indoor) | | For wall-mounted installations |
| Altitude | 0 m | 2000 m | Above 2000 m, derate voltage |

Use DIN rail enclosure for industrial installations. Mount ESP32 + relay
module on DIN-rail adapters for easy replacement.
