// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

#include "meatpilot/model/types.h"

namespace meatpilot::product {

using meatpilot::Seconds;

using ProductId = std::uint16_t;
inline constexpr ProductId kNoProduct = 0;

/// FR-W-01: at least 20 active products. Archived products live on storage,
/// not in RAM, so only the active set is bounded here.
inline constexpr std::size_t kMaxActive = 20;

/// Weigh-ins retained in RAM per product. The full history is append-only on
/// storage (FR-W-07); this is the recent window the rate and ETA are computed
/// from. At a weigh-in every few days, 64 covers most of a year.
inline constexpr std::size_t kMaxWeighIns = 64;

inline constexpr std::size_t kNameLength = 32;
inline constexpr std::size_t kBatchLength = 16;

enum class Status : std::uint8_t { Active, Completed, Archived };

struct WeighIn {
    Seconds timestamp{0.0};
    float   weight{0.0F};
    bool    automatic{false};  ///< from a load cell rather than typed in
    bool    stable{true};      ///< load cell reading had settled
    bool    suspicious{false}; ///< stored, but flagged for AL-07
};

/// Why a weigh-in was or was not accepted.
enum class Accept : std::uint8_t {
    Ok,
    Suspicious,          ///< stored and flagged: implausible step, or weight gain
    RejectedImpossible,  ///< not stored: zero, negative, or absurd
    RejectedUnknown,     ///< no such product
    RejectedFull,        ///< history window full and could not be rolled
    RejectedOutOfOrder,  ///< timestamp precedes the last weigh-in
};

struct Product {
    ProductId id{kNoProduct};
    char      name[kNameLength]{};
    char      batch[kBatchLength]{};
    Seconds   started_at{0.0};
    float     initial_weight{0.0F};
    float     target_loss_pct{35.0F};
    Status    status{Status::Active};
    bool      reference{false};  ///< drives weight-based phase completion

    WeighIn     history[kMaxWeighIns]{};
    std::size_t weigh_in_count{0};
};

/// Derived progress for one product (FR-W-05, FR-W-06).
struct Stats {
    bool    valid{false};
    float   current_weight{0.0F};
    float   absolute_loss{0.0F};
    float   loss_pct{0.0F};
    float   rate_pct_per_day{0.0F};
    bool    has_rate{false};
    bool    has_eta{false};
    Seconds eta{0.0};
    Seconds last_weigh_in{0.0};
    Seconds age_of_last_weigh_in{0.0};
    bool    target_reached{false};
};

/// Plausibility limits (FR-W-08).
struct Limits {
    /// A single weigh-in may not differ from the previous one by more than
    /// this fraction of the previous weight.
    float max_step_fraction{0.15F};
    /// Weight gain beyond this fraction of the initial weight is not drying.
    float max_gain_fraction{0.02F};
    /// Beyond this, the operator is asked for a weigh-in (AL-07).
    Seconds weigh_in_interval{7 * 24 * 3600.0};
};

/// Aggregate view used by the programme engine's completion conditions, so
/// that program/ never has to know what a product is.
struct Progress {
    bool  has_reference{false};
    float reference_loss_pct{0.0F};
    bool  reference_target_reached{false};
    bool  any_target_reached{false};
    bool  all_targets_reached{false};
    bool  any_weigh_in_overdue{false};
    std::size_t active_count{0};
};

/// Products and their weigh-in histories. Pure, time-injected, no allocation.
class Registry {
public:
    explicit Registry(const Limits& limits = Limits{}) noexcept;

    /// Returns kNoProduct when full. The first product created becomes the
    /// reference automatically.
    ProductId create(const char* name, const char* batch, float initial_weight,
                     float target_loss_pct, Seconds now) noexcept;

    bool remove(ProductId id) noexcept;
    bool setReference(ProductId id) noexcept;
    bool setStatus(ProductId id, Status status) noexcept;

    Accept recordWeighIn(ProductId id, float weight, Seconds now,
                         bool automatic = false, bool stable = true) noexcept;

    Stats stats(ProductId id, Seconds now) const noexcept;
    Progress progress(Seconds now) const noexcept;

    const Product* find(ProductId id) const noexcept;
    std::size_t count() const noexcept { return count_; }
    const Product& at(std::size_t i) const noexcept { return products_[i]; }

    ProductId referenceId() const noexcept;

private:
    Product* findMutable(ProductId id) noexcept;

    Limits      limits_;
    Product     products_[kMaxActive]{};
    std::size_t count_{0};
    ProductId   next_id_{1};
};

}  // namespace meatpilot::product
