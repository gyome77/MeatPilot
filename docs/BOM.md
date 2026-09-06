# MeatPilot — Bill of materials (Economy Web / E2)

**Sourced on amazon.fr, 6 September 2026.** Prices and availability drift, and
Amazon listings change seller and ASIN without notice — treat every figure here
as indicative and re-check before ordering. Every link below was opened and its
price confirmed at the time of writing; none is invented.

> ⚠️ **Mains work.** Everything in group 4 switches mains voltage. Wiring,
> fusing, earthing, contactor sizing and enclosure construction must be done or
> reviewed by a competent person (spec §13.2). The relay boards below are
> control signals only — they must not directly switch a compressor.

---

## Summary against the spec's §9.1 budget

| Cost group | Spec target | This BOM | |
|---|---:|---:|---|
| 1 · Controller, display, controls | €30 | €29.69 | ✅ |
| 2 · Environmental sensing | €25 | €23.48 | ✅ |
| 3 · Storage and time | €15 | €39.65 | ❌ over — see note |
| 4 · Relay interface and protection | €25 | €20.78 | ✅ |
| 5 · Power, enclosure, wiring | €40 | €38.68 | ✅ |
| 6 · Weight option (2 channels) | €40 | €14.49 | ✅ |
| **Controller electronics subtotal** | **€175** | **€166.77** | ✅ |
| 7 · Small parts, contingency | €25 | ~€25 | estimate |
| **Total** | **€200** | **~€192** | ✅ **within budget** |

Plus, outside the controller budget, the independent safety cut-out required by
§13.2: **€39.99** (see group 8). That device protects the product when the
firmware is wrong, so it is not optional — but it is a chamber appliance rather
than controller electronics, which is the category the spec's §9.1 excludes.

**Why group 3 blows its target:** the €15 line assumed a commodity microSD.
MeatPilot appends to the card every minute, for years. Commodity cards fail
under that pattern, and a card that dies takes the drying history with it. A
high-endurance card is €26 rather than €8. Capacity is irrelevant here — the
history is 1.4 MB/month, so even 8 GB would last a decade — you are buying write
endurance, not space. Groups 2, 4 and 6 come in under target and absorb it.

---

## Group 1 — Controller, display and controls

| Item | Qty | Price | Link |
|---|---:|---:|---|
| ESP32-S3 DevKitC-1 **N16R8** (16 MB flash, 8 MB PSRAM) | 1 | €11.71 | [B0FZGJYPHK](https://www.amazon.fr/dp/B0FZGJYPHK) |
| OLED 1.3″ I²C SH1106 128×64 (2-pack) | 1 | €11.99 | [B0DFCKSWH9](https://www.amazon.fr/dp/B0DFCKSWH9) |
| Active 5 V buzzer (10-pack) | 1 | €5.99 | [B07RDHNT1P](https://www.amazon.fr/dp/B07RDHNT1P) |
| | | **€29.69** | |

**N16R8, not N8R8.** The spec asks for 8 MB flash; 16 MB costs the same here and
matters, because signed OTA with rollback (API-05, SEC-03) needs *two* app
partitions plus a LittleFS partition for the web assets. 8 MB is workable but
leaves nothing spare.

**These are clone boards.** Fine for development, but SEC-03 wants secure boot
and flash encryption, which means trusting the module's eFuses. For the unit you
actually leave running unattended, buy a genuine Espressif ESP32-S3-DevKitC-1
from Mouser or DigiKey (~€18). Develop on the cheap one.

**Panel buttons** (3–4 needed, UI-06) aren't listed: 12 mm tactile switches or
16 mm panel-mount buttons are ~€8 for a pack and the choice depends on your
enclosure's front panel. Budgeted in group 7.

---

## Group 2 — Environmental sensing

| Item | Qty | Price | Link |
|---|---:|---:|---|
| SHT45 I²C temperature/humidity module | 1 | €15.99 | [B0H1WW65ZP](https://www.amazon.fr/dp/B0H1WW65ZP) |
| DS18B20 waterproof probe, 1 m cable (2-pack) | 1 | €7.49 | [B0H29YXWPY](https://www.amazon.fr/dp/B0H29YXWPY) |
| | | **€23.48** | |

Cheaper SHT45 alternative at €13.99: [B0GMJDHMNV](https://www.amazon.fr/dp/B0GMJDHMNV).
Known-good but pricier, with proper documentation and a Qwiic connector:
[Adafruit SHT45, €30.56](https://www.amazon.fr/dp/B0DXD7MT5S).

**Two warnings that matter more than the price.**

1. **Condensation kills the reading, not the sensor.** At 13 °C and 78 %RH the
   chamber sits a couple of degrees from dew point. A bare SHT45 will wet out
   and read 100 %RH until it dries. It needs a PTFE membrane cap or an
   expanded-PTFE filter, and mounting away from humidifier mist, the cooling
   coil discharge and the fan jet (spec §13.2). This is the single most common
   way a curing-chamber build fails.
2. **I²C does not like cable.** This is the weak link flagged in the
   architecture (§18 item 3). Keep the SHT45 run under ~1 m with a twisted pair
   and strong pull-ups, or fit a P82B715 bus extender. The DS18B20 is 1-Wire and
   genuinely cable-tolerant — which is exactly why it is the cross-check probe
   (FR-T-06) and not the other way round.

---

## Group 3 — Storage and time

| Item | Qty | Price | Link |
|---|---:|---:|---|
| microSD SPI module, 3.3 V/5 V | 1 | €6.37 | [B0D8Q8N7NQ](https://www.amazon.fr/dp/B0D8Q8N7NQ) |
| DS3231 + AT24C32 I²C RTC | 1 | €6.99 | [B01H5NAFUY](https://www.amazon.fr/dp/B01H5NAFUY) |
| SanDisk High Endurance microSD 64 GB | 1 | €26.29 | [B07P3D6Y5B](https://www.amazon.fr/dp/B07P3D6Y5B) |
| | | **€39.65** | |

**The DS3231 is not optional**, despite the spec listing it that way (§8.2). Per
[architecture §3.4](ARCHITECTURE.md), compressor min-OFF cannot be computed
across a power cut without a valid clock at both shutdown and boot. Without an
RTC, every power blip costs a full 7-minute compressor lockout, and NTP can't
help because it needs Wi-Fi — which DP-01 says we must not depend on. €7 buys
that back.

**Check the SD module's level shifting.** Many cheap boards regulate 5 V→3.3 V
for the card but leave the SPI lines at 5 V logic, which is out of spec for an
ESP32-S3 and fails intermittently — the worst failure mode. The one linked
states 3.3 V/5 V operation; verify on arrival, and if in doubt use a module with
a visible level shifter IC.

64 GB is cheaper than the 32 GB high-endurance card (€25.04,
[B07P14QHB7](https://www.amazon.fr/dp/B07P14QHB7)) at the time of sourcing.
Neither capacity is remotely needed; buy whichever is cheaper on the day.

---

## Group 4 — Relay interface and mains switching

| Item | Qty | Price | Link |
|---|---:|---:|---|
| 4-channel 5 V opto-isolated relay module (**2-pack** = 8 channels) | 1 | €9.99 | [B0DJVYZH7N](https://www.amazon.fr/dp/B0DJVYZH7N) |
| Modular DIN contactor 20 A 2NO, 230 V coil | 1 | €10.79 | [B09TH1Y37P](https://www.amazon.fr/dp/B09TH1Y37P) |
| | | **€20.78** | |

**Six outputs, not four.** MeatPilot drives cool, heat, humidify, dehumidify,
circulate and fresh air (FR-T-02, FR-H-02, FR-V-01, FR-A-01). A single
4-channel board is not enough — hence the 2-pack, which gives 8 channels for
less than one ELEGOO 4-channel board ([B06XKST8XC](https://www.amazon.fr/dp/B06XKST8XC), €9.99,
better build quality if you prefer to buy two of those instead).

**One contactor is the minimum, for the compressor.** The spec is explicit that
a PCB relay must not switch a compressor directly (§13.2). Add a contactor for
any other inductive or high-current load — a dehumidifier compressor, a heater
above a few hundred watts. A small ultrasonic humidifier and a PC fan can go on
the relay contacts directly, if you verify the board's real rating rather than
its printed one.

**A board-level requirement that comes from a software test.** Spec §15 demands
mutual exclusion hold *including during reboot*. Firmware cannot guarantee that:
ESP32 GPIOs float for milliseconds after reset. So, per
[architecture §3.5](ARCHITECTURE.md):

- relay-command GPIOs **must not** be strapping pins — on the S3 avoid GPIO 0,
  3, 45 and 46;
- each relay input **needs an external pull resistor** to the de-energised
  level, sized to dominate the ESP32's internal pull (~10 kΩ against the
  board's own ~4.7 kΩ input, so 2.2 kΩ or lower);
- these boards are **active-LOW** — a floating input reads as ON. Confirm
  polarity with a meter before connecting anything to mains.

---

## Group 5 — Power, enclosure and wiring

| Item | Qty | Price | Link |
|---|---:|---:|---|
| Mean Well HDR-15-5 DIN PSU, 5 V 2.4 A | 1 | €9.24 | [B07BDTMZQ6](https://www.amazon.fr/dp/B07BDTMZQ6) |
| IP65 enclosure, 12 DIN modules, clear cover | 1 | €22.99 | [B0DTHHSTZZ](https://www.amazon.fr/dp/B0DTHHSTZZ) |
| MC-38 wired magnetic door contact | 1 | €6.45 | [B093LBSDH1](https://www.amazon.fr/dp/B093LBSDH1) |
| | | **€38.68** | |

**Power budget check:** 8 relay coils × ~70 mA = 560 mA, ESP32-S3 with Wi-Fi
bursts to ~500 mA, display and sensors ~50 mA. Peak ≈ 1.15 A against the
HDR-15-5's 2.4 A. Comfortable. The 3 A HDR-30-5
([B06XWRSXL9](https://www.amazon.fr/dp/B06XWRSXL9), €18.59) buys headroom you
don't need. A genuine Mean Well matters here — spec §8.3 asks for a certified
supply, and brownout is a listed failure mode (AL-06).

12 modules gives room for the PSU, contactor, terminals and DIN rail with space
to work. An 8-module box at €17.70
([B01N2OP64F](https://www.amazon.fr/dp/B01N2OP64F)) fits if you're disciplined.

The door contact feeds FR-V-04 (door-open pause and post-door circulation) and
AL-05 (door open too long).

---

## Group 6 — Automatic weighing (optional, R4/M6)

| Item | Qty | Price | Link |
|---|---:|---:|---|
| HX711 + 5 kg load cell, **2 sets** | 1 | €14.49 | [B0FZW5RQJ1](https://www.amazon.fr/dp/B0FZW5RQJ1) |
| | | **€14.49** | |

Two channels, matching the spec's "up to two HX711 channels in target budget".
Single sets are €9.59 ([B0DB1DTYNK](https://www.amazon.fr/dp/B0DB1DTYNK)) if you
only want one.

Manual weigh-ins are fully supported (FR-W-03) and need no hardware at all, so
this group can be skipped entirely for the first build — which is what the spec
means by "omit if manual weighing is used". 5 kg cells suit individual
saucissons and small whole-muscle cuts; a coppa or a large bresaola may need
10 kg cells. Use shielded cable and mount each cell on its own mechanical
platform (spec §10).

---

## Group 7 — Small parts (estimate, ~€25)

Not individually linked because the right choice depends on your enclosure and
chamber layout:

- Panel buttons ×4, status LED, resistors, pull-up/pull-down resistors
- DIN terminal blocks, DIN rail, cable ferrules and crimp tool if you lack one
- 2-core shielded cable for the sensor runs, 3-core for the load cells
- Cable glands for the enclosure, strain reliefs, replaceable JST/M12
  connectors at the probes (spec §8.3 asks for replaceable sensor connectors)
- Inline fuse holder and fuse on the mains input
- Dupont jumpers and a small perfboard or a screw-terminal HAT

---

## Group 8 — Independent safety cut-out (required, outside the €200)

| Item | Qty | Price | Link |
|---|---:|---:|---|
| Inkbird ITC-308 plug-in thermostat, 2 relays, 230 V | 1 | €39.99 | [B016EYB03G](https://www.amazon.fr/dp/B016EYB03G) |
| | | **€39.99** | |

Spec §13.2 requires *"independent over-temperature/under-temperature protection
for unattended operation"* and §4.2 rank 1 puts it above software: **software
cannot override it.** The ITC-308 is a self-contained thermostat with its own
probe and its own relays, wired in series with the chamber loads. If MeatPilot's
firmware hangs with the heater on, this is what saves the meat.

**Do not buy a KSD9700 bimetal switch for this.** They are the obvious cheap
answer and they are wrong: the available trip points start at 50 °C, and a
curing chamber needs to trip somewhere near 20 °C. A bimetal disc rated for
50 °C offers no protection whatsoever at charcuterie temperatures.

---

## Order-of-purchase suggestion

Milestones M0 and M1 need **no hardware at all** — the engine and its acceptance
suite run on a laptop. So there is no rush, and two useful staging points:

1. **For M2 (bench bring-up):** groups 1, 2, 3. ~€93. Enough to prove every
   driver, the display, the RTC and SD logging on a desk, with no mains
   anywhere near it.
2. **For M3 (chamber install):** add groups 4, 5, 7 and 8. The mains-facing
   half, once the firmware is known good.
3. **Whenever you want it:** group 6.

Buying groups 1–3 first also de-risks the two things most likely to disappoint —
the SHT45's condensation behaviour and the I²C cable run — while they are still
cheap to change.
