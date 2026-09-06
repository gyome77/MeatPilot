# MeatPilot

**Smart control for meat drying and curing.**

An ESP32-S3 controller for a charcuterie curing chamber. It regulates
temperature and humidity, drives internal circulation and fresh-air renewal,
tracks several products by weight, records the drying history and raises clear
alarms when the chamber cannot hold safe conditions.

> ⚠️ **Food-safety disclaimer.** MeatPilot controls the *environment* of a
> curing chamber. It does **not** make meat safe to eat. It measures neither
> water activity (a_w) nor pH. Hygiene, correct salting (including nitrite salt
> where appropriate) and recipes remain the user's responsibility. Software
> alarms do not replace correct curing practice, an independent high/low
> temperature cut-out, or proper electrical protection.

---

## Principles

1. **Local first.** Regulation continues with no Wi-Fi, no Internet, no MQTT and
   no Home Assistant. The network is a viewer, never the regulator.
2. **Fail safe.** Every actuator has a defined safe state after sensor failure,
   restart or communication loss.
3. **Explain every decision.** The controller reports *why* an output is on or
   off, and which timer is blocking it — not just its state.
4. **Provable, not hopeful.** The control engine is a pure function, so the
   whole acceptance suite runs on a laptop in CI.

## Status

Early development. Milestone **M0**: pure control engine plus chamber simulator,
with the specification's §15 acceptance suite green on the host. No hardware
required to build or test this stage.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design,
[docs/PRIOR-ART.md](docs/PRIOR-ART.md) for what was taken from existing
projects, and [docs/BOM.md](docs/BOM.md) for a costed parts list.

## Layout

```
core/       pure C++17 — control, programmes, products, alarms, config
platform/   HAL: esp32/ drivers · sim/ simulated chamber + fault injection
app/        FreeRTOS tasks and wiring
net/        Wi-Fi, NTP, MQTT, Home Assistant discovery, OTA
web/        embedded SPA, REST /api/v1 and WebSocket
docs/       specification, architecture, hardware notes
tests/      host unit, invariant and scenario tests
```

## Editions

| | E1 Economy Panel | E2 Economy Web |
|---|---|---|
| Regulation | fully local | fully local, network-independent |
| Local UI | display, buttons, buzzer | same |
| Remote | none | web UI, REST/WS, OTA, optional MQTT |
| Storage | flash summary | flash + microSD history |
| Target BOM | EUR 70–120 | EUR 150–200 |

One codebase, selected by build profile plus runtime capability detection.
E2 is the current design target.

## Licence

MIT — see [LICENSE](LICENSE).
