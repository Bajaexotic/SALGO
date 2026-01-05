// ============================================================================
// AMT_Snapshots.h
// Observable market data structures, baseline tracking, and drift detection
// Extracted from AuctionSensor_v1.cpp for modularization
// ============================================================================

#ifndef AMT_SNAPSHOTS_H
#define AMT_SNAPSHOTS_H

// Only include sierrachart.h if not already mocked (for standalone testing)
#ifndef SIERRACHART_H
#include "sierrachart.h"
#endif
#include "amt_core.h"
#include "AMT_Helpers.h"
#include <cstdint>
#include <deque>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

namespace AMT {

// ============================================================================
// DEPTH POINT
// ============================================================================

struct DepthPoint
{
    int   distTicks = 0;
    double qty = 0.0;
};

// ============================================================================
// OBSERVABLE SNAPSHOTS (Per-Update Normalized Data)
// ============================================================================

struct StructureSnapshot
{
    // VP (Volume Profile)
    double vpbVAH = 0.0;
    double vpbVAL = 0.0;
    double vpbPOC = 0.0;
    // NOTE: Peaks/Valleys loaded via sc.GetStudyPeakValleyLine() into SessionVolumeProfile

    // TPO
    double tpoVAH = 0.0;
    double tpoVAL = 0.0;
    double tpoPOC = 0.0;

    // VWAP + Bands
    double vwap = 0.0;
    double vwapUpper1 = 0.0;
    double vwapLower1 = 0.0;
    double vwapUpper2 = 0.0;
    double vwapLower2 = 0.0;

    // Daily
    double dailyHigh = 0.0;
    double dailyLow = 0.0;
};

// ============================================================================
// EFFORT SNAPSHOT - SSOT CONTRACT
// ============================================================================
// This struct captures per-bar effort signals with EXPLICIT unit semantics.
//
// ** RATE SIGNALS (per-second intensity, from Numbers Bars Inputs 70-71) **
//   bidVolSec, askVolSec : Volume traded at bid/ask PER SECOND
//   tradesSec            : Number of trades PER SECOND
//   deltaSec             : Net delta (ask-bid volume) PER SECOND
//
// ** TOTAL SIGNALS (per-bar aggregates) - ROBUST POLICY: Native SC arrays as SSOT **
//   totalVolume          : Total volume FOR THE BAR (SSOT: native sc.Volume[idx])
//   delta                : BAR delta = AskVolume - BidVolume (SSOT: sc.AskVolume - sc.BidVolume)
//   maxDelta             : Maximum single-price delta FOR THE BAR (Optional: NB SG8)
//   cumDelta             : DAY cumulative delta (DEBUG ONLY: NB SG10 for cross-check)
//                          Production SSOT: sessionAccum.sessionCumDelta (internal accumulation)
//
// ** SESSION CUMULATIVE DELTA SEMANTICS (CLOSED-BAR ONLY) **
//   sessionAccum.sessionCumDelta includes ONLY CLOSED/FINALIZED bars.
//   The current forming bar is NOT included (it has partial values).
//   This means sessionCumDelta LAGS by the current bar's delta intrabar.
//
//   For live parity with NB's intrabar cumDelta:
//     sessionCumDeltaLive = sessionCumDelta + snap.effort.delta (current bar's partial)
//
//   INTRABAR STABILITY: sessionCumDelta does NOT change between ticks within the same bar.
//   It only updates when a bar CLOSES (detected via isNewBar + lastAccumulatedBarIndex).
//
// ** RATIO SIGNALS (dimensionless) **
//   deltaPct             : Bar deltaRatio = delta / totalVolume (-1 to +1, derived from native SC)
//   ratioAvg             : Bid/Ask volume ratio (bidVolSec / askVolSec)
//
// CONSUMERS MUST NOT MIX UNITS:
//   - Compare rates against rate baselines (vol_sec, trades_sec, delta_sec)
//   - Compare totals against total baselines (total_vol, max_delta)
//   - To derive totals from rates: rate * sc.SecondsPerBar
//   - To derive rates from totals: total / sc.SecondsPerBar
//
// TIMEBASE WARNING:
//   - sc.SecondsPerBar == 0 for non-time-based bars (tick, range, volume charts)
//   - When SecondsPerBar == 0, rate signals are NOT MEANINGFUL (no fixed time denominator)
//   - Diagnostics should show "BarSec=N/A (non-time)" and skip rate→total conversion
//   - MiniVP still works (uses relative proportions) but accumulated "volumes" are rate sums
// ============================================================================
struct EffortSnapshot
{
    // --- RATE SIGNALS (vol/sec) - PRIMARY from Numbers Bars Inputs 70-71 ---
    double bidVolSec = 0.0;     // Volume at bid per second (Input 70: NB SG53)
    double askVolSec = 0.0;     // Volume at ask per second (Input 71: NB SG54)
    double tradesSec = 0.0;     // Trades per second (derived: sc.NumberOfTrades / SecondsPerBar)
    double deltaSec = 0.0;      // Delta per second (derived: delta / SecondsPerBar, + = buying)

    // --- TOTAL SIGNALS (vol/bar) - ROBUST POLICY: Native SC arrays as SSOT ---
    double totalVolume = 0.0;   // SSOT: sc.Volume[idx] (not NB)
    double delta = 0.0;         // SSOT: sc.AskVolume[idx] - sc.BidVolume[idx]
    double maxDelta = 0.0;      // Optional: NB SG8 (single-price imbalance)
    double cumDelta = 0.0;      // DEBUG ONLY: NB SG10 (production uses sessionAccum.sessionCumDelta)

    // --- RATIO SIGNALS (dimensionless) ---
    double deltaPct = 0.0;      // Derived: delta / totalVolume (-1 to +1)
    double ratioAvg = 0.0;      // Bid/Ask ratio (bidVolSec / askVolSec)

    // =========================================================================
    // DEPRECATED: Staging fields only - SSOT is Liq3Result (Dec 2024)
    // These are populated at bar start, then copied to lastLiqSnap.
    // Consumers should read from lastLiqSnap, NOT from snap.effort.*
    // =========================================================================

    // --- FOOTPRINT DIAGONAL DELTA (from Numbers Bars SG43/SG44) ---
    // SSOT: Liq3Result.diagonalPosDeltaSum etc (via lastLiqSnap)
    double diagonalPosDeltaSum = 0.0;  // STAGING ONLY - read from lastLiqSnap
    double diagonalNegDeltaSum = 0.0;  // STAGING ONLY - read from lastLiqSnap
    double diagonalNetDelta = 0.0;     // STAGING ONLY - read from lastLiqSnap
    bool diagonalDeltaValid = false;   // STAGING ONLY - read from lastLiqSnap

    // --- AVERAGE TRADE SIZE (from Numbers Bars SG51/SG52) ---
    // SSOT: Liq3Result.avgBidTradeSize etc (via lastLiqSnap)
    double avgBidTradeSize = 0.0;      // STAGING ONLY - read from lastLiqSnap
    double avgAskTradeSize = 0.0;      // STAGING ONLY - read from lastLiqSnap
    double avgTradeSizeRatio = 0.0;    // STAGING ONLY - read from lastLiqSnap
    bool avgTradeSizeValid = false;    // STAGING ONLY - read from lastLiqSnap
};

struct LiquiditySnapshot
{
    // DOM Size (raw counts from SC)
    double domBidSize = 0.0;
    double domAskSize = 0.0;

    // Stack/Pull - STAGING ONLY, SSOT is Liq3Result.directBidStackPull/directAskStackPull
    // Read at bar start, then copied to lastLiqSnap. Consumers read from lastLiqSnap.
    double bidStackPull = 0.0;    // STAGING ONLY - read from lastLiqSnap
    double askStackPull = 0.0;    // STAGING ONLY - read from lastLiqSnap

    // Depth Bars OHLC (aggregated)
    double depthOpen = 0.0;
    double depthHigh = 0.0;
    double depthLow = 0.0;
    double depthClose = 0.0;

    // Best Bid/Ask
    double bestBid = 0.0;
    double bestAsk = 0.0;

    // Halo metrics (weighted depth around midprice)
    double haloMass = 0.0;        // Total weighted depth within halo radius
    double haloBidMass = 0.0;     // Weighted bid depth
    double haloAskMass = 0.0;     // Weighted ask depth
    double haloImbalance = 0.0;   // (Bid - Ask) / Total, range [-1, +1]
    bool haloValid = false;       // True if computed from valid DOM data
};

struct ObservableSnapshot
{
    SCDateTime barTime;
    StructureSnapshot structure;
    EffortSnapshot effort;
    LiquiditySnapshot liquidity;

    bool isValid = false;
    bool isWarmUp = true;
};

// ============================================================================
// DRIFT TRACKER (Study Drift Safety)
// Note: Uses IsValidPrice() from AMT_Helpers.h
// ============================================================================

struct DriftTracker
{
    // Previous structure anchors for drift detection
    double prevVpbPOC = 0.0;
    double prevTpoPOC = 0.0;
    double prevVwap = 0.0;

    // Bug detection threshold (in ticks) - only fires on truly anomalous jumps
    int bugDetectionTicks = 100;  // 25+ pts on ES - would never happen normally

    // DOM validity tracking
    int consecutiveZeroDomBars = 0;
    int maxZeroDomBarsBeforeWarn = 5;

    // Warm-up tracking
    int barsProcessed = 0;
    int warmUpBarsRequired = 50;  // Baseline needs N bars to stabilize

    // Debug-only: logs anomalies that indicate bugs, doesn't gate any behavior
    void checkForAnomalies(double newVal, double& prevVal, double tickSize, const char* name,
                           SCStudyInterfaceRef sc, int diagLevel, bool isLiveBar = false)
    {
        // Check for value becoming invalid (NaN, 0, negative)
        if (IsValidPrice(prevVal) && !IsValidPrice(newVal))
        {
            if (diagLevel >= 1 && isLiveBar)
            {
                SCString msg;
                msg.Format("[BUG?] %s became invalid: was %.2f, now %.2f", name, prevVal, newVal);
                sc.AddMessageToLog(msg, 1);
            }
            return;  // Don't update prevVal with invalid data
        }

        if (!IsValidPrice(prevVal))
        {
            prevVal = newVal;
            return;
        }

        if (!IsValidPrice(newVal))
            return;

        // Check for truly excessive movement (indicates data bug, not market movement)
        const int driftTicks = static_cast<int>(std::abs(newVal - prevVal) / tickSize);
        if (driftTicks > bugDetectionTicks)
        {
            if (diagLevel >= 1 && isLiveBar)
            {
                SCString msg;
                msg.Format("[BUG?] %s jumped excessively: %.2f -> %.2f (%d ticks)",
                    name, prevVal, newVal, driftTicks);
                sc.AddMessageToLog(msg, 1);
            }
        }

        prevVal = newVal;
    }

    // Returns true if DOM just became stale (hit warning threshold this bar)
    bool checkDomValidity(double bidSize, double askSize, double bidStack, double askStack,
                          SCStudyInterfaceRef sc, int diagLevel, bool isLiveBar = false)
    {
        const bool allZero = (bidSize <= 0.0 && askSize <= 0.0 &&
            std::abs(bidStack) < 1e-9 && std::abs(askStack) < 1e-9);

        bool justBecameStale = false;
        if (allZero)
        {
            consecutiveZeroDomBars++;
            if (consecutiveZeroDomBars == maxZeroDomBarsBeforeWarn)
            {
                justBecameStale = true;
                if (diagLevel >= 1 && isLiveBar)
                {
                    sc.AddMessageToLog("[DRIFT] DOM stack/pull is zero/missing for extended period", 1);
                }
            }
        }
        else
        {
            consecutiveZeroDomBars = 0;
        }
        return justBecameStale;
    }

    bool isWarmedUp() const
    {
        return barsProcessed >= warmUpBarsRequired;
    }

    void incrementBars()
    {
        if (barsProcessed < warmUpBarsRequired)
            barsProcessed++;
    }
};

// ============================================================================
// ROLLING DISTRIBUTION (Robust Statistics)
// ============================================================================

struct RollingDist
{
    std::deque<double> values;
    int window = 300;

    void reset(int w)
    {
        window = w;
        values.clear();
    }

    void push(double v)
    {
        if (!std::isfinite(v))
            return;

        values.push_back(v);
        if (values.size() > static_cast<size_t>(window))
            values.pop_front();
    }

    // ========================================================================
    // BANNED LEGACY APIS - These have silent fallbacks that violate no-fallback contract
    // Use Try* APIs instead. These remain for compile compatibility but assert on misuse.
    // ========================================================================
    double percentile(double val) const
    {
        assert(!values.empty() && "BUG: percentile() called on empty baseline - use TryPercentile()");
        if (values.empty())
            return std::numeric_limits<double>::quiet_NaN();  // NaN propagates errors visibly

        int countBelow = 0;
        for (double v : values)
            if (v < val)
                ++countBelow;

        return static_cast<double>(countBelow) /
            static_cast<double>(values.size()) * 100.0;
    }

    double mean() const
    {
        assert(!values.empty() && "BUG: mean() called on empty baseline - use TryMean()");
        if (values.empty())
            return std::numeric_limits<double>::quiet_NaN();  // NaN propagates errors visibly

        double sum = 0.0;
        for (double v : values)
            sum += v;

        return sum / static_cast<double>(values.size());
    }

    double median() const
    {
        assert(!values.empty() && "BUG: median() called on empty baseline - use TryMedian()");
        if (values.empty())
            return std::numeric_limits<double>::quiet_NaN();  // NaN propagates errors visibly

        std::vector<double> sorted(values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());

        const size_t n = sorted.size();
        if (n % 2 == 0)
            return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
        else
            return sorted[n / 2];
    }

    // MAD (Median Absolute Deviation)
    double mad() const
    {
        if (values.size() < 2)
            return 0.0;

        const double med = median();

        std::vector<double> absDevs;
        absDevs.reserve(values.size());

        for (double v : values)
            absDevs.push_back(std::abs(v - med));

        std::sort(absDevs.begin(), absDevs.end());

        const size_t n = absDevs.size();
        if (n % 2 == 0)
            return (absDevs[n / 2 - 1] + absDevs[n / 2]) / 2.0;
        else
            return absDevs[n / 2];
    }

    // Check if value is extreme (beyond k * MAD from median)
    bool isExtreme(double val, double kFactor = 2.5) const
    {
        if (values.size() < 10)
            return false;

        const double med = median();
        const double m = mad();

        if (m < 1e-9)
            return false;

        return std::abs(val - med) > kFactor * m * 1.4826;  // 1.4826 scales MAD to std dev
    }

    // Percentile rank using robust method
    double percentileRank(double val) const
    {
        assert(!values.empty() && "BUG: percentileRank() called on empty baseline - use TryPercentileRank()");
        if (values.empty())
            return std::numeric_limits<double>::quiet_NaN();  // NaN propagates errors visibly

        const double med = median();
        const double m = mad();

        if (m < 1e-9)
            return (val >= med) ? 75.0 : 25.0;

        // Z-score equivalent using MAD
        const double z = (val - med) / (m * 1.4826);

        // Convert to percentile (approximate normal CDF)
        const double p = 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
        return p * 100.0;
    }

    size_t size() const { return values.size(); }

    // ========================================================================
    // READINESS CHECK (No-Fallback Contract)
    // ========================================================================
    // Returns readiness state based on sample count threshold.
    // Consumers MUST check readiness before using statistical outputs.
    //
    // Usage:
    //   BaselineReadiness r = dist.GetReadiness(MIN_SAMPLES);
    //   if (r != BaselineReadiness::READY) {
    //       // Skip computation, set *Valid=false
    //   }
    // ========================================================================
    BaselineReadiness GetReadiness(size_t minSamples) const {
        if (values.empty()) {
            return BaselineReadiness::UNAVAILABLE;
        }
        if (values.size() < minSamples) {
            return BaselineReadiness::WARMUP;
        }
        return BaselineReadiness::READY;
    }

    // Convenience: IsReady() for common threshold check
    bool IsReady(size_t minSamples) const {
        return GetReadiness(minSamples) == BaselineReadiness::READY;
    }

    // ========================================================================
    // TRY* APIs (No-Fallback Contract)
    // ========================================================================
    // These APIs return explicit validity instead of numeric fallbacks.
    // When valid=false, the value field is UNDEFINED and must not be used.
    //
    // Consumers MUST check result.valid before using result.value:
    //   auto result = dist.TryPercentile(val);
    //   if (!result.valid) {
    //       // Handle NO_EVIDENCE case - exclude from scoring
    //   } else {
    //       // Use result.value
    //   }
    // ========================================================================

    AMT::PercentileResult TryPercentile(double val) const {
        if (values.empty())
            return AMT::PercentileResult::Invalid();

        int countBelow = 0;
        for (double v : values)
            if (v < val)
                ++countBelow;

        const double pct = static_cast<double>(countBelow) /
            static_cast<double>(values.size()) * 100.0;
        return AMT::PercentileResult::Valid(pct);
    }

    AMT::PercentileResult TryPercentileRank(double val) const {
        if (values.empty())
            return AMT::PercentileResult::Invalid();

        const double med = median();  // Note: median() still has fallback, but we gated above
        const double m = mad();

        double pctRank;
        if (m < 1e-9) {
            // Degenerate case: all values identical
            pctRank = (val >= med) ? 75.0 : 25.0;
        } else {
            // Z-score equivalent using MAD
            const double z = (val - med) / (m * 1.4826);
            // Convert to percentile (approximate normal CDF)
            const double p = 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
            pctRank = p * 100.0;
        }
        return AMT::PercentileResult::Valid(pctRank);
    }

    AMT::MeanResult TryMean() const {
        if (values.empty())
            return AMT::MeanResult::Invalid();

        double sum = 0.0;
        for (double v : values)
            sum += v;

        return AMT::MeanResult::Valid(sum / static_cast<double>(values.size()));
    }

    AMT::MeanResult TryMedian() const {
        if (values.empty())
            return AMT::MeanResult::Invalid();

        std::vector<double> sorted(values.begin(), values.end());
        std::sort(sorted.begin(), sorted.end());

        const size_t n = sorted.size();
        double med;
        if (n % 2 == 0)
            med = (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
        else
            med = sorted[n / 2];

        return AMT::MeanResult::Valid(med);
    }
};

// ============================================================================
// BASELINE ENGINE - REMOVED (Dec 2024)
// ============================================================================
// The legacy BaselineEngine struct has been removed. It was a single rolling-
// window that mixed all phases/times together, which violated the requirement
// for phase-specific baselines.
//
// New architecture uses three separate components:
//   1. EffortBaselineStore (below) - Per-bucket (OPEN/MID/POWER) effort distributions
//      from prior 5 RTH sessions. Populated by PopulateEffortBaselines().
//   2. SessionDeltaBaseline (below) - Session-aggregate delta ratio baseline.
//      Populated from prior RTH session aggregates.
//   3. DOMWarmup (below) - Live 15-minute warmup for DOM metrics at RTH open.
//      Populated from live bars, frozen after warmup period.
//
// GBX Policy: Effort baselines return NOT_APPLICABLE outside RTH.
// ============================================================================

// ============================================================================
// EFFORT BUCKET BASELINE (Bar-Sample Distributions per Bucket)
// ============================================================================
// Stores bar-level samples from prior 5 RTH sessions, organized by time bucket.
// Each RollingDist holds individual bar samples (NOT bucket summaries).
//
// Constraint #1: Bar-level baselines are SEPARATE from session-aggregate baselines.
// Do NOT compare sessionDeltaRatio against per-bar delta_pct baseline.
//
// Constraint #2: Coverage threshold is proportional to expected bars in bucket.
// ============================================================================

struct EffortBucketDistribution {
    // Bar-level metric distributions for this bucket
    RollingDist vol_sec;            // All bar vol_sec samples
    RollingDist trades_sec;         // All bar trades_sec samples
    RollingDist delta_pct;          // All bar delta_pct samples (NOT sessionDeltaRatio!)
    RollingDist bar_range;          // All bar range samples in ticks (high - low)
    RollingDist avg_trade_size;     // Bar volume / numTrades - microstructure regime
    RollingDist abs_close_change;   // |close - prevClose| in ticks - directional travel
    RollingDist range_velocity;     // bar_range / bar_duration (ticks/minute) - auction pace

    // Synthetic bar distributions (for 1-min chart regime detection)
    // These are populated once per N bars (when synthetic bar completes)
    // Regime detection queries these instead of bar_range when in synthetic mode
    RollingDist synthetic_bar_range;      // Synthetic range: max(highs) - min(lows) over N bars
    RollingDist synthetic_range_velocity; // Synthetic velocity: synthetic_range / synthetic_duration
    RollingDist synthetic_efficiency;     // Kaufman ER: |net change| / path length [0-1]

    // Session tracking
    int sessionsContributed = 0;     // How many sessions have pushed bars to this bucket
    int totalBarsPushed = 0;         // Total bar samples across all contributing sessions
    int expectedBarsPerSession = 0;  // Expected bar count per session (set from chart timeframe)

    static constexpr int REQUIRED_SESSIONS = 5;
    static constexpr double MIN_COVERAGE_RATIO = 0.5;  // Require at least 50% of expected bars

    void Reset(int window = 6000) {  // ~1000 bars × 5 sessions (covers GLOBEX on 1-min)
        vol_sec.reset(window);
        trades_sec.reset(window);
        delta_pct.reset(window);
        bar_range.reset(window);
        avg_trade_size.reset(window);
        abs_close_change.reset(window);
        range_velocity.reset(window);
        // Synthetic baselines use smaller window (1 entry per N bars)
        // 6000/5 = 1200 synthetic entries (5-bar aggregation)
        synthetic_bar_range.reset(window / 5);
        synthetic_range_velocity.reset(window / 5);
        synthetic_efficiency.reset(window / 5);
        sessionsContributed = 0;
        totalBarsPushed = 0;
        expectedBarsPerSession = 0;
    }

    // Set expected bars per session based on chart timeframe
    void SetExpectedBarsPerSession(int expected) {
        expectedBarsPerSession = expected;
    }

    // Get minimum bars required per session for coverage threshold
    int GetMinBarsPerSession() const {
        if (expectedBarsPerSession <= 0)
            return 10;  // Fallback minimum
        return static_cast<int>(expectedBarsPerSession * MIN_COVERAGE_RATIO);
    }

    AMT::BucketBaselineState GetState() const {
        if (sessionsContributed < REQUIRED_SESSIONS)
            return AMT::BucketBaselineState::INSUFFICIENT_SESSIONS;

        // Check if we have adequate coverage
        const int minTotalBars = REQUIRED_SESSIONS * GetMinBarsPerSession();
        if (totalBarsPushed < minTotalBars)
            return AMT::BucketBaselineState::INSUFFICIENT_COVERAGE;

        return AMT::BucketBaselineState::READY;
    }

    bool IsReady() const { return GetState() == AMT::BucketBaselineState::READY; }

    // Diagnostic: Get session and bar counts
    void GetDiagnostics(int& outSessions, int& outBars, int& outExpected, int& outMinRequired) const {
        outSessions = sessionsContributed;
        outBars = totalBarsPushed;
        outExpected = expectedBarsPerSession * REQUIRED_SESSIONS;
        outMinRequired = GetMinBarsPerSession() * REQUIRED_SESSIONS;
    }
};

// All session phase effort buckets (7 tradeable phases)
struct EffortBaselineStore {
    EffortBucketDistribution buckets[AMT::EFFORT_BUCKET_COUNT];

    void Reset(int window = 6000) {  // ~1000 bars × 5 sessions (covers GLOBEX on 1-min)
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i)
            buckets[i].Reset(window);
    }

    // Get bucket by SessionPhase
    // NO-FALLBACK: Asserts on invalid phase (caller bug). Returns GLOBEX bucket for safety in release.
    EffortBucketDistribution& Get(AMT::SessionPhase phase) {
        const int idx = AMT::SessionPhaseToBucketIndex(phase);
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: Get() called with non-tradeable phase");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    const EffortBucketDistribution& Get(AMT::SessionPhase phase) const {
        const int idx = AMT::SessionPhaseToBucketIndex(phase);
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: Get() called with non-tradeable phase");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    // Get bucket by index directly
    // NO-FALLBACK: Asserts on invalid index (caller bug). Returns GLOBEX bucket for safety in release.
    EffortBucketDistribution& GetByIndex(int idx) {
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: GetByIndex() called with invalid index");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    const EffortBucketDistribution& GetByIndex(int idx) const {
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: GetByIndex() called with invalid index");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    // Set expected bars per session for all buckets based on chart timeframe
    void SetExpectedBarsPerSession(int barIntervalSeconds) {
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            const AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
            const int expected = AMT::GetExpectedBarsInPhase(phase, barIntervalSeconds);
            buckets[i].SetExpectedBarsPerSession(expected);
        }
    }

    // Check if all buckets are ready (for overall readiness)
    bool AllBucketsReady() const {
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            if (!buckets[i].IsReady())
                return false;
        }
        return true;
    }

    // Check if bucket has reached 5 contributing sessions
    bool BucketHasEnoughSessions(AMT::SessionPhase phase) const {
        return Get(phase).sessionsContributed >= EffortBucketDistribution::REQUIRED_SESSIONS;
    }
};

// ============================================================================
// SESSION DELTA BASELINE (Phase-Bucketed - Matches EffortBaselineStore Pattern)
// ============================================================================
// Stores phase-level delta ratios from prior sessions, bucketed by SessionPhase.
// This is SEPARATE from bar-level delta_pct baseline per Constraint #1.
//
// phaseDeltaRatio = phaseCumDelta / phaseTotalVolume (per phase within session)
// Compare current phase's delta ratio against historical same-phase delta ratios.
//
// DESIGN: Each phase bucket tracks cumulative delta/volume for that phase only.
// This allows apples-to-apples comparison: "current IB delta" vs "historical IB deltas".
// ============================================================================

struct SessionDeltaBucket {
    RollingDist delta_ratio;  // |phaseDeltaRatio| from prior sessions for this phase
    int sessionsContributed = 0;
    static constexpr int REQUIRED_SESSIONS = 5;

    void Reset(int window = 50) {
        delta_ratio.reset(window);
        sessionsContributed = 0;
    }

    void Push(double phaseDeltaRatio) {
        delta_ratio.push(std::abs(phaseDeltaRatio));
    }

    void IncrementSessionCount() {
        sessionsContributed++;
    }

    AMT::SessionBaselineState GetState() const {
        if (sessionsContributed < REQUIRED_SESSIONS)
            return AMT::SessionBaselineState::INSUFFICIENT_SESSIONS;
        return AMT::SessionBaselineState::READY;
    }

    bool IsReady() const { return GetState() == AMT::SessionBaselineState::READY; }

    AMT::PercentileResult TryGetPercentile(double phaseDeltaRatio) const {
        if (!IsReady())
            return AMT::PercentileResult::Invalid();
        return delta_ratio.TryPercentile(std::abs(phaseDeltaRatio));
    }
};

struct SessionDeltaBaseline {
    SessionDeltaBucket buckets[AMT::EFFORT_BUCKET_COUNT];

    void Reset(int window = 50) {
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i)
            buckets[i].Reset(window);
    }

    // Get bucket by SessionPhase
    // NO-FALLBACK: Asserts on invalid phase (caller bug). Returns GLOBEX bucket for safety in release.
    SessionDeltaBucket& Get(AMT::SessionPhase phase) {
        const int idx = AMT::SessionPhaseToBucketIndex(phase);
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: Get() called with non-tradeable phase");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    const SessionDeltaBucket& Get(AMT::SessionPhase phase) const {
        const int idx = AMT::SessionPhaseToBucketIndex(phase);
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: Get() called with non-tradeable phase");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    // Push phase delta ratio to appropriate bucket
    void PushPhaseDelta(AMT::SessionPhase phase, double phaseDeltaRatio) {
        Get(phase).Push(phaseDeltaRatio);
    }

    // Increment session count for a phase bucket
    void IncrementPhaseSessionCount(AMT::SessionPhase phase) {
        Get(phase).IncrementSessionCount();
    }

    // Check if a specific phase bucket is ready
    bool IsPhaseReady(AMT::SessionPhase phase) const {
        return Get(phase).IsReady();
    }

    // Try to get percentile for current phase's delta ratio
    AMT::PercentileResult TryGetPercentile(AMT::SessionPhase phase, double phaseDeltaRatio) const {
        return Get(phase).TryGetPercentile(phaseDeltaRatio);
    }

    // Legacy API compatibility - check if any tradeable phase is ready
    // (For gradual migration - prefer IsPhaseReady for new code)
    bool IsReady() const {
        // Return true if at least INITIAL_BALANCE and MID_SESSION are ready (core RTH phases)
        return buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::INITIAL_BALANCE)].IsReady() &&
               buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::MID_SESSION)].IsReady();
    }
};

// ============================================================================
// DOM BASELINE (Phase-Bucketed Historical Baseline)
// ============================================================================
// DOM baseline - populated from historical DOM data via c_ACSILDepthBars API.
// Phase-bucketed like EffortBaselineStore to compare apples-to-apples.
// Requires "Support Downloading Historical Market Depth Data" in Server Settings.
// ============================================================================

// Per-phase DOM bucket - holds all DOM distributions for a single phase
struct DOMBucket {
    static constexpr size_t MIN_SAMPLES = 10;  // Minimum samples before percentiles are valid

    // Core metrics
    RollingDist stackRate;
    RollingDist pullRate;
    RollingDist depthMassCore;

    // Halo metrics
    RollingDist depthMassHalo;
    RollingDist haloImbalance;

    // Spread metric
    RollingDist spreadTicks;

    // Spatial profile metrics (Jan 2025)
    RollingDist levelDepthDist;   // All level depths (for mean/sigma calculation)
    RollingDist obiDist;          // Historical OBI values
    RollingDist polrRatioDist;    // Historical POLR ratios

    // Session tracking (matches EffortBucketDistribution contract)
    int sessionsContributed = 0;
    int totalBarsPushed = 0;
    int expectedBarsPerSession = 0;
    static constexpr int REQUIRED_SESSIONS = 5;
    static constexpr double MIN_COVERAGE_RATIO = 0.5;

    void Reset(int window = 6000) {  // ~1000 bars × 5 sessions (covers GLOBEX on 1-min)
        stackRate.reset(window);
        pullRate.reset(window);
        depthMassCore.reset(window);
        depthMassHalo.reset(window);
        haloImbalance.reset(window);
        spreadTicks.reset(window);
        levelDepthDist.reset(window);
        obiDist.reset(window);
        polrRatioDist.reset(window);
        sessionsContributed = 0;
        totalBarsPushed = 0;
        expectedBarsPerSession = 0;
    }

    void SetExpectedBarsPerSession(int expected) {
        expectedBarsPerSession = expected;
    }

    int GetMinBarsPerSession() const {
        if (expectedBarsPerSession <= 0)
            return 10;  // Fallback minimum
        return static_cast<int>(expectedBarsPerSession * MIN_COVERAGE_RATIO);
    }

    void Push(double stack, double pull, double depth) {
        stackRate.push(stack);
        pullRate.push(pull);
        depthMassCore.push(depth);
        totalBarsPushed++;
    }

    void PushHalo(double haloMass, double imbalance) {
        depthMassHalo.push(haloMass);
        haloImbalance.push(imbalance);
    }

    void PushSpread(double spread) {
        if (spread >= 0.0) {
            spreadTicks.push(spread);
        }
    }

    // Push spatial profile metrics (Jan 2025)
    void PushSpatialMetrics(double avgLevelDepth, double obi, double polr) {
        if (avgLevelDepth > 0.0) {
            levelDepthDist.push(avgLevelDepth);
        }
        // OBI is in [-1, +1], so no positive check needed
        obiDist.push(obi);
        // POLR ratio is in [0, 1]
        if (polr >= 0.0 && polr <= 1.0) {
            polrRatioDist.push(polr);
        }
    }

    bool IsSpatialReady() const {
        return IsReady() && levelDepthDist.size() >= static_cast<size_t>(GetMinBarsPerSession());
    }

    void IncrementSessionCount() { sessionsContributed++; }

    // Session-based readiness (matches EffortBucketDistribution contract)
    AMT::BucketBaselineState GetState() const {
        if (sessionsContributed < REQUIRED_SESSIONS)
            return AMT::BucketBaselineState::INSUFFICIENT_SESSIONS;
        const int minTotalBars = REQUIRED_SESSIONS * GetMinBarsPerSession();
        if (totalBarsPushed < minTotalBars)
            return AMT::BucketBaselineState::INSUFFICIENT_COVERAGE;
        return AMT::BucketBaselineState::READY;
    }

    bool IsReady() const { return GetState() == AMT::BucketBaselineState::READY; }

    bool IsHaloReady() const {
        return IsReady() && depthMassHalo.size() >= static_cast<size_t>(GetMinBarsPerSession());
    }

    bool IsSpreadReady() const {
        return IsReady() && spreadTicks.size() >= static_cast<size_t>(GetMinBarsPerSession());
    }

    size_t SampleCount() const { return depthMassCore.size(); }
    size_t HaloSampleCount() const { return depthMassHalo.size(); }

    void GetDiagnostics(int& outSessions, int& outBars, int& outExpected, int& outMinRequired) const {
        outSessions = sessionsContributed;
        outBars = totalBarsPushed;
        outExpected = expectedBarsPerSession * REQUIRED_SESSIONS;
        outMinRequired = GetMinBarsPerSession() * REQUIRED_SESSIONS;
    }

    AMT::PercentileResult TryStackPercentile(double val) const {
        if (!IsReady()) return AMT::PercentileResult::Invalid();
        return stackRate.TryPercentileRank(val);
    }

    AMT::PercentileResult TryPullPercentile(double val) const {
        if (!IsReady()) return AMT::PercentileResult::Invalid();
        return pullRate.TryPercentileRank(val);
    }

    AMT::PercentileResult TryDepthPercentile(double val) const {
        if (!IsReady()) return AMT::PercentileResult::Invalid();
        return depthMassCore.TryPercentileRank(val);
    }

    AMT::PercentileResult TryHaloPercentile(double val) const {
        if (!IsHaloReady()) return AMT::PercentileResult::Invalid();
        return depthMassHalo.TryPercentileRank(val);
    }

    AMT::PercentileResult TryImbalancePercentile(double val) const {
        if (!IsHaloReady()) return AMT::PercentileResult::Invalid();
        return haloImbalance.TryPercentileRank(val);
    }

    AMT::PercentileResult TrySpreadPercentile(double val) const {
        if (!IsSpreadReady()) return AMT::PercentileResult::Invalid();
        return spreadTicks.TryPercentile(val);
    }
};

// Phase-bucketed DOM baseline store
struct DOMWarmup {
    DOMBucket buckets[AMT::EFFORT_BUCKET_COUNT];

    void Reset(int window = 6000) {  // ~1000 bars × 5 sessions (covers GLOBEX on 1-min)
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i)
            buckets[i].Reset(window);
    }

    // Called at session transition - no longer resets (historical data persists)
    void StartWarmup(int /*actualBarTimeSec*/) {
        // No-op: historical baseline persists across sessions
    }

    // Set expected bars per session for all buckets based on chart timeframe
    void SetExpectedBarsPerSession(int barIntervalSeconds) {
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            const AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
            const int expected = AMT::GetExpectedBarsInPhase(phase, barIntervalSeconds);
            buckets[i].SetExpectedBarsPerSession(expected);
        }
    }

    // Get bucket by SessionPhase
    // NO-FALLBACK: Asserts on invalid phase (caller bug). Returns GLOBEX bucket for safety in release.
    DOMBucket& Get(AMT::SessionPhase phase) {
        const int idx = AMT::SessionPhaseToBucketIndex(phase);
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: Get() called with non-tradeable phase");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    const DOMBucket& Get(AMT::SessionPhase phase) const {
        const int idx = AMT::SessionPhaseToBucketIndex(phase);
        assert(idx >= 0 && idx < AMT::EFFORT_BUCKET_COUNT && "BUG: Get() called with non-tradeable phase");
        if (idx < 0 || idx >= AMT::EFFORT_BUCKET_COUNT)
            return buckets[0];  // Release safety - asserts catch this in debug
        return buckets[idx];
    }

    // Phase-aware push methods
    void Push(AMT::SessionPhase phase, double stack, double pull, double depth) {
        Get(phase).Push(stack, pull, depth);
    }

    void PushHalo(AMT::SessionPhase phase, double haloMass, double imbalance) {
        Get(phase).PushHalo(haloMass, imbalance);
    }

    void PushSpread(AMT::SessionPhase phase, double spread) {
        Get(phase).PushSpread(spread);
    }

    // Push spatial profile metrics (Jan 2025)
    void PushSpatialMetrics(AMT::SessionPhase phase, double avgLevelDepth, double obi, double polr) {
        Get(phase).PushSpatialMetrics(avgLevelDepth, obi, polr);
    }

    void IncrementSessionCount(AMT::SessionPhase phase) {
        Get(phase).IncrementSessionCount();
    }

    // DEPRECATED: Legacy non-phase Push - these are bugs waiting to happen
    // Callers should always provide a phase. Asserts in debug, no-ops in release.
    void Push(double /*stack*/, double /*pull*/, double /*depth*/) {
        assert(false && "BUG: Legacy Push() without phase - use Push(phase, ...) instead");
    }

    void PushHalo(double /*haloMass*/, double /*imbalance*/) {
        assert(false && "BUG: Legacy PushHalo() without phase - use PushHalo(phase, ...) instead");
    }

    void PushSpread(double /*spread*/) {
        assert(false && "BUG: Legacy PushSpread() without phase - use PushSpread(phase, ...) instead");
    }

    // DEPRECATED: Legacy API - these are bugs waiting to happen
    void PushIfWarmup(double /*stack*/, double /*pull*/, double /*depth*/, int /*currentBarTimeSec*/) {
        assert(false && "BUG: Legacy PushIfWarmup() - use Push(phase, ...) instead");
    }

    // Phase-aware ready checks
    bool IsReady(AMT::SessionPhase phase) const { return Get(phase).IsReady(); }
    bool IsHaloReady(AMT::SessionPhase phase) const { return Get(phase).IsHaloReady(); }
    bool IsSpreadReady(AMT::SessionPhase phase) const { return Get(phase).IsSpreadReady(); }
    bool IsSpatialReady(AMT::SessionPhase phase) const { return Get(phase).IsSpatialReady(); }

    // Legacy ready checks (for backward compatibility)
    bool IsReady() const {
        return buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::INITIAL_BALANCE)].IsReady() &&
               buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::MID_SESSION)].IsReady();
    }

    bool IsHaloReady() const {
        return buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::INITIAL_BALANCE)].IsHaloReady() &&
               buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::MID_SESSION)].IsHaloReady();
    }

    bool IsSpreadReady() const {
        return buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::INITIAL_BALANCE)].IsSpreadReady() &&
               buckets[AMT::SessionPhaseToBucketIndex(AMT::SessionPhase::MID_SESSION)].IsSpreadReady();
    }

    size_t SampleCount() const { return buckets[0].SampleCount(); }  // Legacy
    size_t HaloSampleCount() const { return buckets[0].HaloSampleCount(); }  // Legacy

    // Phase-aware percentile queries
    AMT::PercentileResult TryStackPercentile(AMT::SessionPhase phase, double val) const {
        return Get(phase).TryStackPercentile(val);
    }

    AMT::PercentileResult TryPullPercentile(AMT::SessionPhase phase, double val) const {
        return Get(phase).TryPullPercentile(val);
    }

    AMT::PercentileResult TryDepthPercentile(AMT::SessionPhase phase, double val) const {
        return Get(phase).TryDepthPercentile(val);
    }

    AMT::PercentileResult TryHaloPercentile(AMT::SessionPhase phase, double val) const {
        return Get(phase).TryHaloPercentile(val);
    }

    AMT::PercentileResult TryImbalancePercentile(AMT::SessionPhase phase, double val) const {
        return Get(phase).TryImbalancePercentile(val);
    }

    AMT::PercentileResult TrySpreadPercentile(AMT::SessionPhase phase, double val) const {
        return Get(phase).TrySpreadPercentile(val);
    }

    // Legacy non-phase percentile queries (deprecated)
    AMT::PercentileResult TryStackPercentile(double val) const { return buckets[0].TryStackPercentile(val); }
    AMT::PercentileResult TryPullPercentile(double val) const { return buckets[0].TryPullPercentile(val); }
    AMT::PercentileResult TryDepthPercentile(double val) const { return buckets[0].TryDepthPercentile(val); }
    AMT::PercentileResult TryHaloPercentile(double val) const { return buckets[0].TryHaloPercentile(val); }
    AMT::PercentileResult TryImbalancePercentile(double val) const { return buckets[0].TryImbalancePercentile(val); }
    AMT::PercentileResult TrySpreadPercentile(double val) const { return buckets[0].TrySpreadPercentile(val); }
};

// ============================================================================
// DOM QUALITY TRACKING
// Stage 3: domStrength with validity tracking
// Components: coverage (level counts), freshness (change detection), sanity (spread)
// ============================================================================

// Hash function for DOM structure change detection
// PERFORMANCE:
//   - What's hashed: 6 scalar values (level counts, best bid/ask, non-zero counts)
//   - Big-O: O(1) - fixed 6 mix operations regardless of DOM depth
//   - Memory: Single uint64_t accumulator, no allocations
//   - Throttling: Called once per bar via DOMQualityTracker.Update()
//   - Purpose: Detect staleness (DOM frozen = stale market data)
inline uint64_t ComputeDOMStructureHash(
    int bidLevelCount, int askLevelCount,
    double bestBid, double bestAsk,
    int bidNonZeroCount, int askNonZeroCount)
{
    // FNV-1a style hash - fast, low collision for small inputs
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](uint64_t val) {
        hash ^= val;
        hash *= 1099511628211ULL;
    };
    mix(static_cast<uint64_t>(bidLevelCount));
    mix(static_cast<uint64_t>(askLevelCount));
    mix(static_cast<uint64_t>(bestBid * 100));  // 2 decimal precision
    mix(static_cast<uint64_t>(bestAsk * 100));
    mix(static_cast<uint64_t>(bidNonZeroCount));
    mix(static_cast<uint64_t>(askNonZeroCount));
    return hash;
}

struct DOMQualitySnapshot
{
    int bidLevelCount = 0;
    int askLevelCount = 0;
    int bidNonZeroCount = 0;
    int askNonZeroCount = 0;
    double bestBid = 0.0;
    double bestAsk = 0.0;
    uint64_t structureHash = 0;

    // Computed scores - USE ACCESSORS FOR READS (direct access banned except assignment)
    float coverageScore = 0.0f;
    float freshnessScore_ = 0.0f;  // PRIVATE: use GetFreshnessScore()
    float sanityScore = 0.0f;

    // Component validity (NO-FALLBACK POLICY)
    bool freshnessValid = false;  // True only after tracker has DOM history

    bool hasBidLevels() const { return bidLevelCount > 0; }
    bool hasAskLevels() const { return askLevelCount > 0; }
    bool hasAnyLevels() const { return hasBidLevels() || hasAskLevels(); }
    bool hasBothSides() const { return hasBidLevels() && hasAskLevels(); }

    // GUARDED ACCESSOR: asserts validity before returning dead-value field
    float GetFreshnessScore() const
    {
        assert(freshnessValid && "BUG: reading freshnessScore without validity check");
        return freshnessScore_;
    }

    bool hasValidSpread(double tickSize) const
    {
        if (!hasBothSides()) return false;
        if (bestBid <= 0.0 || bestAsk <= 0.0) return false;
        const double spread = bestAsk - bestBid;
        if (spread <= 0.0) return false;  // Crossed or zero
        if (spread > tickSize * 100) return false;  // Excessive spread
        return true;
    }
};

struct DOMQualityTracker
{
    // ========================================================================
    // BAR-LEVEL STALENESS (existing)
    // ========================================================================
    uint64_t lastHash = 0;
    int lastChangeBar = -1;
    int barsSinceChange = 0;
    bool isStaleByBars = false;         // Stale by bar count threshold

    // NO-FALLBACK POLICY: freshness validity requires actual DOM history
    // freshnessValid = false until first DOM update establishes baseline
    bool freshnessValid = false;

    // Bar-level staleness thresholds
    int maxStaleBarsHard = 10;          // Hard limit: stale after N unchanged bars
    float adaptiveExpectedCadence = 0.2f; // Expected changes per bar (adaptive)

    // ========================================================================
    // MILLISECOND-LEVEL STALENESS (new)
    // ========================================================================
    // For execution decisions, sub-second staleness matters.
    // If DOM data is >2 seconds old, it's stale for execution purposes.
    int64_t lastChangeTimeMs = -1;      // Timestamp of last DOM change (ms since epoch)
    int64_t lastUpdateTimeMs = -1;      // Most recent Update() call time
    int staleThresholdMs = 2000;        // DOM older than this is stale (default 2 sec)
    int ageMs = -1;                     // Computed: lastUpdateTimeMs - lastChangeTimeMs
    bool isStaleByMs = false;           // Stale by millisecond threshold
    bool timingValid = false;           // True if timing data has been provided

    // ========================================================================
    // COMBINED STALENESS
    // ========================================================================
    bool isStale = false;               // Combined: isStaleByBars OR isStaleByMs

    void Reset()
    {
        lastHash = 0;
        lastChangeBar = -1;
        barsSinceChange = 0;
        isStaleByBars = false;
        freshnessValid = false;  // Must reestablish baseline after reset

        // Reset millisecond tracking
        lastChangeTimeMs = -1;
        lastUpdateTimeMs = -1;
        ageMs = -1;
        isStaleByMs = false;
        timingValid = false;

        isStale = false;
    }

    // Update with optional millisecond timestamp
    // currentTimeMs: current time in milliseconds (-1 = not provided, skip ms staleness)
    bool Update(const DOMQualitySnapshot& snap, int currentBar, int64_t currentTimeMs = -1)
    {
        bool changed = (snap.structureHash != lastHash);

        // Update millisecond tracking if time provided
        if (currentTimeMs >= 0)
        {
            lastUpdateTimeMs = currentTimeMs;
            timingValid = true;

            if (changed)
            {
                lastChangeTimeMs = currentTimeMs;
                ageMs = 0;
                isStaleByMs = false;
            }
            else if (lastChangeTimeMs >= 0)
            {
                ageMs = static_cast<int>(currentTimeMs - lastChangeTimeMs);
                isStaleByMs = (ageMs > staleThresholdMs);
            }
        }

        // Update bar-level tracking
        if (changed)
        {
            lastHash = snap.structureHash;
            lastChangeBar = currentBar;
            barsSinceChange = 0;
            isStaleByBars = false;
            freshnessValid = true;  // First change establishes baseline
        }
        else if (lastChangeBar >= 0)
        {
            barsSinceChange = currentBar - lastChangeBar;
            // Hard staleness threshold
            if (barsSinceChange >= maxStaleBarsHard)
            {
                isStaleByBars = true;
            }
            // Adaptive threshold (softer)
            else if (adaptiveExpectedCadence > 0.0f)
            {
                const int adaptiveThreshold = static_cast<int>(3.0f / adaptiveExpectedCadence);
                if (barsSinceChange >= adaptiveThreshold)
                {
                    isStaleByBars = true;
                }
            }
        }

        // Combined staleness: either bar-level OR millisecond-level
        isStale = isStaleByBars || isStaleByMs;

        return changed;
    }

    // ========================================================================
    // ACCESSORS
    // ========================================================================

    // Get age in milliseconds (-1 if timing not available)
    int GetAgeMs() const { return ageMs; }

    // Is DOM stale by millisecond threshold?
    bool IsStaleByMs() const { return isStaleByMs; }

    // Is DOM stale by bar count threshold?
    bool IsStaleByBars() const { return isStaleByBars; }

    // Is timing information available?
    bool IsTimingValid() const { return timingValid; }

    // Returns freshness score only when valid (NO-FALLBACK POLICY)
    // Caller must check IsFreshnessValid() before using result
    float ComputeFreshnessScore() const
    {
        if (!freshnessValid)
            return 0.0f;  // Dead value - caller must gate on IsFreshnessValid()

        if (isStale)
            return 0.0f;

        // Decay freshness based on bars since change
        // Full freshness at 0 bars, decays to 0 at maxStaleBarsHard
        const float ratio = static_cast<float>(barsSinceChange) / maxStaleBarsHard;
        return (std::max)(0.0f, 1.0f - ratio);
    }

    bool IsFreshnessValid() const { return freshnessValid; }
};

// Compute DOM strength score with component breakdown
// NO-FALLBACK POLICY: freshness excluded from blend when tracker has no history
inline float ComputeDOMStrength(
    DOMQualitySnapshot& snap,  // Non-const: fills in component scores
    const DOMQualityTracker& tracker,
    int expectedLevelsPerSide,
    double tickSize)
{
    constexpr float W_COVERAGE = 0.4f;
    constexpr float W_FRESHNESS = 0.4f;
    constexpr float W_SANITY = 0.2f;

    // Coverage: how many levels are populated vs expected
    {
        const int expectedTotal = expectedLevelsPerSide * 2;
        const int actualNonZero = snap.bidNonZeroCount + snap.askNonZeroCount;
        float rawCoverage = (expectedTotal > 0)
            ? static_cast<float>(actualNonZero) / expectedTotal
            : 0.0f;

        // Penalty for one-sided book
        if (!snap.hasBothSides())
        {
            rawCoverage *= 0.3f;  // Heavy penalty
        }

        snap.coverageScore = (std::max)(0.0f, (std::min)(1.0f, rawCoverage));
    }

    // Freshness: how recently did DOM structure change
    // NO-FALLBACK POLICY: only valid after tracker has DOM history
    snap.freshnessValid = tracker.IsFreshnessValid();
    snap.freshnessScore_ = tracker.ComputeFreshnessScore();  // Write to private field

    // Sanity: is the spread valid
    snap.sanityScore = snap.hasValidSpread(tickSize) ? 1.0f : 0.0f;

    // Composite score with renormalization for missing components
    float score = 0.0f;
    float totalWeight = 0.0f;

    // Coverage: always included (immediate observation)
    score += W_COVERAGE * snap.coverageScore;
    totalWeight += W_COVERAGE;

    // Freshness: only included if tracker has DOM history
    if (snap.freshnessValid)
    {
        score += W_FRESHNESS * snap.GetFreshnessScore();  // Accessor asserts validity
        totalWeight += W_FRESHNESS;
    }

    // Sanity: always included (immediate observation)
    score += W_SANITY * snap.sanityScore;
    totalWeight += W_SANITY;

    // Renormalize
    if (totalWeight > 0.0f)
    {
        return (std::max)(0.0f, (std::min)(1.0f, score / totalWeight));
    }
    return 0.0f;
}

} // namespace AMT

#endif // AMT_SNAPSHOTS_H
