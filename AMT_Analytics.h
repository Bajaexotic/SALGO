// ============================================================================
// AMT_Analytics.h
// Zone statistics, metrics, and analysis
// Week 4: Comprehensive zone information without trading signals
// ============================================================================

#ifndef AMT_ANALYTICS_H
#define AMT_ANALYTICS_H

#include "amt_core.h"
#include "AMT_Zones.h"
#include "AMT_Updates.h"
#include "AMT_Phase.h"
#include "AMT_Helpers.h"
#include "AMT_Session.h"  // For SessionEngagementAccumulator
#include <vector>
#include <algorithm>
#include <cmath>

namespace AMT {

    // ============================================================================
    // ZONE STATISTICS
    // ============================================================================

    struct ZoneStatistics {
        int zoneId = -1;
        ZoneType type = ZoneType::NONE;
        double anchorPrice = 0.0;

        // Activity metrics
        int totalTouches = 0;
        int acceptances = 0;
        int rejections = 0;
        int failedAuctions = 0;
        double acceptanceRate = 0.0;  // acceptances / total touches
        double rejectionRate = 0.0;   // rejections / total touches

        // Time metrics
        int barsAlive = 0;
        int totalBarsEngaged = 0;
        int avgEngagementDuration = 0;
        int barsSinceLastTouch = 0;

        // Volume metrics
        double avgVolumePerTouch = 0.0;
        double totalVolume = 0.0;
        double avgDelta = 0.0;

        // Strength metrics
        double currentStrength = 0.0;
        double peakStrength = 0.0;
        double avgStrength = 0.0;

        // Touch breakdown
        int tagCount = 0;
        int probeCount = 0;
        int testCount = 0;
        int acceptanceCount = 0;

        /**
         * Calculate from zone runtime
         */
        void Calculate(const ZoneRuntime& zone, int currentBar) {
            zoneId = zone.zoneId;
            type = zone.type;
            anchorPrice = zone.GetAnchorPrice();

            // Activity
            totalTouches = zone.touchCount;

            // Count outcomes
            for (const auto& eng : zone.engagementHistory) {
                if (eng.outcome == AuctionOutcome::ACCEPTED) acceptances++;
                if (eng.outcome == AuctionOutcome::REJECTED) rejections++;
                if (eng.wasFailedAuction) failedAuctions++;

                totalBarsEngaged += eng.barsEngaged;
                totalVolume += eng.cumulativeVolume;
            }

            if (totalTouches > 0) {
                acceptanceRate = static_cast<double>(acceptances) / totalTouches;
                rejectionRate = static_cast<double>(rejections) / totalTouches;
                avgEngagementDuration = totalBarsEngaged / totalTouches;
                avgVolumePerTouch = totalVolume / totalTouches;
            }

            // Time
            barsAlive = currentBar - zone.creationBar;
            barsSinceLastTouch = zone.barsSinceTouch;

            // Touch breakdown
            for (const auto& touch : zone.touchHistory) {
                switch (touch.type) {
                case TouchType::TAG: tagCount++; break;
                case TouchType::PROBE: probeCount++; break;
                case TouchType::TEST: testCount++; break;
                case TouchType::ACCEPTANCE: acceptanceCount++; break;
                case TouchType::UNRESOLVED: break;  // Unresolved engagements not counted in quality metrics
                }
            }

            // Strength
            currentStrength = zone.strengthScore;
            peakStrength = 1.0;  // Would track max historically
            avgStrength = currentStrength;  // Simplified
        }

        /**
         * Get quality grade
         */
        std::string GetQualityGrade() const {
            if (currentStrength >= 1.2) return "A+";
            if (currentStrength >= 1.0) return "A";
            if (currentStrength >= 0.8) return "B";
            if (currentStrength >= 0.6) return "C";
            if (currentStrength >= 0.4) return "D";
            return "F";
        }
    };

    // ============================================================================
    // MARKET STATE BUCKET (Phase-Specific, Prior-First, No Warmup)
    // ============================================================================
    //
    // Design: Pre-populated priors from historical data, no warmup at session open
    //
    // Key Principle: If prior is ready, use it immediately (100% prior at session open)
    //   - priorWeight = priorMass / (sessionBars + priorMass)
    //   - sessionBars=0 → 100% prior (immediate classification)
    //   - As session accumulates, weight shifts to session evidence
    //
    // NO HIDDEN FALLBACKS:
    //   - priorBalance = -1.0 means NOT_READY (not 0.5!)
    //   - priorReady flag explicitly tracks readiness
    //   - UNDEFINED returned only when truly no data available
    //
    // ============================================================================

    struct MarketStateBucket {
        // ---------------------------------------------------------------------
        // Session Evidence (resets each session)
        // ---------------------------------------------------------------------
        int sessionBars = 0;
        int balanceBars = 0;
        int imbalanceBars = 0;

        // ---------------------------------------------------------------------
        // Historical Prior (NO DEFAULT - populated from history)
        // ---------------------------------------------------------------------
        double priorBalance = -1.0;  // -1 = NOT_READY (NOT 0.5!)
        bool priorReady = false;
        int sessionsContributed = 0;

        // ---------------------------------------------------------------------
        // Hysteresis State
        // ---------------------------------------------------------------------
        AMTMarketState confirmedState = AMTMarketState::UNKNOWN;
        AMTMarketState candidateState = AMTMarketState::UNKNOWN;
        int candidateBars = 0;

        // ---------------------------------------------------------------------
        // Configuration
        // ---------------------------------------------------------------------
        static constexpr int REQUIRED_SESSIONS = 5;
        static constexpr int MIN_SESSION_BARS_FOR_PRIOR_UPDATE = 20;
        static constexpr int MIN_CONFIRMATION_BARS = 5;
        static constexpr double CONFIRMATION_MARGIN = 0.1;
        static constexpr double PRIOR_MASS = 30.0;
        static constexpr double PRIOR_INERTIA = 0.8;

        // ---------------------------------------------------------------------
        // Readiness
        // ---------------------------------------------------------------------
        enum class Readiness {
            READY,              // Prior ready, immediate classification
            WARMUP_PRIOR,       // Prior not ready, need historical data
            SESSION_ONLY,       // No prior but have session evidence
            NOT_READY           // No data at all
        };

        Readiness GetReadiness() const {
            if (priorReady) return Readiness::READY;
            if (sessionBars >= MIN_SESSION_BARS_FOR_PRIOR_UPDATE) return Readiness::SESSION_ONLY;
            return Readiness::NOT_READY;
        }

        // Test/diagnostic accessors
        int minConfirmationBars = MIN_CONFIRMATION_BARS;  // Configurable for tests

        bool IsTransitioning() const {
            return candidateState != confirmedState && candidateBars > 0;
        }

        double GetConfirmationProgress() const {
            if (candidateBars <= 0 || confirmedState == candidateState) {
                return 0.0;
            }
            return static_cast<double>(candidateBars) / minConfirmationBars;
        }

        // ---------------------------------------------------------------------
        // Primary Update (called once per closed bar)
        // ---------------------------------------------------------------------
        AMTMarketState Update(AMTMarketState rawState) {
            // Track session evidence
            if (rawState == AMTMarketState::BALANCE) {
                balanceBars++;
                sessionBars++;
            } else if (rawState == AMTMarketState::IMBALANCE) {
                imbalanceBars++;
                sessionBars++;
            }

            // Compute decision ratio (NO session bar gate when prior is ready!)
            double decisionRatio = -1.0;
            bool hasEvidence = false;

            if (priorReady && sessionBars > 0) {
                // BEST: Blend prior + session evidence
                const double sessionRatio = static_cast<double>(balanceBars) / sessionBars;
                const double priorWeight = PRIOR_MASS / (sessionBars + PRIOR_MASS);
                decisionRatio = (1.0 - priorWeight) * sessionRatio + priorWeight * priorBalance;
                hasEvidence = true;
            }
            else if (priorReady && sessionBars == 0) {
                // GOOD: 100% prior (session just started, but have historical)
                decisionRatio = priorBalance;
                hasEvidence = true;
            }
            else if (!priorReady && sessionBars >= MIN_SESSION_BARS_FOR_PRIOR_UPDATE) {
                // DEGRADED: Session-only, no prior available
                decisionRatio = static_cast<double>(balanceBars) / sessionBars;
                hasEvidence = true;
            }
            // else: No data available

            if (!hasEvidence) {
                confirmedState = AMTMarketState::UNKNOWN;
                candidateState = AMTMarketState::UNKNOWN;
                candidateBars = 0;
                return confirmedState;
            }

            // Determine target state from ratio
            AMTMarketState targetState = AMTMarketState::UNKNOWN;
            if (decisionRatio >= 0.5 + CONFIRMATION_MARGIN) {
                targetState = AMTMarketState::BALANCE;
            } else if (decisionRatio <= 0.5 - CONFIRMATION_MARGIN) {
                targetState = AMTMarketState::IMBALANCE;
            }

            // Hysteresis logic
            if (confirmedState == AMTMarketState::UNKNOWN) {
                if (targetState != AMTMarketState::UNKNOWN) {
                    confirmedState = targetState;
                    candidateState = targetState;
                    candidateBars = 0;
                }
            }
            else if (targetState == confirmedState) {
                candidateState = confirmedState;
                candidateBars = 0;
            }
            else if (targetState == candidateState) {
                candidateBars++;
                if (candidateBars >= MIN_CONFIRMATION_BARS) {
                    confirmedState = candidateState;
                    candidateBars = 0;
                }
            }
            else if (targetState != AMTMarketState::UNKNOWN) {
                candidateState = targetState;
                candidateBars = 1;
            }

            return confirmedState;
        }

        // ---------------------------------------------------------------------
        // Session Boundary
        // ---------------------------------------------------------------------
        void FinalizeSession() {
            // Update prior if session had meaningful data
            if (sessionBars < MIN_SESSION_BARS_FOR_PRIOR_UPDATE) return;

            const double sessionRatio = static_cast<double>(balanceBars) / sessionBars;

            if (!priorReady) {
                // First valid session: initialize from evidence
                priorBalance = sessionRatio;
                priorReady = true;
                sessionsContributed = 1;
            } else {
                // EWMA update
                priorBalance = PRIOR_INERTIA * priorBalance + (1.0 - PRIOR_INERTIA) * sessionRatio;
                sessionsContributed++;
            }
        }

        void ResetForSession() {
            sessionBars = 0;
            balanceBars = 0;
            imbalanceBars = 0;
            confirmedState = AMTMarketState::UNKNOWN;
            candidateState = AMTMarketState::UNKNOWN;
            candidateBars = 0;
            // priorBalance, priorReady, sessionsContributed PRESERVED
        }

        // ---------------------------------------------------------------------
        // Set Prior from Historical Data (called during bootstrap)
        // ---------------------------------------------------------------------
        void SetPriorFromHistory(double prior, int sessions) {
            priorBalance = prior;
            priorReady = true;
            sessionsContributed = sessions;
        }

        // ---------------------------------------------------------------------
        // Query (for logging)
        // ---------------------------------------------------------------------
        struct QueryResult {
            Readiness readiness;
            AMTMarketState state;
            int sessionBars;
            double sessionRatio;    // -1 if no session bars
            double blendedRatio;    // -1 if not computable
            double priorWeight;     // 0 if prior not ready
            double priorBalance;    // -1 if not ready
            int sessionsContributed;
        };

        QueryResult Query() const {
            QueryResult r;
            r.readiness = GetReadiness();
            r.state = confirmedState;
            r.sessionBars = sessionBars;
            r.sessionsContributed = sessionsContributed;
            r.priorBalance = priorReady ? priorBalance : -1.0;

            if (sessionBars > 0) {
                r.sessionRatio = static_cast<double>(balanceBars) / sessionBars;
            } else {
                r.sessionRatio = -1.0;
            }

            if (priorReady) {
                r.priorWeight = PRIOR_MASS / (sessionBars + PRIOR_MASS);
                if (sessionBars > 0) {
                    r.blendedRatio = (1.0 - r.priorWeight) * r.sessionRatio + r.priorWeight * priorBalance;
                } else {
                    r.blendedRatio = priorBalance;  // 100% prior
                }
            } else {
                r.priorWeight = 0.0;
                r.blendedRatio = (sessionBars >= MIN_SESSION_BARS_FOR_PRIOR_UPDATE) ? r.sessionRatio : -1.0;
            }

            return r;
        }

        void Reset() {
            sessionBars = 0;
            balanceBars = 0;
            imbalanceBars = 0;
            priorBalance = -1.0;
            priorReady = false;
            sessionsContributed = 0;
            confirmedState = AMTMarketState::UNKNOWN;
            candidateState = AMTMarketState::UNKNOWN;
            candidateBars = 0;
        }
    };

    // ============================================================================
    // MARKET STATE TRACKER (Phase-Bucketed Container)
    // ============================================================================

    struct MarketStateTracker {
        MarketStateBucket buckets[AMT::EFFORT_BUCKET_COUNT];  // 7 phases

        // ---------------------------------------------------------------------
        // Primary Interface
        // ---------------------------------------------------------------------
        AMTMarketState Update(AMT::SessionPhase phase, AMTMarketState rawState) {
            const int idx = AMT::SessionPhaseToBucketIndex(phase);
            if (idx < 0) return AMTMarketState::UNKNOWN;
            return buckets[idx].Update(rawState);
        }

        AMTMarketState GetState(AMT::SessionPhase phase) const {
            const int idx = AMT::SessionPhaseToBucketIndex(phase);
            if (idx < 0) return AMTMarketState::UNKNOWN;
            return buckets[idx].confirmedState;
        }

        // ---------------------------------------------------------------------
        // Session Boundary
        // ---------------------------------------------------------------------
        void FinalizeAllPhases() {
            for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
                buckets[i].FinalizeSession();
            }
        }

        void ResetForSession() {
            for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
                buckets[i].ResetForSession();
            }
        }

        // ---------------------------------------------------------------------
        // Historical Population (called during bootstrap)
        // ---------------------------------------------------------------------
        void SetPriorFromHistory(AMT::SessionPhase phase, double prior, int sessions) {
            const int idx = AMT::SessionPhaseToBucketIndex(phase);
            if (idx < 0) return;
            buckets[idx].SetPriorFromHistory(prior, sessions);
        }

        // ---------------------------------------------------------------------
        // Query
        // ---------------------------------------------------------------------
        MarketStateBucket::QueryResult Query(AMT::SessionPhase phase) const {
            const int idx = AMT::SessionPhaseToBucketIndex(phase);
            if (idx < 0) {
                MarketStateBucket::QueryResult r;
                r.readiness = MarketStateBucket::Readiness::NOT_READY;
                r.state = AMTMarketState::UNKNOWN;
                r.sessionBars = 0;
                r.sessionRatio = -1.0;
                r.blendedRatio = -1.0;
                r.priorWeight = 0.0;
                r.priorBalance = -1.0;
                r.sessionsContributed = 0;
                return r;
            }
            return buckets[idx].Query();
        }

        // ---------------------------------------------------------------------
        // Full Reset
        // ---------------------------------------------------------------------
        void Reset() {
            for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
                buckets[i].Reset();
            }
        }
    };

    // ============================================================================
    // SESSION STATISTICS
    // ============================================================================

    struct SessionStatistics {
        // Value area metrics
        double vaRangeTicks = 0.0;
        double vaRangePercent = 0.0;
        ProfileShape profileShape = ProfileShape::UNDEFINED;

        // POC metrics
        double pocPrice = 0.0;
        int pocMigrationTicks = 0;  // How far POC has moved
        int pocTouches = 0;

        // Boundary metrics
        int vahTests = 0;
        int valTests = 0;
        int vahBreakouts = 0;
        int valBreakouts = 0;
        double vahAcceptanceRate = 0.0;
        double valAcceptanceRate = 0.0;

        // Phase distribution (all buckets must sum to totalBars)
        int rotationBars = 0;
        int testingBars = 0;
        int drivingBars = 0;       // DRIVING_UP + DRIVING_DOWN (directional outside VA)
        int extensionBars = 0;     // RANGE_EXTENSION = active expansion (at session extreme)
        int failedAuctionBars = 0;
        int pullbackBars = 0;
        int acceptingBars = 0;     // ACCEPTING_VALUE = consolidating in new value
        int unknownBars = 0;       // Catch-all for future enum values
        int totalBars = 0;

        // Volume metrics
        double totalVolume = 0.0;
        double avgVolumePerBar = 0.0;
        double avgVolumePerTick = 0.0;
        double totalDelta = 0.0;
        double netDelta = 0.0;
        double avgDeltaPerBar = 0.0;  // Net delta / totalBars

        // =====================================================================
        // ZONE COUNTS (from ZoneManager - CURRENT SNAPSHOT, may change on backfill)
        // These reflect the current state of zones and may reset on zone clearing.
        // Do NOT use these as session truth - use accumulator-derived stats below.
        // =====================================================================
        int activeZones = 0;
        int expiredZones = 0;

        // --- HVN/LVN metrics ---
        int hvnCount = 0;           // Current HVN count
        int lvnCount = 0;           // Current LVN count
        int hvnAdded = 0;           // HVNs added this session
        int hvnRemoved = 0;         // HVNs removed this session
        int lvnAdded = 0;           // LVNs added this session
        int lvnRemoved = 0;         // LVNs removed this session
        double widestLvnTicks = 0.0; // Widest LVN gap in ticks

        // --- Zone engagement metrics ---
        int engagementCount = 0;    // Number of full zone engagements
        int escapeCount = 0;        // Successful escapes from zones
        double avgEngagementBars = 0.0;  // Average bars spent in engagement
        double avgEscapeVelocity = 0.0;  // Average escape velocity

        // --- Extreme condition counts ---
        int extremeVolumeCount = 0;
        int extremeDeltaCount = 0;
        int extremeTradesCount = 0;
        int extremeStackCount = 0;
        int extremePullCount = 0;
        int extremeDepthCount = 0;
        int totalExtremeEvents = 0;  // Sum of all extreme events

        // --- Probe metrics ---
        int probesFired = 0;
        int probesResolved = 0;
        int probesHit = 0;           // Probes that hit target
        int probesMissed = 0;        // Probes that missed
        int probesExpired = 0;       // Probes that timed out
        double avgProbeScore = 0.0;
        double probeHitRate = 0.0;   // probesHit / probesResolved

        // --- Session/state transition events ---
        int sessionChangeCount = 0;       // RTH <-> Globex transitions
        int phaseTransitionCount = 0;     // Phase changes (ROTATION -> DRIVING, etc.)
        int intentChangeCount = 0;        // Intent direction changes
        int marketStateChangeCount = 0;   // BALANCE <-> IMBALANCE flips

        // --- Warning/error events ---
        int zoneWidthMismatchCount = 0;   // Zone width calculation mismatches
        int validationDivergenceCount = 0; // Replay validation divergences
        int configErrorCount = 0;          // Configuration errors
        int vbpWarningCount = 0;           // VBP-related warnings

        // =====================================================================
        // NEW: SSOT OUTCOME COUNTERS (from lifetime counters, survive truncation)
        // =====================================================================

        // Correctly named touch counts
        int vahTouches = 0;             // Total VAH engagement starts
        int valTouches = 0;             // Total VAL engagement starts
        // pocTouches already exists above

        // VAH outcome counts
        int vahAcceptances = 0;
        int vahRejections = 0;
        int vahTags = 0;
        int vahUnresolved = 0;
        int vahProbeRejections = 0;     // Rejection subtype
        int vahTestRejections = 0;      // Rejection subtype

        // VAL outcome counts
        int valAcceptances = 0;
        int valRejections = 0;
        int valTags = 0;
        int valUnresolved = 0;
        int valProbeRejections = 0;
        int valTestRejections = 0;

        // POC outcome counts
        int pocAcceptances = 0;
        int pocRejections = 0;
        int pocTags = 0;
        int pocUnresolved = 0;

        // Session totals (across all zones)
        int totalAcceptances = 0;
        int totalRejections = 0;
        int totalTags = 0;
        int totalUnresolved = 0;

        // Acceptance rates (explicit denominator naming)
        double vahAcceptanceRateOfAttempts = 0.0;   // acceptances / touches
        double vahAcceptanceRateOfDecisions = 0.0;  // acceptances / (acceptances + rejections)
        double valAcceptanceRateOfAttempts = 0.0;
        double valAcceptanceRateOfDecisions = 0.0;
        double pocAcceptanceRateOfAttempts = 0.0;
        double pocAcceptanceRateOfDecisions = 0.0;

        /**
         * Calculate phase distribution percentage (excludes UNKNOWN bars from denominator)
         * This ensures known phases sum to 100%
         */
        double GetPhasePercent(int phaseBars) const {
            const int knownBars = totalBars - unknownBars;
            return knownBars > 0 ? (static_cast<double>(phaseBars) / knownBars * 100.0) : 0.0;
        }

        /**
         * Get count of bars with known phase (excludes UNKNOWN/warmup)
         */
        int GetKnownBars() const {
            return totalBars - unknownBars;
        }

        /**
         * Get sum of all phase buckets (must equal totalBars)
         */
        int GetBucketSum() const {
            return rotationBars + testingBars + drivingBars +
                   extensionBars + failedAuctionBars + pullbackBars +
                   acceptingBars + unknownBars;
        }

        /**
         * Check bucket-sum invariant: sum of all buckets == totalBars
         * Returns true if invariant holds, false if violated
         */
        bool CheckInvariant() const {
            return GetBucketSum() == totalBars;
        }

        /**
         * Get invariant violation details (for diagnostics)
         * Returns empty string if invariant holds
         */
        std::string GetInvariantViolation() const {
            int sum = GetBucketSum();
            if (sum == totalBars) {
                return "";
            }
            return "INVARIANT VIOLATION: buckets=" + std::to_string(sum) +
                   " totalBars=" + std::to_string(totalBars) +
                   " (drift=" + std::to_string(sum - totalBars) + ")";
        }

        // Minimum sample size for MarketState classification
        static constexpr int MIN_SAMPLE_SIZE = 30;

        /**
         * Check if sample size is sufficient for classification
         */
        bool HasSufficientSample() const {
            return totalBars >= MIN_SAMPLE_SIZE;
        }

        /**
         * Get balance/imbalance classification with guardrails
         * Returns UNKNOWN if sample size insufficient
         */
        AMTMarketState GetMarketState() const {
            if (!HasSufficientSample()) {
                return AMTMarketState::UNKNOWN;
            }
            double rotationPercent = GetPhasePercent(rotationBars);
            return (rotationPercent > 60.0) ? AMTMarketState::BALANCE : AMTMarketState::IMBALANCE;
        }

        /**
         * Get raw rotation percentage (without classification)
         * Useful when you need the number regardless of sample size
         */
        double GetRotationPercent() const {
            return GetPhasePercent(rotationBars);
        }
    };

    // ============================================================================
    // ZONE RANKING
    // ============================================================================

    /**
     * Rank zones by importance/relevance
     */
    inline std::vector<const ZoneRuntime*> RankZones(const ZoneManager& zm,
        double currentPrice,
        double tickSize)
    {
        std::vector<const ZoneRuntime*> ranked;

        // Collect all zones
        for (const auto& [id, zone] : zm.activeZones) {
            ranked.push_back(&zone);
        }

        // Sort by extended priority
        std::sort(ranked.begin(), ranked.end(),
            [currentPrice, tickSize](const ZoneRuntime* a, const ZoneRuntime* b) {
                auto prioA = GetZonePriorityExtended(*a, currentPrice, tickSize);
                auto prioB = GetZonePriorityExtended(*b, currentPrice, tickSize);
                return prioA > prioB;  // Highest priority first
            });

        return ranked;
    }

    /**
     * Get top N zones by priority
     */
    inline std::vector<const ZoneRuntime*> GetTopZones(const ZoneManager& zm,
        double currentPrice,
        double tickSize,
        int count = 5)
    {
        auto ranked = RankZones(zm, currentPrice, tickSize);

        if (ranked.size() > static_cast<size_t>(count)) {
            ranked.resize(count);
        }

        return ranked;
    }

    /**
     * Calculate session statistics
     *
     * BACKFILL STABILITY: Reads engagement stats from SessionEngagementAccumulator,
     * NOT from zone objects. This ensures stats survive zone clearing/recreation.
     *
     * @param zm ZoneManager for session context and zone count
     * @param engagementAccum SSOT for per-anchor engagement stats
     * @param poc POC from SessionManager
     * @param vah VAH from SessionManager
     * @param val VAL from SessionManager
     * @param vaRangeTicks VA range in ticks from SessionManager
     * @param currentPhase Current market phase
     * @param currentBar Current bar index
     * @param phaseHistory History of phases for distribution stats
     */
    inline SessionStatistics CalculateSessionStats(const ZoneManager& zm,
        const SessionEngagementAccumulator& engagementAccum,
        double poc, double vah, double val, int vaRangeTicks,
        CurrentPhase currentPhase,
        int currentBar,
        const std::vector<CurrentPhase>& phaseHistory)
    {
        SessionStatistics stats;

        // Value area (from SessionManager SSOT)
        stats.pocPrice = poc;
        stats.vaRangeTicks = vaRangeTicks;
        stats.profileShape = zm.sessionCtx.profileShape;

        if (poc > 0.0) {
            stats.vaRangePercent = (vah - val) / poc * 100.0;
        }

        // =================================================================
        // ENGAGEMENT STATS FROM ACCUMULATOR (SSOT - survives zone clearing)
        // =================================================================

        // VAH stats
        const AnchorEngagementStats& vahStats = engagementAccum.vah;
        stats.vahTests = vahStats.touchCount;  // Legacy field
        stats.vahTouches = vahStats.touchCount;
        stats.vahAcceptances = vahStats.acceptances;
        stats.vahRejections = vahStats.rejections;
        stats.vahTags = vahStats.tags;
        stats.vahUnresolved = vahStats.unresolved;
        stats.vahProbeRejections = vahStats.probes;
        stats.vahTestRejections = vahStats.tests;
        stats.vahAcceptanceRateOfAttempts = vahStats.GetAcceptanceRateOfAttempts();
        stats.vahAcceptanceRateOfDecisions = vahStats.GetAcceptanceRateOfDecisions();
        stats.vahAcceptanceRate = stats.vahAcceptanceRateOfDecisions;  // Legacy

        // VAL stats
        const AnchorEngagementStats& valStats = engagementAccum.val;
        stats.valTests = valStats.touchCount;
        stats.valTouches = valStats.touchCount;
        stats.valAcceptances = valStats.acceptances;
        stats.valRejections = valStats.rejections;
        stats.valTags = valStats.tags;
        stats.valUnresolved = valStats.unresolved;
        stats.valProbeRejections = valStats.probes;
        stats.valTestRejections = valStats.tests;
        stats.valAcceptanceRateOfAttempts = valStats.GetAcceptanceRateOfAttempts();
        stats.valAcceptanceRateOfDecisions = valStats.GetAcceptanceRateOfDecisions();
        stats.valAcceptanceRate = stats.valAcceptanceRateOfDecisions;

        // POC stats
        const AnchorEngagementStats& pocStats = engagementAccum.poc;
        stats.pocTouches = pocStats.touchCount;
        stats.pocAcceptances = pocStats.acceptances;
        stats.pocRejections = pocStats.rejections;
        stats.pocTags = pocStats.tags;
        stats.pocUnresolved = pocStats.unresolved;
        stats.pocAcceptanceRateOfAttempts = pocStats.GetAcceptanceRateOfAttempts();
        stats.pocAcceptanceRateOfDecisions = pocStats.GetAcceptanceRateOfDecisions();

        // =================================================================
        // PHASE DISTRIBUTION
        // =================================================================
        for (CurrentPhase phase : phaseHistory) {
            switch (phase) {
            case CurrentPhase::ROTATION:         stats.rotationBars++; break;
            case CurrentPhase::TESTING_BOUNDARY: stats.testingBars++; break;
            case CurrentPhase::DRIVING_UP:       stats.drivingBars++; break;
            case CurrentPhase::DRIVING_DOWN:     stats.drivingBars++; break;
            case CurrentPhase::RANGE_EXTENSION:  stats.extensionBars++; break;
            case CurrentPhase::FAILED_AUCTION:   stats.failedAuctionBars++; break;
            case CurrentPhase::PULLBACK:         stats.pullbackBars++; break;
            case CurrentPhase::ACCEPTING_VALUE:  stats.acceptingBars++; break;
            case CurrentPhase::UNKNOWN:          stats.unknownBars++; break;
            default:                             stats.unknownBars++; break;
            }
        }
        stats.totalBars = static_cast<int>(phaseHistory.size());

        // Volume
        stats.totalVolume = zm.sessionCtx.sessionTotalVolume;
        stats.avgVolumePerBar = (stats.totalBars > 0)
            ? (stats.totalVolume / stats.totalBars)
            : 0.0;
        stats.avgVolumePerTick = zm.sessionCtx.avgVolumePerTick;

        // =================================================================
        // AGGREGATE STATS FROM ACCUMULATOR
        // =================================================================
        stats.engagementCount = engagementAccum.totalEngagements;
        stats.totalAcceptances = pocStats.acceptances + vahStats.acceptances + valStats.acceptances +
                                 engagementAccum.vwap.acceptances + engagementAccum.ibHigh.acceptances +
                                 engagementAccum.ibLow.acceptances;
        stats.totalRejections = pocStats.rejections + vahStats.rejections + valStats.rejections +
                                engagementAccum.vwap.rejections + engagementAccum.ibHigh.rejections +
                                engagementAccum.ibLow.rejections;
        stats.totalTags = pocStats.tags + vahStats.tags + valStats.tags +
                          engagementAccum.vwap.tags + engagementAccum.ibHigh.tags +
                          engagementAccum.ibLow.tags;
        stats.totalUnresolved = pocStats.unresolved + vahStats.unresolved + valStats.unresolved +
                                engagementAccum.vwap.unresolved + engagementAccum.ibHigh.unresolved +
                                engagementAccum.ibLow.unresolved;

        // Zone counts from ZoneManager (current snapshot - may change on backfill)
        stats.activeZones = 0;
        stats.expiredZones = 0;
        for (const auto& [id, zone] : zm.activeZones) {
            if (zone.strengthTier == ZoneStrength::EXPIRED) {
                stats.expiredZones++;
            }
            else {
                stats.activeZones++;
            }
        }

        return stats;
    }

} // namespace AMT

#endif // AMT_ANALYTICS_H
