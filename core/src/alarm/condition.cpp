// SPDX-License-Identifier: MIT
#include "meatpilot/alarm/condition.h"

namespace meatpilot::alarm {

void Condition::configure(Seconds onset, Seconds reminder,
                          bool latching) noexcept {
    onset_ = onset;
    reminder_ = reminder;
    latching_ = latching;
}

void Condition::acknowledge() noexcept {
    if (triggered_ || latched_) acknowledged_ = true;
}

Transition Condition::update(bool active, Seconds now) noexcept {
    if (active) {
        latched_ = false;  // it is back, so there is nothing to latch
        if (!has_active_since_) {
            has_active_since_ = true;
            active_since_ = now;
        }
        if (!triggered_ && (now - active_since_) >= onset_) {
            triggered_ = true;
            has_notified_ = true;
            last_notified_ = now;
            return Transition::Raised;
        }
        if (triggered_ && !acknowledged_ && reminder_ > 0.0 && has_notified_ &&
            (now - last_notified_) >= reminder_) {
            last_notified_ = now;
            return Transition::Reminder;
        }
        return Transition::None;
    }

    // Not active any more.
    const bool was_triggered = triggered_;
    has_active_since_ = false;
    triggered_ = false;
    has_notified_ = false;

    if (was_triggered && latching_ && !acknowledged_) {
        // Hold it visible. §5: a critical alarm survives the condition that
        // caused it, so that nobody misses an excursion that self-corrected.
        latched_ = true;
        return Transition::None;
    }

    if (latched_ && acknowledged_) {
        latched_ = false;
        acknowledged_ = false;
        return Transition::Cleared;
    }
    if (latched_) return Transition::None;

    acknowledged_ = false;
    return was_triggered ? Transition::Cleared : Transition::None;
}

}  // namespace meatpilot::alarm
