# MeatPilot — Software Architecture

**Status:** Draft for review · **Spec:** `ESP32 Meat Drying and Curing Controller`, v1.0
**Target:** ESP32-S3, ESP-IDF v5.x, PlatformIO · **Licence:** MIT

---

## 0. How to read this document

Every design decision below is traceable to a requirement identifier from the
specification (`DP-xx`, `FR-x-xx`, `AL-xx`, `NET-xx`, `API-xx`, `SEC-xx`,
`DG-xx`, `UI-xx`). Decisions that go **beyond** the specification are marked
**[EXT]** and justified — they come from analysing the reference projects in
Appendix A of the spec, principally `ttiot/curingChamber`.

Section 16 is the traceability matrix. If a requirement is not in it, it is not
yet designed.

---

## 1. The one architectural idea

> **The control engine is a pure function. Everything else is I/O.**

```
Outputs Engine::tick(const Inputs& in, const Config& cfg, Timestamp now)
```

No clock reads. No sensor reads. No GPIO. No allocation. No logging side
effects. Time, sensor values and configuration are all *injected*; the result is
a value describing what should happen.

This single constraint is what makes the specification's acceptance criteria
(§15) achievable:

| Spec §15 test | Why purity makes it tractable |
|---|---|
| Sensor disconnection | Feed `temp_valid = false`; assert commands and alarm. No hardware. |
| Frozen sensor | Feed identical values with an advancing injected clock. |
| Compressor protection | Advance the injected clock by seconds; assert no early start. |
| Mutual exclusion | Property test over randomised input sequences. |
| Power cycling ×100 | Destroy and rebuild engine state from persisted journal; assert outputs. |
| Backup sensor divergence | Inject a delta between two probe readings. |

All of these run **on a laptop, in CI, in milliseconds**. Hardware then only has
to prove the drivers, not the logic. This mirrors `curingChamber`'s
`regulation/engine.py`, which is pure and time-injected for exactly this reason.

The corollary: **no ESP-IDF header may be included anywhere under `core/`.**
This is enforced by a CI check, not by convention.

---

## 2. Layers

```
┌──────────────────────────────────────────────────────────────┐
│ app/          FreeRTOS tasks, wiring, lifecycle              │  ESP32 only
│  ├ acquisition_task  control_task  output_task               │
│  └ storage_task      net_task      ui_task                   │
├──────────────────────────────────────────────────────────────┤
│ web/          embedded SPA · REST /api/v1 · WebSocket        │  ESP32 only
│ net/          Wi-Fi, NTP, MQTT, HA discovery, OTA            │  ESP32 only
├──────────────────────────────────────────────────────────────┤
│ platform/     HAL interfaces — the only seam                 │
│  ├ esp32/     SHT45 DS18B20 HX711 relays SD NVS WDT display  │  ESP32 only
│  └ sim/       simulated chamber physics + fault injection    │  host only
├──────────────────────────────────────────────────────────────┤
│ core/         pure C++17 · zero platform dependencies        │  BOTH
│  ├ control/   engine, guards, priority ladder, safe states   │
│  ├ program/   phases, completion conditions, revisions       │
│  ├ product/   batches, weigh-ins, loss %, rate, ETA          │
│  ├ alarm/     rules, severity, latch, ack, escalation        │
│  ├ sensor/    conditioning, validity, health                 │
│  └ model/     config schema, versioning, CRC, serialisation  │
└──────────────────────────────────────────────────────────────┘
```

`core/` and `platform/sim/` build with plain CMake for the host. `app/`, `web/`,
`net/` and `platform/esp32/` build only under ESP-IDF.

**Dependency rule:** arrows point downward only. `core/` knows nothing of
`platform/`; `platform/` knows nothing of `app/`. Satisfies **DP-06** — one
codebase, two editions, because the editions differ only in which upper layers
are compiled in.

---

## 3. The control engine

### 3.1 Tick contract

The engine is called at **1 Hz** from `control_task`. One tick is a complete,
side-effect-free evaluation.

```cpp
struct Inputs {
    Reading  temp;              // filtered value + validity + raw
    Reading  humidity;
    Reading  temp_backup;       // DS18B20 cross-check          (FR-T-06)
    float    dew_point;         // derived, Magnus              (FR-H-01)
    float    absolute_humidity; // derived, g/m³
    bool     door_open;
    Optional<float> co2;                                      // (FR-A-02)
    Optional<float> drying_rate_pct_per_day; // from product service
    ActuatorFeedback feedback[kActuatorCount]; // where fitted (AL-04)
    bool     safety_trip;       // independent thermostat input, priority 1
};

struct Outputs {
    Command   commands[kActuatorCount];   // desired state per actuator
    Decision  decisions[kActuatorCount];  // reason + blocking timer  (DP-05)
    AlarmEventList events;                // raise / remind / clear
    Mode      mode;                       // OFF MANUAL AUTO PROGRAMME MAINT SAFE
    Summary   summary;                    // "cooling", "idle", "safe"…
};
```

`Decision` is not optional telemetry — it is **DP-05** made structural:

```cpp
struct Decision {
    Actuator actuator;
    bool     desired;       // what regulation wanted
    Reason   reason;        // TEMP_ABOVE_BAND, PHASE_SCHEDULE, SAFE_MODE…
    Blocker  blocked_by;    // NONE, MIN_OFF_TIMER, MUTUAL_EXCLUSION, ABS_LIMIT…
    uint32_t blocked_for_s; // remaining time on the blocking timer
};
```

If a command is refused, the reason and the countdown are always available to
the display, the web UI and MQTT. The operator never sees an unexplained OFF.

### 3.2 Three-stage evaluation

The specification's §4.2 priority table is implemented as three ordered stages.
This structure — rather than a single `if/else` cascade — is what makes the
priority guarantees provable.

```
        ┌─────────────────────────────────────────────┐
Stage A │ INHIBIT   (priorities 1–3)                  │  can only force OFF
        │  safety trip · SAFE mode · absolute limits  │
        └────────────────────┬────────────────────────┘
                             │  InhibitionSet {actuator → reason}
        ┌────────────────────▼────────────────────────┐
Stage B │ REQUEST   (priorities 5–8)                  │  proposes ON/OFF
        │  temp → humidity → air exchange → circ.     │
        └────────────────────┬────────────────────────┘
                             │  RequestSet {actuator → desired, reason}
        ┌────────────────────▼────────────────────────┐
Stage C │ GUARD     (priority 4 + invariants)         │  can only force OFF
        │  min ON/OFF · lockout · anti-oscillation    │
        │  · mutual exclusion (final invariant)       │
        └────────────────────┬────────────────────────┘
                             ▼  CommandSet + Decisions
```

Two properties fall out and are asserted as invariants in tests:

- **Stages A and C can only turn an actuator OFF, never ON.** A safety stage
  that can only remove authority cannot itself create an unsafe command.
- **Mutual exclusion is the last thing applied**, as a pure filter over the
  command set. `(COOL, HEAT)` and `(HUMIDIFY, DEHUMIDIFY)` — if both are ON,
  the one with lower regulation priority is dropped. It is impossible for the
  engine to *emit* a violating pair, regardless of what any earlier stage did.
  Satisfies **FR-T-03**, **FR-H-03** and the §15 mutual-exclusion criterion.

Priority 1 (independent electrical trip) is present in the ladder as an
*input*, but the specification is explicit that software cannot override it —
the physical safety thermostat interrupts the load circuit directly. The
software reads its state to alarm and to enter SAFE mode; it is not in the trip
path.

### 3.3 Temperature / humidity coupling

**FR-H-04** requires accounting for the interaction. The rule, matching the
`curingChamber` decision table:

| Temperature | Humidity | Cool | Heat | Humidify | Dehumidify |
|---|---|---|---|---|---|
| above band | any | **ON** | OFF | may compensate | inhibited |
| below band | any | OFF | **ON** | may compensate | inhibited |
| in band | above band | idle | idle | OFF | **ON** |
| in band | below band | idle | idle | **ON** | OFF |

Temperature has priority (**§4.2 rank 5 > 6**). When cooling runs it also dries
the air, so the humidifier is permitted to compensate concurrently; the
dehumidifier is inhibited as redundant. When heating runs, relative humidity
falls for the same reason.

**Anti-oscillation [EXT]:** humidity actuators may change state at most once per
configurable window (default 600 s). Humidity in a small chamber responds slowly
and noisily; without this window a humidifier will short-cycle against its own
mist. Taken from `curingChamber`; supports **AL-05** (repeated short cycling).

### 3.4 Compressor protection — and the cold-boot problem

**FR-T-04** requires min OFF (default 7 min), min ON (default 3 min) and
power-up lockout (default 2 min). **§13.1** requires the lockout to be honoured
across a restart.

There is a subtlety the specification does not resolve. After a power
interruption the controller cannot know how long the compressor has been off
unless it knows the wall-clock time at shutdown *and* at boot. Therefore:

```
On boot, compressor min-OFF remaining =
    if (RTC valid at shutdown) and (RTC valid at boot):
        max(0, min_off − (boot_time − last_compressor_off_time))
    else:
        max(startup_lockout, min_off)     ← assume the worst
```

**Consequence: a DS3231 RTC is not "optional" (§8.2) if you want fast recovery
after a brief power blip.** Without it, every power cycle costs a full 7-minute
compressor lockout. With NTP only, the clock is invalid until Wi-Fi and NTP
succeed — which violates **DP-01**'s spirit if control quality depends on it.
**Recommendation: promote the DS3231 to a required part for E2** (~2 EUR, well
inside the storage-and-time budget group of 15 EUR).

### 3.5 Safe states and boot behaviour

**DP-02** — each actuator has a fail-safe state. In M0 that state is **OFF for
every actuator**, without exception, which is what keeps the "stages A and C can
only force OFF" invariant strict and machine-checkable. Making it configurable
(some operators want the circulation fan to keep running in SAFE mode, to avoid
a stagnant chamber) would weaken the invariant to "never beyond the fail-safe
state", so it is deferred until there is a real reason to want it.

On boot (**§13.1**):
1. All outputs driven to fail-safe **before** any other initialisation.
2. Reset cause recorded and exposed (**DG-01**).
3. Compressor lockout computed per §3.4 above.
4. Sensors must read valid continuously for `stabilisation_period`
   (default 60 s) before automatic control resumes (**§4.3**).
5. Mode is restored from the journal only if `resume_after_power_loss` is
   configured; otherwise the chamber comes up in OFF.

**Hardware constraint driven by software [EXT]:** §15 requires mutual exclusion
to hold *"including during reboot"*. Software cannot guarantee this — an ESP32
GPIO is an input with an indeterminate level for the first milliseconds after
reset, and some pins are strapping pins driven by the bootloader. Therefore:
- Relay-command GPIOs **must not** be strapping pins (GPIO0, 3, 45, 46 on S3).
- Each relay input **must** have an external pull resistor to the de-energised
  level, sized to dominate the ESP32's internal pull.
- Opto-isolated boards are typically active-LOW; verify polarity per board.

This is recorded in `docs/HARDWARE.md` as a build requirement, not left to the
firmware.

### 3.6 Fail-safe on control-task stall [EXT]

Independently of the watchdog, `output_task` holds a freshness deadline. If
`control_task` has not published a command set within `command_ttl` (default
5 s), `output_task` drives every actuator to its fail-safe state and raises a
critical alarm. This catches a hung or starved control task even when the task
watchdog would take longer, and it costs one timestamp comparison.

Manual commands (**MANUAL** mode) carry their own expiry and are re-validated by
the same guard stage as automatic commands (**§4.3**, **NET-04**).

---

## 4. Sensor conditioning and health

`core/sensor/` — pure, time-injected, one instance per channel.

```
raw ──► range check ──► rolling median (n=5) ──► EMA low-pass ──► filtered
   │                                                                 │
   └──────────────────────── both reach the alarm layer ─────────────┘
```

**§4.3** requires rolling median with outlier rejection followed by a light
low-pass, and requires the alarm logic to also examine **raw** readings so a
rapid excursion is not hidden by the filter. `curingChamber` uses a plain mean;
ours uses a median because a mean is not robust to the single-sample spikes an
I²C bus over a metre of cable in an 85 %RH chamber actually produces.

Validity (`Reading::valid`) is the conjunction of:

| Check | Failure | Requirement |
|---|---|---|
| Present and answering | `UNAVAILABLE` | AL-02 |
| Within physical range | `OUT_OF_RANGE` | AL-02 |
| Changed within `stale_s` | `FROZEN` | AL-02, §15 |
| Continuously valid for `stabilisation_period` | `STABILISING` | §4.3 |
| Agrees with backup probe | `DIVERGENT` | FR-T-06, AL-02 |

**Stabilisation is tracked per quantity, not globally.** Replacing the humidity
probe must not restart the settling window for temperature, and — the case that
actually bit during implementation — a *missing* probe must be reported as
`SENSOR_INVALID`, not as `STABILISING`. A global flag conflates "waiting for a
good reading" with "there is no reading", which is precisely the distinction
DP-05 exists to preserve.

**Frozen-sensor tuning note:** an SHT45 reports to 0.01 °C and will essentially
never repeat a value, so a long `stale_s` (default 1800 s) is safe. A DS18B20 at
0.0625 °C resolution *can* legitimately sit still in a stable chamber — its
`stale_s` must be configured separately and longer. One global timeout would
produce nuisance alarms; the design uses per-channel timeouts.

**Divergence handling (FR-T-06):** when SHT45 and DS18B20 disagree by more than
`divergence_temp` (default 2.0 °C), raise a warning, and record **which source
was selected for control** in the decision log — the §15 acceptance test asks
for exactly that. Selection policy: keep the primary unless the primary is
invalid; never silently switch.

**Humidity failure (FR-H-05):** implausible, saturated (RH ≥ 99 % sustained) or
frozen humidity stops humidity control entirely and enters the configured safe
state. Temperature control may continue if temperature is independently valid.

**Calibration (FR-H-06, DG-05):** offset + gain per channel, persisted with the
configuration, applied before filtering. Load cells get tare + span with a
guided workflow.

---

## 5. Alarm model

`core/alarm/` — every alarm is a `SustainedCondition` with onset delay,
reminder interval and automatic resolution (the anti-spam core, adapted from
`curingChamber/regulation/degraded.py`).

```
        ┌──────┐  active ≥ onset_delay   ┌─────────┐
        │ IDLE ├────────────────────────►│ RAISED  │
        └──▲───┘                         └────┬────┘
           │                          ack     │  every reminder_interval
           │ condition clears              ┌──▼──────┐   while unacknowledged
           │  (+ ack, if latched)          │ REMIND  │
           └───────────────────────────────┴─────────┘
```

Per **§5**, a CRITICAL alarm **latches**: it stays visible after the condition
clears, until acknowledged. WARNING and INFO auto-resolve.

Every record carries: severity, timestamp, current values, the limits that were
breached, the affected product/programme, the automatic action taken,
acknowledgement status and resolution timestamp (**§5**).

### 5.1 Alarm catalogue

| ID | Condition | Severity | Source |
|---|---|---|---|
| AL-01 | Temp/RH high or low; warning delayed, critical immediate | W/C | spec |
| AL-02 | Sensor unavailable / out of range / frozen / divergent | C | spec |
| AL-03 | Actuator ON with no environmental response in `response_timeout` | W | spec |
| AL-04 | Feedback inconsistent with command (stuck relay) | C | spec |
| AL-05 | Door open too long; short cycling; excessive runtime; starts/hour | W | spec |
| AL-06 | Low supply voltage, reboot loop, storage fault, clock invalid, network down, notify failure | W/C | spec |
| AL-07 | Weight channel disconnected / unstable / implausible step / gain drift / missed weigh-in | W | spec |
| **AL-08** | **Case hardening** — RH durably below band, or drying rate > `case_hardening_rate` (default 1.5 %/day) | W | **[EXT]** |
| **AL-09** | **Condensation risk** — chamber temp within `condensation_margin` (default 1.0 °C) of dew point | W | **[EXT]** |
| **AL-10** | **High temperature during drying** — sustained above `high_temp_drying_limit` (default 16 °C) for `high_temp_drying_duration` (default 2 h), independent of the absolute limit | C | **[EXT]** |
| **AL-11** | **Degraded mode** — a quantity is outside the extended band and **no configured actuator can correct it** | W | **[EXT]** |

**Why AL-08 to AL-11 belong in the product.** The spec's alarms all answer
*"is the equipment working?"*. None answers *"is the product being ruined?"* —
which is the question the user actually cares about. Case hardening (*croûtage*)
is the classic charcuterie failure: the surface dries into a shell that traps
moisture inside, and no temperature or humidity limit alarm detects it, because
every reading stays in range while the *rate* is wrong. Condensation is free —
dew point is already computed for **FR-H-01**. All four come from
`curingChamber`, which is the only reference project written by someone actually
curing meat.

### 5.2 Degraded mode and manual-action guidance [EXT]

**§1.3 DP-05** says expose the reason for each decision. AL-11 extends that to
the case where there is no decision to make because no actuator exists.

If humidity sits below the extended band for 15 minutes and **no humidifier is
configured**, the correct output is not silence — it is *"Humidity 62 % (target
75 %): place a tray of salted water in the chamber"*. Every actuator in this
design is optional (a chamber may have cooling only, or nothing at all), so this
is a common state, not an edge case.

Manual actions: `ADD_HUMIDITY`, `REMOVE_HUMIDITY`, `ADD_COOLING`, `ADD_HEATING`,
`VENTILATE`. Delivered to the local display, the web UI and MQTT.

---

## 6. Programme engine

`core/program/` — a programme is an ordered, **revisioned** list of phases
(**§4.1**). A `Run` references a programme *revision*, so editing a recipe never
rewrites the history of a batch already drying against it.

```cpp
struct Phase {
    Name  name;                       // "Fermentation", "Drying", "Holding"
    Optional<float> target_temp, temp_deadband;
    Optional<float> target_humidity, humidity_deadband;
    CirculationRule circulation;      // continuous_low | interval | demand
    FreshAirRule    fresh_air;        // interval | schedule | co2_threshold
    EndCondition    end;              // duration | manual | product_loss
                                      //   | reference_batch_loss
                                      //   | first_of_targets | last_of_targets
    uint32_t max_duration_h;          // safety fallback when weight is absent
    AlarmLimits limits;               // phase-specific warning/critical bounds
};
```

`max_duration_h` is mandatory on any weight-driven phase: if the scale fails or
no weigh-in is entered, the phase must still terminate. The specification calls
this a "safety fallback"; the engine treats a weight-driven phase without one as
an invalid configuration and refuses it at validation time (**UI-05**).

Built-in presets ship as starting points (saucisson sec, coppa, bresaola,
pancetta, lonzo, cellar hold), clearly labelled **indicative** — **§13.2**
forbids presenting them as validated food-safety advice.

---

## 7. Products and weight tracking

`core/product/` — **FR-W-01** requires 20 active + 100 archived products.

- Weigh-in history is **append-only**; a new reading never overwrites an
  earlier one (**FR-W-07**).
- `loss_pct = 100 × (initial − current) / initial` (**FR-W-05**).
- Drying rate is a least-squares slope over the recent window, not a two-point
  difference — two-point is dominated by weigh-in noise and produces a useless
  ETA. ETA is the linear projection of that slope to `target_loss_pct`
  (**FR-W-06**).
- Several products share one programme while keeping independent progress
  (**FR-W-09**); one is marked **reference** and drives weight-based phase
  completion.
- Plausibility checks (**FR-W-08**): negative loss, step change beyond
  `max_step_pct`, disconnected load cell, and `days_since_last_weigh_in` beyond
  a threshold → AL-07.

**Sizing:** 20 active products × ~200 weigh-ins × 24 bytes ≈ 96 KB. Comfortable
on microSD, too large for NVS. Active products live in a LittleFS/SD file with
an in-RAM index; archived products live on SD only.

---

## 8. Configuration and persistence

### 8.1 Configuration — NVS, versioned, CRC, A/B slots

**DP-04** and **UI-05** require survival of power loss and atomic writes with
rollback to the last valid configuration.

```
NVS namespace "meatpilot"
  ├ cfg_active   : uint8   (0 or 1)
  ├ cfg_slot_0   : blob    { schema_version, payload, crc32 }
  ├ cfg_slot_1   : blob    { schema_version, payload, crc32 }
  └ journal      : blob    { mode, run_id, phase_index, phase_started_at,
                             compressor_last_off_at, actuator_runtimes,
                             latched_alarms, reset_count }
```

Write sequence: serialise → CRC → write **inactive** slot → read back and verify
CRC → flip `cfg_active`. A power loss at any point leaves at least one valid
slot. On boot, prefer `cfg_active`; if its CRC fails, fall back to the other
slot and raise AL-06.

`schema_version` drives forward migration. An unknown (newer) version is not
silently accepted — it falls back and alarms.

### 8.2 Time series — append-only, fixed-size records

**§7** requires ≥ 30 days of down-sampled history on microSD.

Fixed-width binary records, one file per UTC day, so seeking to a time offset is
arithmetic rather than parsing:

```
/sd/history/YYYY-MM-DD.mpt      32-byte records, 1 per minute
  uint32 ts        float t_raw  float t_filt  float rh_raw
  float  rh_filt   float dew    uint16 outputs_bitmap  uint16 quality_flags
```

1440 records/day × 32 B = **46 KB/day**, **1.4 MB/month**. A 4 GB card holds
years. Recent high-resolution data (5 s) lives in a RAM ring buffer for the live
chart and is not persisted.

Recovery: the file is append-only and each record is self-contained, so a
truncated final record is discarded on open — no journal replay needed.

Storage failure (**§13.1**): regulation continues from RAM/NVS, logging is
reduced, AL-06 raised. **Storage is never in the control path.**

Export: CSV and JSON for products, weigh-ins, history, alarms and audit
(**§7**).

---

## 9. Task and concurrency model

**§8.1** requires acquisition / control / output / storage / network / UI
separation, with control and output above network and UI, and a hardware
watchdog on the control path.

| Task | Prio | Core | Period | Watchdog |
|---|---|---|---|---|
| `output_task` | 6 | 1 (APP) | 100 ms + event | ✅ subscribed |
| `control_task` | 5 | 1 (APP) | 1 s | ✅ subscribed |
| `acquisition_task` | 4 | 1 (APP) | 1 s (per-sensor rates) | ✅ subscribed |
| `ui_task` (display, buttons, buzzer) | 3 | 1 (APP) | 50 ms | — |
| `storage_task` | 2 | 0 (PRO) | queue-driven | — |
| `net_task` (Wi-Fi, web, MQTT, NTP, OTA) | 1 | 0 (PRO) | event-driven | — |

**Core pinning is the mechanism behind DP-01.** Wi-Fi and lwIP live on core 0
and can saturate it; control, acquisition and output live on core 1 and are
never blocked by a network stall. A Wi-Fi disconnect storm cannot delay a
compressor decision.

Inter-task communication is by **queue and immutable snapshot only** — no shared
mutable state, no mutex on the control path. `acquisition_task` publishes an
`Inputs` snapshot; `control_task` publishes a `CommandSet`; `output_task`
consumes it and applies the freshness deadline of §3.6. `storage_task` and
`net_task` receive copies and can never block a producer (bounded queues, drop
oldest with a counter, exposed in diagnostics).

Sampling rates: temperature ≤ 5 s (**FR-T-01**), humidity ≤ 10 s
(**FR-H-01**) — both satisfied by the 1 s acquisition tick with per-sensor
divisors.

---

## 10. Web interface and API (E2)

**API-01/02**, **UI-01/03/04**.

Served from the ESP32 (**UI-01**), usable from a phone with no app install.
Static assets are gzip-precompressed and stored in a LittleFS partition, served
with `Content-Encoding: gzip` and long-lived `ETag`s. Budget: **≤ 200 KB
gzipped**, which rules out React and points at Preact or vanilla + a small chart
renderer.

```
GET    /api/v1/status                 live snapshot: T/RH/dew, setpoints,
                                       outputs + decisions, phase, alarms, net
GET    /api/v1/stream                 WebSocket, 1 Hz push          (API-02)
GET    /api/v1/history?from=&to=&res= down-sampled series
GET/PUT/api/v1/config                 validated, atomic, versioned  (UI-05)
GET/POST/PUT/DELETE /api/v1/programs
POST   /api/v1/run/{start|stop|pause|resume|next_phase}
GET/POST/PUT/DELETE /api/v1/products
POST   /api/v1/products/{id}/weighins
GET    /api/v1/alarms      POST /api/v1/alarms/{id}/ack
GET    /api/v1/diagnostics                                          (DG-01..04)
POST   /api/v1/ota                    signed image, rollback        (API-05)
GET    /api/v1/export/{csv|json}/{entity}
```

The API is **versioned in the path** and is the same surface the local display
consumes internally — the display is a second client of the control engine, not
a parallel implementation.

Commissioning (**UI-02**): SoftAP + captive portal on first boot; a long-press
on a physical button re-enters setup mode. Wi-Fi credentials in NVS, never in a
config export (**SEC-02**).

**Offline behaviour:** the SPA caches its own assets; when the WebSocket drops
it shows a clear "controller offline" banner rather than stale values. The
chamber keeps regulating regardless (**DP-01**, **UI-06**).

---

## 11. Connectivity — MQTT and Home Assistant

**API-03/04**, **NET-03**, **§6.2**.

- MQTT over TLS to a **private** broker, unique credentials, topic-scoped
  permissions. Outbound only — no inbound port forwarding (**NET-01**).
- Publish: state (retained), alarms, decisions, product progress. Subscribe: a
  **limited, explicitly enumerated** command set.
- **NET-04** — every remote command passes through the same guard stage as a
  local one. There is no privileged path. A remote "compressor ON" is subject to
  min-OFF exactly like a button press.
- Home Assistant discovery entities (**API-04**) for monitoring and commands;
  the ESP32 retains control authority. HA is a viewer, never the regulator.
- Offline queue: alarm events are buffered (bounded, oldest-dropped) while the
  broker is unreachable and flushed on reconnect (**§13.1**, **§15 offline
  control**).

---

## 12. Security

| Req | Design |
|---|---|
| SEC-01 | Unique per-device initial admin credential derived from the eFuse MAC + factory salt, printed at commissioning; forced change on first login. |
| SEC-02 | Argon2id or PBKDF2-SHA256 verifier in NVS. Config exports omit secrets by default and say so. |
| SEC-03 | Signed OTA (RSA-3072 or ECDSA-P256) + dual app partitions + rollback on failed self-test. Secure boot v2 and flash encryption enabled on production modules. |
| SEC-04 | Login rate limit with exponential backoff; auth and config changes written to the audit log (**§7**). |
| SEC-05 | No Telnet. UART console disabled in the release build. JTAG eFuse-disabled on production units. |
| SEC-06 | No cloud dependency of any kind. Remote access via VPN (preferred) or authenticated TLS. |

**HTTPS position.** **NET-02** says "where hardware permits". On an ESP32-S3 a
TLS server means a self-signed certificate (a browser warning on every visit,
which trains users to click through warnings) and ~40 KB RAM per session. The
recommended posture is therefore:

- **Default:** HTTP on the local network, strong auth, session timeout, with
  **NET-01** enforced by never exposing the device to the Internet.
- **Remote:** VPN into the LAN — the spec's own preferred pattern (**§6.2**).
- **TLS where it earns its cost:** outbound MQTT with a pinned CA, where there
  is no certificate-warning UX problem and the server identity is verifiable.
- HTTPS server as a build-time option for users who want it.

This is a deliberate, documented reading of NET-02, not an omission. Flag it if
you disagree — it is reversible but affects the memory budget.

---

## 13. Build profiles

**DP-06** — one codebase, two editions.

```cmake
set(MEATPILOT_EDITION "web" CACHE STRING "panel | web")
```

| | E1 Panel | E2 Web |
|---|---|---|
| `core/`, `platform/esp32/` sensors + outputs | ✅ | ✅ |
| Display, buttons, buzzer, LED | ✅ | ✅ |
| `net/`, `web/`, MQTT, OTA | ✗ | ✅ |
| microSD history | optional | ✅ |
| HX711 channels | optional | ✅ (2) |

Plus **runtime** capability detection (**DP-06**): probe I²C for the display and
RTC, 1-Wire for the backup probe, SPI for the card. A missing optional device
degrades to a diagnostic entry, never a boot failure.

Per your scope decision, **E2 is the design target**; E1 is kept building in CI
from day one so it cannot silently rot, but its button-driven configuration UX
is deferred — recipes and products are configured over USB-serial or a temporary
AP on E1.

---

## 14. Test strategy

### 14.1 Host tests (the majority)

`ctest` over `core/` + `platform/sim/`, run on every commit.

- **Unit** — hysteresis, guards, filters, dew point, loss %, ETA, CRC,
  serialisation round-trips.
- **Invariant / property** — over randomised input and time sequences:
  - mutual exclusion never violated in any emitted command set;
  - min-OFF never violated for any actuator;
  - stages A and C never turn an actuator ON;
  - every emitted command carries a non-empty reason.
- **Scenario** — one test per row of the spec's §15 table, named after it.
- **Fault injection** — `platform/sim` can, on command: remove a probe, freeze a
  probe, inject probe divergence, stick a relay, fail the SD card, brown out,
  reboot mid-write.

### 14.2 The simulated chamber

`platform/sim` models the chamber as coupled first-order systems: thermal mass
with an ambient leak term, water-vapour mass with actuator sources and sinks,
door-open as a step disturbance. It does not need to be quantitatively accurate
— it needs the **right sign and the right time constants**, so that AL-03
("actuator commanded ON without expected response") can be tested both ways: it
must fire when the sim's cooling is disabled, and must not fire when cooling
works but is slow.

### 14.3 Hardware tests

Only what the simulator cannot prove: driver correctness, timing on the real
I²C/1-Wire buses, relay polarity, boot-time GPIO levels (measured with a scope
for the §15 "no unsafe output pulse" criterion), OTA rollback on a real
partition table, and the endurance runs.

---

## 15. Roadmap

Mapped to the spec's §16 releases, re-sequenced for a software-first start.

| Milestone | Content | Exit |
|---|---|---|
| **M0** *(in progress)* | `core/` engine + `platform/sim` + full §15 suite green in CI. No hardware. | All §15 rows pass on host; invariants hold under property testing |
| **M1** | Programme engine, product/weigh-in service, alarm catalogue incl. AL-08..11, config schema + persistence, all host-tested | 30-day simulated run with recipe, batches and injected faults |
| **M2** = R0 | ESP32-S3 bring-up: SHT45, DS18B20, relays, display, buttons, watchdog | 72 h bench run, correct hysteresis and restart behaviour |
| **M3** = R1 | Web UI, REST + WebSocket, microSD history, CSV export, signed OTA | 30-day chamber test; mandatory acceptance tests pass |
| **M4** = R2 | MQTT/TLS, notifications, HA discovery, backup/restore | Offline/online recovery and security tests pass |
| **M5** = R3 | Reliability hardening, brownout recovery, protected outputs, feedback inputs | 90-day endurance + fault injection within EUR 200 BOM |
| **M6** = R4 | HX711 channels, stability detection, tare/span workflow, ETA from auto-weighing | Reference-weight and drift tests pass |

M0 and M1 need no hardware at all — that is roughly half the product, buildable
now.

---

### 15.1 M0 progress

Done: the three-stage engine, hysteresis, compressor and anti-oscillation
guards, mutual exclusion, absolute limits, temperature/humidity coupling,
circulation and fresh-air scheduling, door handling, and 35 host tests covering
unit behaviour, six randomised invariants and the section 15 rows that are
decidable at engine level.

Remaining for M0: `platform/sim` chamber physics (so AL-03 "commanded ON with no
response" can be tested both ways), and `core/sensor` conditioning, which turns
the frozen-probe and divergence rows from injected `valid=false` flags into
tests of the real detection logic.

---

## 16. Traceability matrix

| Requirement | Module | Test |
|---|---|---|
| DP-01 local autonomy | core pure; net_task on core 0 | `scenario/offline_24h` |
| DP-02 fail-safe states | `control/safe_state` | `invariant/failsafe_on_fault` |
| DP-03 compressor protection | `control/guards` | `scenario/compressor_protection` |
| DP-04 survives power loss | `model/persistence` | `scenario/power_cycle_100` |
| DP-05 decision reasons | `control::Decision` | `invariant/every_command_has_reason` |
| DP-06 one codebase | CMake profiles | CI builds both editions |
| FR-T-01..06 | `control/temperature`, `sensor/` | `unit/temperature_*` |
| FR-H-01..06 | `control/humidity`, `sensor/derived` | `unit/humidity_*` |
| FR-V-01..05 | `control/circulation` | `unit/circulation_*` |
| FR-A-01..04 | `control/fresh_air` | `unit/fresh_air_*` |
| FR-W-01..09 | `product/` | `unit/product_*`, `scenario/multi_product` |
| §4.2 priority ladder | `control/engine` stages A/B/C | `invariant/priority_ladder` |
| §4.3 filtering | `sensor/filter` | `unit/filter_median_lowpass` |
| AL-01..07 | `alarm/rules` | `scenario/alarm_*` |
| AL-08..11 **[EXT]** | `alarm/quality`, `alarm/degraded` | `unit/quality_alarms` |
| §5 latch + ack | `alarm/lifecycle` | `unit/alarm_latch` |
| UI-01..06 | `web/`, `app/ui_task` | manual + integration |
| NET-01..04 | `net/`, guard stage | `unit/remote_command_guarded` |
| §7 data model | `model/`, `storage/` | `unit/serialisation_roundtrip` |
| §8.1 tasks | `app/` | hardware integration |
| API-01..05 | `web/api` | `integration/api_contract` |
| SEC-01..06 | `net/auth`, partitions | security review + `unit/auth_*` |
| §13.1 failure responses | `control/`, `app/` | `scenario/fault_injection_*` |
| DG-01..06 | `app/diagnostics`, `web/` | `integration/diagnostics` |
| §15 acceptance | — | one test per row, same names |

---

## 17. Decisions taken, and what they cost

| Decision | Rationale | Cost / risk |
|---|---|---|
| ESP-IDF + PlatformIO | Only path to real task priorities, TWDT, secure boot, dual-OTA rollback (SEC-03, API-05) | Slower start than Arduino; fewer ready-made drivers |
| Pure `core/`, host-tested | Makes §15 executable in CI in milliseconds | Discipline: a CI check must forbid ESP headers under `core/` |
| Three-stage ladder | Priority guarantees become provable invariants | More structure than a simple `if/else` cascade |
| Median filter over mean | Robust to I²C spikes over a long probe cable | Marginally more CPU; negligible |
| Promote DS3231 to required | Without it every power blip costs a 7 min compressor lockout | +2 EUR, one I²C address |
| HTTP + VPN by default, TLS opt-in | Self-signed HTTPS trains users to dismiss warnings; VPN is the spec's own preference | Documented deviation from a literal reading of NET-02 |
| Add AL-08..AL-11 | The spec alarms on equipment; these alarm on the product | Four more rules, ~150 lines, all host-tested |
| E2-first, E1 in CI | E1's button-only config UX is a large design cost for little value now | E1 ships later than E2 |
| Append-only fixed-width history | Crash-safe with no journal; O(1) seek | Fixed schema per file version |

---

## 18. Open items

1. **Chamber and actuator inventory.** Which of cooling / heating / humidifier /
   dehumidifier / circulation / fresh-air actually exist, and their ratings.
   Every actuator is optional in the design, so this does not block M0 — but it
   determines the default configuration and which degraded-mode messages matter.
2. **Does the fridge restart on its own after power loss?** If its internal
   thermostat closes on power-up, the ESP32's compressor lockout is bypassed by
   the appliance. This may require the fridge's own thermostat to be defeated
   and the compressor switched only through our contactor.
3. **Probe cable length**, which decides I²C extender vs. shorter run vs.
   leaning harder on 1-Wire.
4. **Number of weighing channels** (spec says up to 2 in budget; PorkPi ran 4)
   and required resolution.
5. **Fermentation above drying temperature** — affects whether a heater is
   required and the AL-10 threshold.
6. **Retention beyond 30 days**, and whether history must sync anywhere.

None of these block M0 or M1.
