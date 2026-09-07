// SPDX-License-Identifier: MIT
#pragma once

#include "meatpilot/model/types.h"

namespace meatpilot::alarm {

using meatpilot::Seconds;

/// What happened to a tracked condition on one update.
enum class Transition : std::uint8_t { None, Raised, Reminder, Cleared };

/// Tracks one boolean condition over time: onset delay, reminders while it
/// persists unacknowledged, latching, and resolution.
///
/// This is the anti-spam core of §5. A condition that flickers either side of
/// a threshold must not produce a notification per tick, and a critical
/// condition that clears must stay visible until somebody says they have seen
/// it -- otherwise a compressor failure at 3am is a silent line in a log.
class Condition {
public:
    /// `onset` is how long the condition must hold before it fires at all.
    /// `reminder` of 0 means fire once and then wait for resolution.
    /// `latching` keeps the alarm visible after the condition clears, until
    /// acknowledged -- required for CRITICAL by §5.
    void configure(Seconds onset, Seconds reminder, bool latching) noexcept;

    Transition update(bool active, Seconds now) noexcept;

    /// Silence reminders, and release a latch once the condition has cleared.
    void acknowledge() noexcept;

    constexpr bool triggered() const noexcept { return triggered_; }
    constexpr bool latched() const noexcept { return latched_; }
    /// Visible to the operator: either currently true, or latched awaiting ack.
    constexpr bool visible() const noexcept { return triggered_ || latched_; }
    constexpr bool acknowledged() const noexcept { return acknowledged_; }
    constexpr Seconds activeSince() const noexcept { return active_since_; }

private:
    Seconds onset_{0.0};
    Seconds reminder_{0.0};
    bool    latching_{false};

    bool    has_active_since_{false};
    Seconds active_since_{0.0};
    bool    triggered_{false};
    bool    latched_{false};
    bool    acknowledged_{false};
    bool    has_notified_{false};
    Seconds last_notified_{0.0};
};

}  // namespace meatpilot::alarm
