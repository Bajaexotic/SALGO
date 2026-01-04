// ============================================================================
// AMT_Modules.h
// Analysis modules: AuctionContext, DynamicGauge, MiniVP, ZoneStore
// Extracted from AuctionSensor_v1.cpp for modularization
// ============================================================================

#ifndef AMT_MODULES_H
#define AMT_MODULES_H

#include "sierrachart.h"
#include "amt_core.h"
#include "AMT_Probes.h"        // For ProbeRequest, ProbeResult, ProbeDirection, etc.
#include "AMT_VolumeProfile.h" // For VbPLevelContext, ComputeValueAreaFromSortedVector
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>

namespace AMT {

// ============================================================================
// EVIDENCE SCORE (For DynamicGauge)
// ============================================================================

struct EvidenceScore
{
    // Tier 1: Volume signals (0-1)
    double volume_score = 0.0;

    // Tier 2: Delta signals (0-1)
    double delta_score = 0.0;

    // Tier 3: Initiative/Progress (0-1)
    double initiative_score = 0.0;

    // Balanced 33/33/33 weighting
    double Total() const
    {
        return volume_score + delta_score + initiative_score;
    }
};

// ============================================================================
// MICRO VOLUME AT PRICE (For MicroAuction)
// ============================================================================

// NOTE ON UNITS: When fed from EffortSnapshot.bidVolSec/askVolSec (which are rates,
// not totals), the volume fields below hold ACCUMULATED RATE SUMS, not actual volumes.
// This is acceptable because MiniVP only uses these for RELATIVE comparisons (POC/VA
// detection, delta imbalance) where proportions are preserved regardless of unit.
// Do NOT use these values as actual traded volumes without rate→total conversion.
struct MicroVolumeAtPrice
{
    int price_tick = 0;       // Price in ticks
    double total_volume = 0.0;  // Sum of (bidVolSec + askVolSec) across bars (rate sum, not total)
    double bid_volume = 0.0;    // Sum of bidVolSec across bars (rate sum)
    double ask_volume = 0.0;    // Sum of askVolSec across bars (rate sum)
    int tpo_count = 0;        // Time periods at this price

    double Delta() const { return ask_volume - bid_volume; }
};

// ============================================================================
// MICRO NODE CONTEXT (Derived feature from micro-window HVN/LVN)
// ============================================================================

// Context classification for current price relative to micro-window HVN/LVN
// This is SEPARATE from session-level HVN/LVN (which comes from SC peaks/valleys)
enum class MicroNodeContext
{
    NONE = 0,           // Not near any micro HVN/LVN
    NEAR_MICRO_HVN = 1, // Within tolerance of micro HVN
    NEAR_MICRO_LVN = 2  // Within tolerance of micro LVN
};

// Derived micro features (tick domain, computed once per resolution check)
struct MicroNodeFeatures
{
    int distToMicroHVNTicks = INT_MAX;  // Distance to nearest micro HVN (INT_MAX if none)
    int distToMicroLVNTicks = INT_MAX;  // Distance to nearest micro LVN (INT_MAX if none)
    MicroNodeContext context = MicroNodeContext::NONE;
    bool valid = false;                  // True if micro profile has sufficient data

    // For logging
    int closeTicks = 0;
    int toleranceTicks = 0;
};

// ============================================================================
// MICRO AUCTION (For MiniVP)
// ============================================================================

struct MicroAuction
{
    std::map<int, MicroVolumeAtPrice> volume_profile;
    double tick_size = 0.0;
    double anchor_price = 0.0;

    // Derived levels
    double micro_poc = 0.0;
    double micro_vah = 0.0;
    double micro_val = 0.0;

    // Micro-window HVN/LVN stored as SORTED integer ticks for O(log N) nearest search
    // NOTE: These are micro-window derived features, NOT session-level HVN/LVN
    // Session HVN/LVN SSOT is Sierra Chart's GetStudyPeakValleyLine()
    std::vector<int> hvn_ticks;  // Sorted ascending
    std::vector<int> lvn_ticks;  // Sorted ascending

    // POC migration tracking
    double initial_poc = 0.0;
    int poc_migration_ticks = 0;

    // Progress metrics
    double mfe = 0.0;
    double mae = 0.0;
    int observation_bars = 0;
    double start_price = 0.0;

    // VbP context at probe anchor (from session profile)
    VbPLevelContext vbp_context;

    void Reset(double ts, double anchor, double startPrice)
    {
        volume_profile.clear();
        tick_size = ts;
        anchor_price = anchor;
        start_price = startPrice;
        micro_poc = 0.0;
        micro_vah = 0.0;
        micro_val = 0.0;
        hvn_ticks.clear();
        lvn_ticks.clear();
        initial_poc = 0.0;
        poc_migration_ticks = 0;
        mfe = 0.0;
        mae = 0.0;
        observation_bars = 0;
        vbp_context = VbPLevelContext();
    }

    void SetVbPContext(const VbPLevelContext& ctx)
    {
        vbp_context = ctx;
    }

    void AddBar(double high, double low, double close, double bid_vol, double ask_vol, ProbeDirection dir)
    {
        if (tick_size <= 0.0)
            return;

        const int highTick = static_cast<int>(std::llround(high / tick_size));
        const int lowTick = static_cast<int>(std::llround(low / tick_size));
        const int closeTick = static_cast<int>(std::llround(close / tick_size));

        const int range = highTick - lowTick + 1;
        if (range <= 0 || range > 1000)
            return;

        const double baseVol = (bid_vol + ask_vol) / range;
        const double bidBase = bid_vol / range;
        const double askBase = ask_vol / range;

        for (int t = lowTick; t <= highTick; ++t)
        {
            auto& vap = volume_profile[t];
            vap.price_tick = t;
            double weight = (t == closeTick) ? 1.5 : 1.0;
            vap.total_volume += baseVol * weight;
            vap.bid_volume += bidBase * weight;
            vap.ask_volume += askBase * weight;
            vap.tpo_count++;
        }

        observation_bars++;

        // Update MFE/MAE
        const double excursion = (close - start_price) / tick_size;
        if (dir == ProbeDirection::LONG)
        {
            mfe = (std::max)(mfe, excursion);
            mae = (std::min)(mae, excursion);
        }
        else
        {
            mfe = (std::max)(mfe, -excursion);
            mae = (std::min)(mae, -excursion);
        }

        ComputeDerivedLevels();
    }

    void ComputePOC()
    {
        if (volume_profile.empty())
            return;

        double maxVol = 0.0;
        int pocTick = 0;

        for (const auto& kv : volume_profile)
        {
            if (kv.second.total_volume > maxVol)
            {
                maxVol = kv.second.total_volume;
                pocTick = kv.first;
            }
        }

        const double newPoc = pocTick * tick_size;

        if (observation_bars == 1)
        {
            initial_poc = newPoc;
        }

        micro_poc = newPoc;
        poc_migration_ticks = static_cast<int>((micro_poc - initial_poc) / tick_size);
    }

    void ComputeValueArea()
    {
        if (volume_profile.empty() || tick_size <= 0.0)
            return;

        double maxVol = 0.0;
        int pocTick = 0;

        std::vector<std::pair<int, double>> sorted_vols;
        sorted_vols.reserve(volume_profile.size());
        for (const auto& kv : volume_profile)
        {
            sorted_vols.push_back({ kv.first, kv.second.total_volume });
            if (kv.second.total_volume > maxVol)
            {
                maxVol = kv.second.total_volume;
                pocTick = kv.first;
            }
        }
        std::sort(sorted_vols.begin(), sorted_vols.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        int pocIdx = 0;
        for (int i = 0; i < static_cast<int>(sorted_vols.size()); ++i)
        {
            if (sorted_vols[i].first == pocTick)
            {
                pocIdx = i;
                break;
            }
        }

        // SSOT: Use shared value area computation
        ComputeValueAreaFromSortedVector(sorted_vols, pocIdx, tick_size, 0.70, micro_val, micro_vah);
    }

    void ComputeHvnLvn()
    {
        hvn_ticks.clear();
        lvn_ticks.clear();

        if (volume_profile.size() < 5)
            return;

        std::vector<std::pair<int, double>> sorted_vols;
        sorted_vols.reserve(volume_profile.size());
        double totalVol = 0.0;

        for (const auto& kv : volume_profile)
        {
            sorted_vols.push_back({ kv.first, kv.second.total_volume });
            totalVol += kv.second.total_volume;
        }
        std::sort(sorted_vols.begin(), sorted_vols.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

        if (totalVol <= 0.0)
            return;

        const double mean = totalVol / sorted_vols.size();
        double variance = 0.0;
        for (const auto& sv : sorted_vols)
        {
            double diff = sv.second - mean;
            variance += diff * diff;
        }
        const double stddev = std::sqrt(variance / sorted_vols.size());

        const double hvnThreshold = mean + 1.5 * stddev;
        const double lvnThreshold = mean - 0.5 * stddev;

        // Store as integer ticks (sorted_vols is already sorted by tick)
        // Result: hvn_ticks and lvn_ticks are sorted ascending
        for (size_t i = 1; i < sorted_vols.size() - 1; ++i)
        {
            const double prev = sorted_vols[i - 1].second;
            const double curr = sorted_vols[i].second;
            const double next = sorted_vols[i + 1].second;

            if (curr > prev && curr > next && curr > hvnThreshold)
            {
                hvn_ticks.push_back(sorted_vols[i].first);  // Store tick, not price
            }
            else if (curr < prev && curr < next && curr < lvnThreshold)
            {
                lvn_ticks.push_back(sorted_vols[i].first);  // Store tick, not price
            }
        }
    }

    void ComputeDerivedLevels()
    {
        ComputePOC();
        ComputeValueArea();
        ComputeHvnLvn();
    }

    // =========================================================================
    // MICRO NODE FEATURE EXTRACTION (tick domain, O(log N) binary search)
    // =========================================================================

    /**
     * Compute micro-window HVN/LVN derived features for a given close price.
     * Uses binary search for O(log N) nearest neighbor finding.
     *
     * @param closePrice  Current close price
     * @param toleranceTicks  Tolerance for "near" classification (from config)
     * @return MicroNodeFeatures with distances and context classification
     *
     * SSOT: This is for micro-window only. Session-level HVN/LVN comes from
     *       Sierra Chart's GetStudyPeakValleyLine() via vbp_context.
     */
    MicroNodeFeatures GetMicroNodeFeatures(double closePrice, int toleranceTicks) const
    {
        MicroNodeFeatures features;
        features.toleranceTicks = toleranceTicks;

        // Need valid tick_size and sufficient profile data
        if (tick_size <= 0.0 || volume_profile.size() < 5)
        {
            features.valid = false;
            return features;
        }

        // Convert close to integer ticks using canonical conversion
        const int closeTicks = static_cast<int>(std::llround(closePrice / tick_size));
        features.closeTicks = closeTicks;
        features.valid = true;

        // Find distance to nearest micro HVN using binary search
        if (!hvn_ticks.empty())
        {
            features.distToMicroHVNTicks = FindNearestDistance(hvn_ticks, closeTicks);
        }

        // Find distance to nearest micro LVN using binary search
        if (!lvn_ticks.empty())
        {
            features.distToMicroLVNTicks = FindNearestDistance(lvn_ticks, closeTicks);
        }

        // Classify context (HVN takes priority if both within tolerance)
        if (features.distToMicroHVNTicks <= toleranceTicks)
        {
            features.context = MicroNodeContext::NEAR_MICRO_HVN;
        }
        else if (features.distToMicroLVNTicks <= toleranceTicks)
        {
            features.context = MicroNodeContext::NEAR_MICRO_LVN;
        }
        else
        {
            features.context = MicroNodeContext::NONE;
        }

        return features;
    }

private:
    // Binary search to find distance to nearest element in sorted vector
    // Returns absolute distance in ticks, or INT_MAX if vector is empty
    static int FindNearestDistance(const std::vector<int>& sortedTicks, int targetTick)
    {
        if (sortedTicks.empty())
            return INT_MAX;

        // Find first element >= target
        auto it = std::lower_bound(sortedTicks.begin(), sortedTicks.end(), targetTick);

        int minDist = INT_MAX;

        // Check element at/after target
        if (it != sortedTicks.end())
        {
            minDist = std::abs(*it - targetTick);
        }

        // Check element before target
        if (it != sortedTicks.begin())
        {
            --it;
            int distBefore = std::abs(*it - targetTick);
            minDist = (std::min)(minDist, distBefore);
        }

        return minDist;
    }

public:
};

// ============================================================================
// MINI VP MODULE (Micro Validator)
// ============================================================================

class MiniVPModule
{
public:
    // Configuration for micro node tie-breaker (caller sets from ZoneConfig)
    int micro_node_tol_ticks_ = 3;
    int diag_level_ = 0;  // For structured logging

    void SetMicroNodeTolerance(int ticks) { micro_node_tol_ticks_ = ticks; }
    void SetDiagLevel(int level) { diag_level_ = level; }

    void StartProbe(const ProbeRequest& request, double tickSize)
    {
        StartProbeWithContext(request, tickSize, VbPLevelContext(), request.t0);
    }

    void StartProbeWithContext(const ProbeRequest& request, double tickSize,
                                const VbPLevelContext& vbpContext,
                                SCDateTime absoluteStartTime)
    {
        active_request_ = request;
        status_ = ProbeStatus::OBSERVING;
        result_ = ProbeResult();
        result_.probe_id = request.probe_id;
        start_time_ = absoluteStartTime;
        is_active_ = true;

        auction_.Reset(tickSize, request.price, request.price);
        auction_.SetVbPContext(vbpContext);
    }

    void Update(double high, double low, double close, double bid_vol, double ask_vol,
        int bar_index, SCDateTime bar_time, double tickSize)
    {
        (void)bar_index;  // Unused
        if (!is_active_)
            return;

        auction_.AddBar(high, low, close, bid_vol, ask_vol, active_request_.direction);

        // Check timeout using ABSOLUTE TIME
        const double elapsedSeconds = (bar_time.GetAsDouble() - start_time_.GetAsDouble()) * 86400.0;
        if (elapsedSeconds >= static_cast<double>(active_request_.timeout_seconds))
        {
            status_ = ProbeStatus::TIMEOUT;
            result_.observation_time_ms = static_cast<int>(elapsedSeconds * 1000.0);
            FinalizeResult(MechanismTag::TIMEOUT_TAG);
            return;
        }

        EvaluateResolution(close, tickSize);
    }

    double GetElapsedSeconds(SCDateTime current_time) const
    {
        if (!is_active_) return 0.0;
        return (current_time.GetAsDouble() - start_time_.GetAsDouble()) * 86400.0;
    }

    bool IsActive() const { return is_active_; }

    ProbeResult GetResult() const
    {
        ProbeResult r = result_;
        r.status = status_;
        r.mfe = auction_.mfe;
        r.mae = auction_.mae;
        return r;
    }

    const ProbeRequest& GetActiveRequest() const { return active_request_; }

    void Clear()
    {
        is_active_ = false;
        status_ = ProbeStatus::OBSERVING;
    }

private:
    ProbeRequest active_request_;
    MicroAuction auction_;
    ProbeStatus status_ = ProbeStatus::OBSERVING;
    ProbeResult result_;
    SCDateTime start_time_;
    bool is_active_ = false;

    void EvaluateResolution(double close, double tickSize)
    {
        if (auction_.observation_bars < 3)
            return;

        const double excursionTicks = (close - active_request_.price) / tickSize;
        const bool isLong = (active_request_.direction == ProbeDirection::LONG);
        const VbPLevelContext& ctx = auction_.vbp_context;

        const int pocMigration = auction_.poc_migration_ticks;
        const bool favorablePrice = isLong ? (excursionTicks > 0) : (excursionTicks < 0);

        // =========================================================================
        // MICRO NODE TIE-BREAKER FEATURE EXTRACTION
        // SSOT: Session HVN/LVN from SC peaks/valleys (ctx.isHVN/isLVN)
        //       Micro HVN/LVN from local probe window (microFeatures)
        // =========================================================================
        const MicroNodeFeatures microFeatures = auction_.GetMicroNodeFeatures(close, micro_node_tol_ticks_);
        bool microInfluenced = false;

        // VbP-aware acceptance thresholds (baseline from session context)
        int acceptanceBarsRequired = 5;
        int acceptancePocTicks = 2;

        if (ctx.valid)
        {
            if (ctx.isLVN || ctx.distToLVNTicks < 3)
            {
                acceptanceBarsRequired = 3;
                acceptancePocTicks = 1;
            }
            else if (ctx.isHVN || ctx.distToHVNTicks < 3)
            {
                acceptanceBarsRequired = 7;
                acceptancePocTicks = 3;
            }
            else if (ctx.atPOC)
            {
                acceptanceBarsRequired = 10;
                acceptancePocTicks = 4;
            }
            else if (ctx.insideValueArea)
            {
                acceptanceBarsRequired = 6;
                acceptancePocTicks = 2;
            }
        }

        // MICRO TIE-BREAKER: Small bias when near micro HVN/LVN
        // Near micro HVN: Expect resistance → +1 bar requirement
        // Near micro LVN: Expect easier passage → -1 bar requirement
        if (microFeatures.valid && microFeatures.context != MicroNodeContext::NONE)
        {
            if (microFeatures.context == MicroNodeContext::NEAR_MICRO_HVN)
            {
                acceptanceBarsRequired += 1;  // Slightly harder to accept near resistance
                microInfluenced = true;
            }
            else if (microFeatures.context == MicroNodeContext::NEAR_MICRO_LVN)
            {
                acceptanceBarsRequired = (std::max)(2, acceptanceBarsRequired - 1);  // Slightly easier
                microInfluenced = true;
            }
        }

        const bool adjustedFavorablePoc = isLong ?
            (pocMigration >= acceptancePocTicks) : (pocMigration <= -acceptancePocTicks);

        if (adjustedFavorablePoc && favorablePrice &&
            auction_.observation_bars >= acceptanceBarsRequired)
        {
            status_ = ProbeStatus::ACCEPTED;
            FinalizeResultWithMicro(ClassifyAcceptance(), microFeatures, microInfluenced);
            return;
        }

        // VbP-aware rejection thresholds
        double adverseThreshold = -4.0;
        int pinnedBarsThreshold = 10;

        if (ctx.valid)
        {
            if (ctx.isHVN || ctx.atPOC)
            {
                adverseThreshold = -3.0;
                pinnedBarsThreshold = 7;
            }
            else if (ctx.isLVN)
            {
                adverseThreshold = -6.0;
                pinnedBarsThreshold = 15;
            }
            else if (!ctx.insideValueArea)
            {
                adverseThreshold = -5.0;
                pinnedBarsThreshold = 8;
            }
        }

        // MICRO TIE-BREAKER for rejection
        // Near micro HVN: Resistance expected → slightly easier to reject
        // Near micro LVN: Should move → slightly harder to reject
        if (microFeatures.valid && microFeatures.context != MicroNodeContext::NONE)
        {
            if (microFeatures.context == MicroNodeContext::NEAR_MICRO_HVN)
            {
                adverseThreshold += 0.5;  // Easier to reject (less adverse movement needed)
                pinnedBarsThreshold -= 1;
                microInfluenced = true;
            }
            else if (microFeatures.context == MicroNodeContext::NEAR_MICRO_LVN)
            {
                adverseThreshold -= 0.5;  // Harder to reject
                pinnedBarsThreshold += 1;
                microInfluenced = true;
            }
        }

        const bool adverseExcursion = (isLong ? auction_.mae : -auction_.mae) < adverseThreshold;
        const bool pocPinned = (std::abs(pocMigration) < 1) &&
                               (auction_.observation_bars > pinnedBarsThreshold);

        if (adverseExcursion)
        {
            status_ = ProbeStatus::REJECTED;
            FinalizeResultWithMicro(ClassifyRejection(), microFeatures, microInfluenced);
            return;
        }

        if (pocPinned)
        {
            status_ = ProbeStatus::REJECTED;
            FinalizeResultWithMicro(MechanismTag::VALUE_REJECTION, microFeatures, microInfluenced);
            return;
        }
    }

    void FinalizeResult(MechanismTag tag)
    {
        result_.mechanism = tag;
        result_.observation_time_ms = auction_.observation_bars * 1000;
        result_.resolution_log[0] = '\0';  // No structured log for legacy path
        result_.micro_influenced = false;
        is_active_ = false;
    }

    void FinalizeResultWithMicro(MechanismTag tag, const MicroNodeFeatures& micro, bool influenced)
    {
        result_.mechanism = tag;
        result_.observation_time_ms = auction_.observation_bars * 1000;
        result_.micro_influenced = influenced;

        // Build structured resolution log
        // Format: [RESOLUTION] status=X mech=Y bars=N poc_mig=M micro={hvn=H lvn=L ctx=C flag=F}
        const char* ctxStr = "NONE";
        if (micro.context == MicroNodeContext::NEAR_MICRO_HVN)
            ctxStr = "μHVN";
        else if (micro.context == MicroNodeContext::NEAR_MICRO_LVN)
            ctxStr = "μLVN";

        snprintf(result_.resolution_log, sizeof(result_.resolution_log),
            "[RESOLUTION] status=%s mech=%s bars=%d poc_mig=%d mfe=%.1f mae=%.1f micro={hvn=%d lvn=%d ctx=%s infl=%s}",
            to_string(status_),
            to_string(tag),
            auction_.observation_bars,
            auction_.poc_migration_ticks,
            auction_.mfe,
            auction_.mae,
            micro.valid ? micro.distToMicroHVNTicks : -1,
            micro.valid ? micro.distToMicroLVNTicks : -1,
            ctxStr,
            influenced ? "Y" : "N");

        is_active_ = false;
    }

    MechanismTag ClassifyAcceptance() const
    {
        if (auction_.mfe > 6.0)
            return MechanismTag::CLEAN_ACCEPTANCE;
        else
            return MechanismTag::WEAK_ACCEPTANCE;
    }

    MechanismTag ClassifyRejection() const
    {
        if (auction_.mfe > 3.0 && auction_.mae < -3.0)
            return MechanismTag::FALSE_BREAKOUT;

        if (auction_.mae < -6.0)
            return MechanismTag::ABSORPTION_WALL;

        return MechanismTag::VALUE_REJECTION;
    }
};

// ============================================================================
// ZONE STORE (Memory Trace)
// ============================================================================

struct ZoneRecord
{
    int zone_id = 0;
    int scenario_id = 0;
    SCDateTime t0;
    double anchor_price = 0.0;
    double micro_poc = 0.0;
    ProbeStatus result_status = ProbeStatus::OBSERVING;
    MechanismTag mechanism = MechanismTag::NONE;
    int quality_score = 0;
    SCDateTime last_touched;
};

class ZoneStore
{
public:
    void RecordProbeResult(const ProbeRequest& req, const ProbeResult& result, double microPoc)
    {
        ZoneRecord rec;
        rec.zone_id = next_zone_id_++;
        rec.scenario_id = req.scenario_id;
        rec.t0 = req.t0;
        rec.anchor_price = req.price;
        rec.micro_poc = microPoc;
        rec.result_status = result.status;
        rec.mechanism = result.mechanism;
        rec.quality_score = static_cast<int>(req.score);
        rec.last_touched = req.t0;

        zones_.push_back(rec);

        if (zones_.size() > 100)
            zones_.erase(zones_.begin());
    }

    const ZoneRecord* FindNearby(double price, double toleranceTicks, double tickSize) const
    {
        const double tolerance = toleranceTicks * tickSize;

        for (auto it = zones_.rbegin(); it != zones_.rend(); ++it)
        {
            if (std::abs(it->anchor_price - price) <= tolerance)
                return &(*it);
        }
        return nullptr;
    }

    size_t GetZoneCount() const { return zones_.size(); }

private:
    std::vector<ZoneRecord> zones_;
    int next_zone_id_ = 1;
};

// ============================================================================
// AUCTION CONTEXT MODULE (Macro Filter)
// Note: Uses SCENARIO_DATABASE defined in main .cpp file
// ============================================================================

class AuctionContextModule
{
public:
    // Update with external scenario database
    template<typename ScenarioArray>
    void Update(
        const AuctionContext& ctx,
        AMTMarketState detectedState,
        AggressionType detectedAggression,
        AuctionFacilitation detectedFacilitation,
        CurrentPhase detectedPhase,
        bool facilitationKnown,
        const ScenarioArray& scenarios,
        int scenarioCount)
    {
        (void)ctx;  // Unused in this path
        current_state_ = detectedState;
        current_aggression_ = detectedAggression;
        current_facilitation_ = detectedFacilitation;
        current_phase_ = detectedPhase;
        facilitation_known_ = facilitationKnown;

        valid_scenarios_.clear();

        for (int i = 0; i < scenarioCount; ++i)
        {
            const ScenarioEntry& entry = scenarios[i];
            ScenarioMatch match = ComputeMatch(entry);

            if (match.match_score > 0)
            {
                valid_scenarios_.push_back(match);
            }
        }

        std::sort(valid_scenarios_.begin(), valid_scenarios_.end(),
            [](const ScenarioMatch& a, const ScenarioMatch& b)
            {
                int scoreA = a.quality_score + a.match_score + (a.exact_match ? 10 : 0);
                int scoreB = b.quality_score + b.match_score + (b.exact_match ? 10 : 0);
                return scoreA > scoreB;
            });
    }

    const std::vector<ScenarioMatch>& GetValidScenarios() const
    {
        return valid_scenarios_;
    }

    AuctionMode DetermineMode() const
    {
        if (valid_scenarios_.empty())
            return AuctionMode::MODE_LOCKED;

        const ScenarioMatch& top = valid_scenarios_[0];
        if (top.entry == nullptr)
            return AuctionMode::MODE_LOCKED;

        if (top.entry->key.state == AMTMarketState::BALANCE)
            return AuctionMode::MODE_ROTATIONAL;
        else
            return AuctionMode::MODE_DIRECTIONAL;
    }

private:
    AMTMarketState current_state_ = AMTMarketState::BALANCE;
    AggressionType current_aggression_ = AggressionType::NEUTRAL;
    AuctionFacilitation current_facilitation_ = AuctionFacilitation::EFFICIENT;
    CurrentPhase current_phase_ = CurrentPhase::ROTATION;
    bool facilitation_known_ = false;

    std::vector<ScenarioMatch> valid_scenarios_;

    // Helper: Check if two phases are equivalent for scenario matching
    // DRIVING_UP and DRIVING_DOWN are treated as equivalent (both are directional)
    static bool PhasesAreEquivalent(CurrentPhase scenario, CurrentPhase actual)
    {
        if (scenario == actual)
            return true;

        // Directional equivalence: DRIVING_UP matches DRIVING_DOWN and vice versa
        const bool scenarioDirectional = (scenario == CurrentPhase::DRIVING_UP ||
                                          scenario == CurrentPhase::DRIVING_DOWN);
        const bool actualDirectional = (actual == CurrentPhase::DRIVING_UP ||
                                        actual == CurrentPhase::DRIVING_DOWN);

        return scenarioDirectional && actualDirectional;
    }

    ScenarioMatch ComputeMatch(const ScenarioEntry& entry) const
    {
        ScenarioMatch result;
        result.scenario_id = entry.scenario_id;
        result.quality_score = entry.quality_score;
        result.entry = &entry;
        result.match_score = 0;
        result.exact_match = false;

        if (entry.key.state != current_state_)
            return result;
        result.match_score += 3;

        if (entry.key.aggression != current_aggression_)
            return result;
        result.match_score += 3;

        // Phase matching: DRIVING_UP and DRIVING_DOWN are equivalent
        if (!PhasesAreEquivalent(entry.key.phase, current_phase_))
            return result;
        result.match_score += 2;

        if (facilitation_known_)
        {
            if (entry.key.facilitation == current_facilitation_)
            {
                result.match_score += 2;
                result.exact_match = true;
            }
        }
        else
        {
            result.match_score += 1;
        }

        return result;
    }
};

// ============================================================================
// DYNAMIC GAUGE MODULE (Macro Trigger)
// ============================================================================

class DynamicGaugeModule
{
public:
    double threshold_ = 7.0;
    int probe_timeout_ = 120;

    void SetThreshold(double t) { threshold_ = t; }
    void SetTimeout(int t) { probe_timeout_ = t; }

    void Update(
        double volume_percentile,
        double delta_percentile,
        double price,
        double poc,
        double vah,
        double val,
        const std::vector<ScenarioMatch>& valid_scenarios,
        int bar_index,
        SCDateTime bar_time)
    {
        (void)bar_index;  // Unused
        should_fire_ = false;
        pending_request_ = ProbeRequest();

        if (valid_scenarios.empty())
            return;

        const ScenarioMatch& best = valid_scenarios[0];
        if (best.entry == nullptr)
            return;

        EvidenceScore evidence = ComputeEvidence(
            volume_percentile, delta_percentile, price, poc, vah, val);

        const double exact_bonus = best.exact_match ? 1.0 : 0.0;
        computed_score_ = best.quality_score + evidence.Total() + exact_bonus;

        if (computed_score_ < threshold_)
            return;

        should_fire_ = true;
        pending_request_.probe_id = next_probe_id_++;
        pending_request_.scenario_id = best.scenario_id;
        pending_request_.price = price;
        pending_request_.zone_id = -1;
        pending_request_.t0 = bar_time;
        pending_request_.score = computed_score_;
        pending_request_.timeout_seconds = probe_timeout_;
        pending_request_.hypothesis = best.entry->hypothesis_template;

        if (best.entry->primary_intent == AuctionIntent::DISTRIBUTION)
        {
            pending_request_.direction = (price > poc) ? ProbeDirection::SHORT : ProbeDirection::LONG;
        }
        else
        {
            pending_request_.direction = (price < poc) ? ProbeDirection::LONG : ProbeDirection::SHORT;
        }
    }

    bool ShouldFireProbe() const { return should_fire_; }
    ProbeRequest CreateProbeRequest() const { return pending_request_; }
    double GetComputedScore() const { return computed_score_; }

private:
    int next_probe_id_ = 1;
    bool should_fire_ = false;
    double computed_score_ = 0.0;
    ProbeRequest pending_request_;

    EvidenceScore ComputeEvidence(
        double volume_percentile,
        double delta_percentile,
        double price,
        double poc,
        double vah,
        double val) const
    {
        EvidenceScore score;

        // Tier 1: Volume (0-1)
        if (volume_percentile > 80.0)
            score.volume_score = 1.0;
        else if (volume_percentile > 60.0)
            score.volume_score = 0.7;
        else if (volume_percentile > 40.0)
            score.volume_score = 0.4;
        else
            score.volume_score = 0.2;

        // Tier 2: Delta (0-1)
        const double absDelta = std::abs(delta_percentile - 50.0);
        if (absDelta > 30.0)
            score.delta_score = 1.0;
        else if (absDelta > 20.0)
            score.delta_score = 0.7;
        else if (absDelta > 10.0)
            score.delta_score = 0.4;
        else
            score.delta_score = 0.2;

        // Tier 3: Initiative/Progress (0-1)
        if (IsValidPrice(vah) && IsValidPrice(val) && vah > val)
        {
            if (price > vah || price < val)
                score.initiative_score = 1.0;
            else if (price > poc * 1.001 || price < poc * 0.999)
                score.initiative_score = 0.5;
            else
                score.initiative_score = 0.2;
        }
        else
        {
            score.initiative_score = 0.3;
        }

        return score;
    }
};

} // namespace AMT

#endif // AMT_MODULES_H
