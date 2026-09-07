// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/alarm/condition.h"
#include "meatpilot/control/engine.h"
#include "meatpilot/product/registry.h"

namespace meatpilot::alarm {

enum class Severity : std::uint8_t { Info, Warning, Critical };

/// The alarm catalogue. Identifiers are stable: they appear in the REST API,
/// in MQTT payloads and in the stored alarm log, so they are interface.
enum class Key : std::uint8_t {
    // AL-01 -- environmental limits. Warnings are delayed, criticals immediate.
    TemperatureWarnHigh, TemperatureWarnLow,
    TemperatureCriticalHigh, TemperatureCriticalLow,
    HumidityWarnHigh, HumidityWarnLow,
    HumidityCriticalHigh, HumidityCriticalLow,

    // AL-02 -- sensor health.
    TempSensorFault, HumiditySensorFault, ProbeDivergence,

    // AL-03 -- commanded, but nothing happened.
    NoResponseCooling, NoResponseHeating,
    NoResponseHumidify, NoResponseDehumidify,

    // AL-04 -- feedback disagrees with the command (stuck relay).
    ActuatorFeedbackMismatch,

    // AL-05 -- operational.
    DoorOpenTooLong, ShortCycling, ExcessiveStarts,

    // AL-06 -- system health.
    LowSupplyVoltage, RebootLoop, StorageFault,
    ClockInvalid, NetworkDown, NotificationFailed,

    // AL-07 -- weighing.
    WeightChannelFault, WeighInOverdue,

    // AL-08 to AL-11 -- product quality. [EXT]
    CaseHardening, CondensationRisk, HighTempDrying, DegradedMode,
};

inline constexpr std::size_t kKeyCount = 31;

/// What the operator should do by hand when no actuator can fix it (AL-11).
enum class ManualAction : std::uint8_t {
    None, AddHumidity, RemoveHumidity, AddCooling, AddHeating, Ventilate,
};

enum class EventKind : std::uint8_t { Raised, Reminder, Cleared };

/// One alarm transition, for the log, the display and remote notification.
struct Event {
    Key          key{Key::TemperatureWarnHigh};
    Severity     severity{Severity::Warning};
    EventKind    kind{EventKind::Raised};
    Seconds      at{0.0};
    float        value{0.0F};   ///< the reading that triggered it
    float        limit{0.0F};   ///< the boundary it crossed
    ManualAction action{ManualAction::None};
};

struct Config {
    /// Warning band, as a multiple of the phase deadband. Beyond this the
    /// chamber is drifting rather than cycling.
    float   warn_band_factor{2.0F};
    Seconds warning_delay{600.0};
    Seconds reminder_interval{7200.0};

    Seconds door_open_limit{300.0};

    /// AL-03: how long an actuator may run without the quantity moving in the
    /// expected direction by at least `response_min_delta`.
    Seconds response_timeout{2700.0};
    float   response_min_delta{0.3F};

    /// AL-05.
    std::uint16_t max_starts_per_hour{8};
    Seconds       min_cycle_period{300.0};

    /// AL-06.
    std::uint16_t reboot_loop_threshold{3};

    /// AL-08: drying faster than this crusts the surface. [EXT]
    float case_hardening_rate{1.5F};

    /// AL-09: chamber within this of dew point. [EXT]
    float condensation_margin{1.0F};

    /// AL-10: sustained warmth during a drying phase. [EXT]
    float   high_temp_drying_limit{16.0F};
    Seconds high_temp_drying_duration{7200.0};

    /// AL-11: outside `warn_band_factor` x deadband with no actuator able to
    /// correct it. [EXT]
    Seconds degraded_delay{900.0};
};

/// Everything the alarm layer examines. Assembled once per tick by the caller.
struct Signals {
    control::Inputs  inputs{};
    control::Outputs outputs{};

    bool  divergence_checked{false};
    bool  divergent{false};
    float divergence_delta{0.0F};

    product::Progress progress{};
    bool  has_drying_rate{false};
    float drying_rate_pct_per_day{0.0F};

    bool drying_phase{false};

    // System health (AL-06). Defaults are the healthy case.
    bool          storage_ok{true};
    bool          clock_valid{true};
    bool          network_ok{true};
    bool          notifications_ok{true};
    bool          supply_ok{true};
    bool          weight_channels_ok{true};
    std::uint16_t recent_reboots{0};
};

/// Evaluates the catalogue once per tick and emits transitions.
class Manager {
public:
    static constexpr std::size_t kMaxActuatorStarts = 16;

    explicit Manager(const Config& cfg = Config{}) noexcept;

    void acknowledge(Key key) noexcept;
    void acknowledgeAll() noexcept;

    /// Evaluate everything and write transitions into `out`. Returns how many
    /// were written; excess transitions are dropped rather than overflowing,
    /// and the count is reported by `dropped()`.
    std::size_t evaluate(const Signals& s, const control::RegulationConfig& cfg,
                         Seconds now, Event* out, std::size_t capacity) noexcept;

    bool visible(Key key) const noexcept;
    bool anyCritical() const noexcept;
    /// True when a critical condition demands the SAFE state.
    bool requiresSafeState() const noexcept;
    ManualAction manualAction() const noexcept { return manual_action_; }
    std::size_t dropped() const noexcept { return dropped_; }

    static Severity severity(Key key) noexcept;
    static const char* name(Key key) noexcept;
    static const char* name(ManualAction a) noexcept;

private:
    struct ResponseWatch {
        bool    armed{false};
        Seconds since{0.0};
        float   value_at_start{0.0F};
    };

    struct StartLog {
        Seconds     at[kMaxActuatorStarts]{};
        std::size_t count{0};
        std::size_t next{0};
        bool        was_on{false};
        void note(Seconds now) noexcept;
        std::size_t within(Seconds window, Seconds now) const noexcept;
        bool lastIntervalShorterThan(Seconds period, Seconds now) const noexcept;
    };

    void emit(Key key, Transition t, Seconds now, float value, float limit,
              Event* out, std::size_t capacity, std::size_t& written) noexcept;
    void track(Key key, bool active, Seconds now, float value, float limit,
               Event* out, std::size_t capacity, std::size_t& written) noexcept;

    void evaluateResponse(const Signals& s, Seconds now, Event* out,
                          std::size_t capacity, std::size_t& written) noexcept;
    void evaluateDegraded(const Signals& s, const control::RegulationConfig& cfg,
                          Seconds now, Event* out, std::size_t capacity,
                          std::size_t& written) noexcept;

    Config        cfg_;
    Condition     conditions_[kKeyCount]{};
    ResponseWatch response_[control::kActuatorCount]{};
    StartLog      starts_[control::kActuatorCount]{};
    ManualAction  manual_action_{ManualAction::None};
    std::size_t   dropped_{0};
};

}  // namespace meatpilot::alarm
