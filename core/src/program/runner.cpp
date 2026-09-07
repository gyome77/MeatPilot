// SPDX-License-Identifier: MIT
#include "meatpilot/program/runner.h"

namespace meatpilot::program {

Validation Runner::start(const Programme& p, Seconds now) noexcept {
    const Validation v = validate(p);
    if (v != Validation::Ok) return v;

    programme_ = p;
    revision_ = p.revision;
    phase_index_ = 0;
    phase_started_at_ = now;
    next_requested_ = false;
    status_ = RunStatus::Running;
    return Validation::Ok;
}

void Runner::stop() noexcept {
    status_ = RunStatus::Idle;
    next_requested_ = false;
}

void Runner::pause(Seconds now) noexcept {
    if (status_ != RunStatus::Running) return;
    paused_at_ = now;
    status_ = RunStatus::Paused;
}

void Runner::resume(Seconds now) noexcept {
    if (status_ != RunStatus::Paused) return;
    // Credit the paused time back to the phase: a pause is for opening the
    // chamber or fixing a probe, and it should not consume the recipe.
    phase_started_at_ += now - paused_at_;
    status_ = RunStatus::Running;
}

void Runner::requestNextPhase() noexcept { next_requested_ = true; }

const Phase* Runner::currentPhase() const noexcept {
    if (status_ == RunStatus::Idle || phase_index_ >= programme_.phase_count) {
        return nullptr;
    }
    return &programme_.phases[phase_index_];
}

bool Runner::dryingPhase() const noexcept {
    const Phase* p = currentPhase();
    return p != nullptr && p->drying;
}

bool Runner::phaseComplete(Seconds now, const product::Progress& progress,
                           Advance& why) const noexcept {
    const Phase* p = currentPhase();
    if (p == nullptr) return false;

    const Seconds elapsed = now - phase_started_at_;

    if (next_requested_) {
        why = Advance::Manual;
        return true;
    }

    switch (p->end) {
        case EndKind::Duration:
            if (elapsed >= p->duration) {
                why = Advance::Duration;
                return true;
            }
            break;

        case EndKind::Manual:
            break;  // only the operator ends it

        case EndKind::ReferenceLoss:
            if (progress.has_reference && progress.reference_target_reached) {
                why = Advance::WeightTarget;
                return true;
            }
            break;

        case EndKind::FirstOfTargets:
            if (progress.any_target_reached) {
                why = Advance::WeightTarget;
                return true;
            }
            break;

        case EndKind::LastOfTargets:
            if (progress.all_targets_reached) {
                why = Advance::WeightTarget;
                return true;
            }
            break;
    }

    // The safety cap, checked last so that the intended condition is always
    // reported in preference to the fallback.
    if (p->max_duration > 0.0 && elapsed >= p->max_duration) {
        why = Advance::MaxDuration;
        return true;
    }
    return false;
}

TickResult Runner::tick(Seconds now, const product::Progress& progress) noexcept {
    TickResult r{};
    r.phase_index = phase_index_;
    if (status_ != RunStatus::Running) return r;

    Advance why = Advance::None;
    if (!phaseComplete(now, progress, why)) return r;

    next_requested_ = false;
    r.advance = why;

    if (phase_index_ + 1 < programme_.phase_count) {
        ++phase_index_;
        phase_started_at_ = now;
        r.phase_changed = true;
        r.phase_index = phase_index_;
        return r;
    }

    // Last phase done.
    status_ = RunStatus::Completed;
    r.programme_completed = true;
    r.phase_index = phase_index_;
    return r;
}

void Runner::applyTo(control::RegulationConfig& cfg) const noexcept {
    const Phase* p = currentPhase();
    if (p == nullptr) return;

    // A completed programme that holds its last phase keeps regulating at
    // those setpoints -- a cellar hold. One that does not, stops steering and
    // leaves the chamber to the operator.
    if (status_ == RunStatus::Completed && !programme_.hold_last_on_complete) {
        cfg.has_temp_target = false;
        cfg.has_humidity_target = false;
        return;
    }
    if (status_ == RunStatus::Idle) return;

    cfg.has_temp_target = p->has_temp;
    if (p->has_temp) {
        cfg.target_temp = p->target_temp;
        cfg.temp_deadband = p->temp_deadband;
    }
    cfg.has_humidity_target = p->has_humidity;
    if (p->has_humidity) {
        cfg.target_humidity = p->target_humidity;
        cfg.humidity_deadband = p->humidity_deadband;
    }

    cfg.circulation = p->circulation;
    cfg.fresh_air = p->fresh_air;

    if (p->has_limits) {
        cfg.temp_abs_min = p->temp_abs_min;
        cfg.temp_abs_max = p->temp_abs_max;
    }
}

Seconds Runner::phaseTimeRemaining(Seconds now) const noexcept {
    const Phase* p = currentPhase();
    if (p == nullptr || status_ != RunStatus::Running) return 0.0;

    const Seconds elapsed = now - phase_started_at_;
    Seconds deadline = 0.0;
    if (p->end == EndKind::Duration) deadline = p->duration;
    else if (p->max_duration > 0.0) deadline = p->max_duration;
    if (deadline <= 0.0) return 0.0;

    const Seconds left = deadline - elapsed;
    return left > 0.0 ? left : 0.0;
}

}  // namespace meatpilot::program
