// ============================================================================
// AMT_Probes.h
// Probe system structures: ProbeManager, ReplayValidator, Scenarios
// Extracted from AuctionSensor_v1.cpp for modularization
// ============================================================================

#ifndef AMT_PROBES_H
#define AMT_PROBES_H

#include "sierrachart.h"
#include "amt_core.h"
#include <map>

namespace AMT {

// ============================================================================
// PROBE DIRECTION & STATUS
// ============================================================================

enum class ProbeDirection : int {
    LONG = 1,
    SHORT = 2
};

enum class ProbeStatus : int {
    OBSERVING = 0,
    ACCEPTED = 1,
    REJECTED = 2,
    TIMEOUT = 3
};

// ============================================================================
// MECHANISM TAGS FOR PROBE RESULTS
// ============================================================================

enum class MechanismTag : int
{
    NONE = 0,
    CLEAN_ACCEPTANCE = 1,
    WEAK_ACCEPTANCE = 2,
    ABSORPTION_WALL = 3,
    DISTRIBUTION = 4,
    EXHAUSTION = 5,
    FALSE_BREAKOUT = 6,
    TIMEOUT_TAG = 7,
    COUNTER_TREND_SUCCESS = 8,
    VALUE_REJECTION = 9
};

inline const char* to_string(MechanismTag m)
{
    switch (m)
    {
    case MechanismTag::NONE:                  return "NONE";
    case MechanismTag::CLEAN_ACCEPTANCE:      return "CLEAN_ACC";
    case MechanismTag::WEAK_ACCEPTANCE:       return "WEAK_ACC";
    case MechanismTag::ABSORPTION_WALL:       return "ABSORB_WALL";
    case MechanismTag::DISTRIBUTION:          return "DISTRIB";
    case MechanismTag::EXHAUSTION:            return "EXHAUST";
    case MechanismTag::FALSE_BREAKOUT:        return "FALSE_BRK";
    case MechanismTag::TIMEOUT_TAG:           return "TIMEOUT";
    case MechanismTag::COUNTER_TREND_SUCCESS: return "CTR_TREND";
    case MechanismTag::VALUE_REJECTION:       return "VAL_REJ";
    default:                                  return "UNK";
    }
}

inline const char* to_string(ProbeStatus s)
{
    switch (s)
    {
    case ProbeStatus::OBSERVING: return "OBSERVING";
    case ProbeStatus::ACCEPTED:  return "ACCEPTED";
    case ProbeStatus::REJECTED:  return "REJECTED";
    case ProbeStatus::TIMEOUT:   return "TIMEOUT";
    default:                     return "UNK";
    }
}

// ============================================================================
// PROBE REQUEST & RESULT
// ============================================================================

struct ProbeRequest
{
    int probe_id = 0;
    int scenario_id = 0;
    double price = 0.0;
    int zone_id = -1;  // -1 if no nearby zone
    SCDateTime t0;
    ProbeDirection direction = ProbeDirection::LONG;
    const char* hypothesis = "";
    double score = 0.0;
    int timeout_seconds = 120;
};

struct ProbeResult
{
    int probe_id = 0;
    ProbeStatus status = ProbeStatus::OBSERVING;
    MechanismTag mechanism = MechanismTag::NONE;
    int observation_time_ms = 0;
    double mfe = 0.0;  // Max Favorable Excursion (ticks)
    double mae = 0.0;  // Max Adverse Excursion (ticks)

    // Structured resolution log (for diag output)
    // Format: [RESOLUTION] status=X mech=Y bars=N poc_mig=M micro={hvn=H lvn=L ctx=C}
    char resolution_log[256] = {0};
    bool micro_influenced = false;  // True if micro features affected decision
};

// ============================================================================
// SCENARIO STRUCTURES
// ============================================================================

struct ScenarioKey
{
    AMTMarketState state = AMTMarketState::BALANCE;
    AggressionType aggression = AggressionType::NEUTRAL;
    AuctionFacilitation facilitation = AuctionFacilitation::EFFICIENT;
    CurrentPhase phase = CurrentPhase::ROTATION;
};

struct ScenarioEntry
{
    int scenario_id = 0;
    ScenarioKey key;
    int quality_score = 0;
    const char* name = "";
    const char* hypothesis_template = "";
    AuctionIntent primary_intent = AuctionIntent::NEUTRAL;
};

struct ScenarioMatch
{
    int scenario_id = 0;
    int match_score = 0;      // How well current context matches
    int quality_score = 0;    // Scenario's inherent quality
    bool exact_match = false; // All 4 fields matched exactly
    const ScenarioEntry* entry = nullptr;
};

// ============================================================================
// AUCTION MODE
// ============================================================================

enum class AuctionMode : int
{
    MODE_ROTATIONAL = 1,   // Fade extremes (BALANCE scenarios)
    MODE_DIRECTIONAL = 2,  // Breakouts/continuations (IMBALANCE scenarios)
    MODE_LOCKED = 3        // Do nothing
};

inline const char* to_string(AuctionMode m)
{
    switch (m)
    {
    case AuctionMode::MODE_ROTATIONAL:  return "ROTATIONAL";
    case AuctionMode::MODE_DIRECTIONAL: return "DIRECTIONAL";
    case AuctionMode::MODE_LOCKED:      return "LOCKED";
    default:                            return "UNK";
    }
}

// ============================================================================
// PROBE BLOCK REASON
// ============================================================================

enum class ProbeBlockReason : int
{
    NONE = 0,               // No block - can fire
    BACKFILL_PENDING = 1,   // Historical backfill not complete
    REALTIME_ONLY = 2,      // realtime_only=true but not last bar
    PROBE_ACTIVE = 3,       // Another probe is still observing
    COOLDOWN = 4,           // Within cooldown period after last resolution
    SAME_BAR = 5,           // Already fired on this bar (intrabar guard)
    WARMUP_PENDING = 6      // Baseline warmup not complete
};

inline const char* ProbeBlockReasonToString(ProbeBlockReason reason)
{
    switch (reason) {
        case ProbeBlockReason::NONE:             return "NONE";
        case ProbeBlockReason::BACKFILL_PENDING: return "BACKFILL_PENDING";
        case ProbeBlockReason::REALTIME_ONLY:    return "REALTIME_ONLY";
        case ProbeBlockReason::PROBE_ACTIVE:     return "PROBE_ACTIVE";
        case ProbeBlockReason::COOLDOWN:         return "COOLDOWN";
        case ProbeBlockReason::SAME_BAR:         return "SAME_BAR";
        case ProbeBlockReason::WARMUP_PENDING:   return "WARMUP_PENDING";
        default:                                 return "UNKNOWN";
    }
}

// ============================================================================
// PROBE MANAGER (One-probe latch + cooldown + startup gate)
// ============================================================================

class ProbeManager
{
public:
    // Configuration (public for easy access from study inputs)
    int cooldown_bars = 10;         // Minimum bars between probes
    bool realtime_only = true;      // Only fire probes in real-time

    void Reset()
    {
        active_probe_id_ = 0;
        last_resolution_index_ = -999;
        last_fired_index_ = -999;
        is_active_ = false;
        backfill_complete_ = false;
        baseline_warmed_up_ = false;
        last_block_reason_ = ProbeBlockReason::BACKFILL_PENDING;
        prev_block_reason_ = ProbeBlockReason::BACKFILL_PENDING;
        last_logged_bar_ = -999;
        probe_start_time_ = SCDateTime(0);
        fired_bar_index_ = -1;
        total_probes_fired_ = 0;
        total_probes_resolved_ = 0;
    }

    // Called once when historical backfill is done
    void OnBackfillComplete(int current_index)
    {
        backfill_complete_ = true;
        last_resolution_index_ = current_index - cooldown_bars - 1;  // Allow immediate probe
    }

    bool IsBackfillComplete() const { return backfill_complete_; }

    // Update warmup status from DriftTracker
    void SetBaselineWarmedUp(bool warmed_up) { baseline_warmed_up_ = warmed_up; }
    bool IsBaselineWarmedUp() const { return baseline_warmed_up_; }

    // Returns true if we can fire a new probe
    bool CanFireProbe(int current_index, bool is_last_bar)
    {
        // Gate 1: Must have completed backfill
        if (!backfill_complete_) {
            last_block_reason_ = ProbeBlockReason::BACKFILL_PENDING;
            return false;
        }

        // Gate 2: Baselines must be warmed up on live data
        if (!baseline_warmed_up_) {
            last_block_reason_ = ProbeBlockReason::WARMUP_PENDING;
            return false;
        }

        // Gate 3: Real-time only mode
        if (realtime_only && !is_last_bar) {
            last_block_reason_ = ProbeBlockReason::REALTIME_ONLY;
            return false;
        }

        // Gate 4: No active probe
        if (is_active_) {
            last_block_reason_ = ProbeBlockReason::PROBE_ACTIVE;
            return false;
        }

        // Gate 5: Cooldown period
        if (current_index - last_resolution_index_ < cooldown_bars) {
            last_block_reason_ = ProbeBlockReason::COOLDOWN;
            return false;
        }

        // Gate 6: One probe per bar (intrabar safety)
        if (current_index == last_fired_index_) {
            last_block_reason_ = ProbeBlockReason::SAME_BAR;
            return false;
        }

        last_block_reason_ = ProbeBlockReason::NONE;
        return true;
    }

    // Check if block reason changed (for low-noise logging)
    bool ShouldLogBlockChange(int current_bar)
    {
        const bool reasonChanged = (last_block_reason_ != prev_block_reason_);
        const bool barChanged = (current_bar != last_logged_bar_);

        if (reasonChanged || (barChanged && last_block_reason_ != ProbeBlockReason::NONE)) {
            prev_block_reason_ = last_block_reason_;
            last_logged_bar_ = current_bar;
            return reasonChanged;
        }
        return false;
    }

    void OnProbeStarted(int probe_id, int current_index, SCDateTime start_time)
    {
        active_probe_id_ = probe_id;
        is_active_ = true;
        last_fired_index_ = current_index;
        fired_bar_index_ = current_index;
        probe_start_time_ = start_time;
        total_probes_fired_++;
    }

    void OnProbeResolved(int current_index)
    {
        is_active_ = false;
        active_probe_id_ = 0;
        last_resolution_index_ = current_index;
        total_probes_resolved_++;
    }

    bool IsProbeActive() const { return is_active_; }
    int GetActiveProbeId() const { return active_probe_id_; }
    ProbeBlockReason GetLastBlockReason() const { return last_block_reason_; }
    SCDateTime GetProbeStartTime() const { return probe_start_time_; }
    int GetFiredBarIndex() const { return fired_bar_index_; }

    int GetBarsSinceLastResolution(int current_index) const
    {
        return current_index - last_resolution_index_;
    }

    // Stats for replay validation
    int GetTotalProbesFired() const { return total_probes_fired_; }
    int GetTotalProbesResolved() const { return total_probes_resolved_; }

    // Diagnostic: Get state snapshot for logging
    void GetDiagnosticState(int current_index, bool is_last_bar,
                            int& out_cooldown_remaining, bool& out_backfill,
                            bool& out_active, int& out_last_fired) const
    {
        (void)is_last_bar;  // Unused
        out_backfill = backfill_complete_;
        out_active = is_active_;
        out_last_fired = last_fired_index_;
        out_cooldown_remaining = cooldown_bars - (current_index - last_resolution_index_);
        if (out_cooldown_remaining < 0) out_cooldown_remaining = 0;
    }

private:
    int active_probe_id_ = 0;
    int last_resolution_index_ = -999;
    int last_fired_index_ = -999;
    bool is_active_ = false;
    bool backfill_complete_ = false;
    bool baseline_warmed_up_ = false;
    ProbeBlockReason last_block_reason_ = ProbeBlockReason::BACKFILL_PENDING;
    ProbeBlockReason prev_block_reason_ = ProbeBlockReason::BACKFILL_PENDING;
    int last_logged_bar_ = -999;
    SCDateTime probe_start_time_;
    int fired_bar_index_ = -1;
    int total_probes_fired_ = 0;
    int total_probes_resolved_ = 0;
};

// ============================================================================
// REPLAY DETERMINISM VALIDATOR
// Stores probe outcomes for comparison on chart replay
// Uses deterministic signature (fired_bar + scenario + direction)
// ============================================================================

class ReplayValidator
{
public:
    // Deterministic signature: identifies "the same probe" across replays
    struct ProbeSignature
    {
        int fired_bar = 0;
        int scenario_id = 0;
        ProbeDirection direction = ProbeDirection::LONG;

        bool operator<(const ProbeSignature& other) const
        {
            if (fired_bar != other.fired_bar) return fired_bar < other.fired_bar;
            if (scenario_id != other.scenario_id) return scenario_id < other.scenario_id;
            return static_cast<int>(direction) < static_cast<int>(other.direction);
        }

        bool operator==(const ProbeSignature& other) const
        {
            return fired_bar == other.fired_bar &&
                   scenario_id == other.scenario_id &&
                   direction == other.direction;
        }
    };

    struct ProbeOutcome
    {
        ProbeSignature sig;
        ProbeStatus status = ProbeStatus::OBSERVING;
        double mfe = 0.0;
        double mae = 0.0;
        int resolution_bar = 0;
    };

    void Reset()
    {
        outcomes_.clear();
        divergence_count_ = 0;
        validated_count_ = 0;
        is_replay_mode_ = false;
    }

    // Call this after first full pass to enable validation mode
    void EnableReplayValidation()
    {
        if (!is_replay_mode_ && !outcomes_.empty())
        {
            is_replay_mode_ = true;
            expected_outcomes_ = outcomes_;
            outcomes_.clear();
        }
    }

    // Record outcome using deterministic signature
    void RecordOutcome(const ProbeRequest& req, const ProbeResult& result,
                       int fired_bar, int resolution_bar)
    {
        ProbeSignature sig;
        sig.fired_bar = fired_bar;
        sig.scenario_id = req.scenario_id;
        sig.direction = req.direction;

        ProbeOutcome o;
        o.sig = sig;
        o.status = result.status;
        o.mfe = result.mfe;
        o.mae = result.mae;
        o.resolution_bar = resolution_bar;

        if (is_replay_mode_)
        {
            ValidateOutcome(o);
        }

        outcomes_[sig] = o;
    }

    bool IsValidating() const { return is_replay_mode_; }
    int GetValidatedCount() const { return validated_count_; }
    int GetDivergenceCount() const { return divergence_count_; }
    int GetTotalRecorded() const { return static_cast<int>(outcomes_.size()); }

    void LogSummary(SCStudyInterfaceRef sc)
    {
        if (!is_replay_mode_)
            return;

        const int expected = static_cast<int>(expected_outcomes_.size());
        const int actual = static_cast<int>(outcomes_.size());

        SCString msg;
        if (divergence_count_ == 0 && expected == actual)
        {
            msg.Format("[REPLAY-OK] %d probes validated, 0 divergences", validated_count_);
        }
        else
        {
            msg.Format("[REPLAY-WARN] Validated:%d Divergences:%d Expected:%d Actual:%d",
                validated_count_, divergence_count_, expected, actual);
        }
        sc.AddMessageToLog(msg, 0);
    }

private:
    void ValidateOutcome(const ProbeOutcome& actual)
    {
        auto it = expected_outcomes_.find(actual.sig);
        if (it == expected_outcomes_.end())
        {
            divergence_count_++;
            return;
        }

        const ProbeOutcome& expected = it->second;
        validated_count_++;

        bool diverged = false;

        if (expected.status != actual.status)
        {
            diverged = true;
        }
        else if (std::abs(expected.mfe - actual.mfe) > 0.5 ||
                 std::abs(expected.mae - actual.mae) > 0.5)
        {
            diverged = true;
        }
        else if (expected.resolution_bar != actual.resolution_bar)
        {
            diverged = true;
        }

        if (diverged)
        {
            divergence_count_++;
        }
    }

    std::map<ProbeSignature, ProbeOutcome> outcomes_;
    std::map<ProbeSignature, ProbeOutcome> expected_outcomes_;
    int divergence_count_ = 0;
    int validated_count_ = 0;
    bool is_replay_mode_ = false;
};

} // namespace AMT

#endif // AMT_PROBES_H
