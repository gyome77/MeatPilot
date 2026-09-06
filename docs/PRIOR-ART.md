# Prior art — what we took, what we refused

Analysis of the reference projects listed in Appendix A of the specification.
Each was read directly, not summarised from its README alone.

---

## ttiot/curingChamber — Home Assistant integration (Python, MIT, July 2026)

**The most valuable reference by a wide margin**, and the only one written by
someone visibly curing meat rather than demonstrating an architecture.

### Taken

| What | Where it lands in MeatPilot |
|---|---|
| **Pure, time-injected regulation engine** — `tick(inputs, config, now) → {commands, decisions, alerts}` with no I/O | The central design decision. `core/control/engine` |
| `Decision{actuator, desired, reason, blocked_by}` | Satisfies DP-05 structurally rather than by logging |
| `MUTUAL_EXCLUSION` pairs, `DECREASING`/`INCREASING` actuator sets, per-actuator `min_on`/`min_off` | `core/control/guards`, applied as a final filter |
| `SustainedCondition` — onset delay, reminder interval, auto-resolution | `core/alarm/lifecycle`; covers most of spec §5 in one small class |
| Frozen-probe detection via last-change timestamp | `core/sensor/health` |
| **All actuators optional**, including none (monitoring-only) | Runtime configuration model |
| **Degraded mode with manual-action guidance** | New AL-11 |
| **Case hardening, condensation risk, high-temp-during-drying alarms** | New AL-08, AL-09, AL-10 |
| Batch model: reference weight, append-only weigh-ins, per-batch curve and ETA, one batch marked *reference* driving the programme's weight-loss phase | `core/product/` — matches FR-W-07 and FR-W-09 |
| Weight-loss phases carry a max duration as the no-scale fallback | Enforced at config validation |
| Presets shipped as explicitly *indicative* starting points | Required by spec §13.2 |

### Changed

- **Mean → rolling median.** Its `SensorFilter` averages a 3-sample window. A
  mean is not robust to the single-sample spikes an I²C bus over a metre of
  cable in an 85 %RH chamber produces, and spec §4.3 asks for median with
  outlier rejection anyway.
- **Raw values reach the alarm layer.** Spec §4.3 requires alarms to see raw
  readings so a fast excursion is not smoothed away. The reference alarms on
  filtered values only.
- **One global stale timeout → per-channel.** An SHT45 at 0.01 °C resolution
  essentially never repeats a value; a DS18B20 at 0.0625 °C legitimately can. A
  shared timeout produces nuisance alarms on one probe or misses a freeze on the
  other.

### Not applicable

Home Assistant owns the lifecycle, the entities, the storage and the scheduling.
None of that transfers — MeatPilot *is* the platform. The spec's own caution
also applies: very new project, limited field history.

---

## erni313/PorkPi — Raspberry Pi curing controller (Python, 2017, unmaintained)

### Taken

- **Circulate-air and fresh-air-pump as genuinely separate functions** with
  separate schedules. Confirms FR-V-01 and FR-A-01 are a real operational need,
  not over-specification.
- **Four HX711 weighing channels** in a real chamber — evidence that the spec's
  "up to two" is a budget limit rather than a functional one. The data model
  should not hard-code two.
- Watchdog in both hardware and software, and reporting restarts explicitly.

### Refused

- **Configuration and data in Google Sheets.** A cloud dependency in the control
  path — directly contrary to DP-01 and SEC-06.
- **Control by email command.** Unauthenticated, unbounded latency, no
  confirmation. NET-04 requires authenticated, validated, logged commands.
- DHT22 sensing and direct hobby-relay switching, as the spec itself warns.
- Its concern that Python was too slow to bit-bang the HX711, forcing a C
  driver, is moot on an ESP32 with a dedicated driver.

---

## mattcontinisio/meat-curing-chamber — MQTT framework (Python/C++/React/Flutter, 2023)

### Taken

- **Clean per-quantity MQTT topic structure** (`{location}/temperature`,
  `{location}/humidity`), with services free to publish or subscribe
  independently. Informs the MQTT design in §11 of the architecture.
- Services composable so a humidity controller can run without a temperature
  controller — the same "every actuator optional" idea.

### Refused

- **433 MHz remote outlets for switching.** Fire-and-forget with no state
  confirmation. AL-04 requires detecting a command/feedback mismatch; an RF
  outlet makes that structurally impossible.
- **Distributed services with the broker in the control path.** A broker outage
  stops regulation. DP-01 forbids it. MeatPilot's broker is strictly downstream.
- Independent temperature and humidity controllers with no coupling — FR-H-04
  requires the interaction to be accounted for, and uncoupled controllers will
  fight each other.

---

## kizniche/Mycodo and theyosh/TerrariumPI — Raspberry Pi environment control

Both are broad, mature and Pi-centred. Useful as **inspiration for diagnostics
and UI depth** (per-input/output health pages, runtime counters, calibration
workflows — which feed DG-01..06), not as architecture. Neither is specific to
food curing, and both assume a full Linux userland: filesystem, Python runtime,
process supervision and database. None of that exists on an ESP32.

---

## ESPHome / Home Assistant Core

Confirms the value of local embedded automation with remote entities and OTA,
and its component model is a good reference for runtime capability detection.

Rejected as a *base* for the reasons the spec gives: generic components would
still need a curing-specific state machine and safety layer on top, and the
control engine would then live inside someone else's lifecycle — the wrong place
for logic that must satisfy DP-02 and DP-03. MeatPilot instead exposes HA
discovery entities (API-04) so it integrates *with* Home Assistant while keeping
control authority on the ESP32.

---

## Summary of what the prior art changed in our design

1. The engine is a pure function — from `curingChamber`. This is the single
   biggest structural decision in MeatPilot.
2. Four product-quality alarms the specification does not have (AL-08..AL-11).
   Every alarm in the spec asks *"is the equipment working?"*; none asks *"is
   the product being ruined?"*.
3. Manual-action guidance when no actuator can correct a deviation.
4. Confirmation that separate circulation and fresh-air control, and more than
   two weighing channels, are real operational needs.
5. Three concrete anti-patterns to design against: cloud config in the control
   path, unconfirmed RF switching, and a message broker between the sensor and
   the decision.
