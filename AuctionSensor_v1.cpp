// AuctionSensor_v1.cpp
// Auction Sensor v1 (Pure AMT) � Refactored for robustness & configurability

#define NOMINMAX

// ============================================================================
// PERFORMANCE TIMING INSTRUMENTATION
// Set to 1 to enable timing diagnostics, 0 to disable (zero runtime cost)
// ============================================================================
#define PERF_TIMING_ENABLED 1

// ============================================================================
// NO-OP BODY TEST (Experiment 2: Isolate dispatch overhead)
// Set to 1 to skip all processing and measure pure dispatch cost.
// With this enabled, Sierra's per-study ms shows ONLY AutoLoop dispatch overhead.
// ============================================================================
#define NOOP_BODY_TEST 0

// ============================================================================
// MANUAL LOOP MODE (AutoLoop=0 Performance Optimization)
// Set to 1 to use manual bar iteration instead of Sierra's AutoLoop.
// This reduces GetStudyArrayUsingID calls from ~20,000 to ~20 per full recalc.
// ============================================================================
#define USE_MANUAL_LOOP 0

// ============================================================================
// LOGGING VALIDATION MODE (Debug: Detect stale/incorrect logged values)
// Set to 1 to enable validation guards that check:
// - Values logged before they are valid (comparison of live vs stored)
// - State carried over between bars unintentionally
// - Zero/UNDEFINED values logged when they shouldn't be
// ============================================================================
#define LOGGING_VALIDATION_ENABLED 0

#if PERF_TIMING_ENABLED
#include <windows.h>  // For QueryPerformanceCounter

// Simple high-resolution timer for Windows
// Works for both local stack variables and Sierra's persistent storage.
// QueryPerformanceFrequency is cheap (~25 cycles, reads TSC frequency from register).
struct PerfTimer {
    LARGE_INTEGER start, freq;

    void Start() {
        QueryPerformanceFrequency(&freq);  // Always init - cheap call
        QueryPerformanceCounter(&start);
    }

    double ElapsedMs() const {
        if (freq.QuadPart == 0) return 0.0;  // Start() not called
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
    }
};

// Accumulated timing stats for a single full recalculation pass
struct RecalcTimingStats {
    double totalMs = 0.0;
    double snapshotMs = 0.0;
    double vbpMs = 0.0;
    double zoneMs = 0.0;
    double baselineMs = 0.0;
    double sessionDetectMs = 0.0;  // GetVbPSessionInfo + phase detection
    double accumMs = 0.0;          // Session accumulator updates
    double otherMs = 0.0;          // Everything else
    double perBarTotalMs = 0.0;    // Total time inside study function (per-bar)
    double preWorkMs = 0.0;        // Time from function entry to first real work
    int barsProcessed = 0;
    int studyEnterCount = 0;       // Total function entries during full recalc
    int vbpCalls = 0;
    int snapshotCalls = 0;
    int rebuildCount = 0;          // How many times rebuild loop triggered
    int rebuildBarsTotal = 0;      // Total bars scanned in rebuild loops
    bool isFullRecalc = false;
    int updateStartIndex = 0;

    void Reset() {
        totalMs = snapshotMs = vbpMs = zoneMs = baselineMs = 0.0;
        sessionDetectMs = accumMs = otherMs = perBarTotalMs = preWorkMs = 0.0;
        barsProcessed = studyEnterCount = vbpCalls = snapshotCalls = 0;
        rebuildCount = rebuildBarsTotal = 0;
        isFullRecalc = false;
        updateStartIndex = 0;
    }
};
#endif // PERF_TIMING_ENABLED

#include "sierrachart.h"

#include <cmath>
#include <algorithm>
#include <deque>
#include <vector>
#include <map>
#include <cstdio>

// AMT Framework headers (SSOT for zone types and tracking)
#include "amt_core.h"
#include "AMT_config.h"
#include "AMT_Patterns.h"
#include "AMT_Helpers.h"
#include "AMT_Snapshots.h"
#include "AMT_VolumeProfile.h"
#include "AMT_Session.h"
#include "AMT_Probes.h"
#include "AMT_Logger.h"
#include "AMT_LoggingContext.h"  // Logging lifecycle contract implementation
#include "AMT_Modules.h"
#include "AMT_Zones.h"
#include "AMT_Updates.h"
#include "AMT_Phase.h"
#include "AMT_Analytics.h"
#include "AMT_Bridge.h"
#include "AMT_Arbitration_Seam.h"  // SSOT for M0 arbitration constants
#include "AMT_Invariants.h"        // SSOT runtime assertions (SC-compliant logging)
#include "AMT_ContextBuilder.h"    // SSOT for AuctionContext population
#include "AMT_ProfileShape.h"      // Profile shape classification (Phase 1 DayType)
#include "AMT_DayType.h"           // DayType structural classification (Phase 2)
#include "AMT_BehaviorMapping.h"   // Profile shape -> behavior outcome mapping (v1.2)
#include "AMT_TuningTelemetry.h"   // TELEMETRY ONLY: Friction/volatility advisory logging
#include "AMT_Liquidity.h"         // True Liquidity 3-component model (replaces DOMWarmup for LIQ)
#include "AMT_Volatility.h"        // Volatility regime classification (COMPRESSION/NORMAL/EXPANSION/EVENT)
#include "AMT_Imbalance.h"         // Imbalance/displacement detection engine
#include "AMT_VolumeAcceptance.h"  // Volume acceptance/rejection engine (move confirmation)
#include "AMT_ValueLocation.h"     // Value-location/structure engine (Where am I relative to value?)
#include "AMT_DeltaEngine.h"       // Delta participation pressure engine (sustained vs episodic, alignment, confidence)
#include "AMT_Signals.h"           // AMT signal processing: ActivityClassifier, AMTStateTracker, etc.
#include "AMT_Dalton.h"            // Dalton's Market Profile framework: 1TF/2TF, IB, Day Types
#include "AMT_LevelAcceptance.h"   // Unified level acceptance/rejection framework

SCDLLName("AuctionSensor_v1")

// ============================================================================
// PART 1.5: USING DECLARATIONS FOR EXTRACTED TYPES
// Types now in AMT_Probes.h, AMT_Modules.h, etc.
// ============================================================================

// Bring extracted types into global scope for compatibility

// From AMT_Patterns.h
using AMT::VolumeProfilePattern;
using AMT::TPOMechanics;
using AMT::BalanceDOMPattern;
using AMT::ImbalanceDOMPattern;
using AMT::BalanceDeltaPattern;
using AMT::ImbalanceDeltaPattern;
using AMT::DOMControlPattern;
using AMT::DOMEvent;
using AMT::BalanceProfileShape;
using AMT::ImbalanceProfileShape;
using AMT::BalanceStructure;
using AMT::ImbalanceStructure;
using AMT::ConfidenceWeights;
using AMT::ConfidenceAttribute;
using AMT::AuctionContext;

// From AMT_Probes.h
using AMT::ProbeDirection;
using AMT::ProbeStatus;
using AMT::MechanismTag;
using AMT::ProbeRequest;
using AMT::ProbeResult;
using AMT::ScenarioKey;
using AMT::ScenarioEntry;
using AMT::ScenarioMatch;
using AMT::AuctionMode;
using AMT::ProbeBlockReason;
using AMT::ProbeManager;
using AMT::ReplayValidator;

// From AMT_Modules.h
using AMT::EvidenceScore;
using AMT::MicroVolumeAtPrice;
using AMT::MicroAuction;
using AMT::MiniVPModule;
using AMT::ZoneRecord;
using AMT::ZoneStore;
using AMT::AuctionContextModule;
using AMT::DynamicGaugeModule;

// From AMT_VolumeProfile.h
using AMT::VbPLevelContext;
// Note: SessionVolumeProfile stays with full prefix (large struct with methods)

// From AMT_Snapshots.h
using AMT::DepthPoint;
using AMT::StructureSnapshot;
using AMT::EffortSnapshot;
using AMT::LiquiditySnapshot;
using AMT::ObservableSnapshot;
using AMT::DriftTracker;
using AMT::RollingDist;
// NOTE: BaselineEngine removed (Dec 2024) - use EffortBaselineStore, SessionDeltaBaseline, DOMWarmup

// From AMT_Liquidity.h
using AMT::Liq3Result;
using AMT::LiquidityState;
using AMT::LiquidityEngine;

// From AMT_Helpers.h
using AMT::TimeToSeconds;
using AMT::PriceToTicks;
using AMT::SafeGetAt;
using AMT::IsValidPrice;
using AMT::DetermineExactPhase;

// From AMT_Logger.h
using AMT::LogChannel;
using AMT::LogLevel;
using AMT::ThrottleKey;
using AMT::LogCategory;
using AMT::AmtBarData;
using AMT::LogManager;

// From AMT_Session.h
using AMT::SessionPhaseCoordinator;
// Note: SessionContext, SessionManager, SessionAccumulators, SessionStatistics
// are used without prefix as they are not in AMT namespace

// VolumeAtPrice alias (Sierra Chart API struct)
using VolumeAtPrice = s_VolumeAtPriceV2;

// ============================================================================
// M0 ARBITRATION CONSTANTS - SSOT: AMT_Arbitration_Seam.h
// ============================================================================
using namespace AMT_Arb;

// ============================================================================
// SCENARIO DATABASE (11 Scenarios from OF_PLAYBOOK.txt)
// Note: ScenarioEntry, ScenarioKey now from AMT_Probes.h
// ============================================================================

static const ScenarioEntry SCENARIO_DATABASE[] =
{
    // Scenario 45: BOUNDARY_TEST - Potential Fade or Breakout
    {
        45,
        { AMT::AMTMarketState::BALANCE, AMT::AggressionType::INITIATIVE,
          AMT::AuctionFacilitation::LABORED, AMT::CurrentPhase::TESTING_BOUNDARY },
        7,
        "BOUNDARY_TEST",
        "Test boundary for acceptance/rejection",
        AMT::AuctionIntent::ACCUMULATION
    },

    // Scenario 159: FLAG_PATTERN - Counter Stuck, Buy Breakout
    {
        159,
        { AMT::AMTMarketState::IMBALANCE, AMT::AggressionType::RESPONSIVE,
          AMT::AuctionFacilitation::LABORED, AMT::CurrentPhase::PULLBACK },
        8,
        "FLAG_PATTERN",
        "Pullback exhausting, continuation expected",
        AMT::AuctionIntent::ACCUMULATION
    },

    // Scenario 152: BUY_DIP - Weak Counter-Trend
    {
        152,
        { AMT::AMTMarketState::IMBALANCE, AMT::AggressionType::RESPONSIVE,
          AMT::AuctionFacilitation::INEFFICIENT, AMT::CurrentPhase::PULLBACK },
        8,
        "BUY_DIP",
        "Weak counter-trend being absorbed",
        AMT::AuctionIntent::ABSORPTION
    },

    // Scenario 123: SPIKE - Fast Directional Momentum
    // Note: Uses DRIVING_UP as representative directional phase (matches either DRIVING_UP or DRIVING_DOWN)
    {
        123,
        { AMT::AMTMarketState::IMBALANCE, AMT::AggressionType::INITIATIVE,
          AMT::AuctionFacilitation::INEFFICIENT, AMT::CurrentPhase::DRIVING_UP },
        9,
        "SPIKE",
        "Fast momentum, early = accumulation, late = exhaustion",
        AMT::AuctionIntent::ACCUMULATION
    },

    // Scenario 58: PRIME_FADE - Clean Rejection at Edge
    {
        58,
        { AMT::AMTMarketState::BALANCE, AMT::AggressionType::RESPONSIVE,
          AMT::AuctionFacilitation::EFFICIENT, AMT::CurrentPhase::ROTATION },
        9,
        "PRIME_FADE",
        "Clean rejection at rotation extreme",
        AMT::AuctionIntent::DISTRIBUTION
    },

    // Scenario 168: SQUEEZE - Counter-Trend Failed, Rocket Fuel
    {
        168,
        { AMT::AMTMarketState::IMBALANCE, AMT::AggressionType::RESPONSIVE,
          AMT::AuctionFacilitation::FAILED, AMT::CurrentPhase::FAILED_AUCTION },
        10,
        "SQUEEZE",
        "Counter-trend collapsed, explosive continuation",
        AMT::AuctionIntent::ABSORPTION
    },

    // Scenario 116: THE_DRIVE - Max Edge Trend Continuation
    // Note: Uses DRIVING_UP as representative directional phase (matches either DRIVING_UP or DRIVING_DOWN)
    {
        116,
        { AMT::AMTMarketState::IMBALANCE, AMT::AggressionType::INITIATIVE,
          AMT::AuctionFacilitation::EFFICIENT, AMT::CurrentPhase::DRIVING_UP },
        10,
        "THE_DRIVE",
        "Pure trend continuation with efficient facilitation",
        AMT::AuctionIntent::ACCUMULATION
    },

    // Scenario 72: ROTATION_FADE - Mean Reversion
    {
        72,
        { AMT::AMTMarketState::BALANCE, AMT::AggressionType::RESPONSIVE,
          AMT::AuctionFacilitation::LABORED, AMT::CurrentPhase::ROTATION },
        7,
        "ROTATION_FADE",
        "Fade rotation extreme back to value",
        AMT::AuctionIntent::DISTRIBUTION
    },

    // Scenario 999: GENERIC_ROTATION - Basic Balanced Rotation
    {
        999,
        { AMT::AMTMarketState::BALANCE, AMT::AggressionType::RESPONSIVE,
          AMT::AuctionFacilitation::EFFICIENT, AMT::CurrentPhase::ROTATION },
        5,
        "GENERIC_ROTATION",
        "Standard balanced rotation",
        AMT::AuctionIntent::NEUTRAL
    },

    // Scenario 998: GENERIC_IMBALANCE - Basic Trend
    // Note: Uses DRIVING_UP as representative directional phase (matches either DRIVING_UP or DRIVING_DOWN)
    {
        998,
        { AMT::AMTMarketState::IMBALANCE, AMT::AggressionType::INITIATIVE,
          AMT::AuctionFacilitation::EFFICIENT, AMT::CurrentPhase::DRIVING_UP },
        5,
        "GENERIC_IMBALANCE",
        "Standard directional imbalance",
        AMT::AuctionIntent::ACCUMULATION
    },

    // Scenario 997: LABORED_TREND - Grind Up/Down
    // Note: Uses DRIVING_UP as representative directional phase (matches either DRIVING_UP or DRIVING_DOWN)
    {
        997,
        { AMT::AMTMarketState::IMBALANCE, AMT::AggressionType::RESPONSIVE,
          AMT::AuctionFacilitation::LABORED, AMT::CurrentPhase::DRIVING_UP },
        5,
        "LABORED_TREND",
        "Slow grind trend with heavy resistance",
        AMT::AuctionIntent::ABSORPTION
    }
};

static const int SCENARIO_COUNT = sizeof(SCENARIO_DATABASE) / sizeof(SCENARIO_DATABASE[0]);

// Note: EvidenceScore, AuctionContextModule, DynamicGaugeModule, VbPLevelContext
// are now in AMT_Modules.h and AMT_VolumeProfile.h (imported via using declarations)


// ============================================================================
// EXTRACTED CODE REMOVED
// The following types are now in modular headers:
//   - ComputeValueAreaFromSortedVector: AMT_VolumeProfile.h
//   - MicroAuction, MiniVPModule, ZoneStore, AuctionContextModule, DynamicGaugeModule: AMT_Modules.h
//   - LogManager, LogChannel, LogLevel, ThrottleKey: AMT_Logger.h
//   - ProbeManager, ReplayValidator, ProbeBlockReason: AMT_Probes.h
//   - DepthPoint, Snapshots, DriftTracker: AMT_Snapshots.h
//   - SessionVolumeProfile, VbPLevelContext: AMT_VolumeProfile.h
//   - SessionContext, SessionManager, SessionAccumulators: AMT_Session.h
// ============================================================================

struct StudyState
{
    // Runtime state - AMT ZoneManager is now SSOT for all zone data
    int       lastIndex = -1;
    int       lastAmtCsvLoggedBar = -1;  // Prevent duplicate AMT CSV rows per bar
    int       lastStatsLoggedBar = -1;  // Prevent duplicate session stats per bar
    int       lastSessionEventBar = -1;  // Prevent duplicate session events per bar
    int       lastBarCloseStoredBar = -1;  // Track last bar where zone values stored to subgraphs
    bool      initialRecalcComplete = false;  // True after first full recalc finishes

    // NOTE: Legacy BaselineEngine stats removed (Dec 2024)
    // Now using EffortBaselineStore, SessionDeltaBaseline, DOMWarmup below
    AMT::AuctionContext amtContext;

    // ITEM 2.1: Dual session contexts
    SessionManager    sessionMgr;
    SessionPhaseCoordinator phaseCoordinator;  // SSOT for session phase
    BaselineSessionManager baselineSessionMgr;  // SSOT for three-phase execution

    // Progress-conditioned profile baselines (VA width, POC dominance per bucket)
    AMT::HistoricalProfileBaseline rthProfileBaseline;   // RTH session profile baseline
    AMT::HistoricalProfileBaseline gbxProfileBaseline;   // GBX session profile baseline

    // Effort baselines (bar-sample distributions per time bucket, from prior RTH sessions)
    // GBX policy: NOT_APPLICABLE for effort baselines (only RTH bars populate/query)
    AMT::EffortBaselineStore effortBaselines;             // RTH bar-level effort metrics
    AMT::SessionDeltaBaseline sessionDeltaBaseline;       // Session-aggregate delta ratio baseline (separate from bar-level)
    AMT::DOMWarmup domWarmup;                             // Live 15-min DOM warmup (legacy, kept for stack/pull/spread)
    AMT::LiquidityEngine liquidityEngine;                 // True Liquidity 3-component model (REPLACES domWarmup for LIQ)
    AMT::Liq3Result lastLiqSnap;                          // Most recent liquidity computation result
    AMT::SpatialLiquidityProfile lastSpatialProfile;      // Most recent spatial liquidity profile (walls/voids/OBI/POLR)
    AMT::LiquidityErrorCounters liqErrorCounters;         // Session-scoped error tracking (No Silent Failures)
    int lastLiqErrLogBar = -100;                          // Rate-limiting for [LIQ-ERR] logs

    // Volatility Engine (context gate for trigger trustworthiness)
    AMT::VolatilityEngine volatilityEngine;              // Regime classification with hysteresis
    AMT::VolatilityResult lastVolResult;                 // Most recent volatility computation result
    AMT::VolatilityRegime lastLoggedVolRegime = AMT::VolatilityRegime::UNKNOWN;  // Log-on-change tracking
    AMT::AuctionPace lastLoggedPace = AMT::AuctionPace::UNKNOWN;  // Pace log-on-change tracking

    // Delta Engine (participation pressure: sustained vs episodic, alignment, confidence)
    AMT::DeltaEngine deltaEngine;                        // Delta character/alignment classification with hysteresis
    AMT::DeltaResult lastDeltaResult;                    // Most recent delta computation result
    AMT::DeltaCharacter lastLoggedDeltaCharacter = AMT::DeltaCharacter::UNKNOWN;  // Log-on-change tracking

    // Imbalance Engine (displacement detection: diagonal imbalance, divergence, absorption, trapped traders)
    AMT::ImbalanceEngine imbalanceEngine;               // Imbalance/displacement detection with hysteresis
    AMT::ImbalanceResult lastImbalanceResult;           // Most recent imbalance computation result
    AMT::ImbalanceType lastLoggedImbalanceType = AMT::ImbalanceType::NONE;  // Log-on-change tracking
    int lastImbalanceLogBar = -100;                     // Rate-limiting for IMB logs

    // Volume Acceptance Engine (move confirmation: did volume support or reject the move?)
    AMT::VolumeAcceptanceEngine volumeAcceptanceEngine;   // Acceptance/rejection detection with hysteresis
    AMT::VolumeAcceptanceResult lastVolumeResult;         // Most recent volume acceptance result
    AMT::AcceptanceState lastLoggedAcceptanceState = AMT::AcceptanceState::UNKNOWN;  // Log-on-change tracking
    int lastVolumeLogBar = -100;                          // Rate-limiting for VOL logs

    // Value Location Engine (Where am I relative to value and structure?)
    AMT::ValueLocationEngine valueLocationEngine;         // Location/structure classification with hysteresis
    AMT::ValueLocationResult lastValueLocationResult;     // Most recent value location result
    AMT::ValueZone lastLoggedValueZone = AMT::ValueZone::UNKNOWN;  // Log-on-change tracking
    int lastValueLocationLogBar = -100;                   // Rate-limiting for VAL logs

    AMT::AMTMarketState lastLoggedState = AMT::AMTMarketState::BALANCE;
    AMT::CurrentPhase lastLoggedPhase = AMT::CurrentPhase::ROTATION;
    bool              lastLoggedModeLocked = false;
    int               lastLoggedArbReason = -1;  // M0: log-on-change for arbitration

    // ITEM 1.1: Current observable snapshot
    ObservableSnapshot currentSnapshot;

    // ITEM 1.2: Drift tracking
    DriftTracker      drift;

    // Stage 3: DOM quality tracking for domStrength
    AMT::DOMQualityTracker domQualityTracker;

    // Input validity flags
    bool domInputsValid = false;
    bool statsInputsValid = false;
    // hvnLvnInputsValid removed - HVN/LVN inputs no longer used
    bool vwapBandsInputsValid = false;
    bool depthOhlcInputsValid = false;

    // Facilitation computation state (for facilitationKnown truthfulness)
    bool facilitationComputed = false;  // True when baseline stable enough for valid FACIL
    int facilSessionSamples = 0;        // Per-session sample count (reset at session boundary)

    // === NEW: Probe & Scenario Modules ===
    AuctionContextModule auctionCtxModule;
    DynamicGaugeModule   dynamicGauge;
    MiniVPModule         miniVP;
    ZoneStore            zoneStore;
    SessionVolumeProfile sessionVolumeProfile;
    AMT::ProfileStructureResult lastProfileStructureResult;  // Most recent profile structure result

    // === AMT ZONE TRACKING (Week 4 Integration) ===
    // ZoneManager tracks all active zones with full lifecycle
    // PhaseTracker determines current market phase from zone activity
    AMT::ZoneManager     amtZoneManager;
    AMT::PhaseTracker    amtPhaseTracker;
    AMT::ExtremeAcceptanceTracker extremeTracker;  // AMT-aligned acceptance at session extremes
    // NOTE: MarketStateTracker removed - replaced by AMTSignalEngine (Phase 3 migration)
    AMT::DayTypeClassifier dayTypeClassifier;    // Phase 2: structural classification
    AMT::BehaviorSessionManager behaviorMgr;     // v1.2: Profile shape -> behavior outcome SSOT
    AMT::BehaviorHistoryTracker behaviorHistory; // v1.2: Per-shape hit rates for confidence multiplier
    AMT::AMTSignalEngine amtSignalEngine;        // AMT signal processing: activity classification, state tracking
    AMT::StateEvidence   lastStateEvidence;      // Last computed AMT state evidence (for logging)
    std::vector<AMT::SinglePrintZone> singlePrintZones;  // Session-persistent single print tracking

    // === DALTON FRAMEWORK (Proper Market Profile Implementation) ===
    // Per Dalton: 1TF/2TF is the detection mechanism for Balance/Imbalance
    // - 1TF (one-time framing) = IMBALANCE = trending
    // - 2TF (two-time framing) = BALANCE = rotation
    AMT::DaltonEngine    daltonEngine;           // Dalton framework: rotation, IB, day type
    AMT::DaltonState     lastDaltonState;        // Last computed Dalton state (for logging)
    bool                 amtZonesInitialized = false;
    int                  amtLastZoneUpdateBar = -1;

    // === DALTON ADVANCED: Acceptance, Value Migration, Spikes ===
    AMT::SpikeContext    priorSessionSpike;      // Spike from prior session (for opening evaluation)
    double               sessionOpenPrice = 0.0; // First bar open of current session
    bool                 sessionOpenCaptured = false;  // Has current session's open been captured
    double               priorSessionHigh = 0.0; // Prior session extremes (for spike detection)
    double               priorSessionLow = 0.0;

    // === LEVEL ACCEPTANCE ENGINE ===
    AMT::LevelAcceptanceEngine levelAcceptance;  // Unified level acceptance/rejection tracking

    // === WEEK 5: PER-CHART PERSISTENT STATE (no static locals) ===
    // TransitionState: tracks zone entry/exit transitions
    // ZoneTransitionMemory: sticky zone behavior for N bars
    // DOMCachePolicy: bar-based DOM liquidity caching
    // ResolutionPolicy: bar+time based zone resolution
    // ZoneContextSnapshot: current zone context result
    AMT::TransitionState        zoneTransitionState;
    AMT::ZoneTransitionMemory   zoneTransitionMemory;
    AMT::DOMCachePolicy         domCachePolicy;
    AMT::ResolutionPolicy       resolutionPolicy;
    AMT::ZoneContextSnapshot    zoneContextSnapshot;

    // === PHASE 1B: DOM-AWARE DYNAMIC WIDTHS ===
    // Tracks legacy liqTicks for change detection; only updates AMT when changed
    int cachedAmtLiqTicks = 0;

    // === PHASE 2: BASELINE INTEGRATION COUNTERS ===
    // Invariant: amtEngagementsFinalized == amtBaselineSamplesPushed
    int amtEngagementsFinalized = 0;
    int amtBaselineSamplesPushed = 0;

#ifdef VALIDATE_ZONE_MIGRATION
    // === PHASE 3: VALIDATION STATE ===
    // Episode-level comparison between legacy and AMT engagement systems
    // Gated by compile flag - zero runtime cost when disabled
    AMT::ValidationState validationState;
#endif

    // === UNIFIED LOG MANAGER ===
    LogManager           logManager;
    AMT::PatternLogger   patternLogger;  // Phase 4: Event-style pattern logging
    std::vector<AMT::CurrentPhase> amtPhaseHistory;  // For session stats

    // === PERFORMANCE: Reusable buffers (avoid per-call allocation) ===
    std::vector<DepthPoint> depthPointsCache;  // For ComputeLiquidityCoreTicks

    // === SESSION ACCUMULATORS ===
    SessionAccumulators  sessionAccum;  // Tracks counts that aggregate during session
    SessionEngagementAccumulator engagementAccum;  // SSOT for per-anchor engagement stats (survives zone clearing)

    // === Probe Manager ===
    ProbeManager         probeMgr;
    ReplayValidator      replayValidator;

    // Probe tracking (legacy - kept for compatibility, but probeMgr is authoritative)
    int  active_probe_count = 0;
    bool probeSystemEnabled = true;  // Can be toggled via input
    bool vbpDataWarningShown = false;   // Track if VbP data warning has been logged
    bool vbpConfigWarningShown = false; // Track if VbP config warning has been logged
    bool vbpProfileCheckDone = false;   // Temp diag
    SCDateTime lastVbPWarning = 0;      // Throttle timer for VbP warnings

    // === VbP AS SSOT FOR SESSION DETECTION ===
    // Session changes detected by VbP profile m_StartDateTime changing
    SCDateTime vbpSessionStart = 0;       // Current VbP profile start time
    bool       vbpSessionIsEvening = false; // true = Globex, false = RTH

    // === DECOUPLED DISPLAY LEVELS (independent of zone objects) ===
    // These are the SSOT for displayed POC/VAH/VAL values.
    // Updated from VBP inputs every bar. Zones are optional metadata.
    // This ensures display never goes to 0 even if zones are pruned.
    double displayPOC = 0.0;
    double displayVAH = 0.0;
    double displayVAL = 0.0;
    bool   displayLevelsValid = false;  // true once we have valid non-zero VBP data

    // === DIAGNOSTIC RATE-LIMITING (per-instance, not static) ===
    // Per ACSIL guide: static locals are shared across all study instances in DLL.
    // These must be per-instance to avoid cross-chart interference.
    int diagLastValidationBar = -1;        // Input validation rate-limiting
    int diagLastBaselineLogBar = -1;       // Baseline log rate-limiting
    int diagLastViolationBar = -1;         // Invariant violation rate-limiting
    int diagLastDepthDiagBar = -1;         // Depth diagnostic rate-limiting
    int diagLastExtractionDiagBar = -1;    // Depth extraction rate-limiting
    int diagLastLevelsDiagBar = -1;        // Levels diagnostic rate-limiting
    int diagLastFricDiagBar = -1;          // Friction diagnostic rate-limiting
    int diagLastVolBaselineLogBar = -100;  // Volume baseline rate-limiting
    int diagLastSyntheticLogBar = -100;    // Synthetic baseline rate-limiting
    int diagLastShapeFailLogBar = -100;    // Shape fail rate-limiting

#if PERF_TIMING_ENABLED
    // === PERFORMANCE TIMING (compile-time gated) ===
    RecalcTimingStats perfStats;
    PerfTimer perfTimer;
    bool wasFullRecalc = false;  // Track if previous call was full recalc
#endif

    // ========================================================================
    // DRY HELPER: SyncSessionPhase (SSOT Phase Sync)
    // ========================================================================
    // Ensures session phase is consistent across all SSOT consumers.
    // Per CLAUDE.md: Session phase stored in 3 locations that must sync:
    //   1. phaseCoordinator (SSOT owner)
    //   2. sessionMgr.activePhase (for getActiveContext)
    //   3. amtContext.session (for context consumers)
    //
    // USAGE: Always call SyncSessionPhase() instead of UpdatePhase() directly.
    // ========================================================================

    /**
     * Update session phase and sync all SSOT consumers atomically.
     * @param newPhase The new session phase
     * @return true if phase changed
     */
    bool SyncSessionPhase(AMT::SessionPhase newPhase) {
        const bool changed = phaseCoordinator.UpdatePhase(newPhase);
        sessionMgr.activePhase = newPhase;
        amtContext.session = newPhase;
        return changed;
    }

    void resetAll(int baselineWindow, int warmUpBars)
    {
        // NOTE: Legacy BaselineEngine stats removed (Dec 2024)
        // New baselines (effortBaselines, sessionDeltaBaseline, domWarmup) are reset separately
        sessionMgr.reset();
        drift.barsProcessed = 0;
        drift.warmUpBarsRequired = warmUpBars;
        drift.consecutiveZeroDomBars = 0;
        domQualityTracker.Reset();  // Stage 3: Reset DOM freshness tracking
        currentSnapshot = ObservableSnapshot();
        phaseCoordinator.Reset();  // SSOT for session phase
        baselineSessionMgr.Reset();  // SSOT for three-phase execution
        rthProfileBaseline.Reset();  // Progress-conditioned profile baseline (RTH)
        gbxProfileBaseline.Reset();  // Progress-conditioned profile baseline (GBX)
        effortBaselines.Reset();     // RTH bar-level effort baselines
        sessionDeltaBaseline.Reset(); // Session-aggregate delta ratio baseline
        domWarmup.Reset();            // Live DOM warmup (resets each RTH session)
        liquidityEngine.Reset();      // True Liquidity 3-component model (resets each session)
        liquidityEngine.SetDOMWarmup(&domWarmup);  // Wire up phase-aware baseline source (SSOT)
        liqErrorCounters.Reset();     // Session-scoped liquidity error counters
        lastLiqErrLogBar = -100;      // Reset rate-limiter for [LIQ-ERR] logs
        volatilityEngine.Reset();     // Volatility regime classification (resets each session)
        volatilityEngine.SetEffortStore(&effortBaselines);  // Wire up phase-aware baseline source (SSOT)
        volatilityEngine.SetSyntheticMode(true, 5);  // Enable 5-bar synthetic aggregation for 1-min charts
        lastVolResult = AMT::VolatilityResult();  // Clear last result
        lastLoggedVolRegime = AMT::VolatilityRegime::UNKNOWN;  // Reset log-on-change tracking
        lastLoggedPace = AMT::AuctionPace::UNKNOWN;  // Reset pace log-on-change tracking
        deltaEngine.Reset();          // Delta participation pressure (resets each session)
        deltaEngine.SetEffortStore(&effortBaselines);  // Wire up phase-aware baseline source (SSOT)
        deltaEngine.SetSessionDeltaBaseline(&sessionDeltaBaseline);  // Wire up session-aggregate baseline
        lastDeltaResult = AMT::DeltaResult();  // Clear last result
        lastLoggedDeltaCharacter = AMT::DeltaCharacter::UNKNOWN;  // Reset log-on-change tracking
        imbalanceEngine.Reset();      // Imbalance/displacement detection (resets each session)
        imbalanceEngine.SetEffortStore(&effortBaselines);  // Wire up phase-aware baseline source
        lastImbalanceResult = AMT::ImbalanceResult();  // Clear last result
        lastLoggedImbalanceType = AMT::ImbalanceType::NONE;  // Reset log-on-change tracking
        lastImbalanceLogBar = -100;   // Reset rate-limiter for [IMB] logs
        volumeAcceptanceEngine.Reset();  // Volume acceptance/rejection detection (resets each session)
        volumeAcceptanceEngine.SetEffortStore(&effortBaselines);  // Wire up phase-aware baseline source
        lastVolumeResult = AMT::VolumeAcceptanceResult();  // Clear last result
        lastLoggedAcceptanceState = AMT::AcceptanceState::UNKNOWN;  // Reset log-on-change tracking
        lastVolumeLogBar = -100;      // Reset rate-limiter for [VOL] logs
        valueLocationEngine.Reset();  // Value location/structure engine (resets each session)
        lastValueLocationResult = AMT::ValueLocationResult();  // Clear last result
        lastLoggedValueZone = AMT::ValueZone::UNKNOWN;  // Reset log-on-change tracking
        lastValueLocationLogBar = -100;  // Reset rate-limiter for [VAL] logs
        lastProfileStructureResult = AMT::ProfileStructureResult();  // Clear last profile structure result
        vbpDataWarningShown = false;
        vbpConfigWarningShown = false;
        vbpProfileCheckDone = false;
        lastVbPWarning = 0;

        // Reset probe modules
        miniVP.Clear();
        probeMgr.Reset();
        replayValidator.Reset();
        active_probe_count = 0;

        // Reset logging state flags
        lastLoggedState = AMT::AMTMarketState::BALANCE;
        lastLoggedPhase = AMT::CurrentPhase::ROTATION;
        lastLoggedModeLocked = false;
        lastLoggedArbReason = -1;
        facilitationComputed = false;  // Reset facilitation state for warmup
        facilSessionSamples = 0;       // Reset per-session sample count

        // Reset AMT zone tracking (DRY: use atomic helper)
        amtZoneManager.ClearZonesOnly(0, SCDateTime(), AMT::UnresolvedReason::CHART_RESET);
        amtPhaseTracker = AMT::PhaseTracker();
        // NOTE: MarketStateTracker removed - AMTSignalEngine handles state tracking now
        behaviorMgr.Reset();         // v1.2: reset behavior outcome tracking
        behaviorHistory.Reset();     // v1.2: reset per-shape hit rate history

        amtZonesInitialized = false;
        amtLastZoneUpdateBar = -1;

        // Reset decoupled display levels (will be repopulated from VBP as data flows)
        displayPOC = 0.0;
        displayVAH = 0.0;
        displayVAL = 0.0;
        displayLevelsValid = false;

        // Reset Week 5 per-chart persistent state
        zoneTransitionState.Reset();
        zoneTransitionMemory.Reset();
        domCachePolicy.Reset();
        // resolutionPolicy uses defaults, no reset needed
        zoneContextSnapshot.Reset();

        // Reset unified LogManager (handles all CSV files)
        logManager.Shutdown();
        amtPhaseHistory.clear();

        // Reset session accumulators
        sessionAccum.Reset();
        engagementAccum.Reset();  // Per-anchor engagement stats

        // Reset VbP SSOT session tracking
        vbpSessionStart = 0;
        vbpSessionIsEvening = false;

#ifdef VALIDATE_ZONE_MIGRATION
        // Reset Phase 3 validation state
        validationState.StartSession(0);
#endif
    }
};

// --- Liquidity Core Calculation ---

static int ComputeLiquidityCoreTicks(
    SCStudyInterfaceRef sc,
    std::vector<DepthPoint>& pts,  // Reusable buffer (caller owns, avoids allocation)
    double anchorPrice,
    int maxDepthLevelsToRead,
    int maxBandTicks,
    double targetPct,
    double tickSize
)
{
    pts.clear();  // Reuse existing capacity

    if (!IsValidPrice(anchorPrice) || tickSize <= 0.0)
        return 0;

    const long long aTicks = PriceToTicks(anchorPrice, tickSize);

    s_MarketDepthEntry e;

    const int bidLevels =
        (std::min)(sc.GetBidMarketDepthNumberOfLevels(), maxDepthLevelsToRead);

    for (int i = 0; i < bidLevels; ++i)
    {
        if (sc.GetBidMarketDepthEntryAtLevel(e, i) && IsValidPrice(e.Price))
        {
            const int d = static_cast<int>(std::abs(PriceToTicks(e.Price, tickSize) - aTicks));
            if (d <= maxBandTicks)
                pts.push_back({ d, static_cast<double>(e.Quantity) });
        }
    }

    const int askLevels =
        (std::min)(sc.GetAskMarketDepthNumberOfLevels(), maxDepthLevelsToRead);

    for (int i = 0; i < askLevels; ++i)
    {
        if (sc.GetAskMarketDepthEntryAtLevel(e, i) && IsValidPrice(e.Price))
        {
            const int d = static_cast<int>(std::abs(PriceToTicks(e.Price, tickSize) - aTicks));
            if (d <= maxBandTicks)
                pts.push_back({ d, static_cast<double>(e.Quantity) });
        }
    }

    if (pts.empty())
        return 0;

    double total = 0.0;
    for (const auto& p : pts)
        total += p.qty;

    std::sort(
        pts.begin(),
        pts.end(),
        [](const DepthPoint& a, const DepthPoint& b)
        {
            return a.distTicks < b.distTicks;
        }
    );

    const double target = total * targetPct;
    double cum = 0.0;

    for (const auto& p : pts)
    {
        cum += p.qty;
        if (cum >= target)
            return (std::min)(p.distTicks, maxBandTicks);
    }

    return maxBandTicks;
}

// --- Depth Mass Halo Calculation (weighted imbalance around midprice) ---

struct DepthMassHaloResult {
    double bidMass = 0.0;
    double askMass = 0.0;
    double totalMass = 0.0;
    double imbalance = 0.0;  // Bounded [-1, +1]
    bool valid = false;
};

/**
 * Compute weighted depth mass within a price halo around midprice.
 * @param sc Sierra Chart interface
 * @param bestBid Current best bid price
 * @param bestAsk Current best ask price
 * @param haloRadiusTicks Halo radius in ticks (default 8 for ES)
 * @param maxLevels Maximum depth levels to read
 * @param tickSize Tick size for price-to-tick conversion
 * @return DepthMassHaloResult with weighted masses and imbalance
 *
 * Weight function: w(dTicks) = 1 / (1 + dTicks)
 * Imbalance = (BidMass - AskMass) / max(TotalMass, epsilon)
 */
static DepthMassHaloResult ComputeDepthMassHalo(
    SCStudyInterfaceRef sc,
    double bestBid,
    double bestAsk,
    int haloRadiusTicks,
    int maxLevels,
    double tickSize
)
{
    DepthMassHaloResult result;

    // Validate inputs
    if (!IsValidPrice(bestBid) || !IsValidPrice(bestAsk) || tickSize <= 0.0) {
        return result;  // valid = false
    }

    // Reference price: midprice (more stable than last trade)
    const double midPrice = (bestBid + bestAsk) / 2.0;
    const long long midPriceTicks = PriceToTicks(midPrice, tickSize);

    s_MarketDepthEntry e;

    // Process bid levels
    const int bidLevels = (std::min)(sc.GetBidMarketDepthNumberOfLevels(), maxLevels);
    for (int i = 0; i < bidLevels; ++i) {
        if (sc.GetBidMarketDepthEntryAtLevel(e, i) && IsValidPrice(e.Price)) {
            const long long priceTicks = PriceToTicks(e.Price, tickSize);
            const int dTicks = static_cast<int>(std::abs(priceTicks - midPriceTicks));

            if (dTicks <= haloRadiusTicks) {
                // Weight function: w = 1 / (1 + dTicks)
                const double weight = 1.0 / (1.0 + static_cast<double>(dTicks));
                result.bidMass += static_cast<double>(e.Quantity) * weight;
            }
        }
    }

    // Process ask levels
    const int askLevels = (std::min)(sc.GetAskMarketDepthNumberOfLevels(), maxLevels);
    for (int i = 0; i < askLevels; ++i) {
        if (sc.GetAskMarketDepthEntryAtLevel(e, i) && IsValidPrice(e.Price)) {
            const long long priceTicks = PriceToTicks(e.Price, tickSize);
            const int dTicks = static_cast<int>(std::abs(priceTicks - midPriceTicks));

            if (dTicks <= haloRadiusTicks) {
                // Weight function: w = 1 / (1 + dTicks)
                const double weight = 1.0 / (1.0 + static_cast<double>(dTicks));
                result.askMass += static_cast<double>(e.Quantity) * weight;
            }
        }
    }

    result.totalMass = result.bidMass + result.askMass;

    // Compute imbalance (bounded [-1, +1])
    constexpr double epsilon = 1e-9;
    if (result.totalMass > epsilon) {
        result.imbalance = (result.bidMass - result.askMass) / result.totalMass;
        result.valid = true;
    }
    // If totalMass is ~0, leave valid = false (don't push zeros)

    return result;
}

// ============================================================================
// STUDY ARRAY CACHE (For USE_MANUAL_LOOP=1 performance optimization)
// Pre-acquires all study arrays once per call instead of per-bar.
// ============================================================================
#if USE_MANUAL_LOOP
struct StudyArrayCache {
    // VbP Study (Inputs 22-24)
    SCFloatArray vpbPOC;     // Input 22
    SCFloatArray vpbVAH;     // Input 23
    SCFloatArray vpbVAL;     // Input 24

    // TPO Study (Inputs 30-32)
    SCFloatArray tpoPOC;     // Input 30
    SCFloatArray tpoVAH;     // Input 31
    SCFloatArray tpoVAL;     // Input 32

    // Daily OHLC (Inputs 40-43)
    SCFloatArray dailyOpen;  // Input 40
    SCFloatArray dailyHigh;  // Input 41
    SCFloatArray dailyLow;   // Input 42
    SCFloatArray dailyClose; // Input 43

    // VWAP (Inputs 50-54)
    SCFloatArray vwap;       // Input 50
    SCFloatArray vwapUpper1; // Input 51
    SCFloatArray vwapLower1; // Input 52
    SCFloatArray vwapUpper2; // Input 53
    SCFloatArray vwapLower2; // Input 54

    // Best Bid/Ask (Inputs 60-61)
    SCFloatArray bestBid;    // Input 60
    SCFloatArray bestAsk;    // Input 61

    // DOM Study (Inputs 62-65)
    SCFloatArray domBidSize;   // Input 62
    SCFloatArray domAskSize;   // Input 63
    SCFloatArray domBidStack;  // Input 64
    SCFloatArray domAskStack;  // Input 65

    // Numbers Bars (Inputs 70-71, 74-79)
    SCFloatArray nbBidVolSec;  // Input 70
    SCFloatArray nbAskVolSec;  // Input 71
    SCFloatArray nbMaxDelta;   // Input 74
    SCFloatArray nbCumDelta;   // Input 75
    SCFloatArray diagPosDelta; // Input 76: SG43 Diagonal Positive Delta Sum
    SCFloatArray diagNegDelta; // Input 77: SG44 Diagonal Negative Delta Sum
    SCFloatArray avgBidTradeSize; // Input 78: SG51 Average Bid Trade Size
    SCFloatArray avgAskTradeSize; // Input 79: SG52 Average Ask Trade Size

    bool valid = false;

    void Acquire(SCStudyInterfaceRef sc) {
        // Helper lambda to acquire a study array by input index
        auto GetArray = [&](int inputIdx, SCFloatArray& arr) {
            const int studyId = sc.Input[inputIdx].GetStudyID();
            const int subgraphIdx = sc.Input[inputIdx].GetSubgraphIndex();
            if (studyId > 0) {
                sc.GetStudyArrayUsingID(studyId, subgraphIdx, arr);
            }
        };

        // VbP Study (always acquired)
        GetArray(22, vpbPOC);
        GetArray(23, vpbVAH);
        GetArray(24, vpbVAL);

        // TPO Study (always acquired, may be empty)
        GetArray(30, tpoPOC);
        GetArray(31, tpoVAH);
        GetArray(32, tpoVAL);

        // Daily OHLC (always acquired)
        GetArray(40, dailyOpen);
        GetArray(41, dailyHigh);
        GetArray(42, dailyLow);
        GetArray(43, dailyClose);

        // VWAP (always acquired)
        GetArray(50, vwap);
        GetArray(51, vwapUpper1);
        GetArray(52, vwapLower1);
        GetArray(53, vwapUpper2);
        GetArray(54, vwapLower2);

        // Best Bid/Ask (always acquired)
        GetArray(60, bestBid);
        GetArray(61, bestAsk);

        // DOM Study (always acquired, may be empty)
        GetArray(62, domBidSize);
        GetArray(63, domAskSize);
        GetArray(64, domBidStack);
        GetArray(65, domAskStack);

        // Numbers Bars (always acquired)
        GetArray(70, nbBidVolSec);
        GetArray(71, nbAskVolSec);
        GetArray(74, nbMaxDelta);
        GetArray(75, nbCumDelta);
        // Diagonal Delta (Footprint Imbalance)
        GetArray(76, diagPosDelta);
        GetArray(77, diagNegDelta);
        // Average Trade Size (Institutional vs Retail)
        GetArray(78, avgBidTradeSize);
        GetArray(79, avgAskTradeSize);

        valid = true;
    }
};
#endif  // USE_MANUAL_LOOP

// ============================================================================
// ITEM 1.1: SNAPSHOT COLLECTION HELPER
// ============================================================================

#if USE_MANUAL_LOOP
// Optimized version: uses pre-acquired arrays (idx parameter, no per-bar GetStudyArrayUsingID)
static void CollectObservableSnapshot(
    SCStudyInterfaceRef sc,
    StudyState* st,
    int idx,
    ObservableSnapshot& snap,
    const StudyArrayCache& arrays
)
{
    snap.barTime = sc.BaseDateTimeIn[idx];
    snap.isValid = true;

    // --- Helper: Get value from cached array by input index ---
    auto GetCachedValue = [&](const SCFloatArray& arr) -> double {
        if (arr.GetArraySize() == 0) return 0.0;
        return SafeGetAt(arr, idx);
    };

#else
// Original version: calls GetStudyArrayUsingID per bar (AutoLoop=1 compatible)
static void CollectObservableSnapshot(
    SCStudyInterfaceRef sc,
    StudyState* st,
    int idx,
    ObservableSnapshot& snap
)
{
    snap.barTime = sc.BaseDateTimeIn[idx];
    snap.isValid = true;

    // --- Structure: VP ---
    auto GetStudyValue = [&](int inputIdx, const char* inputName = nullptr) -> double
        {
            const int studyId = sc.Input[inputIdx].GetStudyID();
            const int subgraphIdx = sc.Input[inputIdx].GetSubgraphIndex();

            if (studyId == 0)
            {
                // Log once per session if study ID is not configured
                if (idx == 0 && inputName != nullptr)
                {
                    SCString msg;
                    msg.Format("Input[%d] '%s': StudyID=0 (not configured)",
                        inputIdx, inputName);
                    st->logManager.LogOnce(ThrottleKey::INPUT_DIAG, idx, msg.GetChars(), LogCategory::INPUT);
                }
                return 0.0;
            }

            SCFloatArray arr;
            sc.GetStudyArrayUsingID(studyId, subgraphIdx, arr);

            // Log once if array is empty
            if (arr.GetArraySize() == 0)
            {
                if (idx == 0 && inputName != nullptr)
                {
                    SCString msg;
                    msg.Format("Input[%d] '%s': StudyID=%d, SG=%d -> EMPTY ARRAY",
                        inputIdx, inputName, studyId, subgraphIdx);
                    st->logManager.LogOnce(ThrottleKey::INPUT_DIAG, idx, msg.GetChars(), LogCategory::INPUT);
                }
                return 0.0;
            }

            const double value = SafeGetAt(arr, idx);
            return value;
        };
#endif

    // --- Structure: VbP ---
#if USE_MANUAL_LOOP
    snap.structure.vpbPOC = GetCachedValue(arrays.vpbPOC);
    snap.structure.vpbVAH = GetCachedValue(arrays.vpbVAH);
    snap.structure.vpbVAL = GetCachedValue(arrays.vpbVAL);
#else
    snap.structure.vpbPOC = GetStudyValue(22, "VPB:POC");
    snap.structure.vpbVAH = GetStudyValue(23, "VPB:VAH");
    snap.structure.vpbVAL = GetStudyValue(24, "VPB:VAL");
#endif
    // NOTE: VBP Peaks/Valleys are read via sc.GetStudyPeakValleyLine() in PopulateVbPPeaksValleys()

    // --- Structure: TPO ---
#if USE_MANUAL_LOOP
    snap.structure.tpoPOC = GetCachedValue(arrays.tpoPOC);
    snap.structure.tpoVAH = GetCachedValue(arrays.tpoVAH);
    snap.structure.tpoVAL = GetCachedValue(arrays.tpoVAL);
#else
    snap.structure.tpoPOC = GetStudyValue(30, "TPO:POC");
    snap.structure.tpoVAH = GetStudyValue(31, "TPO:VAH");
    snap.structure.tpoVAL = GetStudyValue(32, "TPO:VAL");
#endif

    // === UPDATE DECOUPLED DISPLAY LEVELS (EARLY PASS) ===
    // Try TPO as early fallback - VBP profile is the primary source.
    // This ensures display is valid early if VBP profile hasn't loaded yet.
    if (!st->displayLevelsValid) {
        if (IsValidPrice(snap.structure.tpoPOC) && snap.structure.tpoPOC > 0.0)
            st->displayPOC = snap.structure.tpoPOC;
        if (IsValidPrice(snap.structure.tpoVAH) && snap.structure.tpoVAH > 0.0)
            st->displayVAH = snap.structure.tpoVAH;
        if (IsValidPrice(snap.structure.tpoVAL) && snap.structure.tpoVAL > 0.0)
            st->displayVAL = snap.structure.tpoVAL;

        // Mark display as valid once we have all three
        if (st->displayPOC > 0.0 && st->displayVAH > 0.0 && st->displayVAL > 0.0)
            st->displayLevelsValid = true;
    }

    // --- Structure: Daily ---
#if USE_MANUAL_LOOP
    snap.structure.dailyHigh = GetCachedValue(arrays.dailyHigh);
    snap.structure.dailyLow = GetCachedValue(arrays.dailyLow);
#else
    snap.structure.dailyHigh = GetStudyValue(41, "Daily:High");
    snap.structure.dailyLow = GetStudyValue(42, "Daily:Low");
#endif

    // --- Structure: VWAP + Bands ---
#if USE_MANUAL_LOOP
    snap.structure.vwap = GetCachedValue(arrays.vwap);
    if (st->vwapBandsInputsValid)
    {
        snap.structure.vwapUpper1 = GetCachedValue(arrays.vwapUpper1);
        snap.structure.vwapLower1 = GetCachedValue(arrays.vwapLower1);
        snap.structure.vwapUpper2 = GetCachedValue(arrays.vwapUpper2);
        snap.structure.vwapLower2 = GetCachedValue(arrays.vwapLower2);
    }
#else
    snap.structure.vwap = GetStudyValue(50, "VWAP");
    if (st->vwapBandsInputsValid)
    {
        snap.structure.vwapUpper1 = GetStudyValue(51, "VWAP:Upper1");
        snap.structure.vwapLower1 = GetStudyValue(52, "VWAP:Lower1");
        snap.structure.vwapUpper2 = GetStudyValue(53, "VWAP:Upper2");
        snap.structure.vwapLower2 = GetStudyValue(54, "VWAP:Lower2");
    }
#endif

    // --- Effort (ROBUST POLICY: Native SC arrays as SSOT, minimal cross-study dependencies) ---
    // Bar volume/delta from native SC arrays (no NB dependency for core delta math)
    // NB rate inputs (70-71) still used for per-second rates (optional)
    {
        // SSOT: Bar volume from native SC array
        snap.effort.totalVolume = sc.Volume[idx];

        // SSOT: Bar delta from native SC arrays (AskVolume - BidVolume)
        // Requires sc.MaintainAdditionalChartDataArrays = 1 (set in SetDefaults)
        const double barAskVol = sc.AskVolume[idx];
        const double barBidVol = sc.BidVolume[idx];
        snap.effort.delta = barAskVol - barBidVol;

        // deltaRatio: derived ratio (delta / totalVolume), guarded for divide-by-zero
        // Scale: -1 to +1 (fraction, not percent)
        snap.effort.deltaPct = (snap.effort.totalVolume > 0.0)
            ? snap.effort.delta / snap.effort.totalVolume
            : 0.0;

        // Per-second rates from Numbers Bars (optional, for rate baselines)
#if USE_MANUAL_LOOP
        snap.effort.bidVolSec = GetCachedValue(arrays.nbBidVolSec);
        snap.effort.askVolSec = GetCachedValue(arrays.nbAskVolSec);
        snap.effort.maxDelta = GetCachedValue(arrays.nbMaxDelta);
        snap.effort.cumDelta = GetCachedValue(arrays.nbCumDelta);
#else
        snap.effort.bidVolSec = GetStudyValue(70, "NB:BidVolSec");
        snap.effort.askVolSec = GetStudyValue(71, "NB:AskVolSec");
        snap.effort.maxDelta = GetStudyValue(74, "NB:MaxDelta");
        snap.effort.cumDelta = GetStudyValue(75, "NB:CumDelta");
#endif

        // DEBUG validation: Check if Ask/Bid arrays are valid (sampled, low overhead)
        // If sc.MaintainAdditionalChartDataArrays was not set, arrays may be all zeros
        const int validationInterval = 100;  // Check every N bars
        const int localDiagLevel = sc.Input[80].GetInt();  // Read diagLevel from Input
        if (localDiagLevel >= 2 && idx > 10 && (idx - st->diagLastValidationBar) >= validationInterval)
        {
            st->diagLastValidationBar = idx;
            const double sumCheck = barAskVol + barBidVol;
            const double volDiff = snap.effort.totalVolume - sumCheck;
            const double volDiffPct = (snap.effort.totalVolume > 0.0)
                ? (volDiff / snap.effort.totalVolume * 100.0) : 0.0;

            SCString dbgMsg;
            dbgMsg.Format("Bar %d: Vol=%.0f Ask=%.0f Bid=%.0f Delta=%.0f Sum=%.0f Diff=%.0f (%.1f%%)",
                idx, snap.effort.totalVolume, barAskVol, barBidVol, snap.effort.delta,
                sumCheck, volDiff, volDiffPct);
            st->logManager.LogDebug(idx, dbgMsg.GetChars(), LogCategory::DELTA);

            // Warn if Ask+Bid are both zero but Volume > 0 (arrays not enabled)
            if (snap.effort.totalVolume > 100.0 && barAskVol < 1.0 && barBidVol < 1.0)
            {
                dbgMsg.Format("Bar %d: Ask/Bid arrays appear empty but Vol=%.0f - check sc.MaintainAdditionalChartDataArrays",
                    idx, snap.effort.totalVolume);
                st->logManager.LogWarn(idx, dbgMsg.GetChars(), LogCategory::DELTA);
            }
        }
    }

    // --- Calculated: Trades/sec ---
    // Use Sierra Chart's NumberOfTrades and SecondsPerBar
    {
        const double numTrades = sc.NumberOfTrades[idx];
        const double secPerBar = sc.SecondsPerBar;
        if (secPerBar > 0)
            snap.effort.tradesSec = numTrades / secPerBar;
        else
            snap.effort.tradesSec = numTrades;  // Fallback for tick charts
    }

    // --- Calculated: Delta/sec ---
    // Normalize bar delta to per-second rate
    // Sign convention: positive = aggressive buying, negative = aggressive selling
    {
        const double secPerBar = sc.SecondsPerBar;
        if (secPerBar > 0)
            snap.effort.deltaSec = snap.effort.delta / secPerBar;
        else
            snap.effort.deltaSec = snap.effort.delta;  // Fallback for tick charts
    }

    // --- Calculated: Ratio Avg (Bid/Ask Volume Ratio) ---
    // Calculate from bid/ask volume per second we already have
    {
        const double bidVol = snap.effort.bidVolSec;
        const double askVol = snap.effort.askVolSec;
        if (askVol > 1e-9)
            snap.effort.ratioAvg = bidVol / askVol;
        else if (bidVol > 1e-9)
            snap.effort.ratioAvg = 99.0;  // Heavy bid dominance
        else
            snap.effort.ratioAvg = 1.0;   // Neutral default
    }

    // --- Liquidity: DOM ---
    // NOTE: DOM data only exists on LIVE bars - historical bars always have 0.
    if (st->domInputsValid)
    {
#if USE_MANUAL_LOOP
        snap.liquidity.domBidSize = GetCachedValue(arrays.domBidSize);
        snap.liquidity.domAskSize = GetCachedValue(arrays.domAskSize);
        // Legacy study-based stack/pull (fallback)
        snap.liquidity.bidStackPull = GetCachedValue(arrays.domBidStack);
        snap.liquidity.askStackPull = GetCachedValue(arrays.domAskStack);
#else
        snap.liquidity.domBidSize = GetStudyValue(62, "DOM:BidSize");
        snap.liquidity.domAskSize = GetStudyValue(63, "DOM:AskSize");
        // Legacy study-based stack/pull (fallback)
        snap.liquidity.bidStackPull = GetStudyValue(64, "DOM:BidStack");
        snap.liquidity.askStackPull = GetStudyValue(65, "DOM:AskStack");
#endif
    }

    // --- Direct Stack/Pull API (preferred - no study dependency) ---
    // sc.GetBidMarketDepthStackPullSum() returns aggregated pulling/stacking totals
    // Requires sc.SetUseMarketDepthPullingStackingData(1) in SetDefaults
    // On live bars, this provides real-time stack/pull; historical bars may be 0
    {
        const double directBidStackPull = sc.GetBidMarketDepthStackPullSum();
        const double directAskStackPull = sc.GetAskMarketDepthStackPullSum();

        // If direct API returns valid data, prefer it over study-based values
        // Note: Direct API only works on live bars with DOM displayed
        if (directBidStackPull != 0.0 || directAskStackPull != 0.0) {
            snap.liquidity.bidStackPull = directBidStackPull;
            snap.liquidity.askStackPull = directAskStackPull;
        }
    }

    // --- Diagonal Delta (Footprint Imbalance from Numbers Bars SG43/44) ---
    // Diagonal delta compares bid volume at price N vs ask volume at price N+1
    // Positive net = buyers aggressively lifting offers (bullish imbalance)
    // Negative net = sellers aggressively hitting bids (bearish imbalance)
    {
#if USE_MANUAL_LOOP
        const double diagPosSum = GetCachedValue(arrays.diagPosDelta);
        const double diagNegSum = GetCachedValue(arrays.diagNegDelta);
#else
        const double diagPosSum = GetStudyValue(76, "NB:DiagPosDelta");
        const double diagNegSum = GetStudyValue(77, "NB:DiagNegDelta");
#endif
        if (diagPosSum > 0.0 || diagNegSum > 0.0) {
            snap.effort.diagonalPosDeltaSum = diagPosSum;
            snap.effort.diagonalNegDeltaSum = diagNegSum;
            snap.effort.diagonalNetDelta = diagPosSum - diagNegSum;
            snap.effort.diagonalDeltaValid = true;
        }
    }

    // --- Average Trade Size (from Numbers Bars SG51/52) ---
    // Large avg trade size = institutional activity
    // Small avg trade size = retail/HFT activity
    // Ratio (ask/bid) > 1 means larger trades lifting offers
    {
#if USE_MANUAL_LOOP
        const double avgBidTrade = GetCachedValue(arrays.avgBidTradeSize);
        const double avgAskTrade = GetCachedValue(arrays.avgAskTradeSize);
#else
        const double avgBidTrade = GetStudyValue(78, "NB:AvgBidTrade");
        const double avgAskTrade = GetStudyValue(79, "NB:AvgAskTrade");
#endif
        if (avgBidTrade > 0.0 || avgAskTrade > 0.0) {
            snap.effort.avgBidTradeSize = avgBidTrade;
            snap.effort.avgAskTradeSize = avgAskTrade;
            // Ratio: >1 means larger ask trades (institutional buying)
            if (avgBidTrade > 1e-9) {
                snap.effort.avgTradeSizeRatio = avgAskTrade / avgBidTrade;
            } else if (avgAskTrade > 1e-9) {
                snap.effort.avgTradeSizeRatio = 10.0;  // Heavy ask dominance
            } else {
                snap.effort.avgTradeSizeRatio = 1.0;   // Neutral
            }
            snap.effort.avgTradeSizeValid = true;
        }
    }

    // --- Liquidity: Depth OHLC ---
    if (st->depthOhlcInputsValid)
    {
#if USE_MANUAL_LOOP
        snap.liquidity.depthOpen = GetCachedValue(arrays.dailyOpen);
        snap.liquidity.depthHigh = GetCachedValue(arrays.dailyHigh);
        snap.liquidity.depthLow = GetCachedValue(arrays.dailyLow);
        snap.liquidity.depthClose = GetCachedValue(arrays.dailyClose);
#else
        snap.liquidity.depthOpen = GetStudyValue(40, "Daily:Open");
        snap.liquidity.depthHigh = GetStudyValue(41, "Daily:High");
        snap.liquidity.depthLow = GetStudyValue(42, "Daily:Low");
        snap.liquidity.depthClose = GetStudyValue(43, "Daily:Close");
#endif
    }

    // --- Liquidity: Best Bid/Ask ---
#if USE_MANUAL_LOOP
    snap.liquidity.bestBid = GetCachedValue(arrays.bestBid);
    snap.liquidity.bestAsk = GetCachedValue(arrays.bestAsk);
#else
    snap.liquidity.bestBid = GetStudyValue(60, "BestBid");
    snap.liquidity.bestAsk = GetStudyValue(61, "BestAsk");
#endif

    // --- Warm-up status ---
    snap.isWarmUp = !st->drift.isWarmedUp();
}

// ============================================================================
// ITEM 1.2 & 2.1: SESSION ROUTING AND BASELINE UPDATE
// ============================================================================

static void UpdateSessionBaselines(
    SCStudyInterfaceRef sc,
    StudyState* st,
    const ObservableSnapshot& snap,
    int rthStartSec,
    int rthEndSec,
    int gbxStartSec,
    int diagLevel,
    int barIdx = -1,  // Bar index for AutoLoop=0 compatibility (-1 = use sc.Index)
    AMT::SessionType targetBaselineType = AMT::SessionType::GLOBEX)  // Which baseline domain to write to
{
    // AutoLoop=0 compatibility: use explicit barIdx if provided, else fallback to sc.Index
    const int currentBar = (barIdx >= 0) ? barIdx : sc.Index;

    const double tickSize = sc.TickSize;

    // Session phase is already set by VbP SSOT (or fallback) before this function
    // SessionPhaseCoordinator is now SSOT - we only detect transitions here.
    const AMT::SessionPhase currentPhase = st->phaseCoordinator.GetPhase();
    const AMT::SessionPhase prevPhase = st->phaseCoordinator.GetPrevPhase();

    // SSOT: Use SessionManager for session transition detection
    // PeekSessionChanged() is read-only (doesn't consume the flag)
    // The main ConsumeSessionChange() call happens later in the session reset block
    const bool sessionTransition = st->sessionMgr.PeekSessionChanged();

    // Gate session transition log to live bar only (avoid spam during chart load)
    const bool isLiveBarForSession = (currentBar == sc.ArraySize - 1);

    if (sessionTransition)
    {
        if (diagLevel >= 1 && isLiveBarForSession)
        {
            SCString msg;
            msg.Format("Transition: %s -> %s",
                AMT::SessionPhaseToString(prevPhase),
                AMT::SessionPhaseToString(currentPhase));
            st->logManager.LogInfo(currentBar, msg.GetChars(), LogCategory::SESSION);

#ifdef VALIDATE_ZONE_MIGRATION
            // PHASE 3: End-of-session validation summary
            // Count unmatched episodes before summary
            st->validationState.CountUnmatched();
            const auto& vc = st->validationState.counters;

            SCString sumMsg;
            sumMsg.Format("[VAL-SUMMARY] legacyFin=%d amtFin=%d matched=%d "
                          "mismatches=%d missingLeg=%d missingAmt=%d widthMismatch=%d",
                vc.legacyFinalizedCount, vc.amtFinalizedCount, vc.matchedCount,
                vc.mismatchCount, vc.missingLegacyCount, vc.missingAmtCount,
                vc.widthMismatchCount);
            st->logManager.LogInfo(currentBar, sumMsg.GetChars(), LogCategory::SYSTEM);

            // Detail breakdown if there were mismatches
            if (vc.mismatchCount > 0) {
                SCString detailMsg;
                detailMsg.Format("[VAL-DETAIL] entryBar=%d exitBar=%d barsEngaged=%d escVel=%d coreWidth=%d haloWidth=%d",
                    vc.entryBarDiffCount, vc.exitBarDiffCount, vc.barsEngagedDiffCount,
                    vc.escVelDiffCount, vc.widthCoreDiffCount, vc.widthHaloDiffCount);
                st->logManager.LogInfo(currentBar, detailMsg.GetChars(), LogCategory::SYSTEM);
            }

            // Start new session for validation tracking
            st->validationState.StartSession(currentBar);
#endif
        }
    }

    // Phase tracking is now handled by SessionPhaseCoordinator.UpdatePhase() (SSOT)

    // Get session context by TARGET TYPE (for baseline routing)
    // During Phase 1: targetBaselineType = bar's session type (RTH or GBX)
    // During Phase 2: targetBaselineType = current session type
    SessionContext& ctx = st->sessionMgr.getContextByType(targetBaselineType);

    // Debug-only: check for anomalies that indicate bugs (no gating, just logs)
    const bool isLiveBarForDrift = (currentBar == sc.ArraySize - 1);
    st->drift.checkForAnomalies(snap.structure.vpbPOC, st->drift.prevVpbPOC, tickSize, "VPB_POC", sc, diagLevel, isLiveBarForDrift);
    st->drift.checkForAnomalies(snap.structure.tpoPOC, st->drift.prevTpoPOC, tickSize, "TPO_POC", sc, diagLevel, isLiveBarForDrift);
    st->drift.checkForAnomalies(snap.structure.vwap, st->drift.prevVwap, tickSize, "VWAP", sc, diagLevel, isLiveBarForDrift);

    // ITEM 1.2: Check DOM validity
    // Store result for potential re-application after session reset
    if (st->domInputsValid)
    {
        const bool domJustBecameStale = st->drift.checkDomValidity(
            snap.liquidity.domBidSize,
            snap.liquidity.domAskSize,
            snap.liquidity.bidStackPull,
            snap.liquidity.askStackPull,
            sc,
            diagLevel,
            isLiveBarForDrift
        );
        if (domJustBecameStale)
        {
            st->sessionAccum.domStaleCount++;
            st->sessionAccum.firstBarDomStale = true;  // Flag for re-application after session reset
        }
    }

    // ITEM 1.2: Increment warm-up counter
    st->drift.incrementBars();

    // ITEM 2.2: Session accumulation for delta tracking
    // NOTE: Legacy BaselineEngine pushes removed (Dec 2024)
    // Effort baselines now populated from historical data via PopulateEffortBaselines()
    {
        // ================================================================
            // IDEMPOTENT ACCUMULATION (Differential Update Pattern + Rewind Detection)
            // ================================================================
            // PROBLEM: With sc.AutoLoop=1, sc.UpdateAlways=1, the study is called
            // on every tick. The live bar (sc.ArraySize-1) has PARTIAL values that
            // change until bar close. We must only accumulate FINALIZED bar values.
            //
            // HAZARD 1 - PARTIAL VALUES: Live bar has changing values; only count at close.
            // HAZARD 2 - REWIND: Sierra can recalculate from an earlier bar, requiring rebuild.
            //
            // SOLUTION:
            // - Track sessionStartBarIndex for rebuild anchor
            // - Detect rewind: if currentBar <= lastAccumulatedBarIndex, rebuild from sessionStart
            // - For live bar: accumulate PREVIOUS bar(s) that closed, not current partial bar
            //
            // INVARIANT: sessionCumDelta = sum of barDelta for bars [sessionStartBarIndex..lastAccumulatedBarIndex]
            // ================================================================
            const bool isLiveBarLocal = (currentBar == sc.ArraySize - 1);
            const int sessionStart = st->sessionAccum.sessionStartBarIndex;
            bool needsRebuild = false;

            // REWIND DETECTION: Did Sierra recalculate from an earlier bar?
            // This happens on: full recalc, data reload, replay, study order change
            if (st->sessionAccum.lastAccumulatedBarIndex >= 0 &&
                currentBar <= st->sessionAccum.lastAccumulatedBarIndex &&
                sessionStart >= 0)
            {
                needsRebuild = true;
                if (diagLevel >= 1)
                {
                    SCString rewindMsg;
                    rewindMsg.Format("Detected at bar %d | lastAccum=%d sessionStart=%d | REBUILDING",
                        currentBar, st->sessionAccum.lastAccumulatedBarIndex, sessionStart);
                    st->logManager.LogInfo(currentBar, rewindMsg.GetChars(), LogCategory::ACCUM);
                }
            }

            // REBUILD: Clear accumulators and re-sum from session start
            if (needsRebuild)
            {
                st->sessionAccum.sessionTotalVolume = 0.0;
                st->sessionAccum.sessionCumDelta = 0.0;
                st->sessionAccum.firstBarVolume = 0.0;
                st->sessionAccum.firstBarDelta = 0.0;
                st->sessionAccum.lastAccumulatedBarIndex = sessionStart - 1;  // Will re-accumulate from sessionStart

                // Determine rebuild end: for live bar, stop at currentBar-1; for historical, stop at currentBar
                const int rebuildEnd = isLiveBarLocal ? (currentBar - 1) : currentBar;

                for (int i = sessionStart; i <= rebuildEnd; ++i)
                {
                    const double barVol = sc.Volume[i];
                    const double barDelta = sc.AskVolume[i] - sc.BidVolume[i];

                    if (st->sessionAccum.sessionTotalVolume == 0.0)
                    {
                        st->sessionAccum.firstBarVolume = barVol;
                        st->sessionAccum.firstBarDelta = barDelta;
                    }
                    st->sessionAccum.sessionTotalVolume += barVol;
                    st->sessionAccum.sessionCumDelta += barDelta;
                }
                st->sessionAccum.lastAccumulatedBarIndex = rebuildEnd;

                if (diagLevel >= 1)
                {
                    SCString rebuildMsg;
                    rebuildMsg.Format("Bars %d-%d | sessionVol=%.0f sessionDelta=%.0f",
                        sessionStart, rebuildEnd,
                        st->sessionAccum.sessionTotalVolume, st->sessionAccum.sessionCumDelta);
                    st->logManager.LogInfo(currentBar, rebuildMsg.GetChars(), LogCategory::ACCUM);
                }
            }
            else if (!isLiveBarLocal)
            {
                // HISTORICAL BAR (during initial recalc): finalized, accumulate if not already done
                if (currentBar > st->sessionAccum.lastAccumulatedBarIndex)
                {
                    // Capture first bar's values for diagnostics
                    if (st->sessionAccum.sessionTotalVolume == 0.0)
                    {
                        st->sessionAccum.firstBarVolume = snap.effort.totalVolume;
                        st->sessionAccum.firstBarDelta = snap.effort.delta;
                    }
                    st->sessionAccum.sessionTotalVolume += snap.effort.totalVolume;
                    st->sessionAccum.sessionCumDelta += snap.effort.delta;
                    st->sessionAccum.lastAccumulatedBarIndex = currentBar;
                }
            }
            else
            {
                // LIVE BAR: accumulate any PREVIOUS bars that closed since last accumulation
                // The previous bar (currentBar-1) is now finalized when a new bar starts
                const int lastFinalized = currentBar - 1;
                if (lastFinalized >= 0 && lastFinalized > st->sessionAccum.lastAccumulatedBarIndex)
                {
                    // Accumulate bars from lastAccumulatedBarIndex+1 to lastFinalized
                    for (int i = st->sessionAccum.lastAccumulatedBarIndex + 1; i <= lastFinalized; ++i)
                    {
                        const double barVol = sc.Volume[i];
                        const double barDelta = sc.AskVolume[i] - sc.BidVolume[i];

                        // Capture first bar's values for diagnostics
                        if (st->sessionAccum.sessionTotalVolume == 0.0)
                        {
                            st->sessionAccum.firstBarVolume = barVol;
                            st->sessionAccum.firstBarDelta = barDelta;
                        }
                        st->sessionAccum.sessionTotalVolume += barVol;
                        st->sessionAccum.sessionCumDelta += barDelta;
                    }
                    st->sessionAccum.lastAccumulatedBarIndex = lastFinalized;

                    // DEBUG: Log differential accumulation
                    // - Always log first bar of session (proves idempotency at boundaries)
                    // - Sample every 50 bars at higher diag level
                    const bool isFirstBarOfSession = (st->sessionAccum.firstBarVolume > 0.0 &&
                        st->sessionAccum.sessionTotalVolume <= st->sessionAccum.firstBarVolume * 1.01);
                    if (diagLevel >= 1 && isFirstBarOfSession)
                    {
                        SCString dbgMsg;
                        dbgMsg.Format("Session first bar %d FINALIZED | "
                            "Vol=%.0f Delta=%.0f | sessionVol=%.0f sessionDelta=%.0f",
                            st->sessionAccum.lastAccumulatedBarIndex,
                            st->sessionAccum.firstBarVolume, st->sessionAccum.firstBarDelta,
                            st->sessionAccum.sessionTotalVolume, st->sessionAccum.sessionCumDelta);
                        st->logManager.LogInfo(currentBar, dbgMsg.GetChars(), LogCategory::ACCUM);
                    }
                    else if (diagLevel >= 2 && (lastFinalized % 50 == 0))
                    {
                        SCString dbgMsg;
                        dbgMsg.Format("Bar %d finalized | sessionCumDelta=%.0f sessionVol=%.0f",
                            lastFinalized, st->sessionAccum.sessionCumDelta, st->sessionAccum.sessionTotalVolume);
                        st->logManager.LogDebug(currentBar, dbgMsg.GetChars(), LogCategory::ACCUM);
                    }
                }
                // NOTE: Current live bar (currentBar) is NOT accumulated - it's still forming
            }

        // NOTE: Legacy trades_sec, bar_range, delta_sec baseline pushes removed (Dec 2024)
        // Effort baselines now populated from historical data via PopulateEffortBaselines()
        st->facilSessionSamples++;  // Still track per-session sample count

        // Session-scoped delta ratio (SSOT: internal accumulation)
        // sessionCumDelta = accumulated bar deltas (reset at session boundary)
        // sessionDeltaRatio = sessionCumDelta / sessionTotalVolume (normalized to -1..+1)
        {
            const double sessionCumDeltaClosed = st->sessionAccum.sessionCumDelta;  // SSOT (finalized bars - for live bars excludes forming bar)
            const double sessionVol = st->sessionAccum.sessionTotalVolume;

            // LIVE PARITY: For NB cross-check, compute sessionCumDeltaLive
            // - For LIVE bars: closed + currentBar (current bar not yet accumulated)
            // - For HISTORICAL bars: closed already includes current bar (just accumulated above)
            const bool isLiveBar = (currentBar == sc.ArraySize - 1);
            const double currentBarDelta = snap.effort.delta;  // Current forming bar's delta
            const double sessionCumDeltaLive = isLiveBar
                ? (sessionCumDeltaClosed + currentBarDelta)  // Live: add forming bar
                : sessionCumDeltaClosed;                      // Historical: already included

            // Avoid div-by-zero: use epsilon floor for volume
            const double sessionDeltaRatio = sessionCumDeltaClosed / (std::max)(sessionVol, 1.0);
            // NOTE: Legacy baselines.session_delta_pct push removed (Dec 2024)
            // SessionDeltaBaseline is populated from historical data via PopulateEffortBaselines()

            // ================================================================
            // VERIFICATION EVIDENCE: Intrabar Stability + NB Cross-Check
            // ================================================================
            // INTRABAR STABILITY: For live bars, sessionCumDeltaClosed should NOT change
            // between ticks within the same bar. It only changes when a bar closes.
            //
            // NB PARITY: sessionCumDeltaLive should match NB's cumDelta (within rounding).
            // - For LIVE bars: "closed" lags by currentBarDelta, "live" adds it back
            // - For HISTORICAL bars: "closed" already includes current bar, "live" equals "closed"
            // ================================================================
            if (diagLevel >= 2 && st->sessionAccum.cumDeltaAtSessionStartValid)
            {
                const double nbSessionCumDelta = snap.effort.cumDelta - st->sessionAccum.cumDeltaAtSessionStart;

                // Cross-check LIVE version (should match NB exactly, within rounding)
                const double liveDiff = std::abs(sessionCumDeltaLive - nbSessionCumDelta);

                // Cross-check CLOSED version (expected divergence: currentBarDelta for live, 0 for historical)
                const double closedDiff = std::abs(sessionCumDeltaClosed - nbSessionCumDelta);
                const double expectedDiff = isLiveBar ? std::abs(currentBarDelta) : 0.0;

                // Log if LIVE diverges significantly (indicates bug in accumulation)
                if (liveDiff > 10.0)  // Allow small rounding tolerance
                {
                    SCString dbgMsg;
                    dbgMsg.Format("LIVE MISMATCH | Internal+CurBar=%.0f NB=%.0f Diff=%.0f | "
                        "ClosedDelta=%.0f CurBarDelta=%.0f",
                        sessionCumDeltaLive, nbSessionCumDelta, liveDiff,
                        sessionCumDeltaClosed, currentBarDelta);
                    st->logManager.LogWarn(currentBar, dbgMsg.GetChars(), LogCategory::DELTA);
                }

                // Log closed vs NB at sampled intervals (for live bars: expected divergence = currentBarDelta)
                if ((currentBar % 100 == 0) && diagLevel >= 3)
                {
                    SCString dbgMsg;
                    dbgMsg.Format("Bar %d | Closed=%.0f Live=%.0f NB=%.0f | "
                        "ExpectedDiff=%.0f ActualDiff=%.0f",
                        currentBar, sessionCumDeltaClosed, sessionCumDeltaLive, nbSessionCumDelta,
                        expectedDiff, closedDiff);
                    st->logManager.LogDebug(currentBar, dbgMsg.GetChars(), LogCategory::DELTA);
                }
            }
        }

        // DEPRECATED: Keep NB cumDelta latch for backward compatibility
        st->sessionAccum.lastSeenCumDelta = snap.effort.cumDelta;

        // ITEM 2.2: DOM baseline (stack/pull/depth) - continuously accumulates on live bars
        // DOM data only exists on live bars (historical bars have no DOM snapshots)
        if (st->domInputsValid)
        {
            const double netStack = snap.liquidity.bidStackPull + snap.liquidity.askStackPull;
            const double netDepth = snap.liquidity.domBidSize + snap.liquidity.domAskSize;
            const double netPull = -(std::min)(snap.liquidity.bidStackPull, 0.0)
                - (std::min)(snap.liquidity.askStackPull, 0.0);

            // DOM baseline: Continuously accumulate on ALL bars (phase-bucketed)
            // Numbers Bars data is available historically, so no isLiveBar gate needed
            // Always push - even zeros are valid data points
            // Core metrics from study inputs (available on historical bars via Numbers Bars)
            st->domWarmup.Push(currentPhase, netStack, netPull, netDepth);

            // NOTE: Halo and Spread are now pushed from historical depth data
            // in the liquidity computation block (uses c_ACSILDepthBars for temporal coherence)

            // Live bar only: Update snapshot with current halo for display
            const bool isLiveBar = (currentBar == sc.ArraySize - 1);
            if (isLiveBar) {
                const int haloRadius = sc.Input[15].GetInt();
                const int maxLevels = sc.Input[14].GetInt();
                const double tickSize = sc.TickSize;

                DepthMassHaloResult halo = ComputeDepthMassHalo(
                    sc,
                    snap.liquidity.bestBid,
                    snap.liquidity.bestAsk,
                    haloRadius,
                    maxLevels,
                    tickSize
                );

                if (halo.valid) {
                    st->currentSnapshot.liquidity.haloMass = halo.totalMass;
                    st->currentSnapshot.liquidity.haloBidMass = halo.bidMass;
                    st->currentSnapshot.liquidity.haloAskMass = halo.askMass;
                    st->currentSnapshot.liquidity.haloImbalance = halo.imbalance;
                    st->currentSnapshot.liquidity.haloValid = true;
                }
            }
        }
    }

    // NOTE: Session high/low are now owned by StructureTracker (SSOT), updated via
    // structure.UpdateExtremes(sc.High, sc.Low) in the main bar loop.
    // The old Close-based ctx.sessionHigh/Low writes were semantically wrong
    // (used Close instead of bar High/Low) and have been removed.

    // NOTE: POC/VAH/VAL are now owned by SessionManager (SSOT).
    // The old ctx.sessionVPOC/VAH/VAL writes were dead code (never read).
    // Use st->sessionMgr.UpdateLevels() to update levels from VbP study.
}

// ============================================================================
// PROFILE BASELINE POPULATION (Progress-Conditioned)
// ============================================================================
// Populates HistoricalProfileBaseline for VA width and POC dominance at each
// progress bucket. Called once during PHASE 0 BOOTSTRAP after DiscoverSessions().
// ============================================================================

static void PopulateProfileBaselines(
    SCStudyInterfaceRef sc,
    StudyState* st,
    int rthStartSec,
    int /*rthEndSec*/,
    int diagLevel)
{
    if (st->baselineSessionMgr.completedSessions.empty()) {
        if (diagLevel >= 1) {
            sc.AddMessageToLog("[PROFILE-BASELINE] No completed sessions - skipping", 0);
        }
        return;
    }

    // Get VbP study arrays for POC/VAH/VAL
    SCFloatArray pocArray, vahArray, valArray;
    const int vbpPocStudyId = sc.Input[22].GetStudyID();
    const int vbpPocSG = sc.Input[22].GetSubgraphIndex();
    const int vbpVahStudyId = sc.Input[23].GetStudyID();
    const int vbpVahSG = sc.Input[23].GetSubgraphIndex();
    const int vbpValStudyId = sc.Input[24].GetStudyID();
    const int vbpValSG = sc.Input[24].GetSubgraphIndex();

    if (vbpPocStudyId > 0) sc.GetStudyArrayUsingID(vbpPocStudyId, vbpPocSG, pocArray);
    if (vbpVahStudyId > 0) sc.GetStudyArrayUsingID(vbpVahStudyId, vbpVahSG, vahArray);
    if (vbpValStudyId > 0) sc.GetStudyArrayUsingID(vbpValStudyId, vbpValSG, valArray);

    const int pocSize = pocArray.GetArraySize();
    const int vahSize = vahArray.GetArraySize();
    const int valSize = valArray.GetArraySize();

    if (pocSize == 0 || vahSize == 0 || valSize == 0) {
        if (diagLevel >= 1) {
            sc.AddMessageToLog("[PROFILE-BASELINE] VbP arrays not available", 0);
        }
        return;
    }

    const double tickSize = sc.TickSize;
    if (tickSize <= 0.0) return;

    // Bucket minute thresholds
    const int bucketMinutes[] = {15, 30, 60, 120, 9999};  // EOD = session end

    int rthSnapshots = 0, gbxSnapshots = 0;
    int contaminatedSessions = 0;  // Sessions where all buckets have identical VA width (EOD contamination)

    // Process each completed session
    int partialSessionsSkipped = 0;
    for (const auto& session : st->baselineSessionMgr.completedSessions) {
        if (!session.IsValid() || session.firstBarIndex < 0) continue;

        // DOMAIN PURITY: Skip partial sessions (need at least 60 bars for meaningful profile)
        // This excludes holiday half-days, data gaps, and sessions cut short by data issues
        constexpr int MIN_SESSION_BARS = 60;  // ~1 hour at 1-min bars
        if (session.barCount < MIN_SESSION_BARS) {
            partialSessionsSkipped++;
            continue;
        }

        const bool isRTH = (session.key.sessionType == AMT::SessionType::RTH);
        AMT::HistoricalProfileBaseline& baseline = isRTH
            ? st->rthProfileBaseline
            : st->gbxProfileBaseline;

        // Track session high/low as we progress through bars
        double sessionHigh = 0.0;
        double sessionLow = 1e12;  // Large initial value

        // AS-OF EXTRACTION VALIDATION: Track VA widths per bucket to detect contamination
        // If all buckets show identical VA width, the VbP study is NOT providing as-of values
        double bucketVAWidths[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
        int validBucketCount = 0;

        // For each bucket, find the bar index and extract features
        for (int bucketIdx = 0; bucketIdx < static_cast<int>(AMT::ProgressBucket::BUCKET_COUNT); ++bucketIdx) {
            const AMT::ProgressBucket bucket = static_cast<AMT::ProgressBucket>(bucketIdx);
            const int targetMinutes = bucketMinutes[bucketIdx];

            // Calculate target bar index for this bucket
            int targetBarIdx;
            if (targetMinutes >= 9999) {
                // EOD - use last bar of session
                targetBarIdx = session.lastBarIndex;
            } else {
                // Calculate bar index based on minutes (assuming 1-min bars)
                // For other timeframes, this would need adjustment
                targetBarIdx = session.firstBarIndex + targetMinutes;
                // Clamp to session bounds
                if (targetBarIdx > session.lastBarIndex) {
                    targetBarIdx = session.lastBarIndex;
                }
            }

            if (targetBarIdx < 0 || targetBarIdx >= pocSize) continue;

            // Update session high/low up to this bar
            for (int bar = session.firstBarIndex; bar <= targetBarIdx && bar < sc.ArraySize; ++bar) {
                if (sc.High[bar] > sessionHigh) sessionHigh = sc.High[bar];
                if (sc.Low[bar] < sessionLow && sc.Low[bar] > 0.0) sessionLow = sc.Low[bar];
            }

            // Read POC/VAH/VAL at this bar
            const double poc = (targetBarIdx < pocSize) ? pocArray[targetBarIdx] : 0.0;
            const double vah = (targetBarIdx < vahSize) ? vahArray[targetBarIdx] : 0.0;
            const double val = (targetBarIdx < valSize) ? valArray[targetBarIdx] : 0.0;

            // Skip if data is invalid
            if (poc <= 0.0 || vah <= 0.0 || val <= 0.0 || vah <= val) continue;

            // Build ProfileFeatureSnapshot
            AMT::ProfileFeatureSnapshot snap;
            snap.bucket = bucket;
            snap.minutesIntoSession = targetMinutes;

            // VA width in ticks
            snap.vaWidthTicks = (vah - val) / tickSize;

            // Session range in ticks
            snap.sessionRangeTicks = (sessionHigh > sessionLow)
                ? (sessionHigh - sessionLow) / tickSize
                : 0.0;

            // Compute derived (vaWidthRatio)
            snap.ComputeDerived();

            // POC share - we don't have direct volume-at-POC data from VbP arrays
            // This would require accessing the full VAP array at each bucket boundary
            // Mark as invalid per NO-FALLBACK POLICY - don't synthesize
            snap.pocShare = 0.0;
            snap.pocShareValid = false;  // Explicitly mark as unavailable
            snap.pocVolume = 0.0;
            snap.totalVolume = 0.0;

            // Mark valid if we have VA width
            snap.valid = (snap.vaWidthTicks > 0.0);

            if (snap.valid) {
                // Track VA width for contamination detection
                bucketVAWidths[bucketIdx] = snap.vaWidthTicks;
                validBucketCount++;
            }
        }

        // AS-OF EXTRACTION VALIDATION: Check for contamination
        // If all valid buckets have identical VA width (within 1 tick), the data is likely EOD-repeated
        bool sessionContaminated = false;
        if (validBucketCount >= 3) {  // Need at least 3 buckets to detect
            bool allIdentical = true;
            double firstValidWidth = 0.0;
            for (int i = 0; i < 5; ++i) {
                if (bucketVAWidths[i] > 0.0) {
                    if (firstValidWidth == 0.0) {
                        firstValidWidth = bucketVAWidths[i];
                    } else if (std::abs(bucketVAWidths[i] - firstValidWidth) > 1.0) {
                        allIdentical = false;
                        break;
                    }
                }
            }
            if (allIdentical && firstValidWidth > 0.0) {
                sessionContaminated = true;
                contaminatedSessions++;
            }
        }

        // Only push snapshots if session is NOT contaminated
        if (!sessionContaminated) {
            for (int bucketIdx = 0; bucketIdx < 5; ++bucketIdx) {
                if (bucketVAWidths[bucketIdx] > 0.0) {
                    // Rebuild snapshot for pushing (reuse tracked width)
                    AMT::ProfileFeatureSnapshot snap;
                    snap.bucket = static_cast<AMT::ProgressBucket>(bucketIdx);
                    snap.vaWidthTicks = bucketVAWidths[bucketIdx];
                    snap.valid = true;
                    baseline.PushSnapshot(snap);
                    if (isRTH) rthSnapshots++;
                    else gbxSnapshots++;
                }
            }
            baseline.sessionsAccumulated++;
        }
    }

    st->baselineSessionMgr.profileBaselinesPopulated = true;

    // CRITICAL WARNING: If contamination rate is high, VbP study is not providing as-of values
    const int totalSessions = static_cast<int>(st->baselineSessionMgr.completedSessions.size());
    const double contaminationRate = (totalSessions > 0)
        ? (100.0 * contaminatedSessions / totalSessions) : 0.0;

    if (contaminatedSessions > 0) {
        char warnBuf[256];
        snprintf(warnBuf, sizeof(warnBuf),
            "[PROFILE-BASELINE] WARNING: %d/%d sessions (%.0f%%) show EOD contamination - "
            "VbP study may not be outputting developing profile values",
            contaminatedSessions, totalSessions, contaminationRate);
        sc.AddMessageToLog(warnBuf, 1);  // Priority 1 = warning

        if (contaminationRate > 50.0) {
            sc.AddMessageToLog(
                "[PROFILE-BASELINE] CRITICAL: >50% contamination - progress baselines unreliable. "
                "Ensure VbP study uses 'Developing POC Line' or similar developing output mode.", 1);
        }
    }

    if (diagLevel >= 1) {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "[PROFILE-BASELINE] Populated RTH=%d GBX=%d snapshots from %d sessions (partial=%d contaminated=%d)",
            rthSnapshots, gbxSnapshots, totalSessions, partialSessionsSkipped, contaminatedSessions);
        sc.AddMessageToLog(buf, 0);

        // Log sample counts per bucket at higher diag level
        if (diagLevel >= 2) {
            for (int b = 0; b < static_cast<int>(AMT::ProgressBucket::BUCKET_COUNT); ++b) {
                const AMT::ProgressBucket bucket = static_cast<AMT::ProgressBucket>(b);
                size_t rthVA, rthPOC, gbxVA, gbxPOC;
                st->rthProfileBaseline.GetSampleCounts(bucket, rthVA, rthPOC);
                st->gbxProfileBaseline.GetSampleCounts(bucket, gbxVA, gbxPOC);
                snprintf(buf, sizeof(buf),
                    "[PROFILE-BASELINE] Bucket %s: RTH(VA=%zu) GBX(VA=%zu)",
                    AMT::ProgressBucketToString(bucket),
                    rthVA, gbxVA);
                sc.AddMessageToLog(buf, 0);
            }
        }
    }
}

// ============================================================================
// EFFORT BASELINE POPULATION (Bar-Sample Distributions per Time Bucket)
// ============================================================================
// ============================================================================
// CONSTRAINT-COMPLIANT EFFORT BASELINE POPULATION (SessionPhase-Based)
// ============================================================================
// Population constraints (Dec 2024 - SessionPhase-Based):
// 1. Each SessionPhase has its own bucket (7 tradeable phases)
// 2. Coverage threshold is proportional to expected bars per phase
// 3. Continue until EACH bucket has 5 contributing sessions OR history exhausted
// 4. All tradeable phases are included (RTH + GLOBEX phases)
// 5. Session-aggregate metrics (sessionDeltaRatio) are SEPARATE from bar-level
// ============================================================================

static void PopulateEffortBaselines(
    SCStudyInterfaceRef sc,
    StudyState* st,
    int rthStartSec,
    int rthEndSec,
    int diagLevel)
{
    if (st->baselineSessionMgr.completedSessions.empty()) {
        if (diagLevel >= 1) {
            sc.AddMessageToLog("[EFFORT-BASELINE] No completed sessions - skipping", 0);
        }
        return;
    }

    const double tickSize = sc.TickSize;
    if (tickSize <= 0.0) return;

    // Calculate bar interval from chart (for expected bars per bucket)
    const int barIntervalSec = (sc.SecondsPerBar > 0) ? static_cast<int>(sc.SecondsPerBar) : 60;

    // Initialize expected bars per bucket based on chart timeframe
    st->effortBaselines.SetExpectedBarsPerSession(barIntervalSec);
    st->domWarmup.SetExpectedBarsPerSession(barIntervalSec);

    // Get historical market depth data
    c_ACSILDepthBars* p_DepthBars = sc.GetMarketDepthBars();
    const bool histDepthAvailable = (p_DepthBars != nullptr);
    int domBarsWithData = 0;
    int domBarsChecked = 0;
    int firstBarWithDepth = -1;
    int lastBarWithDepth = -1;

    // Diagnostic: Check depth data availability and find where data actually exists
    if (histDepthAvailable) {
        // Scan to find where depth data actually exists
        const int numBars = p_DepthBars->NumBars();
        for (int i = 0; i < numBars && firstBarWithDepth < 0; ++i) {
            if (p_DepthBars->DepthDataExistsAt(i)) firstBarWithDepth = i;
        }
        for (int i = numBars - 1; i >= 0 && lastBarWithDepth < 0; --i) {
            if (p_DepthBars->DepthDataExistsAt(i)) lastBarWithDepth = i;
        }
    }
    if (diagLevel >= 1) {
        char diagBuf[256];
        snprintf(diagBuf, sizeof(diagBuf),
            "[DOM-BASELINE] c_ACSILDepthBars ptr=%s NumBars=%d | DepthRange=[%d..%d]",
            histDepthAvailable ? "OK" : "NULL",
            histDepthAvailable ? p_DepthBars->NumBars() : 0,
            firstBarWithDepth, lastBarWithDepth);
        sc.AddMessageToLog(diagBuf, 0);
    }

    // Tracking per bucket (7 phases)
    int sessionsContributedToBucket[AMT::EFFORT_BUCKET_COUNT] = {0};
    int barsThisSession[AMT::EFFORT_BUCKET_COUNT] = {0};
    int domBarsThisSession[AMT::EFFORT_BUCKET_COUNT] = {0};

    // Market state tracking per bucket (for prior population)
    // Accumulated across ALL sessions, then converted to ratio
    int totalBalanceBars[AMT::EFFORT_BUCKET_COUNT] = {0};
    int totalImbalanceBars[AMT::EFFORT_BUCKET_COUNT] = {0};
    int marketStateSessionsContributed[AMT::EFFORT_BUCKET_COUNT] = {0};

    // Synthetic bar aggregation tracking per bucket (for regime baseline pre-warm)
    // Aggregates N raw bars into synthetic bars for stable regime classification
    // Uses TRUE RANGE to capture gaps between synthetic bars (critical for RTH open)
    constexpr int SYNTHETIC_AGGREGATION_BARS = 5;  // Match VolatilityEngine default
    double synthRunningHigh[AMT::EFFORT_BUCKET_COUNT];
    double synthRunningLow[AMT::EFFORT_BUCKET_COUNT];
    double synthLastClose[AMT::EFFORT_BUCKET_COUNT] = {0.0};    // Last close in current window
    double synthPrevClose[AMT::EFFORT_BUCKET_COUNT] = {0.0};    // Previous synthetic bar's close
    bool synthPrevCloseValid[AMT::EFFORT_BUCKET_COUNT] = {false};
    int synthBarCount[AMT::EFFORT_BUCKET_COUNT] = {0};
    double synthDurationSec[AMT::EFFORT_BUCKET_COUNT] = {0.0};
    int synthSamplesPushed = 0;

    // Efficiency ratio tracking per bucket (Kaufman ER = |net| / path)
    double synthFirstClose[AMT::EFFORT_BUCKET_COUNT] = {0.0};   // First close in synthetic window
    bool synthFirstCloseValid[AMT::EFFORT_BUCKET_COUNT] = {false};
    double synthPathLength[AMT::EFFORT_BUCKET_COUNT] = {0.0};   // Sum of |close[i] - close[i-1]|
    double synthPrevBarClose[AMT::EFFORT_BUCKET_COUNT] = {0.0}; // Previous bar close for path calc

    // Initialize running high/low
    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
        synthRunningHigh[i] = -1e9;
        synthRunningLow[i] = 1e9;
    }

    int sessionsProcessed = 0;
    int totalBarsPushed = 0;
    int sessionsSkippedPartial = 0;

    // Process ALL completed sessions (both RTH and GLOBEX)
    for (const auto& session : st->baselineSessionMgr.completedSessions) {
        if (!session.IsValid() || session.firstBarIndex < 0) continue;

        // Skip very short sessions (need minimum bars for meaningful baseline)
        constexpr int MIN_SESSION_BARS = 30;
        if (session.barCount < MIN_SESSION_BARS) {
            sessionsSkippedPartial++;
            continue;
        }

        // Reset per-session bar counts
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            barsThisSession[i] = 0;
            domBarsThisSession[i] = 0;
        }

        // Track per-PHASE delta for SessionDeltaBaseline (phase-bucketed)
        double phaseCumDelta[AMT::EFFORT_BUCKET_COUNT] = {0.0};
        double phaseTotalVolume[AMT::EFFORT_BUCKET_COUNT] = {0.0};
        const bool isRTHSession = (session.key.sessionType == AMT::SessionType::RTH);

        // Track per-session market state bars (for prior computation)
        int sessionBalanceBars[AMT::EFFORT_BUCKET_COUNT] = {0};
        int sessionImbalanceBars[AMT::EFFORT_BUCKET_COUNT] = {0};

        // Walk all bars in this session
        for (int bar = session.firstBarIndex; bar <= session.lastBarIndex && bar < sc.ArraySize; ++bar) {
            // Get bar time to determine phase
            const SCDateTime barTime = sc.BaseDateTimeIn[bar];
            int hour, minute, second;
            barTime.GetTimeHMS(hour, minute, second);
            const int barTimeSec = hour * 3600 + minute * 60 + second;

            // Determine SessionPhase from time using drift-proof wrapper
            const AMT::SessionPhase phase = AMT::DetermineSessionPhase(barTimeSec, rthStartSec, rthEndSec);

            // Get bucket index (-1 for non-tradeable phases like MAINTENANCE)
            const int bucketIdx = AMT::SessionPhaseToBucketIndex(phase);
            if (bucketIdx < 0) continue;  // Skip MAINTENANCE, UNKNOWN

            // Read bar data from native Sierra Chart arrays
            const double barVolume = sc.Volume[bar];
            const double barHigh = sc.High[bar];
            const double barLow = sc.Low[bar];
            const double barClose = sc.Close[bar];

            // Delta from native bid/ask volume arrays
            const double askVol = sc.AskVolume[bar];
            const double bidVol = sc.BidVolume[bar];
            const double barDelta = askVol - bidVol;

            // Skip bars with zero volume (data gaps)
            if (barVolume <= 0.0) continue;

            // Compute bar metrics
            const double barRangeTicks = (barHigh - barLow) / tickSize;
            const double deltaPct = barDelta / barVolume;

            // Estimate vol_sec and trades_sec
            const double volSec = (barIntervalSec > 0) ? barVolume / barIntervalSec : barVolume;
            const double numTrades = static_cast<double>(sc.NumberOfTrades[bar]);
            const double tradesSec = (barIntervalSec > 0)
                ? (numTrades > 0 ? numTrades / barIntervalSec : volSec / 10.0)
                : 1.0;

            // Push to bucket distribution
            AMT::EffortBucketDistribution& dist = st->effortBaselines.GetByIndex(bucketIdx);
            dist.vol_sec.push(volSec);
            dist.trades_sec.push(tradesSec);
            dist.delta_pct.push(deltaPct);
            dist.bar_range.push(barRangeTicks);

            // range_velocity: ticks per minute (for auction pace baseline)
            const double barDurationMin = barIntervalSec / 60.0;
            if (barDurationMin > 0.001) {
                const double rangeVelocity = barRangeTicks / barDurationMin;
                dist.range_velocity.push(rangeVelocity);
            }

            // Synthetic bar aggregation for regime baseline pre-warm
            // Track running high/low across N bars, then push TRUE RANGE metrics
            synthRunningHigh[bucketIdx] = (std::max)(synthRunningHigh[bucketIdx], barHigh);
            synthRunningLow[bucketIdx] = (std::min)(synthRunningLow[bucketIdx], barLow);
            synthLastClose[bucketIdx] = barClose;  // Track last close for True Range
            synthBarCount[bucketIdx]++;
            synthDurationSec[bucketIdx] += barIntervalSec;

            // Efficiency ratio tracking: first close and path length
            if (!synthFirstCloseValid[bucketIdx]) {
                synthFirstClose[bucketIdx] = barClose;
                synthFirstCloseValid[bucketIdx] = true;
            }
            if (synthPrevBarClose[bucketIdx] > 0.0) {
                synthPathLength[bucketIdx] += std::abs(barClose - synthPrevBarClose[bucketIdx]);
            }
            synthPrevBarClose[bucketIdx] = barClose;

            // When we have enough bars, push TRUE RANGE synthetic metrics and reset
            if (synthBarCount[bucketIdx] >= SYNTHETIC_AGGREGATION_BARS) {
                // Compute TRUE RANGE using DRY helper (AMT_Volatility.h)
                double trueHigh, trueLow;
                AMT::ComputeTrueRange(synthRunningHigh[bucketIdx], synthRunningLow[bucketIdx],
                                      synthPrevClose[bucketIdx], synthPrevCloseValid[bucketIdx],
                                      trueHigh, trueLow);

                const double synthTrueRangeTicks = (trueHigh - trueLow) / tickSize;
                const double synthDurationMin = synthDurationSec[bucketIdx] / 60.0;

                if (synthTrueRangeTicks > 0.0) {
                    dist.synthetic_bar_range.push(synthTrueRangeTicks);

                    if (synthDurationMin > 0.001) {
                        const double synthVelocity = synthTrueRangeTicks / synthDurationMin;
                        dist.synthetic_range_velocity.push(synthVelocity);
                    }
                    synthSamplesPushed++;
                }

                // Compute and push Kaufman Efficiency Ratio: |net| / path
                // ER ranges from 0.0 (pure chop) to 1.0 (perfect trend)
                if (synthFirstCloseValid[bucketIdx] && synthPathLength[bucketIdx] > 1e-10) {
                    const double netChange = std::abs(synthLastClose[bucketIdx] - synthFirstClose[bucketIdx]);
                    const double er = netChange / synthPathLength[bucketIdx];
                    dist.synthetic_efficiency.push((std::min)(1.0, er));  // Clamp to [0,1]
                }

                // Save current close as previous for next synthetic bar
                synthPrevClose[bucketIdx] = synthLastClose[bucketIdx];
                synthPrevCloseValid[bucketIdx] = true;

                // Reset for next synthetic bar
                synthRunningHigh[bucketIdx] = -1e9;
                synthRunningLow[bucketIdx] = 1e9;
                synthBarCount[bucketIdx] = 0;
                synthDurationSec[bucketIdx] = 0.0;
                synthFirstCloseValid[bucketIdx] = false;
                synthPathLength[bucketIdx] = 0.0;
            }

            // avg_trade_size: only push when numTrades > 0 (no fallback, no contamination)
            if (numTrades > 0) {
                dist.avg_trade_size.push(barVolume / numTrades);
            }

            // abs_close_change: push for all bars with valid prevClose (including zeros)
            // Skip bar 0 only (no prevClose available) - one bar per session, negligible
            if (bar > 0 && tickSize > 0.0) {
                const double prevClose = sc.Close[bar - 1];
                if (prevClose > 0.0) {
                    const double absCloseChange = std::abs(barClose - prevClose) / tickSize;
                    dist.abs_close_change.push(absCloseChange);  // Zero is valid (unchanged close)
                }
            }
            dist.totalBarsPushed++;

            barsThisSession[bucketIdx]++;
            totalBarsPushed++;

            // Market state classification for prior population
            // Use delta magnitude as proxy for IMBALANCE (directional pressure)
            // Threshold: |deltaPct| > 0.3 (30%+ one-sided) → IMBALANCE
            const bool isDirectionalBar = std::abs(deltaPct) > 0.3;
            if (isDirectionalBar) {
                sessionImbalanceBars[bucketIdx]++;
            } else {
                sessionBalanceBars[bucketIdx]++;
            }

            // Accumulate per-PHASE delta and volume (for phase-bucketed baseline)
            phaseCumDelta[bucketIdx] += barDelta;
            phaseTotalVolume[bucketIdx] += barVolume;

            // DOM baseline from historical depth data (c_ACSILDepthBars)
            domBarsChecked++;
            const bool depthExists = histDepthAvailable && p_DepthBars->DepthDataExistsAt(bar);
            if (depthExists) {
                domBarsWithData++;
                const double refPrice = barClose;
                const int haloRadius = 10;  // Default halo radius in ticks

                // Collect depth at this bar using GetLastDominantSide() to properly
                // classify each price level (prevents double-counting from historical artifacts)
                double bidMass = 0.0, askMass = 0.0;
                double haloBidMass = 0.0, haloAskMass = 0.0;
                double bestBidPrice = 0.0, bestAskPrice = 0.0;  // For spread calculation
                int priceTickIdx = p_DepthBars->GetBarLowestPriceTickIndex(bar);
                do {
                    const float levelPrice = p_DepthBars->TickIndexToPrice(priceTickIdx);

                    // Use GetLastDominantSide to determine which side "owns" this price level
                    const BuySellEnum dominantSide = p_DepthBars->GetLastDominantSide(bar, priceTickIdx);

                    if (dominantSide == BSE_BUY) {
                        const int bidQty = p_DepthBars->GetLastBidQuantity(bar, priceTickIdx);
                        if (bidQty > 0) {
                            bidMass += static_cast<double>(bidQty);
                            // Track best bid (highest price with bid quantity)
                            if (levelPrice > bestBidPrice) {
                                bestBidPrice = levelPrice;
                            }
                            // Halo depth (distance-weighted within radius)
                            if (tickSize > 0.0) {
                                const int distTicks = static_cast<int>(std::abs(levelPrice - refPrice) / tickSize + 0.5);
                                if (distTicks <= haloRadius) {
                                    const double weight = 1.0 / (1.0 + distTicks);
                                    haloBidMass += bidQty * weight;
                                }
                            }
                        }
                    } else if (dominantSide == BSE_SELL) {
                        const int askQty = p_DepthBars->GetLastAskQuantity(bar, priceTickIdx);
                        if (askQty > 0) {
                            askMass += static_cast<double>(askQty);
                            // Track best ask (lowest price with ask quantity)
                            if (bestAskPrice == 0.0 || levelPrice < bestAskPrice) {
                                bestAskPrice = levelPrice;
                            }
                            // Halo depth (distance-weighted within radius)
                            if (tickSize > 0.0) {
                                const int distTicks = static_cast<int>(std::abs(levelPrice - refPrice) / tickSize + 0.5);
                                if (distTicks <= haloRadius) {
                                    const double weight = 1.0 / (1.0 + distTicks);
                                    haloAskMass += askQty * weight;
                                }
                            }
                        }
                    }
                    // BSE_UNDEFINED: skip ambiguous levels
                } while (p_DepthBars->GetNextHigherPriceTickIndex(bar, priceTickIdx));

                // Push to DOM baseline
                const double totalDepth = bidMass + askMass;
                if (totalDepth > 0.0) {
                    st->domWarmup.Push(phase, 0.0, 0.0, totalDepth);  // stack/pull not available historically
                    domBarsThisSession[bucketIdx]++;

                    // Push halo if valid
                    const double haloTotal = haloBidMass + haloAskMass;
                    if (haloTotal > 0.0) {
                        const double haloImbalance = (haloBidMass - haloAskMass) / haloTotal;
                        st->domWarmup.PushHalo(phase, haloTotal, haloImbalance);
                    }

                    // Push spread if we have valid best bid/ask
                    if (bestBidPrice > 0.0 && bestAskPrice > 0.0 && tickSize > 0.0) {
                        // Compute actual spread from best bid/ask (now properly classified)
                        const double spreadTicks = std::abs(bestAskPrice - bestBidPrice) / tickSize;
                        st->domWarmup.PushSpread(phase, spreadTicks);
                    }
                }
            }
        }

        // Check coverage per bucket and update sessionsContributed
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            AMT::EffortBucketDistribution& dist = st->effortBaselines.buckets[i];
            const int minBars = dist.GetMinBarsPerSession();

            // Session contributed to this bucket if it had adequate coverage
            if (barsThisSession[i] >= minBars) {
                dist.sessionsContributed++;
                sessionsContributedToBucket[i]++;
            }

            // DOM session contribution: Use effort bar count (Numbers Bars data is available for all bars)
            // Note: c_ACSILDepthBars only has ~1 day of data, but Numbers Bars has full history
            // The DOM metrics (stack/pull/depth) from Numbers Bars are pushed via UpdateSessionBaselines()
            AMT::DOMBucket& domBucket = st->domWarmup.buckets[i];
            if (barsThisSession[i] >= minBars) {
                domBucket.sessionsContributed++;
            }

            // Market state session contribution
            // Minimum 20 bars in bucket for meaningful balance/imbalance ratio
            const int sessionBucketBars = sessionBalanceBars[i] + sessionImbalanceBars[i];
            if (sessionBucketBars >= 20) {
                totalBalanceBars[i] += sessionBalanceBars[i];
                totalImbalanceBars[i] += sessionImbalanceBars[i];
                marketStateSessionsContributed[i]++;
            }
        }

        // Push per-PHASE delta ratios to SessionDeltaBaseline (phase-bucketed)
        // Each phase with sufficient volume gets its own delta ratio pushed
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            if (phaseTotalVolume[i] > 0.0 && barsThisSession[i] >= 5) {  // Minimum 5 bars for meaningful phase ratio
                const double phaseDeltaRatio = phaseCumDelta[i] / phaseTotalVolume[i];
                const AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
                st->sessionDeltaBaseline.PushPhaseDelta(phase, phaseDeltaRatio);
                st->sessionDeltaBaseline.IncrementPhaseSessionCount(phase);

                // DIAGNOSTIC: Log each phase's delta ratio being pushed
                if (diagLevel >= 1) {
                    char diagBuf[256];
                    snprintf(diagBuf, sizeof(diagBuf),
                        "[EFFORT-BASELINE] PhaseDelta PUSH: date=%d phase=%s bars=%d cumDelta=%.0f vol=%.0f ratio=%.4f",
                        session.key.tradingDay,
                        AMT::SessionPhaseToString(phase),
                        barsThisSession[i],
                        phaseCumDelta[i], phaseTotalVolume[i], phaseDeltaRatio);
                    sc.AddMessageToLog(diagBuf, 0);
                }
            }
        }

        sessionsProcessed++;

        // Check if ALL buckets have reached 5 contributing sessions
        bool allBucketsSatisfied = true;
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            if (st->effortBaselines.buckets[i].sessionsContributed < AMT::EffortBucketDistribution::REQUIRED_SESSIONS) {
                allBucketsSatisfied = false;
                break;
            }
        }
        if (allBucketsSatisfied) {
            break;
        }
    }

    // NOTE: Market state priors no longer used - AMTSignalEngine uses leaky accumulator
    // Historical balance/imbalance stats are logged for reference only
    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
        const int totalBars = totalBalanceBars[i] + totalImbalanceBars[i];
        const int sessions = marketStateSessionsContributed[i];

        if (diagLevel >= 1 && sessions >= 5 && totalBars > 0) {
            const double balanceRatio = static_cast<double>(totalBalanceBars[i]) / totalBars;
            const AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "[MARKET-STATE-HISTORY] Phase %s: sessions=%d bars=%d balance=%.1f%% imbalance=%.1f%%",
                AMT::SessionPhaseToString(phase), sessions, totalBars,
                balanceRatio * 100.0, (1.0 - balanceRatio) * 100.0);
            sc.AddMessageToLog(buf, 0);
        }
    }

    // Log results
    if (diagLevel >= 1) {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "[EFFORT-BASELINE] Processed %d sessions, %d bars total, %d synthetic bars (skipped: %d partial)",
            sessionsProcessed, totalBarsPushed, synthSamplesPushed, sessionsSkippedPartial);
        sc.AddMessageToLog(buf, 0);

        // Log per-bucket status
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            const AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
            const AMT::EffortBucketDistribution& dist = st->effortBaselines.buckets[i];
            const char* stateStr = (dist.GetState() == AMT::BucketBaselineState::READY) ? "READY" :
                                   (dist.GetState() == AMT::BucketBaselineState::INSUFFICIENT_SESSIONS) ? "INSUFF_SESS" :
                                   "INSUFF_COV";
            snprintf(buf, sizeof(buf),
                "[EFFORT-BASELINE] Phase %s: sessions=%d/%d bars=%d state=%s",
                AMT::SessionPhaseToString(phase),
                dist.sessionsContributed, AMT::EffortBucketDistribution::REQUIRED_SESSIONS,
                dist.totalBarsPushed, stateStr);
            sc.AddMessageToLog(buf, 0);
        }

        // Log session delta baseline status (phase-bucketed)
        snprintf(buf, sizeof(buf), "[EFFORT-BASELINE] SessionDeltaBaseline (phase-bucketed):");
        sc.AddMessageToLog(buf, 0);
        for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
            const AMT::SessionPhase phase = AMT::BucketIndexToSessionPhase(i);
            const auto& deltaBucket = st->sessionDeltaBaseline.Get(phase);
            const char* stateStr = deltaBucket.IsReady() ? "READY" : "INSUFF_SESS";
            snprintf(buf, sizeof(buf),
                "[EFFORT-BASELINE]   DeltaPhase %s: sessions=%d/%d state=%s",
                AMT::SessionPhaseToString(phase),
                deltaBucket.sessionsContributed, AMT::SessionDeltaBucket::REQUIRED_SESSIONS,
                stateStr);
            sc.AddMessageToLog(buf, 0);
        }

        // DOM depth data diagnostic
        int minBarChecked = INT_MAX, maxBarChecked = 0;
        for (const auto& session : st->baselineSessionMgr.completedSessions) {
            if (session.IsValid() && session.firstBarIndex >= 0) {
                if (session.firstBarIndex < minBarChecked) minBarChecked = session.firstBarIndex;
                if (session.lastBarIndex > maxBarChecked) maxBarChecked = session.lastBarIndex;
            }
        }
        snprintf(buf, sizeof(buf),
            "[DOM-BASELINE] c_ACSILDepthBars: checked=%d withData=%d (%.1f%%) | SessionBars=[%d..%d] vs DepthRange=[%d..%d]",
            domBarsChecked, domBarsWithData,
            domBarsChecked > 0 ? (100.0 * domBarsWithData / domBarsChecked) : 0.0,
            minBarChecked, maxBarChecked, firstBarWithDepth, lastBarWithDepth);
        sc.AddMessageToLog(buf, 0);
    }
}


// ============================================================================
// LIQUIDITY BASELINE PRE-WARM (Eliminates Cold-Start at Session Open)
// ============================================================================
// Uses historical depth data to seed LiquidityEngine baselines at session start.
// This allows liquidity data to be available from bar 1 of RTH instead of
// waiting 10+ bars for baseline warmup.
//
// lookbackBars: Number of historical bars to scan (default: 50)
// ============================================================================

static void PreWarmLiquidityBaselines(
    SCStudyInterfaceRef sc,
    StudyState* st,
    int lookbackBars,
    int diagLevel)
{
    c_ACSILDepthBars* p_DepthBars = sc.GetMarketDepthBars();
    if (!p_DepthBars) {
        if (diagLevel >= 1) {
            sc.AddMessageToLog("[LIQ-PREWARM] c_ACSILDepthBars unavailable - skipping pre-warm", 0);
        }
        return;
    }

    const double tickSize = sc.TickSize;
    if (tickSize <= 0.0) return;

    const int numBars = p_DepthBars->NumBars();
    const int currentBar = sc.Index;
    const int startBar = (std::max)(0, currentBar - lookbackBars);
    const double barDurationSec = (sc.SecondsPerBar > 0) ? static_cast<double>(sc.SecondsPerBar) : 60.0;

    // RTH times for phase-aware pre-warm (SSOT: DOMWarmup is phase-bucketed)
    const int rthStartSec = sc.Input[0].GetTime();  // RTH Start Time
    const int rthEndSec = sc.Input[1].GetTime();    // RTH End Time (INCLUSIVE)

    const int maxLevels = st->liquidityEngine.config.maxDomLevels;
    const int dmaxTicks = st->liquidityEngine.config.dmaxTicks;

    int barsProcessed = 0;
    int barsWithDepth = 0;
    double prevDepthMass = -1.0;  // -1 means no previous

    for (int bar = startBar; bar < currentBar; ++bar) {
        if (bar < 0 || bar >= numBars) continue;
        if (!p_DepthBars->DepthDataExistsAt(bar)) continue;

        barsProcessed++;

        // Get reference price (close of this historical bar)
        const double refPrice = sc.Close[bar];
        if (refPrice <= 0.0) continue;

        // Convert reference price to tick index
        const int refTickIndex = static_cast<int>(refPrice / tickSize);

        // Extract historical depth levels using GetLastDominantSide pattern
        std::vector<std::pair<double, double>> bidLevels;
        std::vector<std::pair<double, double>> askLevels;
        bidLevels.reserve(maxLevels);
        askLevels.reserve(maxLevels);

        // Scan around reference price for bid/ask levels
        for (int offset = 0; offset <= dmaxTicks + 2 &&
             (static_cast<int>(bidLevels.size()) < maxLevels ||
              static_cast<int>(askLevels.size()) < maxLevels); ++offset) {

            // Check bid side (below reference)
            if (static_cast<int>(bidLevels.size()) < maxLevels) {
                const int bidTickIdx = refTickIndex - offset;
                if (bidTickIdx > 0) {
                    const BuySellEnum side = p_DepthBars->GetLastDominantSide(bar, bidTickIdx);
                    if (side == BSE_BUY) {
                        const int qty = p_DepthBars->GetLastBidQuantity(bar, bidTickIdx);
                        if (qty > 0) {
                            const double price = bidTickIdx * tickSize;
                            bidLevels.push_back({price, static_cast<double>(qty)});
                        }
                    }
                }
            }

            // Check ask side (above reference)
            if (static_cast<int>(askLevels.size()) < maxLevels && offset > 0) {
                const int askTickIdx = refTickIndex + offset;
                const BuySellEnum side = p_DepthBars->GetLastDominantSide(bar, askTickIdx);
                if (side == BSE_SELL) {
                    const int qty = p_DepthBars->GetLastAskQuantity(bar, askTickIdx);
                    if (qty > 0) {
                        const double price = askTickIdx * tickSize;
                        askLevels.push_back({price, static_cast<double>(qty)});
                    }
                }
            }
        }

        if (bidLevels.empty() && askLevels.empty()) continue;

        barsWithDepth++;

        // Compute depth mass from extracted levels
        AMT::DepthMassResult depth = st->liquidityEngine.ComputeDepthMassFromLevels(
            refPrice, tickSize, bidLevels, askLevels);

        if (!depth.valid) continue;

        // Get historical aggressive volumes
        const double askVol = sc.AskVolume[bar];
        const double bidVol = sc.BidVolume[bar];

        // Compute historical spread from best bid/ask levels
        // bidLevels are sorted descending (highest bid first), askLevels ascending (lowest ask first)
        double spreadTicks = -1.0;
        if (!bidLevels.empty() && !askLevels.empty()) {
            const double bestBid = bidLevels[0].first;   // Highest bid
            const double bestAsk = askLevels[0].first;   // Lowest ask
            if (bestAsk > bestBid) {
                spreadTicks = (bestAsk - bestBid) / tickSize;
            }
        }

        // Determine phase for this historical bar (for phase-aware baselines)
        SCDateTime barDT = sc.BaseDateTimeIn[bar];
        int barHour, barMinute, barSecond;
        barDT.GetTimeHMS(barHour, barMinute, barSecond);
        const int barTimeSec = barHour * 3600 + barMinute * 60 + barSecond;
        const AMT::SessionPhase barPhase = AMT::DetermineSessionPhase(barTimeSec, rthStartSec, rthEndSec);

        // Push to baselines with phase (SSOT: depth/spread go to DOMWarmup, stress/resilience local)
        st->liquidityEngine.PreWarmFromBar(
            depth.totalMass,
            askVol,
            bidVol,
            prevDepthMass,
            barDurationSec,
            barPhase,
            spreadTicks  // Historical spread for Kyle's Tightness component
        );

        prevDepthMass = depth.totalMass;
    }

    // Log pre-warm results
    if (diagLevel >= 1) {
        const auto status = st->liquidityEngine.GetPreWarmStatus();
        char buf[320];
        snprintf(buf, sizeof(buf),
            "[LIQ-PREWARM] Scanned %d bars (range [%d..%d]), %d with depth | "
            "Baselines: depth=%zu stress=%zu res=%zu spread=%zu | Ready=%s",
            barsProcessed, startBar, currentBar - 1, barsWithDepth,
            status.depthSamples, status.stressSamples, status.resilienceSamples,
            status.spreadSamples, status.allReady ? "YES" : "NO");
        sc.AddMessageToLog(buf, 0);
    }
}


// ============================================================================
// MAIN STUDY ENTRY POINT
// ============================================================================

SCSFExport scsf_AuctionSensor_v1(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName = "AMT";
        sc.StudyDescription = "Integrated Zone Registry + Pure AMT Logic + DOM Wiring.";
#if USE_MANUAL_LOOP
        sc.AutoLoop = 0;  // Manual bar iteration for performance
#else
        sc.AutoLoop = 1;  // Sierra Chart handles bar-by-bar iteration
#endif
        sc.UpdateAlways = 1;
        sc.ChartUpdateIntervalInMilliseconds = 100;  // Throttle to max 10 updates/sec (prevents every-tick overhead)
        sc.UsesMarketDepthData = 1;  // Required for direct DOM access (GetBid/AskMarketDepthEntryAtLevel)

        // =====================================================================
        // INPUT ORGANIZATION (Smart Numbering with Room to Grow)
        // =====================================================================
        // 0-9:     Session & Timing
        // 10-19:   Zone Core (enables + params)
        // 20-29:   VPB Study Refs
        // 30-39:   TPO Study Refs
        // 40-49:   Daily OHLC Refs
        // 50-59:   VWAP Study Refs
        // 60-69:   DOM Refs & Params
        // 70-79:   Numbers Bars Refs
        // 80-89:   Facilitation
        // 90-99:   Drift & Validation
        // 100-109: Probe System
        // 110-119: Logging
        // =====================================================================

        // === 0-9: SESSION & TIMING ===
        // NOTE: VbP Study is SSOT for session boundaries. These are fallback only
        // (used if VbP profile not available, e.g. during startup)
        sc.Input[0].Name = "RTH Start (Fallback)"; sc.Input[0].SetTime(HMS_TIME(9, 30, 0));
        sc.Input[1].Name = "RTH End (Fallback)";   sc.Input[1].SetTime(HMS_TIME(16, 14, 59));
        sc.Input[2].Name = "Warm-up Bars";         sc.Input[2].SetInt(50);
        sc.Input[3].Name = "Baseline Window";      sc.Input[3].SetInt(300);
        sc.Input[4].Name = "Prior Session Age";    sc.Input[4].SetInt(3);  // Keep HVN/LVN from N prior sessions
        sc.Input[5].Name = "---";                  sc.Input[5].SetInt(0);  // Reserved
        sc.Input[6].Name = "---";                  sc.Input[6].SetInt(0);  // Reserved
        sc.Input[7].Name = "---";                  sc.Input[7].SetInt(0);  // Reserved
        sc.Input[8].Name = "---";                  sc.Input[8].SetInt(0);  // Reserved
        sc.Input[9].Name = "---";                  sc.Input[9].SetInt(0);  // Reserved

        // === 10-19: ZONE CORE ===
        sc.Input[10].Name = "Enable VPB Zones";    sc.Input[10].SetYesNo(1);
        sc.Input[11].Name = "Enable TPO Zones";    sc.Input[11].SetYesNo(1);
        sc.Input[12].Name = "Enable Daily Zones";  sc.Input[12].SetYesNo(1);
        sc.Input[13].Name = "Enable VWAP Zone";    sc.Input[13].SetYesNo(1);
        sc.Input[14].Name = "Max Depth Levels";    sc.Input[14].SetInt(80);
        sc.Input[15].Name = "Max Band Ticks";      sc.Input[15].SetInt(40);
        sc.Input[16].Name = "Target Depth Mass %"; sc.Input[16].SetFloat(0.60f);
        sc.Input[17].Name = "Halo Multiplier";     sc.Input[17].SetFloat(2.0f);
        sc.Input[18].Name = "Resolve Outside Bars"; sc.Input[18].SetInt(2);
        sc.Input[19].Name = "---";                 sc.Input[19].SetInt(0);  // Reserved

        // === 20-29: VPB STUDY REFS ===
        sc.Input[20].Name = "VbP Study ID";        sc.Input[20].SetInt(3);  // REQUIRED
        sc.Input[21].Name = "--- (deprecated)";    sc.Input[21].SetInt(0);  // Profile index now auto-detected
        sc.Input[22].Name = "VPB: POC";            sc.Input[22].SetStudySubgraphValues(3, 1);  // ID3.SG2
        sc.Input[23].Name = "VPB: VAH";            sc.Input[23].SetStudySubgraphValues(3, 2);  // ID3.SG3
        sc.Input[24].Name = "VPB: VAL";            sc.Input[24].SetStudySubgraphValues(3, 3);  // ID3.SG4
        sc.Input[25].Name = "VPB: Peaks (HVN)";    sc.Input[25].SetStudySubgraphValues(3, 17); // ID3.SG18
        sc.Input[26].Name = "VPB: Valleys (LVN)";  sc.Input[26].SetStudySubgraphValues(3, 18); // ID3.SG19
        sc.Input[27].Name = "---";                 sc.Input[27].SetInt(0);  // Reserved
        sc.Input[28].Name = "---";                 sc.Input[28].SetInt(0);  // Reserved
        sc.Input[29].Name = "---";                 sc.Input[29].SetInt(0);  // Reserved

        // === 30-39: TPO STUDY REFS ===
        sc.Input[30].Name = "TPO: POC";            sc.Input[30].SetStudySubgraphValues(4, 0);  // ID4.SG1
        sc.Input[31].Name = "TPO: VAH";            sc.Input[31].SetStudySubgraphValues(4, 1);  // ID4.SG2
        sc.Input[32].Name = "TPO: VAL";            sc.Input[32].SetStudySubgraphValues(4, 2);  // ID4.SG3
        sc.Input[33].Name = "---";                 sc.Input[33].SetInt(0);  // Reserved
        sc.Input[34].Name = "---";                 sc.Input[34].SetInt(0);  // Reserved
        sc.Input[35].Name = "---";                 sc.Input[35].SetInt(0);  // Reserved
        sc.Input[36].Name = "---";                 sc.Input[36].SetInt(0);  // Reserved
        sc.Input[37].Name = "---";                 sc.Input[37].SetInt(0);  // Reserved
        sc.Input[38].Name = "---";                 sc.Input[38].SetInt(0);  // Reserved
        sc.Input[39].Name = "---";                 sc.Input[39].SetInt(0);  // Reserved

        // === 40-49: DAILY OHLC REFS ===
        sc.Input[40].Name = "Daily: Open";         sc.Input[40].SetStudySubgraphValues(8, 0);  // ID8.SG1
        sc.Input[41].Name = "Daily: High";         sc.Input[41].SetStudySubgraphValues(8, 1);  // ID8.SG2
        sc.Input[42].Name = "Daily: Low";          sc.Input[42].SetStudySubgraphValues(8, 2);  // ID8.SG3
        sc.Input[43].Name = "Daily: Close";        sc.Input[43].SetStudySubgraphValues(8, 3);  // ID8.SG4
        sc.Input[44].Name = "---";                 sc.Input[44].SetInt(0);  // Reserved
        sc.Input[45].Name = "---";                 sc.Input[45].SetInt(0);  // Reserved
        sc.Input[46].Name = "---";                 sc.Input[46].SetInt(0);  // Reserved
        sc.Input[47].Name = "---";                 sc.Input[47].SetInt(0);  // Reserved
        sc.Input[48].Name = "---";                 sc.Input[48].SetInt(0);  // Reserved
        sc.Input[49].Name = "---";                 sc.Input[49].SetInt(0);  // Reserved

        // === 50-59: VWAP STUDY REFS ===
        sc.Input[50].Name = "VWAP";                sc.Input[50].SetStudySubgraphValues(5, 0);  // ID5.SG1
        sc.Input[51].Name = "VWAP: Upper Band 1";  sc.Input[51].SetStudySubgraphValues(5, 1);  // ID5.SG2
        sc.Input[52].Name = "VWAP: Lower Band 1";  sc.Input[52].SetStudySubgraphValues(5, 2);  // ID5.SG3
        sc.Input[53].Name = "VWAP: Upper Band 2";  sc.Input[53].SetStudySubgraphValues(5, 3);  // ID5.SG4
        sc.Input[54].Name = "VWAP: Lower Band 2";  sc.Input[54].SetStudySubgraphValues(5, 4);  // ID5.SG5
        sc.Input[55].Name = "---";                 sc.Input[55].SetInt(0);  // Reserved
        sc.Input[56].Name = "---";                 sc.Input[56].SetInt(0);  // Reserved
        sc.Input[57].Name = "---";                 sc.Input[57].SetInt(0);  // Reserved
        sc.Input[58].Name = "---";                 sc.Input[58].SetInt(0);  // Reserved
        sc.Input[59].Name = "---";                 sc.Input[59].SetInt(0);  // Reserved

        // === 60-69: DOM REFS & PARAMS ===
        sc.Input[60].Name = "Best Bid Price";      sc.Input[60].SetStudySubgraphValues(2, 1);  // ID2.SG2
        sc.Input[61].Name = "Best Ask Price";      sc.Input[61].SetStudySubgraphValues(2, 3);  // ID2.SG4
        sc.Input[62].Name = "DOM: Bid Size";       sc.Input[62].SetStudySubgraphValues(2, 0);  // ID2.SG1
        sc.Input[63].Name = "DOM: Ask Size";       sc.Input[63].SetStudySubgraphValues(2, 2);  // ID2.SG3
        sc.Input[64].Name = "DOM: Bid Stack/Pull"; sc.Input[64].SetStudySubgraphValues(2, 4);  // ID2.SG5
        sc.Input[65].Name = "DOM: Ask Stack/Pull"; sc.Input[65].SetStudySubgraphValues(2, 5);  // ID2.SG6
        sc.Input[66].Name = "DOM: Liquidity Norm"; sc.Input[66].SetFloat(2000.0f);
        sc.Input[67].Name = "DOM: Stack Norm";     sc.Input[67].SetFloat(500.0f);
        sc.Input[68].Name = "DOM: Eval Interval";  sc.Input[68].SetInt(5);
        sc.Input[69].Name = "---";                 sc.Input[69].SetInt(0);  // Reserved

        // === 70-79: NUMBERS BARS REFS (ROBUST POLICY: Only for rate baselines, debug cross-checks) ===
        // Volume/Delta SSOT is native sc arrays, NOT these NB inputs
        sc.Input[70].Name = "NB: Bid Vol/sec";     sc.Input[70].SetStudySubgraphValues(1, 52); // Optional: rate baselines
        sc.Input[71].Name = "NB: Ask Vol/sec";     sc.Input[71].SetStudySubgraphValues(1, 53); // Optional: rate baselines
        sc.Input[72].Name = "[DEBUG] NB: TotalVol";sc.Input[72].SetStudySubgraphValues(1, 12); // Debug only: SSOT is sc.Volume[]
        sc.Input[73].Name = "[UNUSED] NB: Delta%"; sc.Input[73].SetStudySubgraphValues(1, 10); // Deprecated: SSOT is sc.AskVolume - sc.BidVolume
        sc.Input[74].Name = "[OPT] NB: Max Delta"; sc.Input[74].SetStudySubgraphValues(1, 7);  // Optional: single-price imbalance
        sc.Input[75].Name = "[DEBUG] NB: CumDelta";sc.Input[75].SetStudySubgraphValues(1, 9);  // Debug only: SSOT is internal sessionCumDelta
        // Diagonal Delta (Footprint Imbalance) - SG43/44: Compare bid@N vs ask@N+1
        sc.Input[76].Name = "NB: Diag Pos Delta";  sc.Input[76].SetStudySubgraphValues(1, 42); // SG43: Diagonal Positive Delta Sum
        sc.Input[77].Name = "NB: Diag Neg Delta";  sc.Input[77].SetStudySubgraphValues(1, 43); // SG44: Diagonal Negative Delta Sum
        // Average Trade Size - SG51/52: Detect institutional vs retail activity
        sc.Input[78].Name = "NB: Avg Bid Trade";   sc.Input[78].SetStudySubgraphValues(1, 50); // SG51: Average Bid Trade Size
        sc.Input[79].Name = "NB: Avg Ask Trade";   sc.Input[79].SetStudySubgraphValues(1, 51); // SG52: Average Ask Trade Size

        // === 80-89: FACILITATION ===
        sc.Input[80].Name = "Facil: Labored Vol Ratio"; sc.Input[80].SetFloat(1.2f);
        sc.Input[81].Name = "Facil: Labored Max Ticks"; sc.Input[81].SetFloat(4.0f);
        sc.Input[82].Name = "Facil: Ineff Vol Ratio";   sc.Input[82].SetFloat(0.6f);
        sc.Input[83].Name = "Facil: Ineff Min Ticks";   sc.Input[83].SetFloat(8.0f);
        sc.Input[84].Name = "Facil: Failed Vol Ratio";  sc.Input[84].SetFloat(0.3f);
        sc.Input[85].Name = "Facil: Failed Max Ticks";  sc.Input[85].SetFloat(4.0f);
        sc.Input[86].Name = "---";                 sc.Input[86].SetInt(0);  // Reserved
        sc.Input[87].Name = "---";                 sc.Input[87].SetInt(0);  // Reserved
        sc.Input[88].Name = "---";                 sc.Input[88].SetInt(0);  // Reserved
        sc.Input[89].Name = "---";                 sc.Input[89].SetInt(0);  // Reserved

        // === 90-99: DRIFT & VALIDATION ===
        sc.Input[90].Name = "Bug Detection Ticks"; sc.Input[90].SetInt(100);
        sc.Input[91].Name = "Max Zero DOM Bars";   sc.Input[91].SetInt(5);
        sc.Input[92].Name = "---";                 sc.Input[92].SetInt(0);  // Reserved
        sc.Input[93].Name = "---";                 sc.Input[93].SetInt(0);  // Reserved
        sc.Input[94].Name = "---";                 sc.Input[94].SetInt(0);  // Reserved
        sc.Input[95].Name = "---";                 sc.Input[95].SetInt(0);  // Reserved
        sc.Input[96].Name = "---";                 sc.Input[96].SetInt(0);  // Reserved
        sc.Input[97].Name = "---";                 sc.Input[97].SetInt(0);  // Reserved
        sc.Input[98].Name = "---";                 sc.Input[98].SetInt(0);  // Reserved
        sc.Input[99].Name = "---";                 sc.Input[99].SetInt(0);  // Reserved

        // === 100-109: PROBE SYSTEM ===
        sc.Input[100].Name = "Enable Probe System";     sc.Input[100].SetYesNo(1);
        sc.Input[101].Name = "Probe Score Threshold";   sc.Input[101].SetFloat(7.0f);
        sc.Input[102].Name = "Probe Timeout (RTH)";     sc.Input[102].SetInt(120);
        sc.Input[103].Name = "Probe Timeout (GBX)";     sc.Input[103].SetInt(300);
        sc.Input[104].Name = "Probe Cooldown Bars";     sc.Input[104].SetInt(10);
        sc.Input[105].Name = "Probe Real-Time Only";    sc.Input[105].SetYesNo(1);
        sc.Input[106].Name = "---";                 sc.Input[106].SetInt(0);  // Reserved
        sc.Input[107].Name = "---";                 sc.Input[107].SetInt(0);  // Reserved
        sc.Input[108].Name = "---";                 sc.Input[108].SetInt(0);  // Reserved
        sc.Input[109].Name = "---";                 sc.Input[109].SetInt(0);  // Reserved

        // === 110-120: LOGGING ===
        sc.Input[110].Name = "Log Level (0-3)";         sc.Input[110].SetInt(1);
        sc.Input[111].Name = "Log Last N Bars";         sc.Input[111].SetInt(100);
        sc.Input[112].Name = "Log: Base Directory";     sc.Input[112].SetString("E:\\SierraChart\\Data\\Logging");
        sc.Input[113].Name = "Log: Throttle Cooldown";  sc.Input[113].SetInt(5);
        sc.Input[114].Name = "Log: Session Events CSV";   sc.Input[114].SetYesNo(0);  // OFF - _events.csv (engagements, phase, intent)
        sc.Input[115].Name = "Log: Probe Lifecycle CSV"; sc.Input[115].SetYesNo(0);  // OFF - _probes.csv (fired/resolved)
        sc.Input[116].Name = "Log: Per-Bar Zones CSV";   sc.Input[116].SetYesNo(0);  // OFF - _amt.csv (per-bar zone tracking)
        sc.Input[117].Name = "Log: AMT Every Bar";      sc.Input[117].SetYesNo(1);  // ON by default
        sc.Input[118].Name = "Log: AMT Stats Interval"; sc.Input[118].SetInt(50);
        sc.Input[119].Name = "Log: CSV Flush Interval"; sc.Input[119].SetInt(100);
        sc.Input[120].Name = "Log: SC Message";         sc.Input[120].SetYesNo(1);

        // Phase 5: Module-level diagnostic enables
        sc.Input[121].Name = "Log: VBP Diagnostics";      sc.Input[121].SetYesNo(0);  // OFF - detailed VBP/profile logging
        sc.Input[122].Name = "Log: Session Diagnostics";  sc.Input[122].SetYesNo(0);  // OFF - session transition details
        sc.Input[123].Name = "Log: Zone Diagnostics";     sc.Input[123].SetYesNo(0);  // OFF - zone engagement/proximity
        sc.Input[124].Name = "Log: Delta Diagnostics";    sc.Input[124].SetYesNo(0);  // OFF - delta/effort signal details

        // CRITICAL: Required for VbP data access
        sc.MaintainVolumeAtPriceData = 1;

        // CRITICAL: Required for native sc.AskVolume[] and sc.BidVolume[] arrays
        // Without this, these arrays may be invalid or all zeros
        sc.MaintainAdditionalChartDataArrays = 1;

        // CRITICAL: Required for historical market depth data access via c_ACSILDepthBars
        // Enables temporal coherence in 3-Component Liquidity Model (Dec 2024)
        // User must also enable "Support Downloading Historical Market Depth Data" in
        // Global Settings >> Sierra Chart Server Settings
        sc.MaintainHistoricalMarketDepthData = 1;

        // CRITICAL: Enable native DOM Pulling/Stacking calculations
        // Allows direct API access via sc.GetBidMarketDepthStackPullSum() instead of study refs
        sc.SetUseMarketDepthPullingStackingData(1);

        // === AMT ZONE VISUALIZATION SUBGRAPHS ===
        sc.Subgraph[0].Name = "AMT: VAH";
        sc.Subgraph[0].DrawStyle = DRAWSTYLE_LINE;
        sc.Subgraph[0].PrimaryColor = RGB(255, 0, 0);
        sc.Subgraph[0].LineWidth = 2;
        sc.Subgraph[0].DrawZeros = 0;

        sc.Subgraph[1].Name = "AMT: POC";
        sc.Subgraph[1].DrawStyle = DRAWSTYLE_LINE;
        sc.Subgraph[1].PrimaryColor = RGB(0, 255, 0);
        sc.Subgraph[1].LineWidth = 3;
        sc.Subgraph[1].DrawZeros = 0;

        sc.Subgraph[2].Name = "AMT: VAL";
        sc.Subgraph[2].DrawStyle = DRAWSTYLE_LINE;
        sc.Subgraph[2].PrimaryColor = RGB(0, 100, 255);
        sc.Subgraph[2].LineWidth = 2;
        sc.Subgraph[2].DrawZeros = 0;

        sc.Subgraph[3].Name = "AMT: Phase";
        sc.Subgraph[3].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[3].DrawZeros = 0;

        sc.Subgraph[4].Name = "AMT: Proximity";
        sc.Subgraph[4].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[4].DrawZeros = 0;

        sc.Subgraph[5].Name = "AMT: Zone Strength";
        sc.Subgraph[5].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[5].DrawZeros = 0;

        // === LOGGING FIX: Store per-bar zone values at close for retrospective CSV logging ===
        // These subgraphs capture zone state AT BAR CLOSE, enabling accurate historical logging
        sc.Subgraph[6].Name = "Log: POC Price";
        sc.Subgraph[6].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[6].DrawZeros = 0;

        sc.Subgraph[7].Name = "Log: VAH Price";
        sc.Subgraph[7].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[7].DrawZeros = 0;

        sc.Subgraph[8].Name = "Log: VAL Price";
        sc.Subgraph[8].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[8].DrawZeros = 0;

        sc.Subgraph[9].Name = "Log: POC Proximity";
        sc.Subgraph[9].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[9].DrawZeros = 0;

        sc.Subgraph[10].Name = "Log: VAH Proximity";
        sc.Subgraph[10].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[10].DrawZeros = 0;

        sc.Subgraph[11].Name = "Log: VAL Proximity";
        sc.Subgraph[11].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[11].DrawZeros = 0;

        sc.Subgraph[12].Name = "Log: Facilitation";
        sc.Subgraph[12].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[12].DrawZeros = 0;

        sc.Subgraph[13].Name = "Log: Market State";
        sc.Subgraph[13].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[13].DrawZeros = 0;

        sc.Subgraph[14].Name = "Log: Delta Consistency";
        sc.Subgraph[14].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[14].DrawZeros = 0;

        return;
    }

    // --- Persistence (Corrected for multi-instance safety) ---

    StudyState* st = static_cast<StudyState*>(sc.GetPersistentPointer(1));

    // Set SC context for SSOT invariant logging (routes violations to SC message log)
    ::SetSSOTLogContext(&sc);

    if (sc.LastCallToFunction)
    {
        if (st != nullptr)
        {
            // Shutdown LogManager (closes all CSV files, flushes buffers)
            st->logManager.Shutdown();

            delete st;
            sc.SetPersistentPointer(1, nullptr);
        }
        return;
    }

    // Skip processing during historical data download (per ACSIL Programming Concepts)
    // Heavy processing can cause performance issues while SC is downloading data
    if (sc.DownloadingHistoricalData)
    {
        return;
    }

    const int baselineWindow = sc.Input[3].GetInt();   // Baseline Window
    const int warmUpBars = sc.Input[2].GetInt();       // Warm-up Bars

    // Track if we need to initialize state (new allocation OR full recalc)
    bool needsStateInit = false;
    const char* initReason = "NONE";

    if (st == nullptr)
    {
        st = new StudyState(); // Allocate unique memory for THIS instance
        sc.SetPersistentPointer(1, st);
        needsStateInit = true;
        initReason = "NEW_ALLOC";
    }
    else if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
    {
        // CRITICAL: Reset state on full recalculation (per SC documentation)
        // This ensures stale flags don't persist from failed initial loads
        needsStateInit = true;
        initReason = "FULL_RECALC";
    }

    // ========================================================================
    // NO-OP BODY TEST: Early return to measure pure dispatch overhead
    // Enable by setting NOOP_BODY_TEST to 1 at top of file
    // PLACEMENT: Immediately after state allocation, BEFORE any:
    //   - Logging / string formatting
    //   - Input validation
    //   - CollectObservableSnapshot()
    //   - GetStudyArrayUsingID / VbP calls
    //   - map/vector operations
    // ========================================================================
#if NOOP_BODY_TEST
    // Minimal work: write zeros to subgraphs and return immediately
    // This isolates Sierra's AutoLoop dispatch cost from function body work
    // NOTE: NOOP_BODY_TEST is only valid with AutoLoop=1 (USE_MANUAL_LOOP=0)
#if USE_MANUAL_LOOP
    #error "NOOP_BODY_TEST requires AutoLoop=1 (USE_MANUAL_LOOP=0) - they are mutually exclusive"
#endif
    sc.Subgraph[0][sc.Index] = 0.0f;
    sc.Subgraph[1][sc.Index] = 0.0f;
    sc.Subgraph[2][sc.Index] = 0.0f;
    return;
#endif

    // CRITICAL DIAGNOSTIC: Log at start of full recalc
    if (sc.UpdateStartIndex == 0)
    {
        SCString initMsg;
        initMsg.Format("Bar 0 | st=%s | IsFullRecalc=%d | needsStateInit=%d | reason=%s | activeZones=%d | initialized=%d",
            st ? "VALID" : "NULL",
            sc.IsFullRecalculation ? 1 : 0,
            needsStateInit ? 1 : 0,
            initReason,
            st ? static_cast<int>(st->amtZoneManager.activeZones.size()) : -1,
            st ? (st->amtZonesInitialized ? 1 : 0) : -1);
        st->logManager.LogOnce(ThrottleKey::INIT_PATH, 0, initMsg.GetChars(), LogCategory::INIT);
    }

    if (needsStateInit)
    {
        st->resetAll(baselineWindow, warmUpBars);

        // PRE-WARM: Seed liquidity baselines from historical depth data
        // Uses sufficient history to populate ALL phase-aware baselines (no warmup period)
        // 7 phases × 10 min samples = 70 minimum, but phases are uneven so use 500+
        const int liqPreWarmBars = 500;  // Enough to cover all phases with 10+ samples each
        const int preWarmDiagLevel = sc.Input[110].GetInt();  // Get diagLevel early for pre-warm
        PreWarmLiquidityBaselines(sc, st, liqPreWarmBars, preWarmDiagLevel);

        // LOUD DIAGNOSTIC: State was reset
        SCString initMsg;
        initMsg.Format("UpdateStart=%d | IsFullRecalc=%d | resetAll() called | activeZones=%d initialized=%d",
            sc.UpdateStartIndex, sc.IsFullRecalculation ? 1 : 0,
            static_cast<int>(st->amtZoneManager.activeZones.size()),
            st->amtZonesInitialized ? 1 : 0);
        st->logManager.LogOnce(ThrottleKey::STATE_RESET, 0, initMsg.GetChars(), LogCategory::INIT);

        // Log zone posture (one-time diagnostic at init)
        // Confirms: VBP=ON, PRIOR=ON, TPO=OFF, STRUCT=ON (track-only by default)
        {
            std::string postureStr = AMT::g_zonePosture.ToString();
            SCString postureMsg;
            postureMsg.Format("%s", postureStr.c_str());
            st->logManager.LogOnce(ThrottleKey::ZONE_POSTURE, 0, postureMsg.GetChars(), LogCategory::ZONE);

            // Assert TPO is disabled (posture enforcement)
            if (AMT::g_zonePosture.enableTPO) {
                st->logManager.LogError(0, "TPO zones enabled but should be disabled by posture!", LogCategory::ERROR_CAT);
            }
        }

        // Configure drift tracker from inputs
        st->drift.bugDetectionTicks = sc.Input[90].GetInt();      // Bug Detection Ticks
        st->drift.maxZeroDomBarsBeforeWarn = sc.Input[91].GetInt(); // Max Zero DOM Bars

        // ============================================================
        // FIX: Register engagement callback at INIT time, not zone creation time
        // This ensures the callback exists BEFORE any bars are processed,
        // fixing the bug where early sessions had no callback registered.
        // The callback uses config.tickSize (updated before zone creation)
        // instead of capturing tickSize by value.
        // ============================================================
        st->amtZoneManager.onEngagementFinalized = [st, &sc](
            const AMT::ZoneRuntime& zone,
            const AMT::FinalizationResult& result)
        {
            const AMT::EngagementMetrics& eng = result.metrics;
            const double tickSize = st->amtZoneManager.config.tickSize;

            st->amtEngagementsFinalized++;
            // NOTE: Legacy time_in_zone/escape_velocity baseline pushes removed (Dec 2024)
            // Zone behavior baselines are not used in current decision logic

            // --- ACCUMULATE ENGAGEMENT STATS (SessionAccumulators) ---
            st->sessionAccum.engagementCount++;
            st->sessionAccum.totalEngagementBars += eng.barsEngaged;
            if (eng.escapeVelocity > 0.0) {
                st->sessionAccum.escapeCount++;
                st->sessionAccum.totalEscapeVelocity += eng.escapeVelocity;
            }

            // --- RECORD TO SESSION ENGAGEMENT ACCUMULATOR (SSOT) ---
            // This is the SSOT for per-anchor engagement stats
            // CalculateSessionStats MUST read from here, NOT zone.lifetime*
            st->engagementAccum.RecordEngagement(zone.type, result.touchRecord.type);

            // --- M4: ENGAGEMENT_FINAL CSV LOGGING ---
            // Structured session-grouped format
            // Skip logging CHART_RESET engagements (invalid timestamps from before chart reload)
            const bool isChartReset = (result.touchRecord.unresolvedReason == AMT::UnresolvedReason::CHART_RESET);
            const bool hasValidTime = (eng.endBar > 0 && eng.endTime.GetAsDouble() > 1.0);

            if (st->logManager.ShouldEmit(LogChannel::EVENTS_CSV, LogLevel::MINIMAL) &&
                !isChartReset && hasValidTime)
            {
                // Compute tick distances
                const long long entryTicks = static_cast<long long>(std::round(eng.entryPrice / tickSize));
                const long long exitTicks = static_cast<long long>(std::round(eng.exitPrice / tickSize));
                const long long anchorTicks = zone.GetAnchorTicks();
                const int entryDist = static_cast<int>(std::abs(entryTicks - anchorTicks));
                const int exitDist = static_cast<int>(std::abs(exitTicks - anchorTicks));

                // CONTRACT: Sample context from engagement end bar (not current bar)
                // Engagement may have finalized in the past, so we use SampleHistoricalContext
                // to get the state as it was at engagement end time.
                const AMT::LoggingContext engCtx = AMT::SampleHistoricalContext(sc, eng.endBar, tickSize);

                AMT::SessionEvent evt;
                evt.type = AMT::SessionEventType::ENGAGEMENT_FINAL;
                evt.timestamp = eng.endTime;
                evt.bar = eng.endBar;
                evt.zoneId = zone.zoneId;
                evt.zoneType = AMT::ZoneTypeToString(zone.type);
                evt.entryPrice = eng.entryPrice;
                evt.exitPrice = eng.exitPrice;
                evt.engagementBars = eng.barsEngaged;
                evt.peakDist = eng.peakPenetrationTicks;
                evt.entryDist = entryDist;
                evt.exitDist = exitDist;
                evt.escapeVel = eng.escapeVelocity;
                evt.volRatio = eng.volumeRatio;
                evt.outcome = AMT::TouchTypeToString(result.touchRecord.type);

                // CONTRACT: Use sampled context for intent/state fields (not cached amtContext)
                evt.deltaConf = engCtx.deltaConfValid ? engCtx.deltaConf : 0.0;
                evt.facilitation = engCtx.GetFacilitationStr();
                evt.marketState = engCtx.GetMarketStateStr();
                evt.phase = AMT::CurrentPhaseToString(
                    static_cast<AMT::CurrentPhase>(static_cast<int>(sc.Subgraph[3][eng.endBar])));

                // VBP fields (from sessionMgr - engagement is within current session)
                evt.poc = st->sessionMgr.GetPOC();
                evt.vah = st->sessionMgr.GetVAH();
                evt.val = st->sessionMgr.GetVAL();

                st->logManager.LogSessionEvent(evt);
            }
        };
    }

    // CRITICAL: Input validity must be checked EVERY iteration, not just at init.
    // If user configures inputs after study loads, flags would otherwise stay false.

    // DOM inputs validity (60-69 range)
    st->domInputsValid = (sc.Input[62].GetStudyID() != 0 &&   // DOM: Bid Size
        sc.Input[63].GetStudyID() != 0 &&   // DOM: Ask Size
        sc.Input[64].GetStudyID() != 0 &&   // DOM: Bid Stack/Pull
        sc.Input[65].GetStudyID() != 0);    // DOM: Ask Stack/Pull

    // Stats inputs validity - Input 74 (MaxDelta) is optional, has fallback to bar delta
    // NOTE: Inputs 70-71 (Bid/Ask Vol/sec) are PRIMARY effort signals from Numbers Bars.
    // These are per-second RATES, distinct from native sc.BidVolume/AskVolume (per-bar TOTALS).
    // See EffortSnapshot SSOT contract in AMT_Snapshots.h for unit semantics.
    st->statsInputsValid = (sc.Input[74].GetStudyID() != 0);  // NB: Max Delta (optional)

    // VWAP bands inputs validity (50-59 range)
    st->vwapBandsInputsValid = (sc.Input[51].GetStudyID() != 0 ||  // VWAP: Upper Band 1
        sc.Input[52].GetStudyID() != 0);    // VWAP: Lower Band 1

    // Depth OHLC inputs validity (40-43 = Daily OHLC)
    st->depthOhlcInputsValid = (sc.Input[40].GetStudyID() != 0 &&
        sc.Input[41].GetStudyID() != 0 &&
        sc.Input[42].GetStudyID() != 0 &&
        sc.Input[43].GetStudyID() != 0);

    // === CONSTANTS (outside loop - don't change per bar) ===
    const int rthStartSec = sc.Input[0].GetTime();  // RTH Start Time
    const int rthEndSec = sc.Input[1].GetTime();    // RTH End Time (INCLUSIVE last second)

    // =========================================================================
    // PHASE BOUNDARY CONTRACT (SSOT)
    // =========================================================================
    // Input[1] returns the INCLUSIVE last RTH second (e.g., 58499 = 16:14:59)
    //
    // USE THE DRIFT-PROOF WRAPPER:
    //   DetermineSessionPhase(tSec, rthStartSec, rthEndSec)  // rthEndSec = INCLUSIVE
    //
    // The wrapper internally converts INCLUSIVE→EXCLUSIVE (+1) before calling
    // DetermineExactPhase. This makes drift structurally impossible.
    //
    // DO NOT call DetermineExactPhase directly - use DetermineSessionPhase.
    // =========================================================================
    const int gbxStartSec = rthEndSec + 1;  // EXCLUSIVE RTH end = Globex start (used elsewhere)
    const int diagLevel = sc.Input[110].GetInt();   // Log Level
    const int logLastN = sc.Input[111].GetInt();    // Log Last N Bars

    // DEBUG: Unconditional startup log to verify study is running
    // Use UpdateStartIndex == 0 for AutoLoop=0 compatibility (sc.Index == 0 never true in manual loop)
    if (sc.UpdateStartIndex == 0)
    {
        SCString startupMsg;
        startupMsg.Format("Study running. diagLevel=%d vbpStudyId=%d ArraySize=%d",
            diagLevel, sc.Input[20].GetInt(), sc.ArraySize);
        st->logManager.LogOnce(ThrottleKey::AMT_STARTUP, 0, startupMsg.GetChars(), LogCategory::AMT);

        // Log input validation status (critical for delta/liquidity)
        // Inputs 70-71: Per-second rates from Numbers Bars (PRIMARY effort source)
        // Input 74 (MaxDelta) is optional - has fallback to bar delta
        startupMsg.Format("Effort(70-71):NB_RATES | MaxDelta(74):%s | DOM(62-65):%s | DepthOHLC(40-43):%s | VWAP(50-54):%s",
            st->statsInputsValid ? "OK" : "FALLBACK",
            st->domInputsValid ? "OK" : "MISSING",
            st->depthOhlcInputsValid ? "OK" : "MISSING",
            st->vwapBandsInputsValid ? "OK" : "MISSING");
        if (st->domInputsValid)
            st->logManager.LogOnce(ThrottleKey::INPUT_DIAG, 0, startupMsg.GetChars(), LogCategory::INPUT);
        else
            st->logManager.LogWarn(0, startupMsg.GetChars(), LogCategory::INPUT);
    }

    // One-time diagnostic log for RTH configuration
    if (sc.UpdateStartIndex == 0 && diagLevel >= 1)
    {
        const int barTimeSec = TimeToSeconds(sc.BaseDateTimeIn[0]);  // First bar time
        SCString msg;
        msg.Format("RTH: %02d:%02d:%02d to %02d:%02d:%02d | Globex: %02d:%02d:%02d | Bar: %02d:%02d:%02d",
            rthStartSec / 3600, (rthStartSec % 3600) / 60, rthStartSec % 60,
            rthEndSec / 3600, (rthEndSec % 3600) / 60, rthEndSec % 60,
            gbxStartSec / 3600, (gbxStartSec % 3600) / 60, gbxStartSec % 60,
            barTimeSec / 3600, (barTimeSec % 3600) / 60, barTimeSec % 60);
        st->logManager.LogOnce(ThrottleKey::SESSION_DIAG, 0, msg.GetChars(), LogCategory::SESSION);
    }

    // G3 Guardrail: Log session schedule at init (verifies correct boundary configuration)
    // See docs/ssot_session_contract.md for SSOT contract
    if (sc.UpdateStartIndex == 0)
    {
        SCString schedMsg;
        schedMsg.Format("RTH=%02d:%02d-%02d:%02d ET | SessionKey via ComputeSessionKey()",
            rthStartSec / 3600, (rthStartSec % 3600) / 60,
            rthEndSec / 3600, (rthEndSec % 3600) / 60);
        st->logManager.LogOnce(ThrottleKey::SESSION_DIAG, 0, schedMsg.GetChars(), LogCategory::SESSION, 2);
    }

    // ========================================================================
    // PHASE 0 BOOTSTRAP: O(sessions) discovery at recalc start
    // Runs ONCE per full recalc to identify:
    //   - currentChartSessionKey (session of last bar)
    //   - completedSessions (all prior sessions with bar ranges)
    //   - baselineEligibleSessionKeys (last N sessions, capped at 300 bars)
    // This ensures current session NEVER contributes to baselines.
    // ========================================================================
    if (needsStateInit && sc.UpdateStartIndex == 0)
    {
        st->baselineSessionMgr.DiscoverSessions(sc, rthStartSec, rthEndSec, baselineWindow, 10);
        st->baselineSessionMgr.PopulateHistoricalLevels(sc, diagLevel);
        st->baselineSessionMgr.LogDiscoveryResults(sc, diagLevel);

        // Populate progress-conditioned profile baselines (VA width per bucket)
        PopulateProfileBaselines(sc, st, rthStartSec, rthEndSec, diagLevel);

        // Populate effort baselines (bar-sample distributions per time bucket)
        PopulateEffortBaselines(sc, st, rthStartSec, rthEndSec, diagLevel);
    }

    const bool inLogWindow =
        (logLastN == 0) || (sc.Index >= sc.ArraySize - logLastN);
    const bool isLiveBar = (sc.Index == sc.ArraySize - 1);  // Gate logs to live bar only

#if PERF_TIMING_ENABLED
    // === PERFORMANCE TIMING: Per-bar timer ===
    st->perfTimer.Start();

    // On first bar (full recalc), reset timing stats
    // NOTE: Must check sc.Index == 0 because with AutoLoop=1, sc.UpdateStartIndex stays 0
    // for all bars during a full recalc, but we only want to reset once.
    if (sc.UpdateStartIndex == 0 && sc.Index == 0)
    {
        st->perfStats.Reset();
        st->perfStats.isFullRecalc = true;
        st->perfStats.updateStartIndex = sc.UpdateStartIndex;

        SCString perfMsg;
        perfMsg.Format("Full recalc | ArraySize=%d | UpdateStartIndex=%d | NOOP=%d",
            sc.ArraySize, sc.UpdateStartIndex, NOOP_BODY_TEST);
        st->logManager.LogOnce(ThrottleKey::PERF_DIAG, 0, perfMsg.GetChars(), LogCategory::PERF);
    }
    st->perfStats.studyEnterCount++;
#endif

    // Live mode control for event logging (per SC documentation):
    // - sc.IsFullRecalculation: Skip event logging during full recalc
    // - After full recalc completes: Enable live logging
    // This follows SC's recommended pattern for preventing duplicate processing
    const bool isFullRecalc = sc.IsFullRecalculation;
    st->logManager.SetLiveMode(!isFullRecalc);
    st->initialRecalcComplete = !isFullRecalc;

    const double tickSize = sc.TickSize;
    if (tickSize <= 0.0)
        return;  // Invalid tick size - cannot process

    // --- VbP Data Verification ---
    if (sc.MaintainVolumeAtPriceData == 1 && sc.ArraySize > 0)
    {
        // Only check at the end of calculation to ensure data had a chance to load
        // and avoid log spam
        if (isLiveBar && !st->vbpDataWarningShown)
        {
            if (sc.VolumeAtPriceForBars == nullptr)
            {
                st->logManager.LogWarn(sc.ArraySize - 1,
                    "Volume at Price data requested but missing. Enable 'Chart >> Chart Settings >> Data >> Maintain Volume at Price Data'",
                    LogCategory::VBP);
                st->sessionAccum.configErrorCount++;
                st->vbpDataWarningShown = true;
            }
        }

        // Per SC protocol: Ensure VbP data is fully loaded before accessing
        // (VolumeAtPriceForBars may exist but not yet have all bars populated during chart load)
        if (sc.VolumeAtPriceForBars != nullptr &&
            static_cast<int>(sc.VolumeAtPriceForBars->GetNumberOfBars()) < sc.ArraySize)
        {
            return;  // VbP data still loading - exit and wait for next call
        }
    }

    // =========================================================================
    // NOTE: With AutoLoop=0, sc.UpdateStartIndex handles incremental updates.
    // The loop only runs from UpdateStartIndex to ArraySize.
    // =========================================================================

#if USE_MANUAL_LOOP
    // === PRE-ACQUIRE STUDY ARRAYS (once per call, not per bar) ===
    StudyArrayCache arrayCache;
    arrayCache.Acquire(sc);

    // === MANUAL LOOP: Iterate over bars from UpdateStartIndex to ArraySize ===
    for (int BarIndex = sc.UpdateStartIndex; BarIndex < sc.ArraySize; BarIndex++)
    {
#endif

    // =========================================================================
    // BAR INDEX ABSTRACTION
    // In manual loop mode, use BarIndex. In AutoLoop mode, use sc.Index.
    // =========================================================================
#if USE_MANUAL_LOOP
    const int curBarIdx = BarIndex;
    // Shadow pre-loop variables with correct per-bar values for manual loop
    const bool isLiveBar = (curBarIdx == sc.ArraySize - 1);
    const bool inLogWindow = (logLastN == 0) || (curBarIdx >= sc.ArraySize - logLastN);
#else
    const int curBarIdx = sc.Index;
    // In AutoLoop mode, use the pre-loop isLiveBar and inLogWindow (already defined)
#endif

    // =========================================================================
    // ITEM 1.1: COLLECT OBSERVABLE SNAPSHOT
    // =========================================================================

#if PERF_TIMING_ENABLED
    // Capture time from function entry to here (pre-work overhead)
    if (sc.IsFullRecalculation) {
        st->perfStats.preWorkMs += st->perfTimer.ElapsedMs();
    }
    PerfTimer snapshotTimer;
    snapshotTimer.Start();
#endif

#if USE_MANUAL_LOOP
    CollectObservableSnapshot(sc, st, curBarIdx, st->currentSnapshot, arrayCache);
#else
    CollectObservableSnapshot(sc, st, curBarIdx, st->currentSnapshot);
#endif

#if PERF_TIMING_ENABLED
    if (sc.IsFullRecalculation) {
        st->perfStats.snapshotMs += snapshotTimer.ElapsedMs();
        st->perfStats.snapshotCalls++;
    }
#endif

    // --- Initialize cumDeltaAtSessionStart on first valid cumDelta reading ---
    // FIX: Capture actual NB baseline (NB cumDelta - current bar delta)
    // This handles both NB reset (baseline=0) and non-reset (baseline=prior value)
    // PHASE-AWARE: Only capture during ACTIVE_SESSION, not during baseline accumulation.
    // Primary capture happens in session change block (line ~2256); this is a fallback.
    if (!st->sessionAccum.cumDeltaAtSessionStartValid &&
        std::isfinite(st->currentSnapshot.effort.cumDelta) &&
        st->baselineSessionMgr.currentPhase == AMT::BaselinePhase::ACTIVE_SESSION)
    {
        const double nbCumDelta = st->currentSnapshot.effort.cumDelta;
        const double barDelta = st->currentSnapshot.effort.delta;
        st->sessionAccum.cumDeltaAtSessionStart = nbCumDelta - barDelta;
        st->sessionAccum.lastSeenCumDelta = nbCumDelta;
        st->sessionAccum.cumDeltaAtSessionStartValid = true;
    }

    // =========================================================================
    // VbP AS SSOT FOR SESSION BOUNDARIES + Time-based PHASE CLASSIFICATION
    // =========================================================================
    // VbP's m_StartDateTime detects session BOUNDARIES (when RTH/Globex changes)
    // DetermineExactPhase() provides granular PHASE within session (IB, MID, CLOSING, etc.)

#if PERF_TIMING_ENABLED
    PerfTimer sessionDetectTimer;
    sessionDetectTimer.Start();
#endif

    const int vbpStudyId = sc.Input[20].GetInt();       // VbP Study ID
    const int tSec = TimeToSeconds(st->currentSnapshot.barTime);
    // gbxStartSec already declared above

    // Query VbP for session boundary info (SSOT for RTH vs Globex transition)
    // VBP profile 0 = current session. Session type is derived FROM profile metadata.
    const auto vbpSession = st->sessionVolumeProfile.GetVbPSessionInfo(
        sc, vbpStudyId, false /*ignored*/, rthStartSec, rthEndSec, diagLevel);

    if (vbpSession.valid && vbpSession.sessionStart != 0)
    {
        // Check for session BOUNDARY change: VbP profile's start time changed
        if (st->vbpSessionStart != vbpSession.sessionStart)
        {
            const bool isFirstRead = (st->vbpSessionStart == 0);
            st->vbpSessionStart = vbpSession.sessionStart;
            st->vbpSessionIsEvening = vbpSession.isEvening;

            // PHASE-AWARE: Only log session boundaries during ACTIVE_SESSION phase
            // Suppress logs during baseline accumulation (prior session processing)
            const bool isActivePhase = st->baselineSessionMgr.currentPhase == AMT::BaselinePhase::ACTIVE_SESSION;
            if (!isFirstRead && isActivePhase && diagLevel >= 1)
            {
                SCString msg;
                msg.Format("Session boundary: %s started",
                    vbpSession.isEvening ? "GLOBEX" : "RTH");
                st->logManager.LogThrottled(ThrottleKey::SESSION_CHANGE, curBarIdx, 1, msg.GetChars(), LogCategory::VBP);
            }
        }
    }

    // Session phase classification (DRY: use atomic sync helper)
    // SSOT: Uses DetermineSessionPhase wrapper which handles INCLUSIVE→EXCLUSIVE conversion
    const AMT::SessionPhase newPhase = AMT::DetermineSessionPhase(tSec, rthStartSec, rthEndSec);
    st->SyncSessionPhase(newPhase);

    // DEBUG: Boundary validation hook (logs only near phase transitions to avoid spam)
    // Enable with diagLevel >= 4 to capture phase boundary behavior at 16:14-16:16 ET
#ifdef _DEBUG
    if (diagLevel >= 4)
    {
        // Define boundary windows (±5 seconds of each transition point)
        const int rthEndExcl = rthEndSec + 1;  // Derived EXCLUSIVE end
        const bool nearClosingEnd = (tSec >= rthEndSec - 5 && tSec <= rthEndExcl + 5);       // 16:14:54-16:15:05
        const bool nearIBEnd = (tSec >= rthStartSec + 3595 && tSec <= rthStartSec + 3605);   // 10:29:55-10:30:05
        const bool nearClosingStart = (tSec >= rthEndExcl - 2705 && tSec <= rthEndExcl - 2695); // 15:29:55-15:30:05

        if (nearClosingEnd || nearIBEnd || nearClosingStart)
        {
            SCString msg;
            msg.Format("[PHASE-BOUNDARY-DBG] Bar %d | tSec=%d | rthStartSec=%d rthEndSecIncl=%d rthEndExcl=%d | PHASE=%s",
                curBarIdx, tSec, rthStartSec, rthEndSec, rthEndExcl,
                AMT::SessionPhaseToString(newPhase));
            sc.AddMessageToLog(msg, 0);
        }
    }
#endif

#if PERF_TIMING_ENABLED
    if (sc.IsFullRecalculation) {
        st->perfStats.sessionDetectMs += sessionDetectTimer.ElapsedMs();
    }
#endif

    // =========================================================================
    // ITEM 1.2 & 2.1: SESSION ROUTING + BASELINE UPDATE
    // =========================================================================

    // Track if this is a NEW bar (first time seeing this index)
    const bool isNewBar = (st->lastIndex == -1 || curBarIdx != st->lastIndex);

    // =========================================================================
    // SSOT SESSION KEY COMPUTATION (once per bar, before any session-dependent logic)
    // =========================================================================
    // Compute SessionKey from bar timestamp using chart's RTH/Globex boundaries.
    // This is the SINGLE AUTHORITATIVE source for session identity.
    // All session-scoped resets must derive from SessionManager.ConsumeSessionChange().
    // =========================================================================
    bool sessionKeyChanged = false;
    if (isNewBar)
    {
        const SCDateTime barTime = sc.BaseDateTimeIn[curBarIdx];
        const int barDateYMD = barTime.GetYear() * 10000 + barTime.GetMonth() * 100 + barTime.GetDay();
        const int barTimeSec = AMT::TimeToSeconds(barTime);
        const AMT::SessionKey newSessionKey = AMT::ComputeSessionKey(barDateYMD, barTimeSec, rthStartSec, rthEndSec);

        // UpdateSession returns true if session boundary crossed
        sessionKeyChanged = st->sessionMgr.UpdateSession(newSessionKey);

        // Debug log on transition (guarded by diag level)
        if (sessionKeyChanged && diagLevel >= 1)
        {
            SCString msg;
            const AMT::SessionKey& oldKey = st->sessionMgr.previousSession;
            const AMT::SessionKey& newKey = st->sessionMgr.currentSession;
            // Store strings to avoid dangling pointer from temporary .c_str()
            std::string oldKeyStr = oldKey.IsValid() ? oldKey.ToString() : "INIT";
            std::string newKeyStr = newKey.ToString();
            msg.Format("Bar %d @ %04d-%02d-%02d %02d:%02d:%02d | %s -> %s",
                curBarIdx,
                barTime.GetYear(), barTime.GetMonth(), barTime.GetDay(),
                barTime.GetHour(), barTime.GetMinute(), barTime.GetSecond(),
                oldKeyStr.c_str(),
                newKeyStr.c_str());
            st->logManager.LogThrottled(ThrottleKey::SESSION_CHANGE, curBarIdx, 1, msg.GetChars(), LogCategory::SESSION);
        }

        // =====================================================================
        // CRITICAL FIX: Reset session accumulators BEFORE UpdateSessionBaselines
        // when session changes. This ensures the first bar of the new session
        // is accumulated into the NEW session, not the old one.
        // The later reset block (line ~2348) will be skipped for accumulators
        // since they're already properly initialized here.
        // =====================================================================
        if (sessionKeyChanged)
        {
            // Capture prior values for diagnostic logging
            const double priorSessionVol = st->sessionAccum.sessionTotalVolume;
            const double priorSessionDelta = st->sessionAccum.sessionCumDelta;
            const int priorStartBar = st->sessionAccum.sessionStartBarIndex;

            // Reset accumulators for new session
            st->sessionAccum.sessionTotalVolume = 0.0;
            st->sessionAccum.sessionCumDelta = 0.0;
            st->sessionAccum.firstBarVolume = 0.0;
            st->sessionAccum.firstBarDelta = 0.0;

            // FIX: Capture actual NB baseline at session start
            // NB cumDelta at bar 0 = baseline + bar0's delta
            // So baseline = NB cumDelta - bar0's delta (from SC native arrays)
            // This handles both NB reset (baseline=0) and NB not resetting (baseline=prior value)
            const double nbCumDeltaNow = st->currentSnapshot.effort.cumDelta;
            const double bar0DeltaSC = st->currentSnapshot.effort.delta;  // sc.AskVolume - sc.BidVolume
            st->sessionAccum.cumDeltaAtSessionStart = nbCumDeltaNow - bar0DeltaSC;
            st->sessionAccum.cumDeltaAtSessionStartValid = true;  // Mark as captured at session boundary

            // Set session boundaries: current bar is first bar of new session
            // lastAccumulatedBarIndex = curBarIdx - 1 means "nothing accumulated yet"
            // so the first bar (curBarIdx) will be accumulated by UpdateSessionBaselines
            st->sessionAccum.sessionStartBarIndex = curBarIdx;
            st->sessionAccum.lastAccumulatedBarIndex = curBarIdx - 1;

            // LOGGING FIX: Reset logging tracking on session change
            // This ensures per-session logging state doesn't carry over incorrectly
            st->lastBarCloseStoredBar = curBarIdx - 1;  // Allow first bar to be stored
            st->lastSessionEventBar = curBarIdx - 1;    // Allow first bar events to be logged

            if (diagLevel >= 1)
            {
                SCString resetMsg;
                resetMsg.Format("Bar %d | Prior: startBar=%d vol=%.0f delta=%.0f | "
                    "New: startBar=%d lastAccum=%d | Ready for first bar accumulation",
                    curBarIdx, priorStartBar, priorSessionVol, priorSessionDelta,
                    st->sessionAccum.sessionStartBarIndex, st->sessionAccum.lastAccumulatedBarIndex);
                st->logManager.LogThrottled(ThrottleKey::ACCUM_DIAG, curBarIdx, 1, resetMsg.GetChars(), LogCategory::ACCUM);
            }

            // SESSION TRANSITION: Log session change (baselines persist via historical data)
            // NOTE: liquidityEngine and domWarmup are NOT reset - they accumulate from historical
            // c_ACSILDepthBars data across all sessions during BASELINE_ACCUMULATION phase.
            const AMT::SessionKey& newSessKey = st->sessionMgr.currentSession;
            if (diagLevel >= 1)
            {
                const SCDateTime barTime = sc.BaseDateTimeIn[curBarIdx];
                int hour, minute, second;
                barTime.GetTimeHMS(hour, minute, second);
                const char* sessTypeStr = (newSessKey.sessionType == AMT::SessionType::RTH) ? "RTH" : "GBX";
                SCString transMsg;
                transMsg.Format("Bar %d | Session transition to %s at %02d:%02d:%02d | Baselines persist (historical)",
                    curBarIdx, sessTypeStr, hour, minute, second);
                st->logManager.LogThrottled(ThrottleKey::BASELINE_PHASE, curBarIdx, 1,
                    transMsg.GetChars(), LogCategory::BASELINE);
            }
        }

        // =====================================================================
        // THREE-PHASE EXECUTION: Per-bar phase detection
        // Phase is determined by SESSION MEMBERSHIP (primary gate), not bar index.
        // RTH bars → RTH baseline, GBX bars → GBX baseline (type-matched)
        // - BASELINE_ACCUMULATION: Bar's session in eligible set for its type
        // - ACTIVE_SESSION: Bar's session == currentChartSessionKey
        // NO BAR CAPS - entire sessions only
        // =====================================================================
        const AMT::SessionKey& barSessionKey = st->sessionMgr.currentSession;
        const AMT::BaselinePhase curPhase = st->baselineSessionMgr.UpdatePhase(barSessionKey);

        // Type-matched eligibility: RTH bar → RTH eligible, GBX bar → GBX eligible
        const bool isBaselineEligible = st->baselineSessionMgr.IsBaselineEligibleSession(barSessionKey);
        const bool isActiveSession = st->baselineSessionMgr.IsActiveSessionBar(barSessionKey);
        const bool isBaselinePhase = isBaselineEligible;  // No bar cap - entire sessions only

        // Get session type for baseline routing
        const AMT::SessionType barSessionType = barSessionKey.sessionType;

        // Log phase transitions (rate-limited by inLogWindow)
        static AMT::BaselinePhase lastLoggedPhase = AMT::BaselinePhase::BOOTSTRAP;
        const bool phaseJustChanged = (curPhase != lastLoggedPhase);

        if (phaseJustChanged && diagLevel >= 1)
        {
            SCString phaseMsg;
            phaseMsg.Format("Bar %d | Session=%s | %s -> %s | RTH=%d bars GBX=%d bars",
                curBarIdx,
                barSessionKey.ToString().c_str(),
                AMT::BaselinePhaseToString(lastLoggedPhase),
                AMT::BaselinePhaseToString(curPhase),
                st->baselineSessionMgr.rthBaselineBarCount,
                st->baselineSessionMgr.gbxBaselineBarCount);
            st->logManager.LogThrottled(ThrottleKey::BASELINE_PHASE, curBarIdx, 1, phaseMsg.GetChars(), LogCategory::BASELINE);

            // Special log when entering ACTIVE_SESSION phase
            if (curPhase == AMT::BaselinePhase::ACTIVE_SESSION)
            {
                const char* baselineTypeStr = (barSessionType == AMT::SessionType::RTH) ? "RTH" : "GBX";
                const int baselineBars = (barSessionType == AMT::SessionType::RTH)
                    ? st->baselineSessionMgr.rthBaselineBarCount
                    : st->baselineSessionMgr.gbxBaselineBarCount;
                const int baselineSessions = (barSessionType == AMT::SessionType::RTH)
                    ? static_cast<int>(st->baselineSessionMgr.eligibleRTHSessionKeys.size())
                    : static_cast<int>(st->baselineSessionMgr.eligibleGBXSessionKeys.size());
                const bool baselineReady = st->baselineSessionMgr.IsBaselineReadyForType(barSessionType);

                SCString initMsg;
                initMsg.Format("%s | Using %s baseline | Sessions=%d Bars=%d | Ready=%s",
                    barSessionKey.ToString().c_str(),
                    baselineTypeStr,
                    baselineSessions,
                    baselineBars,
                    baselineReady ? "YES" : "NO (degraded)");
                st->logManager.LogThrottled(ThrottleKey::ACTIVE_SESSION, curBarIdx, 1, initMsg.GetChars(), LogCategory::SESSION);

                // =====================================================================
                // BOOTSTRAP PRIOR VBP: Fetch prior session's VBP from study
                // =====================================================================
                // During baseline phase, we skipped VBP population, so prior values are 0.
                // Fetch the prior session's VBP now to enable PRIOR zones.
                // Prior session type is OPPOSITE of active session type.
                // =====================================================================
                const bool priorIsRTH = (barSessionType == AMT::SessionType::GLOBEX);  // If active is GBX, prior is RTH

                // Ensure tick_size is set (may not be initialized during baseline phase)
                if (st->sessionVolumeProfile.tick_size <= 0.0) {
                    st->sessionVolumeProfile.tick_size = sc.TickSize;
                }

                const bool priorFetchSuccess = st->sessionVolumeProfile.PopulateFromVbPStudy(
                    sc, vbpStudyId, priorIsRTH, rthStartSec, rthEndSec, diagLevel, false, curBarIdx);

                if (priorFetchSuccess)
                {
                    // Capture fetched values as prior session levels
                    const double priorPOC = st->sessionVolumeProfile.session_poc;
                    const double priorVAH = st->sessionVolumeProfile.session_vah;
                    const double priorVAL = st->sessionVolumeProfile.session_val;

                    st->amtZoneManager.sessionCtx.CapturePriorSession(priorPOC, priorVAH, priorVAL, sc.TickSize);

                    SCString priorMsg;
                    priorMsg.Format("Prior VBP fetched: %s POC=%.2f VAH=%.2f VAL=%.2f",
                        priorIsRTH ? "RTH" : "GBX", priorPOC, priorVAH, priorVAL);
                    st->logManager.LogThrottled(ThrottleKey::PRIOR_VBP, curBarIdx, 1, priorMsg.GetChars(), LogCategory::VBP);
                }
                else
                {
                    SCString priorMsg;
                    priorMsg.Format("Prior VBP fetch FAILED for %s session",
                        priorIsRTH ? "RTH" : "GBX");
                    st->logManager.LogWarn(curBarIdx, priorMsg.GetChars(), LogCategory::VBP);
                }
            }

            lastLoggedPhase = curPhase;
        }

#if PERF_TIMING_ENABLED
        PerfTimer baselineTimer;
        baselineTimer.Start();
#endif

        // SESSION-INDEXED GATE: Accumulate to TYPE-MATCHED baseline domain
        if (isBaselinePhase)
        {
            // Phase 1: Historical eligible session → accumulate to matching baseline (RTH or GBX)
            UpdateSessionBaselines(sc, st, st->currentSnapshot, rthStartSec, rthEndSec, gbxStartSec, diagLevel, curBarIdx, barSessionType);
            st->baselineSessionMgr.IncrementBaselineCount(barSessionType);
        }
        else if (isActiveSession)
        {
            // Phase 2: Active session → accumulate to current session's baseline type
            // NOTE: Current session baselines are for live display only, NOT used for extremeness detection
            // Extremeness is always compared against LOCKED baseline from prior sessions
            const AMT::SessionType activeType = st->baselineSessionMgr.currentChartSessionKey.sessionType;
            UpdateSessionBaselines(sc, st, st->currentSnapshot, rthStartSec, rthEndSec, gbxStartSec, diagLevel, curBarIdx, activeType);
        }
        else
        {
            // Non-eligible historical bar - still need to accumulate session volume/delta
            // This ensures TotVol/Net are correct even when baseline accumulation is skipped
            // Use bar's session type for baseline routing (even though baseline isn't updated)
            UpdateSessionBaselines(sc, st, st->currentSnapshot, rthStartSec, rthEndSec, gbxStartSec, diagLevel, curBarIdx, barSessionType);
        }

#if PERF_TIMING_ENABLED
        if (sc.IsFullRecalculation) {
            st->perfStats.baselineMs += baselineTimer.ElapsedMs();
        }
#endif
    }

    st->lastIndex = curBarIdx;

    // =========================================================================
    // THREE-PHASE EARLY EXIT: Skip strategy logic for baseline phase bars
    // =========================================================================
    // During BASELINE_ACCUMULATION phase, bars only contribute to baselines.
    // No zones, probes, engagements, or output signals are computed.
    // This eliminates historical bleed from old session data affecting live logic.
    // =========================================================================
    if (st->baselineSessionMgr.currentPhase == AMT::BaselinePhase::BASELINE_ACCUMULATION)
    {
        // Zero out subgraphs (no signal during baseline phase)
        sc.Subgraph[0][curBarIdx] = 0.0f;
        sc.Subgraph[1][curBarIdx] = 0.0f;
        sc.Subgraph[2][curBarIdx] = 0.0f;

        // Log early-exit on first active session bar (once)
        if (st->baselineSessionMgr.IsActiveSessionBar(st->sessionMgr.currentSession) == false &&
            inLogWindow && diagLevel >= 2)
        {
            // Rate-limited log for baseline bars (per-instance via StudyState, not static)
            if (curBarIdx != st->diagLastBaselineLogBar && (curBarIdx % 100 == 0))
            {
                const AMT::SessionType sessType = st->sessionMgr.currentSession.sessionType;
                const int barCount = st->baselineSessionMgr.GetBaselineBarCount(sessType);
                const int sessCount = st->baselineSessionMgr.GetBaselineSessionCount(sessType);
                const char* typeStr = (sessType == AMT::SessionType::RTH) ? "RTH" : "GBX";
                SCString exitMsg;
                exitMsg.Format("Bar %d | Phase=%s | %s Bars=%d Sessions=%d",
                    curBarIdx,
                    AMT::BaselinePhaseToString(st->baselineSessionMgr.currentPhase),
                    typeStr, barCount, sessCount);
                st->logManager.LogDebug(curBarIdx, exitMsg.GetChars(), LogCategory::BASELINE);
                st->diagLastBaselineLogBar = curBarIdx;
            }
        }
        return;  // Skip all live strategy logic
    }

    // =========================================================================
    // ITEM 1.2: WARM-UP GATE - Skip inference during warm-up
    // =========================================================================

    // WARMUP REMOVED: Historical data on chart is sufficient for baselines
    // No need to wait for live bars - we have days of data already

    // =========================================================================
    // ITEM 2.2 DELIVERABLE: Check if current behavior is extreme vs session
    // =========================================================================
    // SESSION-TYPE BASELINES: Use TYPE-MATCHED baseline for extremeness detection
    // RTH session → compare against RTH baseline (from prior RTH sessions)
    // GBX session → compare against GBX baseline (from prior GBX sessions)
    // This baseline is LOCKED at session start - current session never contributes
    // =========================================================================

    // NOTE: Legacy BaselineEngine consumer code removed (Dec 2024)
    // extremeCheck, deltaConsistency, liquidityAvailability now use new bucket-based APIs
    // Pending migration to EffortBaselineStore, SessionDeltaBaseline, DOMWarmup

    // CLOSED BAR POLICY: Read DOM depth from closed bar for consistency with delta
    // (current bar DOM arrays may lag behind actual order book state)
    double closedBarDepth = 0.0;
    {
        const int closedBarIdx = (curBarIdx > 0) ? (curBarIdx - 1) : 0;

        // Read DOM arrays and get closed bar values
        if (st->domInputsValid) {
            const int domBidStudyId = sc.Input[62].GetStudyID();
            const int domBidSG = sc.Input[62].GetSubgraphIndex();
            const int domAskStudyId = sc.Input[63].GetStudyID();
            const int domAskSG = sc.Input[63].GetSubgraphIndex();

            SCFloatArray domBidArr, domAskArr;
            if (domBidStudyId > 0) sc.GetStudyArrayUsingID(domBidStudyId, domBidSG, domBidArr);
            if (domAskStudyId > 0) sc.GetStudyArrayUsingID(domAskStudyId, domAskSG, domAskArr);

            const double closedBidSize = (domBidArr.GetArraySize() > closedBarIdx) ? domBidArr[closedBarIdx] : 0.0;
            const double closedAskSize = (domAskArr.GetArraySize() > closedBarIdx) ? domAskArr[closedBarIdx] : 0.0;
            closedBarDepth = closedBidSize + closedAskSize;
        }
    }

    // --- DELTA SEMANTIC FIX (Dec 2024) ---
    // deltaConsistency: aggressor FRACTION [0,1] where 0.5=neutral
    //   Formula: 0.5 + 0.5 * deltaPct  (equivalent to AskVol / TotalVol)
    //   Thresholds: >0.7 = extreme buying (70%+ at ask), <0.3 = extreme selling (70%+ at bid)
    // deltaStrength: MAGNITUDE [0,1] where 0=neutral, 1=max one-sided
    //   Formula: |deltaPct|
    //   Used for confidence scoring (direction-agnostic signal strength)
    //
    // CLOSED BAR POLICY (Dec 2024):
    //   Use the most recently CLOSED bar for delta metrics, not the forming bar.
    //   Rationale: sc.AskVolume/sc.BidVolume arrays may lag behind sc.Volume for
    //   the current forming bar, causing false "thin" classifications even when
    //   sc.Volume shows significant activity. Closed bars have complete data.
    //
    // INVARIANT: deltaPct must be signed ratio in [-1, +1]
    //   Source: (AskVol - BidVol) / totalVolume where totalVolume = sc.Volume[idx]
    //   Violation indicates data inconsistency (sc.Volume != AskVol + BidVol)
    //
    // THIN-BAR: Uses AMT_config.h deltaMinVolAbs (SSOT for threshold)
    //   Thin bars get neutral values and invalid flags (prevents false extreme flags)
    {
        // SSOT: Use config threshold (avoids hard-coded constant drift)
        AMT::ZoneConfig cfg;  // Uses defaults from AMT_config.h
        const double thinBarThreshold = cfg.deltaMinVolAbs;  // Default: 20.0

        // Use CLOSED bar (curBarIdx - 1) for reliable delta data
        const int closedBarIdx = (curBarIdx > 0) ? (curBarIdx - 1) : 0;
        const double closedBarAskVol = sc.AskVolume[closedBarIdx];
        const double closedBarBidVol = sc.BidVolume[closedBarIdx];
        const double closedBarVol = sc.Volume[closedBarIdx];
        const double closedBarVolAskBid = closedBarAskVol + closedBarBidVol;

        // Compute deltaPct from closed bar
        const double closedBarDelta = closedBarAskVol - closedBarBidVol;
        const double closedBarDeltaPct = (closedBarVol > 0.0)
            ? closedBarDelta / closedBarVol
            : 0.0;

        // INVARIANT CHECK: deltaPct must be in [-1, +1]
        // Violation means upstream data inconsistency (sc.Volume != Ask+Bid)
        const double absClosedDeltaPct = std::abs(closedBarDeltaPct);
        const bool deltaPctInRange = (absClosedDeltaPct <= 1.0001);  // Small epsilon for float

        if (!deltaPctInRange) {
            // Log invariant violation (rate-limited, per-instance via StudyState)
            if (curBarIdx - st->diagLastViolationBar > 100) {  // Log at most every 100 bars
                // Get bar time for diagnostics
                SCDateTime barTime = sc.BaseDateTimeIn[closedBarIdx];
                int hour, minute, second;
                barTime.GetTimeHMS(hour, minute, second);

                SCString msg;
                msg.Format("[DELTA-INVARIANT] ClosedBar %d @ %02d:%02d:%02d | |deltaPct|=%.4f > 1.0 | "
                    "Ask=%.0f Bid=%.0f scVol=%.0f Sum=%.0f | action=INVALIDATE",
                    closedBarIdx, hour, minute, second,
                    absClosedDeltaPct, closedBarAskVol, closedBarBidVol,
                    closedBarVol, closedBarVolAskBid);
                sc.AddMessageToLog(msg, 1);
                st->diagLastViolationBar = curBarIdx;
            }
            // Treat as invalid (same as thin bar)
            st->amtContext.confidence.deltaConsistency = 0.5f;
            st->amtContext.confidence.deltaConsistencyValid = false;
            st->amtContext.confidence.deltaStrength = 0.0f;
            st->amtContext.confidence.deltaStrengthValid = false;
        }
        else if (closedBarVolAskBid >= thinBarThreshold) {
            // Sufficient volume: compute both metrics
            // deltaConsistency = 0.5 + 0.5 * deltaPct maps [-1,+1] to [0,1] with 0.5=neutral
            const double fraction = 0.5 + 0.5 * closedBarDeltaPct;
            st->amtContext.confidence.deltaConsistency = static_cast<float>((std::max)(0.0, (std::min)(1.0, fraction)));
            st->amtContext.confidence.deltaConsistencyValid = true;

            // deltaStrength = |deltaPct| for scoring (magnitude, direction-agnostic)
            st->amtContext.confidence.deltaStrength = static_cast<float>((std::min)(1.0, absClosedDeltaPct));
            st->amtContext.confidence.deltaStrengthValid = true;
        } else {
            // Thin bar: set to neutral, mark invalid (prevents false extreme flags)
            st->amtContext.confidence.deltaConsistency = 0.5f;  // Neutral fraction
            st->amtContext.confidence.deltaConsistencyValid = false;
            st->amtContext.confidence.deltaStrength = 0.0f;     // No signal
            st->amtContext.confidence.deltaStrengthValid = false;
        }
    }

    // =========================================================================
    // TRUE LIQUIDITY: 3-Component Model (Dec 2024 Rewrite)
    // =========================================================================
    // Components:
    //   1. DepthMass   - Distance-weighted resting volume within Dmax ticks
    //   2. Stress      - Aggressive demand relative to near-touch depth
    //   3. Resilience  - Refill speed after depletion (bar-to-bar)
    //
    // Composite: LIQ = DepthRank * (1 - StressRank) * ResRank
    //
    // NO FALLBACKS: If any baseline not ready, emit LIQ_NOT_READY
    //
    // TEMPORAL COHERENCE (Dec 2024 - Option B):
    //   ALL components use CLOSED BAR data for full temporal alignment:
    //   - DepthMass uses CLOSED BAR DOM via c_ACSILDepthBars API
    //   - Stress uses CLOSED BAR aggressive volumes (sc.AskVolume/BidVolume)
    //   - Reference price uses CLOSED BAR close price
    //
    // REQUIRES: sc.MaintainHistoricalMarketDepthData = 1 (set in SetDefaults)
    //           User must enable "Support Downloading Historical Market Depth Data"
    //           in Global Settings >> Sierra Chart Server Settings
    // =========================================================================
    {
        const double tickSize = sc.TickSize;
        const int maxLevels = sc.Input[14].GetInt();  // Max Depth Levels input
        const double barDurationSec = (sc.SecondsPerBar > 0) ? static_cast<double>(sc.SecondsPerBar) : 60.0;

        // CLOSED BAR INDEX - all components use this for temporal coherence
        const int closedBarIdx = (curBarIdx > 0) ? (curBarIdx - 1) : 0;

        // Reset historical bid/ask validity for this bar (will be set to true if depth data found)
        st->lastLiqSnap.histBidAskValid = false;
        st->lastLiqSnap.histBestBid = 0.0;
        st->lastLiqSnap.histBestAsk = 0.0;
        st->lastLiqSnap.histSpreadTicks = 0.0;

        // Phase of CLOSED BAR (for phase-bucketed DOM baseline)
        SCDateTime closedBarDT = sc.BaseDateTimeIn[closedBarIdx];
        int cbHour, cbMinute, cbSecond;
        closedBarDT.GetTimeHMS(cbHour, cbMinute, cbSecond);
        const int cbTimeSec = cbHour * 3600 + cbMinute * 60 + cbSecond;
        // Uses drift-proof wrapper (handles INCLUSIVE→EXCLUSIVE conversion internally)
        const AMT::SessionPhase closedBarPhase = AMT::DetermineSessionPhase(cbTimeSec, rthStartSec, rthEndSec);

        // Reference price: use CLOSED BAR close price for temporal coherence
        const double refPrice = sc.Close[closedBarIdx];

        // Aggressive volumes from closed bar
        const double closedAskVol = sc.AskVolume[closedBarIdx];  // Aggressive buys
        const double closedBidVol = sc.BidVolume[closedBarIdx];  // Aggressive sells

        // Get historical market depth data via c_ACSILDepthBars
        c_ACSILDepthBars* p_DepthBars = sc.GetMarketDepthBars();

        // Collect historical depth levels into vectors for lambda access
        // Structure: vector of (price, quantity) pairs
        // Last quantities = end-of-bar state, Max quantities = peak during bar
        std::vector<std::pair<double, double>> histBidLevels;      // Last (end-of-bar)
        std::vector<std::pair<double, double>> histAskLevels;      // Last (end-of-bar)
        std::vector<std::pair<double, double>> histMaxBidLevels;   // Peak during bar
        std::vector<std::pair<double, double>> histMaxAskLevels;   // Peak during bar
        bool histDepthAvailable = false;

        // Diagnostic: Log depth API availability (rate-limited, per-instance via StudyState)
        if (isLiveBar && diagLevel >= 2) {
            if (curBarIdx - st->diagLastDepthDiagBar > 100) {
                const bool ptrOk = (p_DepthBars != nullptr);
                const bool dataExists = ptrOk && p_DepthBars->DepthDataExistsAt(closedBarIdx);
                const int numDepthBars = ptrOk ? p_DepthBars->NumBars() : 0;
                SCString diagMsg;
                diagMsg.Format("[LIQ-DIAG] Bar %d | closedBar=%d | DepthBars=%s NumBars=%d | DataExists=%s",
                    curBarIdx, closedBarIdx,
                    ptrOk ? "OK" : "NULL", numDepthBars,
                    dataExists ? "YES" : "NO");
                sc.AddMessageToLog(diagMsg, 0);
                st->diagLastDepthDiagBar = curBarIdx;
            }
        }

        if (p_DepthBars != nullptr && p_DepthBars->DepthDataExistsAt(closedBarIdx)) {
            histDepthAvailable = true;

            // Iterate through all price levels at closed bar
            int priceTickIdx = p_DepthBars->GetBarLowestPriceTickIndex(closedBarIdx);
            int levelsIterated = 0;
            int nonZeroBids = 0;
            int nonZeroAsks = 0;
            do {
                levelsIterated++;
                const float levelPrice = p_DepthBars->TickIndexToPrice(priceTickIdx);

                // Use GetLastDominantSide to determine which side "owns" this price level
                // This prevents crossed markets from historical depth artifacts
                const BuySellEnum dominantSide = p_DepthBars->GetLastDominantSide(closedBarIdx, priceTickIdx);

                if (dominantSide == BSE_BUY) {
                    // This price level is a bid level
                    const int bidQty = p_DepthBars->GetLastBidQuantity(closedBarIdx, priceTickIdx);
                    const int maxBidQty = p_DepthBars->GetMaxBidQuantity(closedBarIdx, priceTickIdx);
                    if (bidQty > 0) {
                        histBidLevels.emplace_back(static_cast<double>(levelPrice),
                                                    static_cast<double>(bidQty));
                        nonZeroBids++;
                    }
                    // Always collect max (even if last is 0 - liquidity was consumed)
                    if (maxBidQty > 0) {
                        histMaxBidLevels.emplace_back(static_cast<double>(levelPrice),
                                                       static_cast<double>(maxBidQty));
                    }
                } else if (dominantSide == BSE_SELL) {
                    // This price level is an ask level
                    const int askQty = p_DepthBars->GetLastAskQuantity(closedBarIdx, priceTickIdx);
                    const int maxAskQty = p_DepthBars->GetMaxAskQuantity(closedBarIdx, priceTickIdx);
                    if (askQty > 0) {
                        histAskLevels.emplace_back(static_cast<double>(levelPrice),
                                                    static_cast<double>(askQty));
                        nonZeroAsks++;
                    }
                    // Always collect max (even if last is 0 - liquidity was consumed)
                    if (maxAskQty > 0) {
                        histMaxAskLevels.emplace_back(static_cast<double>(levelPrice),
                                                       static_cast<double>(maxAskQty));
                    }
                }
                // BSE_UNDEFINED: equal quantities on both sides - skip to avoid ambiguity
            } while (p_DepthBars->GetNextHigherPriceTickIndex(closedBarIdx, priceTickIdx));

            // Diagnostic: Log extraction results (one-time on issue, per-instance via StudyState)
            if (isLiveBar && diagLevel >= 1 && histBidLevels.empty() && histAskLevels.empty()) {
                if (curBarIdx - st->diagLastExtractionDiagBar > 100) {
                    SCString extractMsg;
                    extractMsg.Format("[DEPTH-EXTRACT] Bar %d closedBar=%d | levels=%d | nonZeroBids=%d nonZeroAsks=%d | ALL ZERO",
                        curBarIdx, closedBarIdx, levelsIterated, nonZeroBids, nonZeroAsks);
                    sc.AddMessageToLog(extractMsg, 0);
                    st->diagLastExtractionDiagBar = curBarIdx;
                }
            }

            // Sort bid levels by price descending (best bid first)
            std::sort(histBidLevels.begin(), histBidLevels.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
            std::sort(histMaxBidLevels.begin(), histMaxBidLevels.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });

            // Sort ask levels by price ascending (best ask first)
            std::sort(histAskLevels.begin(), histAskLevels.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            std::sort(histMaxAskLevels.begin(), histMaxAskLevels.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            // Extract historical best bid/ask for Execution Friction (temporal coherence)
            if (!histBidLevels.empty() && !histAskLevels.empty()) {
                st->lastLiqSnap.histBestBid = histBidLevels[0].first;   // Highest bid (sorted desc)
                st->lastLiqSnap.histBestAsk = histAskLevels[0].first;   // Lowest ask (sorted asc)
                if (tickSize > 0.0) {
                    // Use absolute spread - historical depth can show "crossed" markets due to
                    // data artifacts (bid/ask quantities from different points in the bar)
                    st->lastLiqSnap.histSpreadTicks =
                        std::abs(st->lastLiqSnap.histBestAsk - st->lastLiqSnap.histBestBid) / tickSize;
                    st->lastLiqSnap.histBidAskValid = true;
                }

                // Diagnostic: Log collected levels (rate-limited, per-instance via StudyState)
                if (isLiveBar && diagLevel >= 2) {
                    if (curBarIdx - st->diagLastLevelsDiagBar > 50) {
                        SCString lvlMsg;
                        lvlMsg.Format("[LIQ-DIAG] Bar %d | HistDepth: bids=%zu asks=%zu | BestBid=%.2f BestAsk=%.2f spread=%.1f",
                            curBarIdx,
                            histBidLevels.size(), histAskLevels.size(),
                            st->lastLiqSnap.histBestBid, st->lastLiqSnap.histBestAsk,
                            st->lastLiqSnap.histSpreadTicks);
                        sc.AddMessageToLog(lvlMsg, 0);
                        st->diagLastLevelsDiagBar = curBarIdx;
                    }
                }

                // Push historical spread to DOMWarmup (phase-bucketed execution friction baseline)
                st->domWarmup.PushSpread(closedBarPhase, st->lastLiqSnap.histSpreadTicks);

                // Compute historical halo from collected levels and push to DOMWarmup
                // Halo = distance-weighted depth around reference price
                const int haloRadius = sc.Input[15].GetInt();
                double histHaloBidMass = 0.0;
                double histHaloAskMass = 0.0;
                for (const auto& lvl : histBidLevels) {
                    const int distTicks = static_cast<int>(std::abs(lvl.first - refPrice) / tickSize + 0.5);
                    if (distTicks <= haloRadius) {
                        const double weight = 1.0 / (1.0 + distTicks);
                        histHaloBidMass += lvl.second * weight;
                    }
                }
                for (const auto& lvl : histAskLevels) {
                    const int distTicks = static_cast<int>(std::abs(lvl.first - refPrice) / tickSize + 0.5);
                    if (distTicks <= haloRadius) {
                        const double weight = 1.0 / (1.0 + distTicks);
                        histHaloAskMass += lvl.second * weight;
                    }
                }
                const double histHaloTotal = histHaloBidMass + histHaloAskMass;
                if (histHaloTotal > 0.0) {
                    const double histHaloImbalance = (histHaloBidMass - histHaloAskMass) / histHaloTotal;
                    st->domWarmup.PushHalo(closedBarPhase, histHaloTotal, histHaloImbalance);
                }

                // Compute spatial liquidity profile (walls, voids, OBI, POLR)
                st->lastSpatialProfile = st->liquidityEngine.ComputeSpatialProfile(
                    histBidLevels, histAskLevels,
                    refPrice, tickSize,
                    curBarIdx
                );

                // Copy spatial summary to Liq3Result for downstream consumers
                st->liquidityEngine.CopySpatialSummary(st->lastLiqSnap, st->lastSpatialProfile);

                // Push spatial metrics to DOMWarmup baselines (phase-aware)
                if (st->lastSpatialProfile.valid) {
                    st->domWarmup.PushSpatialMetrics(
                        closedBarPhase,
                        st->lastSpatialProfile.meanDepth,
                        st->lastSpatialProfile.direction.orderBookImbalance,
                        st->lastSpatialProfile.direction.polrRatio
                    );
                }
            }
        }

        // Lambda to get historical bid level (from collected vector)
        auto getBidLevel = [&histBidLevels](int level, double& price, double& volume) -> bool {
            if (level < 0 || level >= static_cast<int>(histBidLevels.size())) {
                return false;
            }
            price = histBidLevels[level].first;
            volume = histBidLevels[level].second;
            return (price > 0.0 && volume > 0.0);
        };

        // Lambda to get historical ask level (from collected vector)
        auto getAskLevel = [&histAskLevels](int level, double& price, double& volume) -> bool {
            if (level < 0 || level >= static_cast<int>(histAskLevels.size())) {
                return false;
            }
            price = histAskLevels[level].first;
            volume = histAskLevels[level].second;
            return (price > 0.0 && volume > 0.0);
        };

        // Lambda to get MAX bid level (peak depth during bar)
        auto getMaxBidLevel = [&histMaxBidLevels](int level, double& price, double& volume) -> bool {
            if (level < 0 || level >= static_cast<int>(histMaxBidLevels.size())) {
                return false;
            }
            price = histMaxBidLevels[level].first;
            volume = histMaxBidLevels[level].second;
            return (price > 0.0 && volume > 0.0);
        };

        // Lambda to get MAX ask level (peak depth during bar)
        auto getMaxAskLevel = [&histMaxAskLevels](int level, double& price, double& volume) -> bool {
            if (level < 0 || level >= static_cast<int>(histMaxAskLevels.size())) {
                return false;
            }
            price = histMaxAskLevels[level].first;
            volume = histMaxAskLevels[level].second;
            return (price > 0.0 && volume > 0.0);
        };

        // Preserve hist* fields before Compute() overwrites lastLiqSnap
        const double savedHistBestBid = st->lastLiqSnap.histBestBid;
        const double savedHistBestAsk = st->lastLiqSnap.histBestAsk;
        const double savedHistSpreadTicks = st->lastLiqSnap.histSpreadTicks;
        const bool savedHistBidAskValid = st->lastLiqSnap.histBidAskValid;

        // Compute full liquidity snapshot (with explicit error tracking)
        // Pass spreadTicks for Kyle's Tightness component (consumed computed after)
        // Set phase BEFORE Compute() for phase-aware baseline queries (SSOT: DOMWarmup)
        st->liquidityEngine.SetPhase(closedBarPhase);
        if (st->domInputsValid && refPrice > 0.0 && tickSize > 0.0 && histDepthAvailable) {
            st->lastLiqSnap = st->liquidityEngine.Compute(
                refPrice,
                tickSize,
                maxLevels,
                getBidLevel,
                getAskLevel,
                closedAskVol,
                closedBidVol,
                barDurationSec,
                savedHistBidAskValid ? savedHistSpreadTicks : -1.0  // Kyle's Tightness (spread in ticks)
                // Note: consumed values passed as -1.0 (computed externally below)
            );
            st->lastLiqSnap.errorBar = curBarIdx;  // Tag with bar index for diagnostics

            // Restore hist* fields (extracted separately, not computed by Compute())
            st->lastLiqSnap.histBestBid = savedHistBestBid;
            st->lastLiqSnap.histBestAsk = savedHistBestAsk;
            st->lastLiqSnap.histSpreadTicks = savedHistSpreadTicks;
            st->lastLiqSnap.histBidAskValid = savedHistBidAskValid;

            // Compute PEAK LIQUIDITY (GetMax* functions capture maximum depth during bar)
            // Peak = maximum depth that was available at any point during the bar
            // Consumed = Peak - Ending = depth that was absorbed during the bar
            if (!histMaxBidLevels.empty() || !histMaxAskLevels.empty()) {
                const int dmax = st->liquidityEngine.config.dmaxTicks;
                double peakBidMass = 0.0, peakAskMass = 0.0;

                // Compute peak bid mass
                for (const auto& lvl : histMaxBidLevels) {
                    const double distTicks = (refPrice - lvl.first) / tickSize;
                    if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                        const double weight = 1.0 / (1.0 + distTicks);
                        peakBidMass += lvl.second * weight;
                    }
                }

                // Compute peak ask mass
                for (const auto& lvl : histMaxAskLevels) {
                    const double distTicks = (lvl.first - refPrice) / tickSize;
                    if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                        const double weight = 1.0 / (1.0 + distTicks);
                        peakAskMass += lvl.second * weight;
                    }
                }

                // Populate peak fields
                st->lastLiqSnap.peakBidMass = peakBidMass;
                st->lastLiqSnap.peakAskMass = peakAskMass;
                st->lastLiqSnap.peakDepthMass = peakBidMass + peakAskMass;
                st->lastLiqSnap.peakValid = (peakBidMass > 0.0 || peakAskMass > 0.0);

                // Compute LIQUIDITY CONSUMED during bar (peak - ending)
                // High consumed value = significant depth absorption (aggressive trading)
                // Uses ending depth from Compute() result (st->lastLiqSnap.depthMass etc would require accessing internal)
                // We'll compute from the vectors we already have
                double endingBidMass = 0.0, endingAskMass = 0.0;
                for (const auto& lvl : histBidLevels) {
                    const double distTicks = (refPrice - lvl.first) / tickSize;
                    if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                        const double weight = 1.0 / (1.0 + distTicks);
                        endingBidMass += lvl.second * weight;
                    }
                }
                for (const auto& lvl : histAskLevels) {
                    const double distTicks = (lvl.first - refPrice) / tickSize;
                    if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                        const double weight = 1.0 / (1.0 + distTicks);
                        endingAskMass += lvl.second * weight;
                    }
                }

                // Consumed = max(0, peak - ending) - only positive (depth absorbed)
                st->lastLiqSnap.consumedBidMass = (std::max)(0.0, peakBidMass - endingBidMass);
                st->lastLiqSnap.consumedAskMass = (std::max)(0.0, peakAskMass - endingAskMass);
                st->lastLiqSnap.consumedDepthMass = st->lastLiqSnap.consumedBidMass + st->lastLiqSnap.consumedAskMass;

                // Compute ORDER FLOW TOXICITY PROXY (VPIN-lite)
                // High asymmetry = one side consuming disproportionately = informed flow
                // Formula: |consumedBid - consumedAsk| / (consumedBid + consumedAsk + ε)
                const double consumedTotal = st->lastLiqSnap.consumedDepthMass;
                if (consumedTotal > 1.0) {  // Require meaningful consumption
                    st->lastLiqSnap.toxicityProxy = std::abs(st->lastLiqSnap.consumedBidMass - st->lastLiqSnap.consumedAskMass) / consumedTotal;
                    st->lastLiqSnap.toxicityValid = true;
                }
            }

            // Copy Stack/Pull from ObservableSnapshot (SSOT: read once, copy to Liq3Result)
            // Stack/Pull was already read at bar start into currentSnapshot.liquidity - don't call API again (DRY)
            {
                st->lastLiqSnap.directBidStackPull = st->currentSnapshot.liquidity.bidStackPull;
                st->lastLiqSnap.directAskStackPull = st->currentSnapshot.liquidity.askStackPull;
                st->lastLiqSnap.directStackPullValid = (st->currentSnapshot.liquidity.bidStackPull != 0.0 || st->currentSnapshot.liquidity.askStackPull != 0.0);
            }

            // Copy Diagonal Delta from ObservableSnapshot (SSOT: Liq3Result is authoritative)
            // Was read at bar start into currentSnapshot.effort (staging), now move to SSOT location
            if (st->currentSnapshot.effort.diagonalDeltaValid) {
                st->lastLiqSnap.diagonalPosDeltaSum = st->currentSnapshot.effort.diagonalPosDeltaSum;
                st->lastLiqSnap.diagonalNegDeltaSum = st->currentSnapshot.effort.diagonalNegDeltaSum;
                st->lastLiqSnap.diagonalNetDelta = st->currentSnapshot.effort.diagonalNetDelta;
                st->lastLiqSnap.diagonalDeltaValid = true;
            }

            // Copy Avg Trade Size from ObservableSnapshot (SSOT: Liq3Result is authoritative)
            // Was read at bar start into currentSnapshot.effort (staging), now move to SSOT location
            if (st->currentSnapshot.effort.avgTradeSizeValid) {
                st->lastLiqSnap.avgBidTradeSize = st->currentSnapshot.effort.avgBidTradeSize;
                st->lastLiqSnap.avgAskTradeSize = st->currentSnapshot.effort.avgAskTradeSize;
                st->lastLiqSnap.avgTradeSizeRatio = st->currentSnapshot.effort.avgTradeSizeRatio;
                st->lastLiqSnap.avgTradeSizeValid = true;
            }

            // Wire DOM staleness from DOMQualityTracker to Liq3Result
            // This enables execution decisions to consider live DOM freshness
            if (st->domQualityTracker.IsTimingValid()) {
                st->lastLiqSnap.depthAgeMs = st->domQualityTracker.GetAgeMs();
                st->lastLiqSnap.depthStale = st->domQualityTracker.isStale;

                // If DOM is stale, override action to HARD_BLOCK for safety
                if (st->domQualityTracker.isStale && st->lastLiqSnap.liqValid) {
                    st->lastLiqSnap.recommendedAction = AMT::LiquidityAction::HARD_BLOCK;
                    // Don't invalidate liqValid - the computed values are still valid,
                    // just the data is stale for execution purposes
                }
            }

            // Update amtContext.confidence for backward compatibility
            if (st->lastLiqSnap.liqValid) {
                st->amtContext.confidence.liquidityAvailability = static_cast<float>(st->lastLiqSnap.liq);
                st->amtContext.confidence.liquidityAvailabilityValid = true;
            } else {
                st->amtContext.confidence.liquidityAvailabilityValid = false;
            }
        } else {
            // Pre-Compute validation failed - set specific error reason (F1/F2/F3/F4)
            st->lastLiqSnap = AMT::Liq3Result();
            st->lastLiqSnap.errorBar = curBarIdx;

            // Determine specific error reason (priority order)
            if (!st->domInputsValid) {
                st->lastLiqSnap.errorReason = AMT::LiquidityErrorReason::ERR_DOM_INPUTS_INVALID;
            } else if (refPrice <= 0.0) {
                st->lastLiqSnap.errorReason = AMT::LiquidityErrorReason::ERR_REF_PRICE_INVALID;
            } else if (tickSize <= 0.0) {
                st->lastLiqSnap.errorReason = AMT::LiquidityErrorReason::ERR_TICK_SIZE_INVALID;
            } else if (!histDepthAvailable) {
                st->lastLiqSnap.errorReason = AMT::LiquidityErrorReason::ERR_HIST_DEPTH_UNAVAILABLE;
            }

            // Restore hist* fields (may have been extracted even if Compute() can't run)
            st->lastLiqSnap.histBestBid = savedHistBestBid;
            st->lastLiqSnap.histBestAsk = savedHistBestAsk;
            st->lastLiqSnap.histSpreadTicks = savedHistSpreadTicks;
            st->lastLiqSnap.histBidAskValid = savedHistBidAskValid;

            st->amtContext.confidence.liquidityAvailabilityValid = false;
        }

        // ====================================================================
        // UNIFIED ERROR COUNTING AND LOGGING (No Silent Failures)
        // ====================================================================
        st->liqErrorCounters.IncrementFor(st->lastLiqSnap.errorReason);

        // Rate-limited [LIQ-ERR] logging for all error types
        if (!st->lastLiqSnap.liqValid && isLiveBar) {
            const int rateLimit = st->lastLiqSnap.IsWarmup() ? 50 : 100;  // Warmup logs more often
            if (curBarIdx - st->lastLiqErrLogBar > rateLimit) {
                SCString errMsg;
                errMsg.Format("[LIQ-ERR] Bar %d | %s | reason=%s | depth=%s stress=%s res=%s | samples: d=%zu s=%zu r=%zu | totals: valid=%d err=%d warmup=%d",
                    curBarIdx,
                    st->lastLiqSnap.IsWarmup() ? "WARMUP" : "ERROR",
                    AMT::LiquidityErrorReasonToString(st->lastLiqSnap.errorReason),
                    st->lastLiqSnap.depthBaselineReady ? "OK" : "WAIT",
                    st->lastLiqSnap.stressBaselineReady ? "OK" : "WAIT",
                    st->lastLiqSnap.resilienceBaselineReady ? "OK" : "WAIT",
                    st->liquidityEngine.depthBaselineFallback.Size(),
                    st->liquidityEngine.stressBaseline.Size(),
                    st->liquidityEngine.resilienceBaseline.Size(),
                    st->liqErrorCounters.totalValidBars,
                    st->liqErrorCounters.totalErrorBars,
                    st->liqErrorCounters.warmupBarsCount);
                sc.AddMessageToLog(errMsg, 0);
                st->lastLiqErrLogBar = curBarIdx;
            }
        }
    }

    // ========================================================================
    // COMMON CLOSED BAR COMPUTATIONS (DRY: compute once, use by all engines)
    // ========================================================================
    // All engines (Volatility, Delta, Imbalance) use closed bar data for temporal
    // coherence. Compute these values once here rather than repeating in each block.
    const int closedBarIdx = (curBarIdx > 0) ? (curBarIdx - 1) : 0;
    AMT::SessionPhase closedBarPhase = AMT::SessionPhase::UNKNOWN;
    if (closedBarIdx >= 0) {
        SCDateTime closedBarDT = sc.BaseDateTimeIn[closedBarIdx];
        int cbHour, cbMinute, cbSecond;
        closedBarDT.GetTimeHMS(cbHour, cbMinute, cbSecond);
        const int cbTimeSec = cbHour * 3600 + cbMinute * 60 + cbSecond;
        closedBarPhase = AMT::DetermineSessionPhase(cbTimeSec, rthStartSec, rthEndSec);
    }

    // Bar range in ticks (current bar - used by volatility engine and context builder)
    const double curBarRangeTicks = (tickSize > 0.0) ?
        (sc.High[curBarIdx] - sc.Low[curBarIdx]) / tickSize : 0.0;

    // ========================================================================
    // VOLATILITY ENGINE: Compute regime + pace classification (phase-aware)
    // Uses bar_range from EffortBaselineStore (no new data collection)
    // Pace uses range_velocity: barRangeTicks / barDurationMinutes
    //
    // SYNTHETIC BAR MODE (for 1-min charts):
    // - Aggregates N 1-min bars into synthetic periods for regime detection
    // - Separates execution timeframe (1-min) from regime timeframe (5-min)
    // - 3-bar hysteresis on synthetic = 15 minutes (not 3 minutes)
    // ========================================================================
    {
        // Set phase BEFORE Compute() for phase-aware baseline queries
        st->volatilityEngine.SetPhase(closedBarPhase);

        // Bar duration in seconds (for pace/synthetic calculation)
        const double barDurationSec = static_cast<double>(sc.SecondsPerBar);

        // Use ComputeFromRawBar for synthetic bar aggregation on 1-min charts
        // This aggregates N bars before computing regime (default: 5 bars = 5-min synthetic)
        st->lastVolResult = st->volatilityEngine.ComputeFromRawBar(
            sc.High[curBarIdx], sc.Low[curBarIdx], sc.Close[curBarIdx],
            barDurationSec, tickSize, 0.0);

        // Populate synthetic baseline when a new synthetic bar forms
        // This happens every N raw bars (e.g., every 5 bars for 5-bar aggregation)
        if (st->lastVolResult.newSyntheticBarFormed && st->lastVolResult.usingSyntheticBars) {
            auto& dist = st->effortBaselines.Get(closedBarPhase);
            dist.synthetic_bar_range.push(st->lastVolResult.syntheticRangeTicks);
            dist.synthetic_range_velocity.push(st->lastVolResult.syntheticRangeVelocity);
            // Push efficiency ratio (Kaufman ER: 0=chop, 1=trend)
            if (st->lastVolResult.efficiencyValid) {
                dist.synthetic_efficiency.push(st->lastVolResult.efficiencyRatio);
            }
        }

        // Log-on-change for regime or pace transitions (rate-limited to changes)
        if (isLiveBar && st->lastVolResult.IsReady()) {
            const AMT::VolatilityRegime curRegime = st->lastVolResult.regime;
            const AMT::AuctionPace curPace = st->lastVolResult.pace;

            // Log regime change
            if (curRegime != st->lastLoggedVolRegime) {
                SCString volMsg;
                if (st->lastVolResult.usingSyntheticBars) {
                    volMsg.Format("[VOL] Bar %d | REGIME=%s (was %s) | pctile=%.1f stable=%d | "
                        "SYNTH=%dbar range=%.0fT | tradability: entries=%s breakouts=%s pos=%.2fx",
                        curBarIdx,
                        AMT::VolatilityRegimeToString(curRegime),
                        AMT::VolatilityRegimeToString(st->lastLoggedVolRegime),
                        st->lastVolResult.rangePercentile,
                        st->lastVolResult.stabilityBars,
                        st->lastVolResult.syntheticAggregationBars,
                        st->lastVolResult.syntheticRangeTicks,
                        st->lastVolResult.tradability.allowNewEntries ? "OK" : "BLOCK",
                        st->lastVolResult.tradability.blockBreakouts ? "BLOCK" : "OK",
                        st->lastVolResult.tradability.positionSizeMultiplier);
                } else {
                    volMsg.Format("[VOL] Bar %d | REGIME=%s (was %s) | pctile=%.1f stable=%d | "
                        "tradability: entries=%s breakouts=%s pos=%.2fx",
                        curBarIdx,
                        AMT::VolatilityRegimeToString(curRegime),
                        AMT::VolatilityRegimeToString(st->lastLoggedVolRegime),
                        st->lastVolResult.rangePercentile,
                        st->lastVolResult.stabilityBars,
                        st->lastVolResult.tradability.allowNewEntries ? "OK" : "BLOCK",
                        st->lastVolResult.tradability.blockBreakouts ? "BLOCK" : "OK",
                        st->lastVolResult.tradability.positionSizeMultiplier);
                }
                sc.AddMessageToLog(volMsg, 0);
                st->lastLoggedVolRegime = curRegime;
            }

            // Log pace change (if pace is ready)
            if (st->lastVolResult.IsPaceReady() && curPace != st->lastLoggedPace) {
                SCString paceMsg;
                paceMsg.Format("[VOL-PACE] Bar %d | PACE=%s (was %s) | vel=%.1f t/min pctile=%.1f | "
                    "multipliers: confirm=%.2fx size=%.2fx",
                    curBarIdx,
                    AMT::AuctionPaceToString(curPace),
                    AMT::AuctionPaceToString(st->lastLoggedPace),
                    st->lastVolResult.rangeVelocity,
                    st->lastVolResult.rangeVelocityPercentile,
                    st->lastVolResult.tradability.paceConfirmationMultiplier,
                    st->lastVolResult.tradability.paceSizeMultiplier);
                sc.AddMessageToLog(paceMsg, 0);
                st->lastLoggedPace = curPace;
            }
        }
    }

    // ========================================================================
    // DELTA ENGINE: Participation pressure classification
    // ========================================================================
    // Answers: Is aggression sustained or episodic? Aligned with price or diverging?
    // Uses CLOSED BAR data for temporal coherence with other engines.
    // NOTE: closedBarIdx and closedBarPhase computed above (DRY consolidation)
    {
        if (closedBarIdx >= 0) {
            // Get closed bar data
            const double barDelta = sc.AskVolume[closedBarIdx] - sc.BidVolume[closedBarIdx];
            const double barVolume = sc.Volume[closedBarIdx];
            const double priceChangeTicks = (closedBarIdx > 0)
                ? (sc.Close[closedBarIdx] - sc.Close[closedBarIdx - 1]) / tickSize
                : 0.0;

            // Session aggregates (SSOT: internal accumulation)
            const double sessionCumDelta = st->sessionAccum.sessionCumDelta;
            const double sessionVolume = st->sessionAccum.sessionTotalVolume;

            // ----------------------------------------------------------------
            // CONTEXT EXTRACTION (from other engines)
            // ----------------------------------------------------------------
            // Liquidity context
            const AMT::LiquidityState liqState = st->lastLiqSnap.liqValid
                ? st->lastLiqSnap.liqState : AMT::LiquidityState::LIQ_NOT_READY;
            const double stressRank = st->lastLiqSnap.stressRankValid
                ? (st->lastLiqSnap.stressRank / 100.0) : 0.0;

            // Volatility context
            const AMT::VolatilityRegime volRegime = st->lastVolResult.IsReady()
                ? st->lastVolResult.regime : AMT::VolatilityRegime::UNKNOWN;

            // Dalton context (optional)
            const AMT::AMTMarketState daltonState = st->lastDaltonState.phase;
            const bool is1TF = (st->lastDaltonState.timeframe == AMT::TimeframePattern::ONE_TIME_FRAMING_UP ||
                               st->lastDaltonState.timeframe == AMT::TimeframePattern::ONE_TIME_FRAMING_DOWN);

            // ----------------------------------------------------------------
            // LOCATION CONTEXT (value-relative)
            // ----------------------------------------------------------------
            const double poc = st->sessionVolumeProfile.session_poc;
            const double vah = st->sessionVolumeProfile.session_vah;
            const double val = st->sessionVolumeProfile.session_val;
            const double priorPOC = st->amtZoneManager.sessionCtx.prior_poc;
            const AMT::StructureTracker& structure = st->amtZoneManager.structure;
            const double sessionHigh = structure.GetSessionHigh();
            const double sessionLow = structure.GetSessionLow();
            const double ibHigh = structure.GetIBHigh();
            const double ibLow = structure.GetIBLow();

            AMT::DeltaLocationContext locCtx = AMT::DeltaLocationContext::Build(
                sc.Close[closedBarIdx], poc, vah, val, tickSize,
                2.0,   // edgeToleranceTicks
                8.0,   // discoveryThresholdTicks
                sessionHigh, sessionLow, ibHigh, ibLow, priorPOC);

            // Set phase BEFORE Compute() for phase-aware baseline queries
            st->deltaEngine.SetPhase(closedBarPhase);

            // Build DeltaInput with extended metrics (Jan 2025)
            AMT::DeltaInput deltaInput;
            deltaInput.WithCore(barDelta, barVolume, priceChangeTicks,
                               sessionCumDelta, sessionVolume, closedBarIdx);

            // Add extended inputs from current snapshot if available
            const auto& effort = st->currentSnapshot.effort;
            const double barRangeTicks = (sc.High[closedBarIdx] - sc.Low[closedBarIdx]) / tickSize;
            const double tradesPerSec = effort.tradesSec;
            const double avgBid = effort.avgBidTradeSize;
            const double avgAsk = effort.avgAskTradeSize;

            if (tradesPerSec > 0.0 || barRangeTicks > 0.0 || avgBid > 0.0 || avgAsk > 0.0) {
                deltaInput.WithExtended(barRangeTicks, 0.0, tradesPerSec, avgBid, avgAsk);
            }

            // Compute delta with full context (location + context gates + extended metrics)
            st->lastDeltaResult = st->deltaEngine.Compute(
                deltaInput, locCtx,
                liqState, volRegime, stressRank,
                daltonState, is1TF);

            // Log-on-change for character transitions (rate-limited)
            if (isLiveBar && st->lastDeltaResult.IsReady()) {
                const AMT::DeltaCharacter curChar = st->lastDeltaResult.character;
                const auto& dr = st->lastDeltaResult;
                if (curChar != st->lastLoggedDeltaCharacter ||
                    dr.reversalDetected || dr.divergenceStarted) {

                    // Build tape type suffix
                    const char* tapeSuffix = "";
                    if (dr.thinTapeType == AMT::ThinTapeType::TRUE_THIN) tapeSuffix = " THIN";
                    else if (dr.thinTapeType == AMT::ThinTapeType::HFT_FRAGMENTED) tapeSuffix = " HFT";
                    else if (dr.thinTapeType == AMT::ThinTapeType::INSTITUTIONAL) tapeSuffix = " INST";

                    SCString deltaMsg;
                    deltaMsg.Format("[DELTA] Bar %d | CHAR=%s ALIGN=%s CONF=%s | "
                        "bar=%.0f sess=%.0f vol=%.0f%s%s%s%s",
                        curBarIdx,
                        AMT::DeltaCharacterToString(curChar),
                        AMT::DeltaAlignmentToString(dr.alignment),
                        AMT::DeltaConfidenceToString(dr.confidence),
                        dr.barDeltaPctile, dr.sessionDeltaPctile, dr.volumePctile,
                        dr.reversalDetected ? " !REV" : "",
                        dr.divergenceStarted ? " !DIV" : "",
                        tapeSuffix,
                        dr.rangeAdaptiveApplied ? " (range-adj)" : "");
                    sc.AddMessageToLog(deltaMsg, 0);

                    // Extended metrics line (when baselines ready)
                    if (dr.hasExtendedInputs && (dr.tradesBaselineReady || dr.rangeBaselineReady)) {
                        SCString extMsg;
                        extMsg.Format("[DELTA-EXT] trades_P=%.0f range_P=%.0f avg_P=%.0f | "
                            "noise=%.1f strong=%.1f | hyst: char_req=%d align_req=%d",
                            dr.tradesPctile, dr.rangePctile, dr.avgTradeSizePctile,
                            dr.effectiveNoiseFloor, dr.effectiveStrongSignal,
                            dr.characterConfirmationRequired, dr.alignmentConfirmationRequired);
                        sc.AddMessageToLog(extMsg, 0);
                    }

                    st->lastLoggedDeltaCharacter = curChar;
                }
            }
        }
    }

    // Market composition: Use avg_trade_size baseline from EffortBaselineStore
    // Only valid when: (1) numTrades > 0, (2) baseline ready for current phase
    {
        const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
        const int bucketIdx = AMT::SessionPhaseToBucketIndex(curPhase);

        // Get current bar's trade count from Numbers Bars
        const double curNumTrades = st->currentSnapshot.effort.tradesSec *
            ((sc.SecondsPerBar > 0) ? static_cast<double>(sc.SecondsPerBar) : 60.0);
        const double curBarVolume = st->currentSnapshot.effort.totalVolume;

        if (bucketIdx >= 0 && curNumTrades > 0 && curBarVolume > 0) {
            const auto& bucketDist = st->effortBaselines.Get(curPhase);
            const double curAvgTradeSize = curBarVolume / curNumTrades;

            const AMT::PercentileResult atsResult = bucketDist.avg_trade_size.TryPercentile(curAvgTradeSize);
            if (atsResult.valid) {
                // Higher percentile = larger lots = higher conviction
                st->amtContext.confidence.marketComposition = static_cast<float>(atsResult.value / 100.0);
                st->amtContext.confidence.marketCompositionValid = true;
            } else {
                st->amtContext.confidence.marketCompositionValid = false;
            }
        } else {
            // No trades this bar or non-tradeable phase - cannot compute
            st->amtContext.confidence.marketCompositionValid = false;
        }
    }

    // ========================================================================
    // IMBALANCE ENGINE: Displacement detection (diagonal imbalance, divergence, absorption)
    // ========================================================================
    // Answers: Did something actually move the auction?
    // Uses CLOSED BAR data for temporal coherence with other engines.
    // NOTE: closedBarIdx, closedBarPhase, tickSize computed above (DRY consolidation)
    {
        if (closedBarIdx >= 0) {
            // OHLC for closed bar
            const double high = sc.High[closedBarIdx];
            const double low = sc.Low[closedBarIdx];
            const double close = sc.Close[closedBarIdx];
            const double open = sc.Open[closedBarIdx];

            // Previous bar OHLC (for swing/divergence detection)
            const double prevHigh = (closedBarIdx > 0) ? sc.High[closedBarIdx - 1] : high;
            const double prevLow = (closedBarIdx > 0) ? sc.Low[closedBarIdx - 1] : low;
            const double prevClose = (closedBarIdx > 0) ? sc.Close[closedBarIdx - 1] : close;

            // Profile levels (current session)
            const double poc = st->sessionVolumeProfile.session_poc;
            const double vah = st->sessionVolumeProfile.session_vah;
            const double val = st->sessionVolumeProfile.session_val;

            // Prior profile levels (for value migration)
            const double prevPOC = st->amtZoneManager.sessionCtx.prior_poc;
            const double prevVAH = st->amtZoneManager.sessionCtx.prior_vah;
            const double prevVAL = st->amtZoneManager.sessionCtx.prior_val;

            // Diagonal delta from Numbers Bars (SG43/SG44)
            const double diagPosDelta = st->currentSnapshot.effort.diagonalDeltaValid
                ? st->currentSnapshot.effort.diagonalPosDeltaSum : -1.0;
            const double diagNegDelta = st->currentSnapshot.effort.diagonalDeltaValid
                ? st->currentSnapshot.effort.diagonalNegDeltaSum : -1.0;

            // Volume and delta
            const double totalVolume = sc.Volume[closedBarIdx];
            const double barDelta = sc.AskVolume[closedBarIdx] - sc.BidVolume[closedBarIdx];
            const double cumDelta = st->sessionAccum.sessionCumDelta;

            // Context gates from other engines
            const AMT::LiquidityState liqState = st->lastLiqSnap.liqValid
                ? st->lastLiqSnap.liqState : AMT::LiquidityState::LIQ_NOT_READY;
            const AMT::VolatilityRegime volRegime = st->lastVolResult.IsReady()
                ? st->lastVolResult.regime : AMT::VolatilityRegime::UNKNOWN;
            const double execFriction = st->lastLiqSnap.frictionValid
                ? st->lastLiqSnap.executionFriction : -1.0;

            // IB and session extremes from structure tracker (use accessors, not private members)
            const AMT::StructureTracker& structure = st->amtZoneManager.structure;
            const double ibHigh = structure.GetIBHigh();
            const double ibLow = structure.GetIBLow();
            const double sessionHigh = structure.GetSessionHigh();
            const double sessionLow = structure.GetSessionLow();

            // Dalton state (rotation factor and 1TF)
            const int rotationFactor = st->lastDaltonState.rotationFactor;
            const bool is1TF = (st->lastDaltonState.timeframe == AMT::TimeframePattern::ONE_TIME_FRAMING_UP ||
                               st->lastDaltonState.timeframe == AMT::TimeframePattern::ONE_TIME_FRAMING_DOWN);

            // Set phase BEFORE Compute() for phase-aware baseline queries
            st->imbalanceEngine.SetPhase(closedBarPhase);

            // Compute imbalance/displacement
            st->lastImbalanceResult = st->imbalanceEngine.Compute(
                high, low, close, open,
                prevHigh, prevLow, prevClose,
                tickSize, closedBarIdx,
                poc, vah, val,
                prevPOC, prevVAH, prevVAL,
                diagPosDelta, diagNegDelta,
                totalVolume, barDelta, cumDelta,
                liqState, volRegime, execFriction,
                ibHigh, ibLow, sessionHigh, sessionLow,
                rotationFactor, is1TF
            );

            // Log-on-change for imbalance type transitions (rate-limited)
            if (isLiveBar && st->lastImbalanceResult.IsReady()) {
                const AMT::ImbalanceType curType = st->lastImbalanceResult.confirmedType;
                const bool typeChanged = (curType != st->lastLoggedImbalanceType);
                const bool eventOccurred = st->lastImbalanceResult.imbalanceEntered ||
                                          st->lastImbalanceResult.imbalanceResolved ||
                                          st->lastImbalanceResult.convictionChanged;

                // Log on type change or significant event (rate-limited to 1 per 10 bars minimum)
                if ((typeChanged || eventOccurred) && (curBarIdx - st->lastImbalanceLogBar >= 10)) {
                    SCString imbMsg;
                    imbMsg.Format("[IMB] Bar %d | TYPE=%s DIR=%s CONV=%s | str=%.2f conf=%.2f disp=%.2f",
                        curBarIdx,
                        AMT::ImbalanceTypeToString(curType),
                        AMT::ImbalanceDirectionToString(st->lastImbalanceResult.direction),
                        AMT::ConvictionTypeToString(st->lastImbalanceResult.conviction),
                        st->lastImbalanceResult.strengthScore,
                        st->lastImbalanceResult.confidenceScore,
                        st->lastImbalanceResult.displacementScore);
                    sc.AddMessageToLog(imbMsg, 0);

                    // Detailed diagnostic log at higher levels
                    if (diagLevel >= 2) {
                        SCString imbDetail;
                        imbDetail.Format("[IMB-DETAIL] diag=%.0f/%.0f stack=%d/%d | div=%s absorb=%s trapped=%s | "
                            "gates: liq=%s vol=%s chop=%s",
                            st->lastImbalanceResult.diagonalPosDelta,
                            st->lastImbalanceResult.diagonalNegDelta,
                            st->lastImbalanceResult.stackedBuyLevels,
                            st->lastImbalanceResult.stackedSellLevels,
                            st->lastImbalanceResult.hasDeltaDivergence ? "YES" : "NO",
                            st->lastImbalanceResult.absorptionDetected ? "YES" : "NO",
                            st->lastImbalanceResult.trappedTradersDetected ? "YES" : "NO",
                            st->lastImbalanceResult.contextGate.liquidityOK ? "OK" : "BLOCK",
                            st->lastImbalanceResult.contextGate.volatilityOK ? "OK" : "BLOCK",
                            st->lastImbalanceResult.contextGate.chopOK ? "OK" : "BLOCK");
                        sc.AddMessageToLog(imbDetail, 0);
                    }

                    st->lastLoggedImbalanceType = curType;
                    st->lastImbalanceLogBar = curBarIdx;
                }
            }
        }
    }

    // ========================================================================
    // VOLUME ACCEPTANCE ENGINE: Move confirmation (did volume support or reject?)
    // ========================================================================
    // Answers: Was this move accepted by the market?
    // Uses CLOSED BAR data for temporal coherence with other engines.
    // NOTE: closedBarIdx, closedBarPhase, tickSize computed above (DRY consolidation)
    {
        if (closedBarIdx >= 0) {
            // Price data for closed bar
            const double close = sc.Close[closedBarIdx];
            const double high = sc.High[closedBarIdx];
            const double low = sc.Low[closedBarIdx];

            // Volume data
            const double totalVolume = sc.Volume[closedBarIdx];
            const double bidVolume = sc.BidVolume[closedBarIdx];
            const double askVolume = sc.AskVolume[closedBarIdx];
            const double delta = askVolume - bidVolume;

            // Volume per second from snapshot (if available)
            const double volumePerSecond = st->currentSnapshot.effort.bidVolSec + st->currentSnapshot.effort.askVolSec;

            // Profile levels (current session)
            const double poc = st->sessionVolumeProfile.session_poc;
            const double vah = st->sessionVolumeProfile.session_vah;
            const double val = st->sessionVolumeProfile.session_val;

            // Prior profile levels (for overlap calculation)
            const double priorPOC = st->amtZoneManager.sessionCtx.prior_poc;
            const double priorVAH = st->amtZoneManager.sessionCtx.prior_vah;
            const double priorVAL = st->amtZoneManager.sessionCtx.prior_val;

            // Set phase BEFORE Compute() for phase-aware baseline queries
            st->volumeAcceptanceEngine.SetPhase(closedBarPhase);

            // Set prior session levels for overlap calculation
            if (priorVAH > 0.0 && priorVAL > 0.0) {
                st->volumeAcceptanceEngine.SetPriorSessionLevels(priorPOC, priorVAH, priorVAL);
            }

            // Compute volume acceptance/rejection
            st->lastVolumeResult = st->volumeAcceptanceEngine.Compute(
                close, high, low, tickSize, closedBarIdx,
                totalVolume,
                bidVolume, askVolume, delta,
                poc, vah, val,
                priorPOC, priorVAH, priorVAL,
                volumePerSecond
            );

            // Log-on-change for acceptance state transitions (rate-limited)
            if (isLiveBar && st->lastVolumeResult.IsReady()) {
                const AMT::AcceptanceState curState = st->lastVolumeResult.confirmedState;
                const bool stateChanged = (curState != st->lastLoggedAcceptanceState);
                const bool eventOccurred = st->lastVolumeResult.acceptanceConfirmed ||
                                          st->lastVolumeResult.rejectionConfirmed;

                // Log on state change or significant event (rate-limited to 1 per 10 bars minimum)
                if ((stateChanged || eventOccurred) && (curBarIdx - st->lastVolumeLogBar >= 10)) {
                    SCString volMsg;
                    volMsg.Format("[VOL] Bar %d | STATE=%s INT=%s MIGR=%s | acc=%.2f rej=%.2f mult=%.2f",
                        curBarIdx,
                        AMT::AcceptanceStateToString(curState),
                        AMT::VolumeIntensityToShortString(st->lastVolumeResult.intensity),
                        AMT::ValueMigrationStateToString(st->lastVolumeResult.migration),
                        st->lastVolumeResult.acceptanceScore,
                        st->lastVolumeResult.rejectionScore,
                        st->lastVolumeResult.confirmationMultiplier);
                    sc.AddMessageToLog(volMsg, 0);

                    // Detailed diagnostic log at higher levels
                    if (diagLevel >= 2) {
                        SCString volDetail;
                        volDetail.Format("[VOL-DETAIL] pct=%.1f ratio=%.2f | POC_shift=%.1ft migr=%d | "
                            "VA_ovl=%.2f VA_exp=%.1ft bias=%d | rej: lowVol=%s fast=%s wick=%s delta=%s",
                            st->lastVolumeResult.volumePercentile,
                            st->lastVolumeResult.volumeRatioToAvg,
                            st->lastVolumeResult.pocShiftTicks,
                            st->lastVolumeResult.migrationDirection,
                            st->lastVolumeResult.vaOverlapPct,
                            st->lastVolumeResult.vaExpansionTicks,
                            st->lastVolumeResult.vaExpansionBias,
                            st->lastVolumeResult.lowVolumeBreakout ? "YES" : "NO",
                            st->lastVolumeResult.fastReturn ? "YES" : "NO",
                            st->lastVolumeResult.wickRejection ? "YES" : "NO",
                            st->lastVolumeResult.deltaRejection ? "YES" : "NO");
                        sc.AddMessageToLog(volDetail, 0);
                    }

                    st->lastLoggedAcceptanceState = curState;
                    st->lastVolumeLogBar = curBarIdx;
                }
            }
        }
    }

    // ========================================================================
    // VALUE LOCATION ENGINE: Where am I relative to value and structure?
    // ========================================================================
    // Answers 5 questions:
    //   1. Where am I relative to value? (ValueZone classification)
    //   2. Am I in balance or imbalance structurally? (VA overlap)
    //   3. What session context applies? (Session phase, IB status)
    //   4. What nearby reference levels matter? (Prior levels, IB, HVN/LVN)
    //   5. How does location gate strategies? (Fade in value, breakout from balance)
    // Uses CLOSED BAR data for temporal coherence with other engines.
    // NOTE: closedBarIdx, closedBarPhase, tickSize computed above (DRY consolidation)
    {
        if (closedBarIdx >= 0) {
            // Price data for closed bar
            const double close = sc.Close[closedBarIdx];

            // Profile levels (current session)
            const double poc = st->sessionVolumeProfile.session_poc;
            const double vah = st->sessionVolumeProfile.session_vah;
            const double val = st->sessionVolumeProfile.session_val;

            // Prior profile levels (for overlap calculation)
            const double priorPOC = st->amtZoneManager.sessionCtx.prior_poc;
            const double priorVAH = st->amtZoneManager.sessionCtx.prior_vah;
            const double priorVAL = st->amtZoneManager.sessionCtx.prior_val;

            // Set phase BEFORE Compute() for phase-aware processing
            st->valueLocationEngine.SetPhase(closedBarPhase);

            // Get market state from Dalton (1TF=IMBALANCE, 2TF=BALANCE)
            const AMT::AMTMarketState marketState = st->lastDaltonState.phase;

            // Get HVN/LVN from session volume profile (may be empty)
            const std::vector<double>* hvnLevels = st->sessionVolumeProfile.session_hvn.empty() ?
                nullptr : &st->sessionVolumeProfile.session_hvn;
            const std::vector<double>* lvnLevels = st->sessionVolumeProfile.session_lvn.empty() ?
                nullptr : &st->sessionVolumeProfile.session_lvn;

            // Compute value location
            st->lastValueLocationResult = st->valueLocationEngine.Compute(
                close, tickSize, closedBarIdx,
                poc, vah, val,
                priorPOC, priorVAH, priorVAL,
                st->amtZoneManager.structure,  // Delegate session/IB extremes
                st->amtZoneManager,             // Delegate nearest zone lookup
                hvnLevels, lvnLevels,
                marketState
            );

            // Log-on-change for zone transitions (rate-limited)
            if (isLiveBar && st->lastValueLocationResult.IsReady()) {
                const AMT::ValueZone curZone = st->lastValueLocationResult.confirmedZone;
                const bool zoneChanged = (curZone != st->lastLoggedValueZone);
                const bool eventOccurred = st->lastValueLocationResult.zoneChanged ||
                                          st->lastValueLocationResult.enteredValue ||
                                          st->lastValueLocationResult.exitedValue;

                // Log on zone change or significant event (rate-limited to 1 per 10 bars minimum)
                if ((zoneChanged || eventOccurred) && (curBarIdx - st->lastValueLocationLogBar >= 10)) {
                    SCString valMsg;
                    valMsg.Format("[VAL-LOC] Bar %d | %s",
                        curBarIdx,
                        st->lastValueLocationResult.FormatForLog().c_str());
                    sc.AddMessageToLog(valMsg, 0);

                    // Detailed diagnostic log at higher levels
                    if (diagLevel >= 2) {
                        SCString valDetail;
                        valDetail.Format("[VAL-STRUCT] %s",
                            st->lastValueLocationResult.FormatStructureForLog().c_str());
                        sc.AddMessageToLog(valDetail, 0);

                        SCString valRef;
                        valRef.Format("[VAL-REF] %s",
                            st->lastValueLocationResult.FormatReferencesForLog().c_str());
                        sc.AddMessageToLog(valRef, 0);

                        SCString valGate;
                        valGate.Format("[VAL-GATE] %s | rec=%s",
                            st->lastValueLocationResult.FormatGatingForLog().c_str(),
                            st->lastValueLocationResult.gating.GetRecommendation());
                        sc.AddMessageToLog(valGate, 0);
                    }

                    st->lastLoggedValueZone = curZone;
                    st->lastValueLocationLogBar = curBarIdx;
                }
            }
        }
    }

    // Execution friction: Use historical spread from c_ACSILDepthBars (temporal coherence)
    // Uses spread at CLOSED BAR, matching all other liquidity components
    // NOTE: closedBarIdx, closedBarPhase computed above (DRY consolidation)
    {
        // IMPORTANT: histBidAskValid is set fresh each bar in the liquidity block above.
        // If depth data wasn't available for the closed bar, it will be false.
        const double histSpreadTicks = st->lastLiqSnap.histSpreadTicks;
        const bool histValid = st->lastLiqSnap.histBidAskValid;

        // Diagnostic: Log friction state (rate-limited, per-instance via StudyState)
        if (isLiveBar && diagLevel >= 2 && !histValid) {
            if (curBarIdx - st->diagLastFricDiagBar > 100) {
                const bool spreadReady = st->domWarmup.IsSpreadReady(closedBarPhase);
                SCString fricDiag;
                fricDiag.Format("[FRIC-DIAG] Bar %d | histValid=%d | histBid=%.2f histAsk=%.2f spread=%.1f | phase=%s spreadReady=%d",
                    curBarIdx, histValid ? 1 : 0,
                    st->lastLiqSnap.histBestBid, st->lastLiqSnap.histBestAsk, histSpreadTicks,
                    AMT::SessionPhaseToString(closedBarPhase),
                    spreadReady ? 1 : 0);
                sc.AddMessageToLog(fricDiag, 0);
                st->diagLastFricDiagBar = curBarIdx;
            }
        }

        if (histValid && tickSize > 0.0) {
            if (st->domWarmup.IsSpreadReady(closedBarPhase)) {
                const AMT::PercentileResult spreadResult = st->domWarmup.TrySpreadPercentile(closedBarPhase, histSpreadTicks);

                if (spreadResult.valid) {
                    // Special case: locked market (spread = 0)
                    if (histSpreadTicks == 0.0) {
                        st->amtContext.friction = AMT::ExecutionFriction::LOCKED;
                    } else if (spreadResult.value <= 25.0) {
                        st->amtContext.friction = AMT::ExecutionFriction::TIGHT;
                    } else if (spreadResult.value >= 75.0) {
                        st->amtContext.friction = AMT::ExecutionFriction::WIDE;
                    } else {
                        st->amtContext.friction = AMT::ExecutionFriction::NORMAL;
                    }
                    st->amtContext.frictionValid = true;
                } else {
                    st->amtContext.friction = AMT::ExecutionFriction::UNKNOWN;
                    st->amtContext.frictionValid = false;
                }
            } else {
                // Spread baseline not ready
                st->amtContext.friction = AMT::ExecutionFriction::UNKNOWN;
                st->amtContext.frictionValid = false;
            }
        } else {
            // Historical bid/ask not available - cannot compute spread
            st->amtContext.friction = AMT::ExecutionFriction::UNKNOWN;
            st->amtContext.frictionValid = false;
        }
    }

    // =========================================================================
    // Stage 3: volumeProfileClarity
    // Computed from SessionVolumeProfile with maturity gating + baseline context
    // =========================================================================
    {
        // Determine session type (RTH vs GBX) from sessionMgr
        const bool isCurrentRTH = st->sessionMgr.IsRTH();

        // Build maturity + baseline context
        ProfileClarityContext clarityCtx;
        clarityCtx.sessionBars = curBarIdx - st->sessionAccum.sessionStartBarIndex + 1;

        // Calculate session minutes from bar time
        const SCDateTime curBarTime = sc.BaseDateTimeIn[curBarIdx];
        const int barTimeSec = static_cast<int>(curBarTime.GetTimeInSeconds());
        const int sessionStartTimeSec = isCurrentRTH ? rthStartSec : gbxStartSec;
        const int elapsedSec = barTimeSec - sessionStartTimeSec;
        clarityCtx.sessionMinutes = (elapsedSec > 0) ? (elapsedSec / 60) : 0;

        clarityCtx.sessionTotalVolume = st->sessionAccum.sessionTotalVolume;
        clarityCtx.baseline = isCurrentRTH ? &st->rthProfileBaseline : &st->gbxProfileBaseline;
        clarityCtx.isRTH = isCurrentRTH;

        ProfileClarityResult clarityResult = ComputeVolumeProfileClarity(
            st->sessionVolumeProfile, tickSize, clarityCtx);

        if (clarityResult.valid)
        {
            st->amtContext.confidence.volumeProfileClarity = clarityResult.clarity;
            st->amtContext.confidence.volumeProfileClarityValid = true;

            // Log baseline comparison when available (throttled)
            // NO-FALLBACK POLICY: vaWidthPercentileValid gates access to baseline percentile
            if (clarityResult.vaWidthPercentileValid)
            {
                if (diagLevel >= 2)
                {
                    SCString baselineMsg;
                    baselineMsg.Format("VPC baseline: bucket=%s VA=%d pct=%.0f%% samples=%zu",
                        AMT::ProgressBucketToString(clarityResult.currentBucket),
                        clarityResult.vaWidthTicks,
                        clarityResult.vaWidthPercentile,
                        clarityResult.baselineSamples);
                    st->logManager.LogThrottled(ThrottleKey::AMT_DECISION, curBarIdx, 50, baselineMsg.GetChars(), LogCategory::AMT);
                }
            }

            // NO-FALLBACK POLICY: Log when volume baseline not ready (rate-limited)
            // Reason code: PROFILE_VOLUME_BASELINE_NOT_READY (distinct from "profile thin")
            if (!clarityResult.maturity.volumeSufficiencyValid)
            {
                // Rate limit: every 50 bars (per-instance via StudyState, not static)
                if (curBarIdx - st->diagLastVolBaselineLogBar >= 50)
                {
                    const AMT::HistoricalProfileBaseline* baseline = isCurrentRTH ? &st->rthProfileBaseline : &st->gbxProfileBaseline;
                    const int bucketIdx = static_cast<int>(clarityResult.currentBucket);
                    const size_t priorSessions = (baseline != nullptr) ? baseline->volumeSoFar[bucketIdx].size() : 0;
                    constexpr size_t requiredSessions = 5;  // ProfileAMT::BaselineMinSamples::VA_WIDTH

                    SCString volBaselineMsg;
                    volBaselineMsg.Format(
                        "PROFILE_VOLUME_BASELINE_NOT_READY: domain=%s bucket=%s priorSessions=%zu required=%zu cumVol=%.0f | structural: levels=%s bars=%s mins=%s",
                        isCurrentRTH ? "RTH" : "GBX",
                        AMT::ProgressBucketToString(clarityResult.currentBucket),
                        priorSessions,
                        requiredSessions,
                        clarityResult.maturity.totalVolume,
                        clarityResult.maturity.hasMinLevels ? "PASS" : "FAIL",
                        clarityResult.maturity.hasMinBars ? "PASS" : "FAIL",
                        clarityResult.maturity.hasMinMinutes ? "PASS" : "FAIL");
                    st->logManager.LogInfo(curBarIdx, volBaselineMsg.GetChars(), LogCategory::AMT);
                    st->diagLastVolBaselineLogBar = curBarIdx;
                }
            }

            // NO-FALLBACK POLICY: Log POC share baseline when available (throttled)
            if (clarityResult.currentPocShareValid)
            {
                if (clarityResult.pocSharePercentileValid)
                {
                    if (diagLevel >= 2)
                    {
                        SCString pocShareMsg;
                        pocShareMsg.Format("POC share baseline: bucket=%s share=%.1f%% pct=%.0f%% samples=%zu",
                            AMT::ProgressBucketToString(clarityResult.currentBucket),
                            clarityResult.currentPocShare * 100.0,
                            clarityResult.pocSharePercentile,
                            clarityResult.pocShareBaselineSamples);
                        st->logManager.LogThrottled(ThrottleKey::AMT_DECISION, curBarIdx, 50, pocShareMsg.GetChars(), LogCategory::AMT);
                    }
                }
            }
        }
        else
        {
            st->amtContext.confidence.volumeProfileClarityValid = false;

            // Log maturity gate failure reason (rate-limited, per-instance via StudyState)
            if (!clarityResult.profileMature && clarityResult.maturity.gateFailedReason != nullptr)
            {
                if (curBarIdx - st->diagLastSyntheticLogBar >= 20)  // Rate limit to every 20 bars
                {
                    SCString maturityMsg;
                    maturityMsg.Format("VPC immature: %s (levels=%d vol=%.0f bars=%d mins=%d)",
                        clarityResult.maturity.gateFailedReason,
                        clarityResult.maturity.priceLevels,
                        clarityResult.maturity.totalVolume,
                        clarityResult.maturity.sessionBars,
                        clarityResult.maturity.sessionMinutes);
                    st->logManager.LogInfo(curBarIdx, maturityMsg.GetChars(), LogCategory::AMT);
                    st->diagLastSyntheticLogBar = curBarIdx;
                }
            }
        }
    }

    // =========================================================================
    // Stage 3: tpoAcceptance
    // Computed from TPO study inputs when available + session maturity gate
    // =========================================================================
    {
        const double tpoPOC = st->currentSnapshot.structure.tpoPOC;
        const double tpoVAH = st->currentSnapshot.structure.tpoVAH;
        const double tpoVAL = st->currentSnapshot.structure.tpoVAL;
        const double vbpPOC = st->sessionVolumeProfile.session_poc;  // VBP as reference

        // Session maturity gate for TPO (reuse context from volumeProfileClarity)
        // TPO profile is thin early in session - require minimum development time
        const int tpoSessionBars = curBarIdx - st->sessionAccum.sessionStartBarIndex + 1;
        const SCDateTime tpoBarTime = sc.BaseDateTimeIn[curBarIdx];
        const int tpoBarTimeSec = static_cast<int>(tpoBarTime.GetTimeInSeconds());
        const bool tpoIsRTH = st->sessionMgr.IsRTH();
        const int tpoSessionStartSec = tpoIsRTH ? rthStartSec : gbxStartSec;
        const int tpoElapsedSec = tpoBarTimeSec - tpoSessionStartSec;
        const int tpoSessionMinutes = (tpoElapsedSec > 0) ? (tpoElapsedSec / 60) : 0;

        // Apply maturity gate: require minimum bars and minutes
        const bool tpoMature = (tpoSessionBars >= AMT::ProfileMaturity::MIN_BARS &&
                                tpoSessionMinutes >= AMT::ProfileMaturity::MIN_MINUTES);

        TPOAcceptanceResult tpoResult = ComputeTPOAcceptance(
            tpoPOC, tpoVAH, tpoVAL, vbpPOC, tickSize);

        if (tpoResult.valid && tpoMature)
        {
            st->amtContext.confidence.tpoAcceptance = tpoResult.acceptance;
            st->amtContext.confidence.tpoAcceptanceValid = true;
        }
        else
        {
            st->amtContext.confidence.tpoAcceptanceValid = false;
            // Note: numeric value left unchanged - calculate_score() will skip it

            // Log maturity gate failure (rate-limited)
            if (!tpoMature && tpoResult.valid)
            {
                static int tpoLastLoggedBar = -100;
                if (curBarIdx - tpoLastLoggedBar >= 20)
                {
                    SCString tpoMaturityMsg;
                    tpoMaturityMsg.Format("TPO immature: bars=%d mins=%d (req: bars>=%d mins>=%d)",
                        tpoSessionBars, tpoSessionMinutes,
                        AMT::ProfileMaturity::MIN_BARS, AMT::ProfileMaturity::MIN_MINUTES);
                    st->logManager.LogInfo(curBarIdx, tpoMaturityMsg.GetChars(), LogCategory::AMT);
                    tpoLastLoggedBar = curBarIdx;
                }
            }
        }
    }

    // =========================================================================
    // Stage 3: domStrength
    // Computed from DOM level data when DOM inputs are valid
    // =========================================================================
    {
        if (st->domInputsValid)
        {
            // Build DOM quality snapshot from SC API
            AMT::DOMQualitySnapshot domSnap;
            const int maxLevels = sc.Input[14].GetInt();  // Max Depth Levels

            // Get level counts from SC
            domSnap.bidLevelCount = sc.GetBidMarketDepthNumberOfLevels();
            domSnap.askLevelCount = sc.GetAskMarketDepthNumberOfLevels();

            // Get best bid/ask from current snapshot
            domSnap.bestBid = st->currentSnapshot.liquidity.bestBid;
            domSnap.bestAsk = st->currentSnapshot.liquidity.bestAsk;

            // Count non-zero levels
            int bidNonZero = 0, askNonZero = 0;
            s_MarketDepthEntry e;
            const int bidLevelsToCheck = (std::min)(domSnap.bidLevelCount, maxLevels);
            const int askLevelsToCheck = (std::min)(domSnap.askLevelCount, maxLevels);

            for (int i = 0; i < bidLevelsToCheck; ++i)
            {
                if (sc.GetBidMarketDepthEntryAtLevel(e, i) && e.Quantity > 0)
                    ++bidNonZero;
            }
            for (int i = 0; i < askLevelsToCheck; ++i)
            {
                if (sc.GetAskMarketDepthEntryAtLevel(e, i) && e.Quantity > 0)
                    ++askNonZero;
            }
            domSnap.bidNonZeroCount = bidNonZero;
            domSnap.askNonZeroCount = askNonZero;

            // Compute structure hash for change detection
            domSnap.structureHash = AMT::ComputeDOMStructureHash(
                domSnap.bidLevelCount, domSnap.askLevelCount,
                domSnap.bestBid, domSnap.bestAsk,
                domSnap.bidNonZeroCount, domSnap.askNonZeroCount);

            // Get current system time in milliseconds for staleness detection
            // This enables sub-second staleness tracking for execution decisions
            const int64_t currentTimeMs = static_cast<int64_t>(
                sc.CurrentSystemDateTime.GetMillisecondsSinceBaseDate());

            // Update tracker (for freshness and staleness)
            st->domQualityTracker.Update(domSnap, curBarIdx, currentTimeMs);

            // Compute strength if we have any levels
            if (domSnap.hasAnyLevels())
            {
                const float strength = AMT::ComputeDOMStrength(
                    domSnap, st->domQualityTracker, maxLevels, tickSize);
                st->amtContext.confidence.domStrength = strength;
                st->amtContext.confidence.domStrengthValid = true;
            }
            else
            {
                st->amtContext.confidence.domStrengthValid = false;
            }
        }
        else
        {
            // DOM inputs not configured - mark invalid
            st->amtContext.confidence.domStrengthValid = false;
        }
    }

    // =========================================================================
    // FACILITATION COMPUTATION (Adaptive Percentile-Based)
    // =========================================================================
    // SSOT: Computed once per bar here, stored in amtContext.facilitation
    // Thresholds derived adaptively from EffortBaselineStore bucket distributions:
    //   - LABORED: High volume effort (>=75th pctl) with low range progress (<=25th pctl)
    //   - INEFFICIENT: Low volume effort (<=25th pctl) with high range slip (>=75th pctl)
    //   - FAILED: Very low effort (<=10th pctl) AND very low range (<=10th pctl) - auction stalling
    //   - EFFICIENT: All other conditions (normal facilitation)
    // =========================================================================
    {
        // Use current SessionPhase for bucket selection
        const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
        const int bucketIdx = AMT::SessionPhaseToBucketIndex(curPhase);

        if (bucketIdx >= 0) {  // Tradeable phase
            const auto& bucketDist = st->effortBaselines.Get(curPhase);

            // Compute volume-per-second for current bar
            const double barIntervalSec = (sc.SecondsPerBar > 0) ? static_cast<double>(sc.SecondsPerBar) : 60.0;
            const double curVolSec = st->currentSnapshot.effort.totalVolume / barIntervalSec;

            // Query percentiles from phase-specific baselines (use pre-computed curBarRangeTicks - DRY)
            const AMT::PercentileResult volResult = bucketDist.vol_sec.TryPercentile(curVolSec);
            const AMT::PercentileResult rangeResult = bucketDist.bar_range.TryPercentile(curBarRangeTicks);

            if (volResult.valid && rangeResult.valid) {
                const double volPctile = volResult.value;
                const double rangePctile = rangeResult.value;

                // Classify facilitation based on effort/result relationship
                if (volPctile <= 10.0 && rangePctile <= 10.0) {
                    // Very low effort AND very low range = auction stalling
                    st->amtContext.facilitation = AMT::AuctionFacilitation::FAILED;
                }
                else if (volPctile >= 75.0 && rangePctile <= 25.0) {
                    // High effort with low result = labored auction
                    st->amtContext.facilitation = AMT::AuctionFacilitation::LABORED;
                }
                else if (volPctile <= 25.0 && rangePctile >= 75.0) {
                    // Low effort with high range = inefficient (gap/vacuum)
                    st->amtContext.facilitation = AMT::AuctionFacilitation::INEFFICIENT;
                }
                else {
                    // Normal facilitation
                    st->amtContext.facilitation = AMT::AuctionFacilitation::EFFICIENT;
                }
                st->facilitationComputed = true;
            }
            else {
                // Baselines not ready yet
                st->amtContext.facilitation = AMT::AuctionFacilitation::UNKNOWN;
                st->facilitationComputed = false;
            }
        }
        else {
            // Non-tradeable phase (MAINTENANCE, UNKNOWN) - no baseline data available
            st->amtContext.facilitation = AMT::AuctionFacilitation::UNKNOWN;
            st->facilitationComputed = false;
        }
    }

    // NOTE: Legacy extremeCheck logging removed (Dec 2024) - depended on BaselineEngine

    // =========================================================================
    // PROBE SYSTEM ORCHESTRATION (Using ProbeManager + LogManager)
    // =========================================================================

    const bool probeEnabled = sc.Input[100].GetYesNo();         // Enable Probe System
    const float probeThreshold = sc.Input[101].GetFloat();     // Probe Score Threshold
    const int probeTimeoutRTH = sc.Input[102].GetInt();        // Probe Timeout (RTH)
    const int probeTimeoutGBX = sc.Input[103].GetInt();        // Probe Timeout (GBX)
    const int probeCooldown = sc.Input[104].GetInt();          // Probe Cooldown Bars
    const bool probeRealtimeOnly = sc.Input[105].GetYesNo();   // Probe Real-Time Only

    // Granular logging controls (diagLevel/logLastN already declared above)
    const char* baseLogDir = sc.Input[112].GetString();        // Log: Base Directory
    const int throttleCooldown = sc.Input[113].GetInt();       // Log: Throttle Cooldown
    const bool probeEventsLogging = sc.Input[114].GetYesNo();  // Log: Probe Events (CSV)
    const bool probeResultsLogging = sc.Input[115].GetYesNo(); // Log: Probe Results (CSV)
    const bool amtCsvEnabled = sc.Input[116].GetYesNo();       // Log: AMT Zones (CSV)
    const int csvFlushInterval = sc.Input[119].GetInt();       // Log: CSV Flush Interval
    const bool amtMessageLogEnabled = sc.Input[120].GetYesNo();// Log: AMT to SC Message

    // Phase 5: Module-level diagnostic enables
    const bool enableVBPDiag = sc.Input[121].GetYesNo();       // Log: VBP Diagnostics
    const bool enableSessionDiag = sc.Input[122].GetYesNo();   // Log: Session Diagnostics
    const bool enableZoneDiag = sc.Input[123].GetYesNo();      // Log: Zone Diagnostics
    const bool enableDeltaDiag = sc.Input[124].GetYesNo();     // Log: Delta Diagnostics

    // --- Initialize LogManager FIRST (required for ALL logging, not just probes) ---
    if (!st->logManager.IsInitialized())
    {
        st->logManager.Initialize(sc, "AuctionSensor_v1", baseLogDir);
        // Wire up LogManager to SessionVolumeProfile for centralized logging
        st->sessionVolumeProfile.SetLogManager(&st->logManager);
    }

    // DISABLED: Clear log files - testing if this causes slowdown
    // TODO: Re-enable once performance is verified
    // Phase 2.1: Clear logs on full recalc to prevent duplicate data accumulation
    if (sc.IsFullRecalculation && curBarIdx == 0)
    {
        st->logManager.ClearLogsForFullRecalc();
    }

    // Configure LogManager each bar (allows runtime input changes)
    st->logManager.Configure(
        static_cast<LogLevel>(diagLevel),
        logLastN,
        probeEventsLogging,
        probeResultsLogging,
        amtMessageLogEnabled,
        amtCsvEnabled,
        csvFlushInterval
    );
    st->logManager.ConfigureModuleDiag(enableVBPDiag, enableSessionDiag, enableZoneDiag, enableDeltaDiag);
    st->logManager.UpdateArraySize(sc.ArraySize);

    // --- Configure ProbeManager (if enabled) ---
    if (probeEnabled)
    {
        st->probeMgr.cooldown_bars = probeCooldown;
        st->probeMgr.realtime_only = probeRealtimeOnly;

        // Detect backfill completion (first time we reach last bar)
        if (isLiveBar && !st->probeMgr.IsBackfillComplete())
        {
            st->probeMgr.OnBackfillComplete(curBarIdx);

            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format("Backfill complete at bar %d.", curBarIdx);
                st->logManager.LogThrottled(ThrottleKey::BACKFILL_COMPLETE, curBarIdx, 1, msg.GetChars(), LogCategory::PROBE);
            }

        }
    }

    const double probeOpen = sc.Open[curBarIdx];
    const double probeClose = sc.Close[curBarIdx];
    const double probeHigh = sc.High[curBarIdx];
    const double probeLow = sc.Low[curBarIdx];
    const double probeBidVol = st->currentSnapshot.effort.bidVolSec;
    const double probeAskVol = st->currentSnapshot.effort.askVolSec;
    const SCDateTime probeBarTime = sc.BaseDateTimeIn[curBarIdx];

    // Session phase from SSOT coordinator (already updated earlier in this function)
    const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
    const AMT::SessionPhase prevPhase = st->phaseCoordinator.GetPrevPhase();
    const bool isCurRTH = st->sessionMgr.IsRTH();
    const bool isPrevRTH = st->sessionMgr.previousSession.IsRTH();

    // =========================================================================
    // SESSION TRANSITION DETECTION (SSOT: SessionManager.ConsumeSessionChange())
    // =========================================================================
    // ConsumeSessionChange() returns true ONCE per session change, then auto-clears.
    // This eliminates the need for manual ClearSessionChanged() calls.
    // Triggers on: (1) First valid SessionKey, (2) Session boundary crossings.
    // Legacy profileNeedsInit fallback removed - first bar handled by UpdateSession().
    // =========================================================================
    const bool sessionChanged = (curPhase != AMT::SessionPhase::UNKNOWN &&
                                 st->sessionMgr.ConsumeSessionChange());

    if (sessionChanged)
    {
        // =====================================================================
        // SESSION BOUNDARY RESET - APPLIES TO BOTH LIVE AND HISTORICAL BARS
        // =====================================================================
        // INVARIANT: Session boundaries (RTH <-> Globex) create logically isolated
        // contexts for zones, engagements, and statistics. This segmentation MUST
        // occur regardless of live/historical to ensure:
        // 1. Zones from prior session cannot be selected by current-session logic
        // 2. Engagement stats are session-scoped, not cross-session aggregated
        // 3. Phase history reflects current session only
        // =====================================================================

        // --- EXACTLY-ONCE GUARD: Compute EARLY to protect all event logging ---
        // Session ID = hash of SessionKey (tradingDay * 10 + isRTH flag)
        // This prevents duplicate event logging if recalculation re-triggers session boundary
        const AMT::SessionKey& curSessionKeyEarly = st->sessionMgr.currentSession;
        const int sessionIdForLogging = curSessionKeyEarly.tradingDay * 10 + (curSessionKeyEarly.IsRTH() ? 1 : 0);
        const bool isDuplicateSessionEvent = (sessionIdForLogging == st->sessionAccum.lastResetSessionId);

        // LOG SESSION TRANSITION WITH SSOT SessionKey (replaces legacy RTH/GLOBEX string)
        {
            const AMT::SessionKey& oldKey = st->sessionMgr.previousSession;
            const AMT::SessionKey& newKey = st->sessionMgr.currentSession;
            const bool tradingDayRolled = (oldKey.IsValid() && oldKey.tradingDay != newKey.tradingDay);

            // Store strings to avoid dangling pointer from temporary .c_str()
            std::string oldKeyStr = oldKey.IsValid() ? oldKey.ToString() : "INIT";
            std::string newKeyStr = newKey.ToString();
            SCString msg1;
            msg1.Format("Bar %d | SessionKey: %s -> %s%s",
                curBarIdx,
                oldKeyStr.c_str(),
                newKeyStr.c_str(),
                tradingDayRolled ? " [TRADING DAY ROLL]" : "");
            st->logManager.LogThrottled(ThrottleKey::SESSION_CHANGE, curBarIdx, 1, msg1.GetChars(), LogCategory::SESSION);

            // Show ending session stats BEFORE reset
            SCString msg2;
            msg2.Format("ENDING: Engagements=%d Touches(VAH=%d POC=%d VAL=%d) PhaseHistory=%d",
                st->sessionAccum.engagementCount,
                st->engagementAccum.vah.touchCount,
                st->engagementAccum.poc.touchCount,
                st->engagementAccum.val.touchCount,
                static_cast<int>(st->amtPhaseHistory.size()));
            st->logManager.LogInfo(curBarIdx, msg2.GetChars(), LogCategory::SESSION);

            SCString msg3;
            msg3.Format("ENDING: HVN(+%d/-%d) LVN(+%d/-%d) Extremes=%d Probes=%d",
                st->sessionAccum.hvnAdded, st->sessionAccum.hvnRemoved,
                st->sessionAccum.lvnAdded, st->sessionAccum.lvnRemoved,
                st->sessionAccum.extremeVolumeCount + st->sessionAccum.extremeDeltaCount +
                st->sessionAccum.extremeTradesCount + st->sessionAccum.extremeStackCount +
                st->sessionAccum.extremePullCount + st->sessionAccum.extremeDepthCount,
                st->sessionAccum.probesFired);
            st->logManager.LogInfo(curBarIdx, msg3.GetChars(), LogCategory::SESSION);

            // v1.2: Finalize behavior outcome tracking for ending session
            st->behaviorMgr.FinalizeSession(curBarIdx);

            // Finalize volatility engine (updates EWMA priors for next session)
            st->volatilityEngine.FinalizeSession();

            // Log behavior outcome (structured, one per session)
            if (st->behaviorMgr.frozen) {
                const auto& obs = st->behaviorMgr.observation;
                const auto& hyp = st->behaviorMgr.hypothesis;
                const bool matched = st->behaviorMgr.WasHypothesisCorrect();
                const AMT::ProfileShape shape = obs.frozen.shape;

                // Record to history tracker (accumulates across sessions)
                st->behaviorHistory.RecordSession(shape, matched);

                // Get current hit rate for this shape
                int attempts = 0, matches = 0;
                float hitRate = 0.0f;
                st->behaviorHistory.GetStats(shape, attempts, matches, hitRate);
                const float confMult = st->behaviorHistory.GetConfidenceMultiplier(shape);

                SCString behaviorMsg;
                behaviorMsg.Format("BEHAVIOR: t_freeze=%d outcome=%s hypothesis=%s match=%s | "
                    "POC_0=%.2f VAH_0=%.2f VAL_0=%.2f W_va=%.2f",
                    obs.frozen.t_freeze,
                    AMT::BehaviorOutcomeToString(obs.outcome),
                    AMT::HypothesisTypeToString(hyp.hypothesis),
                    matched ? "YES" : "NO",
                    obs.frozen.POC_0, obs.frozen.VAH_0, obs.frozen.VAL_0, obs.frozen.W_va);
                st->logManager.LogInfo(curBarIdx, behaviorMsg.GetChars(), LogCategory::SESSION);

                // Log historical hit rate for this shape
                SCString historyMsg;
                historyMsg.Format("BEHAVIOR-HIST: shape=%s attempts=%d matches=%d hitRate=%.1f%% confMult=%.2f",
                    AMT::ProfileShapeToString(shape),
                    attempts, matches, hitRate * 100.0f, confMult);
                st->logManager.LogInfo(curBarIdx, historyMsg.GetChars(), LogCategory::SESSION);
            }
        }

        // Clear phase history for new session
        // Prevents stale/incorrect phase classifications from prior session
        st->amtPhaseHistory.clear();

        // Reset PhaseTracker internal buffers (history + POC distance) for new session
        st->amtPhaseTracker.Reset();

        // NOTE: MarketStateTracker removed - AMTSignalEngine handles state tracking now
        // Session boundary processing for other components
        {
            // PHASE 2 FINALIZATION: Classify ending session before reset
            // TryClassifyAtSessionEnd() sets BALANCED if no RE was accepted (pure rotation)
            // This must happen BEFORE Reset() destroys the classification state
            {
                const bool classified = st->dayTypeClassifier.TryClassifyAtSessionEnd(curBarIdx, probeBarTime);
                if (classified && diagLevel >= 1) {
                    char reSummary[64];
                    st->dayTypeClassifier.FormatRESummary(reSummary, sizeof(reSummary));
                    SCString classMsg;
                    classMsg.Format("Bar %d | DAYTYPE (session end): %s | %s",
                        curBarIdx,
                        AMT::to_string(st->dayTypeClassifier.GetClassification()),
                        reSummary);
                    st->logManager.LogInfo(curBarIdx, classMsg.GetChars(), LogCategory::DAYTYPE);
                }
            }

            // Reset day type classifier for new session
            st->dayTypeClassifier.Reset(curBarIdx);

            // v1.2: Reset behavior mapping for new session
            st->behaviorMgr.Reset();
        }

        // Reset zones with force-finalize (records pending engagements to accumulators)
        // CRITICAL: ResetForSession() calls ForceFinalizePendingEngagements() internally,
        // ensuring no engagement data is lost before zones are destroyed.
        // This also fixes "ghost zones" - zones from prior session can no longer be selected.
        {
            SCString sessMsg;
            sessMsg.Format("Bar %d | BEFORE ResetForSession: activeZones=%d pocId=%d vahId=%d valId=%d initialized=%d",
                curBarIdx,
                static_cast<int>(st->amtZoneManager.activeZones.size()),
                st->amtZoneManager.pocId, st->amtZoneManager.vahId, st->amtZoneManager.valId,
                st->amtZonesInitialized ? 1 : 0);
            st->logManager.LogDebug(curBarIdx, sessMsg.GetChars(), LogCategory::SESSION);
        }

        // SSOT: Capture finalized session levels as prior_* BEFORE reset
        // This preserves the previous session's POC/VAH/VAL for reference
        {
            // DIAGNOSTIC: Log sessionMgr values being captured as PRIOR
            SCString capMsg;
            capMsg.Format("Bar %d | sessionMgr: POC=%.2f VAH=%.2f VAL=%.2f | sessionVolumeProfile: POC=%.2f VAH=%.2f VAL=%.2f",
                curBarIdx,
                st->sessionMgr.GetPOC(), st->sessionMgr.GetVAH(), st->sessionMgr.GetVAL(),
                st->sessionVolumeProfile.session_poc, st->sessionVolumeProfile.session_vah, st->sessionVolumeProfile.session_val);
            st->logManager.LogDebug(curBarIdx, capMsg.GetChars(), LogCategory::VBP);
        }
        st->amtZoneManager.sessionCtx.CapturePriorSession(st->sessionMgr.GetPOC(), st->sessionMgr.GetVAH(), st->sessionMgr.GetVAL(), sc.TickSize);

        // =====================================================================
        // DALTON ADVANCED: Capture prior session spike & extremes, compute value migration
        // =====================================================================
        {
            // 1. Preserve spike context from prior session (if any)
            st->priorSessionSpike = st->lastDaltonState.spikeContext;

            // 2. Capture prior session extremes for reference
            st->priorSessionHigh = st->amtZoneManager.structure.GetSessionHigh();
            st->priorSessionLow = st->amtZoneManager.structure.GetSessionLow();

            // 3. Compute value migration (current VA vs prior VA)
            // Current VA = values from ending session
            // Prior VA = prior_vah/prior_val captured from previous session
            const double curVAH = st->sessionMgr.GetVAH();
            const double curVAL = st->sessionMgr.GetVAL();
            const double priorVAH = st->amtZoneManager.sessionCtx.prior_vah;
            const double priorVAL = st->amtZoneManager.sessionCtx.prior_val;

            // Compute and store for new session
            st->lastDaltonState.valueMigration = AMT::ComputeValueMigration(
                curVAH, curVAL, priorVAH, priorVAL);

            // Log value migration on transition
            if (diagLevel >= 1) {
                SCString vmMsg;
                vmMsg.Format("Bar %d | VALUE_MIGRATION: %s | curVA=[%.2f-%.2f] priorVA=[%.2f-%.2f]",
                    curBarIdx,
                    AMT::ValueMigrationToString(st->lastDaltonState.valueMigration),
                    curVAL, curVAH,
                    priorVAL, priorVAH);
                st->logManager.LogInfo(curBarIdx, vmMsg.GetChars(), LogCategory::AMT);
            }

            // 4. Reset session open capture flag for new session
            st->sessionOpenCaptured = false;

            // 5. Reset level acceptance engine for new session
            st->levelAcceptance.Reset();
            st->levelAcceptance.SetTickSize(sc.TickSize);

            // 6. Reset volatility engine for new session (preserves priors)
            st->volatilityEngine.ResetForSession();

            // 7. Reset delta engine for new session (clears history, preserves hysteresis)
            st->deltaEngine.ResetForSession();

            // 8. Reset imbalance engine for new session (clears swing points, preserves hysteresis)
            st->imbalanceEngine.ResetForSession();

            // 9. Reset volume acceptance engine for new session (clears price history, preserves baselines)
            st->volumeAcceptanceEngine.ResetForSession();

            // 10. Reset session volume profile structure for new session (includes opening range tracker)
            // Pass isRTH to configure session-appropriate gates (IB vs SOR)
            st->sessionVolumeProfile.ResetForNewSession(st->sessionMgr.IsRTH());
        }

        st->amtZoneManager.ResetForSession(curBarIdx, probeBarTime);

        // CRITICAL: Reset amtZonesInitialized so zones are recreated for new session.
        // Without this, needsZoneCreation stays false after the session boundary bar,
        // and zones are never recreated (they just stay cleared).
        st->amtZonesInitialized = false;

        // Reset facilitation state for new session warmup
        // (facilSessionSamples tracks per-session samples, so FACIL will be UNKNOWN until minSamples reached)
        st->facilitationComputed = false;
        st->facilSessionSamples = 0;

        // SSOT: Record session start bar in SessionManager (single source of truth)
        st->sessionMgr.SetSessionStartBar(curBarIdx);

        // --- Capture NB baseline for NEW session BEFORE Reset() ---
        // FIX: Instead of assuming NB resets to 0, capture actual baseline.
        // NB cumDelta at bar 0 = baseline + bar0's delta
        // So baseline = NB cumDelta - bar0's delta (from SC native arrays)
        {
            const double priorCumDeltaStart = st->sessionAccum.cumDeltaAtSessionStart;
            const double priorSessionVolume = st->sessionAccum.sessionTotalVolume;
            const double currentCumDelta = st->currentSnapshot.effort.cumDelta;
            const double currentBarDelta = st->currentSnapshot.effort.delta;

            // FIX: Capture actual NB baseline (handles both reset and non-reset cases)
            const double newBaseline = currentCumDelta - currentBarDelta;
            st->sessionAccum.cumDeltaAtSessionStart = newBaseline;

            // BOUNDARY AUDIT LOG: Show actual captured baseline
            if (diagLevel >= 1)
            {
                const SCDateTime barTime = st->currentSnapshot.barTime;
                const bool isRTH = st->sessionMgr.IsRTH();
                SCString auditMsg;
                auditMsg.Format("Bar %d @ %04d-%02d-%02d %02d:%02d:%02d | "
                    "Session=%s | DidSessionChange=YES | "
                    "NB_cumDelta=%.0f | SC_barDelta=%.0f | baseline=%.0f | "
                    "priorVol=%.0f | priorBaseline=%.0f",
                    curBarIdx,
                    barTime.GetYear(), barTime.GetMonth(), barTime.GetDay(),
                    barTime.GetHour(), barTime.GetMinute(), barTime.GetSecond(),
                    isRTH ? "RTH" : "GLOBEX",
                    currentCumDelta,
                    currentBarDelta,
                    newBaseline,
                    priorSessionVolume,
                    priorCumDeltaStart);
                st->logManager.LogThrottled(ThrottleKey::AUDIT_DIAG, curBarIdx, 1, auditMsg.GetChars(), LogCategory::AUDIT);
            }
        }

        // Capture first-bar flags BEFORE Reset() for re-application
        const bool firstBarDomStaleCapture = st->sessionAccum.firstBarDomStale;

        // =====================================================================
        // CHECK: Did the early reset already run (in the sessionKeyChanged block)?
        // If sessionStartBarIndex == sc.Index, the early reset properly initialized
        // accumulators and UpdateSessionBaselines() may have already accumulated
        // the first bar's delta. We must NOT wipe that out with Reset().
        // =====================================================================
        const bool earlyResetAlreadyRan = (st->sessionAccum.sessionStartBarIndex == curBarIdx);

        if (earlyResetAlreadyRan)
        {
            // Early reset handled accumulators - only reset non-accumulator fields
            // This preserves sessionTotalVolume, sessionCumDelta, and session indices
            st->sessionAccum.hvnAdded = st->sessionAccum.hvnRemoved = 0;
            st->sessionAccum.lvnAdded = st->sessionAccum.lvnRemoved = 0;
            st->sessionAccum.engagementCount = st->sessionAccum.escapeCount = 0;
            st->sessionAccum.totalEngagementBars = 0;
            st->sessionAccum.totalEscapeVelocity = 0.0;
            st->sessionAccum.extremeVolumeCount = st->sessionAccum.extremeDeltaCount = 0;
            st->sessionAccum.extremeTradesCount = st->sessionAccum.extremeStackCount = 0;
            st->sessionAccum.extremePullCount = st->sessionAccum.extremeDepthCount = 0;
            st->sessionAccum.domStaleCount = st->sessionAccum.pocDriftCount = 0;
            st->sessionAccum.profileRefreshCount = 0;
            st->sessionAccum.probesFired = st->sessionAccum.probesResolved = 0;
            st->sessionAccum.probesHit = st->sessionAccum.probesMissed = 0;
            st->sessionAccum.probesExpired = 0;
            st->sessionAccum.totalProbeScore = 0.0;
            st->sessionAccum.sessionChangeCount = st->sessionAccum.phaseTransitionCount = 0;
            st->sessionAccum.intentChangeCount = st->sessionAccum.marketStateChangeCount = 0;
            st->sessionAccum.zoneWidthMismatchCount = st->sessionAccum.validationDivergenceCount = 0;
            st->sessionAccum.configErrorCount = st->sessionAccum.vbpWarningCount = 0;
            st->sessionAccum.firstBarDomStale = false;
            // NOTE: NOT touching sessionTotalVolume, sessionCumDelta, firstBarVolume,
            // firstBarDelta, sessionStartBarIndex, lastAccumulatedBarIndex

            if (diagLevel >= 1)
            {
                SCString skipMsg;
                skipMsg.Format("Bar %d | Early reset already ran | "
                    "Preserving: vol=%.0f delta=%.0f startIdx=%d lastAccum=%d",
                    curBarIdx, st->sessionAccum.sessionTotalVolume, st->sessionAccum.sessionCumDelta,
                    st->sessionAccum.sessionStartBarIndex, st->sessionAccum.lastAccumulatedBarIndex);
                st->logManager.LogDebug(curBarIdx, skipMsg.GetChars(), LogCategory::ACCUM);
            }
        }
        else
        {
            // Full reset - early reset did not run (e.g., profileNeedsInit case)
            st->sessionAccum.Reset();

            // Set session boundaries
            st->sessionAccum.sessionStartBarIndex = curBarIdx;
            st->sessionAccum.lastAccumulatedBarIndex = curBarIdx - 1;
            st->sessionAccum.firstBarVolume = 0.0;
            st->sessionAccum.firstBarDelta = 0.0;
        }

        st->engagementAccum.Reset();  // Per-anchor engagement stats for new session
        st->lastAmtCsvLoggedBar = -1;  // Allow fresh logging for new session
        st->lastStatsLoggedBar = -1;  // Allow fresh stats for new session

        // Reset zone transition tracking for new session
        // SSOT FIX: Prevents stale zone IDs/proximity from prior session causing false transitions
        st->zoneTransitionState.Reset();
        st->zoneTransitionMemory.Reset();
        st->zoneContextSnapshot.Reset();

        // Reset extreme acceptance tracking for new session
        st->extremeTracker.OnSessionReset();

        // Reset AMT signal engine for new session
        st->amtSignalEngine.ResetSession();
        st->singlePrintZones.clear();
        st->lastStateEvidence.Reset();

        // =====================================================================
        // DALTON SESSION BRIDGE (Jan 2025)
        // =====================================================================
        // Handle prior RTH capture and overnight session setup at session boundaries.
        // This enables gap calculation and opening type classification.
        // =====================================================================
        const bool wasRTH = st->sessionMgr.previousSession.IsRTH();
        const bool isNowRTH = st->sessionMgr.currentSession.IsRTH();
        const bool isNowGlobex = st->sessionMgr.currentSession.IsGlobex();

        // RTH → GLOBEX: Capture prior RTH levels for gap calculation
        if (wasRTH && isNowGlobex) {
            const double rthHigh = st->amtZoneManager.GetSessionHigh();
            const double rthLow = st->amtZoneManager.GetSessionLow();
            const double rthClose = (curBarIdx > 0) ? sc.Close[curBarIdx - 1] : sc.Close[curBarIdx];
            st->sessionMgr.CapturePriorRTH(rthHigh, rthLow, rthClose);

            if (diagLevel >= 1) {
                SCString bridgeMsg;
                bridgeMsg.Format("Bar %d | PRIOR RTH CAPTURED: H=%.2f L=%.2f C=%.2f POC=%.2f VAH=%.2f VAL=%.2f",
                    curBarIdx, rthHigh, rthLow, rthClose,
                    st->sessionMgr.GetPriorRTHPOC(),
                    st->sessionMgr.GetPriorRTHVAH(),
                    st->sessionMgr.GetPriorRTHVAL());
                st->logManager.LogInfo(curBarIdx, bridgeMsg.GetChars(), LogCategory::SESSION);
            }
        }

        // GLOBEX → RTH: Capture overnight and set up session bridge
        if (!wasRTH && isNowRTH) {
            // Capture overnight session structure
            AMT::OvernightSession on;
            on.onHigh = st->amtZoneManager.GetSessionHigh();
            on.onLow = st->amtZoneManager.GetSessionLow();
            on.onClose = (curBarIdx > 0) ? sc.Close[curBarIdx - 1] : sc.Close[curBarIdx];
            on.onMidpoint = (on.onHigh + on.onLow) / 2.0;
            on.onPOC = st->sessionMgr.GetPOC();
            on.onVAH = st->sessionMgr.GetVAH();
            on.onVAL = st->sessionMgr.GetVAL();
            on.overnightPattern = st->lastDaltonState.timeframe;
            on.overnightRotation = st->lastDaltonState.rotationFactor;
            // Get mini-IB from GLOBEX tracker if available
            const auto& miniIB = st->daltonEngine.GetGlobexMiniIBTracker().GetState();
            on.miniIBHigh = miniIB.high;
            on.miniIBLow = miniIB.low;
            on.miniIBFrozen = miniIB.frozen;
            on.valid = (on.onHigh > 0.0 && on.onLow > 0.0);
            on.barCount = curBarIdx - st->sessionMgr.GetSessionStartBar();

            // Set prior RTH context from SessionManager
            st->daltonEngine.SetPriorRTHContext(
                st->sessionMgr.GetPriorRTHHigh(),
                st->sessionMgr.GetPriorRTHLow(),
                st->sessionMgr.GetPriorRTHClose(),
                st->sessionMgr.GetPriorRTHPOC(),
                st->sessionMgr.GetPriorRTHVAH(),
                st->sessionMgr.GetPriorRTHVAL());

            // Capture overnight session
            st->daltonEngine.CaptureOvernightSession(on);

            // Classify gap at RTH open
            const double rthOpenPrice = probeOpen;
            st->daltonEngine.ClassifyGap(rthOpenPrice, sc.TickSize);

            if (diagLevel >= 1) {
                const auto& bridge = st->daltonEngine.GetSessionBridge();
                SCString bridgeMsg;
                bridgeMsg.Format("Bar %d | OVERNIGHT CAPTURED: H=%.2f L=%.2f C=%.2f | INV=%s SCORE=%.2f | GAP=%s SIZE=%.0ft",
                    curBarIdx, on.onHigh, on.onLow, on.onClose,
                    AMT::InventoryPositionToString(bridge.inventory.position),
                    bridge.inventory.score,
                    AMT::GapTypeToString(bridge.gap.type),
                    bridge.gap.gapSize);
                st->logManager.LogInfo(curBarIdx, bridgeMsg.GetChars(), LogCategory::SESSION);
            }
        }

        // Reset Dalton framework for new session (with session type)
        st->daltonEngine.ResetSession(isNowGlobex);
        st->lastDaltonState = AMT::DaltonState();  // Clear last state

        // --- ACCUMULATE SESSION CHANGE (after reset so it's not lost) ---
        st->sessionAccum.sessionChangeCount++;

        // Debug log for duplicate detection (uses early-computed isDuplicateSessionEvent)
        if (isDuplicateSessionEvent && diagLevel >= 1)
        {
            SCString dupMsg;
            dupMsg.Format("Bar %d | sessionId=%d | SKIPPING duplicate event logging",
                curBarIdx, sessionIdForLogging);
            st->logManager.LogDebug(curBarIdx, dupMsg.GetChars(), LogCategory::ACCUM);
        }

        // Update session tracking (even on duplicate, for consistency)
        st->sessionAccum.lastResetSessionId = sessionIdForLogging;

        // DEBUG: Log session boundary accumulator state with session ID
        if (diagLevel >= 1)
        {
            const std::string sessionKeyStr = curSessionKeyEarly.ToString();
            SCString accumMsg;
            accumMsg.Format("Bar %d | sessionId=%d (%s) | startIdx=%d lastAccumIdx=%d | "
                "sessionVol=%.0f sessionDelta=%.0f",
                curBarIdx, sessionIdForLogging, sessionKeyStr.c_str(),
                st->sessionAccum.sessionStartBarIndex, st->sessionAccum.lastAccumulatedBarIndex,
                st->sessionAccum.sessionTotalVolume, st->sessionAccum.sessionCumDelta);
            st->logManager.LogDebug(curBarIdx, accumMsg.GetChars(), LogCategory::ACCUM);
        }

        // Re-apply DOM stale flag if it was set on first bar
        if (firstBarDomStaleCapture)
        {
            st->sessionAccum.domStaleCount++;
            st->sessionAccum.firstBarDomStale = true;
        }

        // Update session tracking on first occurrence (not duplicate recalc triggers)
        if (!isDuplicateSessionEvent)
        {
            st->logManager.OnSessionChange(isCurRTH ? "RTH" : "GLOBEX", probeBarTime);
        }

        // SSOT: Archive prior session nodes BEFORE reset
        // This preserves HVN/LVN from closing session for cross-session reference
        st->sessionVolumeProfile.ArchivePriorSession(
            curBarIdx,
            st->sessionVolumeProfile.session_phase);  // Pass closing session type

        // =========================================================
        // SESSION BOUNDARY DIAGNOSTIC LOGGING
        // =========================================================
        // SSOT Archive diagnostics (single throttle gate for block)
        if (st->logManager.ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MODERATE, curBarIdx) &&
            st->logManager.ShouldLog(ThrottleKey::SSOT_DIAG, curBarIdx, 10))
        {
            const auto& archLog = st->sessionVolumeProfile.lastArchiveLog;

            // Get session type string (use SSOT function from amt_core.h)
            const char* sessTypeStr = AMT::SessionPhaseToString(archLog.sessionType);

            SCString msg1;
            msg1.Format("[SSOT-ARCHIVE] bar=%d sessionType=%s | archived: HVN=%d LVN=%d",
                archLog.bar, sessTypeStr,
                archLog.hvnArchived, archLog.lvnArchived);
            st->logManager.LogToSC(LogCategory::VBP, msg1, false);

            SCString msg2;
            msg2.Format("[SSOT-ARCHIVE] priorCounts: HVN %d->%d LVN %d->%d",
                archLog.priorHvnCountBefore, archLog.priorHvnCountAfter,
                archLog.priorLvnCountBefore, archLog.priorLvnCountAfter);
            st->logManager.LogToSC(LogCategory::VBP, msg2, false);

            if (archLog.hvnArchived > 0) {
                SCString msg3;
                msg3.Format("[SSOT-ARCHIVE] firstHVNs: %.2f %.2f %.2f",
                    archLog.firstHvnPrices[0],
                    archLog.firstHvnPrices[1],
                    archLog.firstHvnPrices[2]);
                st->logManager.LogToSC(LogCategory::VBP, msg3, false);
            }

            // Verify age progression: check first prior node age
            if (!st->sessionVolumeProfile.priorSessionHVN.empty()) {
                int minAge = 999, maxAge = 0;
                for (const auto& node : st->sessionVolumeProfile.priorSessionHVN) {
                    if (node.sessionAge < minAge) minAge = node.sessionAge;
                    if (node.sessionAge > maxAge) maxAge = node.sessionAge;
                }
                SCString msg4;
                msg4.Format("[SSOT-ARCHIVE] priorHVN ageRange: %d-%d (expect newest=1)",
                    minAge, maxAge);
                st->logManager.LogToSC(LogCategory::VBP, msg4, false);
            }
        }

        // Prune old references (configurable via Input[4])
        const int maxPriorSessionAge = sc.Input[4].GetInt();  // Prior Session Age
        st->sessionVolumeProfile.PrunePriorReferences(maxPriorSessionAge);

        // Log replay validation summary at session boundary
        if (diagLevel >= 1 && st->replayValidator.GetTotalRecorded() > 0)
        {
            st->replayValidator.LogSummary(sc);
        }

        // Enable replay validation after first session (for subsequent chart replays)
        st->replayValidator.EnableReplayValidation();

        st->sessionVolumeProfile.Reset(tickSize);
        st->sessionVolumeProfile.session_phase = curPhase;
        st->sessionVolumeProfile.session_start = probeBarTime;

        // Reset pattern logger for new session
        st->patternLogger.ResetForNewSession();

        // Log pattern detection capability (once per session, semantic facts only)
        if (!st->patternLogger.capabilityLoggedThisSession)
        {
            const bool domAvailable = st->domInputsValid;
            SCString capMsg;
            capMsg.Format("[PATTERN-CAPABILITY] volume=true tpo=proxy dom=%s tape=false",
                domAvailable ? "live_only" : "unavailable");
            st->logManager.LogInfo(curBarIdx, capMsg.GetChars(), LogCategory::SESSION);
            st->patternLogger.capabilityLoggedThisSession = true;
        }

        // NOTE: ConsumeSessionChange() auto-clears flag - no manual clear needed
    }

    // =========================================================================
    // DALTON ADVANCED: Session Open Capture & Spike Opening Evaluation
    // =========================================================================
    // Capture session open price on first bar and evaluate prior session spike
    if (!st->sessionOpenCaptured && probeOpen > 0.0)
    {
        st->sessionOpenPrice = probeOpen;
        st->sessionOpenCaptured = true;

        // Evaluate spike opening relation if prior session had a spike
        if (st->priorSessionSpike.hasSpike)
        {
            st->priorSessionSpike.EvaluateOpening(probeOpen);

            // Copy evaluated spike context to current Dalton state
            st->lastDaltonState.spikeContext = st->priorSessionSpike;

            // Log spike evaluation
            if (diagLevel >= 1)
            {
                SCString spikeMsg;
                spikeMsg.Format("Bar %d | SPIKE_EVAL: open=%.2f spike=[%.2f-%.2f] dir=%s relation=%s target=%.2f",
                    curBarIdx,
                    probeOpen,
                    st->priorSessionSpike.spikeLow,
                    st->priorSessionSpike.spikeHigh,
                    st->priorSessionSpike.isUpSpike ? "UP" : "DOWN",
                    AMT::SpikeOpenRelationToString(st->priorSessionSpike.todayOpen),
                    st->priorSessionSpike.GetSpikeTarget());
                st->logManager.LogInfo(curBarIdx, spikeMsg.GetChars(), LogCategory::AMT);
            }
        }
    }

    // =========================================================================
    // PROFILE UPDATE: VbP Study is SSOT (Single Source of Truth)
    // =========================================================================

    // Validate VbP configuration
    if (vbpStudyId <= 0)
    {
        if (!st->vbpConfigWarningShown)
        {
            st->logManager.LogError(curBarIdx, "VbP Study ID not configured. Set Input 20 to your VbP study ID.", LogCategory::VBP);
            st->sessionAccum.configErrorCount++;
            st->vbpConfigWarningShown = true;
        }
    }
    else
    {
        // TEMP DIAGNOSTIC: Check Profiles 0 and 1
        if (!st->vbpProfileCheckDone && diagLevel >= 1)
        {
            for (int pIdx = 0; pIdx < 2; ++pIdx)
            {
                n_ACSIL::s_StudyProfileInformation pInfo;
                if (sc.GetStudyProfileInformation(vbpStudyId, pIdx, pInfo))
                {
                    SCString tStart = sc.FormatDateTime(pInfo.m_StartDateTime);
                    SCString tEnd = sc.FormatDateTime(pInfo.m_EndDateTime);
                    SCString msg;
                    msg.Format("ProfileIdx=%d | Start=%s | End=%s",
                        pIdx, tStart.GetChars(), tEnd.GetChars());
                    st->logManager.LogDebug(curBarIdx, msg.GetChars(), LogCategory::VBP);
                }
                else
                {
                    SCString msg;
                    msg.Format("ProfileIdx=%d | GetInfo FAILED", pIdx);
                    st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::VBP);
                }
            }
            st->vbpProfileCheckDone = true;
        }

        // --- VbP Profile Mode (SSOT) ---
        // Update every 5 bars or on session change
        const bool shouldUpdateVbP = sessionChanged ||
                                      st->sessionVolumeProfile.bars_since_last_compute >= 5 ||
                                      st->sessionVolumeProfile.volume_profile.empty();

#if PERF_TIMING_ENABLED
        PerfTimer vbpTimer;
        if (shouldUpdateVbP) vbpTimer.Start();
#endif

        if (shouldUpdateVbP)
        {
            const bool success = st->sessionVolumeProfile.PopulateFromVbPStudy(
                sc,
                vbpStudyId,
                isCurRTH,
                rthStartSec,
                rthEndSec,
                diagLevel,
                isLiveBar,
                curBarIdx);  // AutoLoop=0 compatibility

            // DIAGNOSTIC: On session change, log what VbP returned
            if (sessionChanged)
            {
                SCString vbpMsg;
                vbpMsg.Format("Bar %d | success=%d isCurRTH=%d | VbP returned: POC=%.2f VAH=%.2f VAL=%.2f",
                    curBarIdx, success ? 1 : 0, isCurRTH ? 1 : 0,
                    st->sessionVolumeProfile.session_poc,
                    st->sessionVolumeProfile.session_vah,
                    st->sessionVolumeProfile.session_val);
                st->logManager.LogThrottled(ThrottleKey::VBP_SESSION_SUMMARY, curBarIdx, 1, vbpMsg.GetChars(), LogCategory::VBP);
            }

            if (!success)
            {
                // Throttled warning - only log every 5 minutes
                if (probeBarTime.IsDateSet() &&
                    (st->lastVbPWarning == 0 || probeBarTime - st->lastVbPWarning > SCDateTime::SECONDS(300)))
                {
                    if (diagLevel >= 1)
                    {
                        SCString msg;
                        msg.Format("VbP profile read failed. StudyID=%d. Using cached data.", vbpStudyId);
                        st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::VBP);
                    }
                    st->sessionAccum.vbpWarningCount++;
                    st->lastVbPWarning = probeBarTime;
                }
            }
            else
            {
                // VBP session diagnostics: check stability and detect POC migration
                // CheckStability returns true if POC migrated >= 2 ticks (zones need update)
                const bool pocMigratedThisUpdate = st->sessionVolumeProfile.CheckStability(sc, probeBarTime, diagLevel);

                // === COMPUTE PROFILE STRUCTURE (engine-like: validity + metrics + maturity FSM) ===
                {
                    const int sessionBars = curBarIdx - st->sessionAccum.sessionStartBarIndex + 1;
                    const int tSecLocal = TimeToSeconds(sc.BaseDateTimeIn[curBarIdx]);
                    const int sessionMinutes = isCurRTH
                        ? (tSecLocal >= rthStartSec ? (tSecLocal - rthStartSec) / 60 : 0)
                        : (tSecLocal >= gbxStartSec ? (tSecLocal - gbxStartSec) / 60 : ((86400 - gbxStartSec + tSecLocal) / 60));
                    const double sessionRangeTicks = static_cast<double>(st->amtZoneManager.structure.GetSessionRangeTicks());
                    const AMT::HistoricalProfileBaseline* baseline =
                        isCurRTH ? &st->rthProfileBaseline : &st->gbxProfileBaseline;

                    // === UPDATE OPENING RANGE TRACKER (IB for RTH, SOR for Globex) ===
                    // Tracks the opening range for shape confirmation gates.
                    // Must call every VBP update to capture range extremes and detect failed auctions.
                    st->sessionVolumeProfile.openingRangeTracker_.Update(
                        sc.High[curBarIdx],
                        sc.Low[curBarIdx],
                        sc.Close[curBarIdx],
                        sessionMinutes,
                        curBarIdx);

                    // Check for failed auction (price returned to opening range after breach)
                    st->sessionVolumeProfile.openingRangeTracker_.CheckFailedAuction(
                        sc.Close[curBarIdx],
                        sessionMinutes,
                        curBarIdx);

                    st->lastProfileStructureResult = st->sessionVolumeProfile.ComputeStructure(
                        curBarIdx, sessionBars, sessionMinutes, sessionRangeTicks, baseline);

                    // Log on maturity state change (deterministic transition logging)
                    if (st->lastProfileStructureResult.maturityChanged && diagLevel >= 2)
                    {
                        SCString msg;
                        msg.Format("[PROFILE] Maturity: %s (bars=%d mins=%d) | POC_DOM=%.2f VA_W=%dt RATIO=%.2f",
                            AMT::ProfileMaturityStateToString(st->lastProfileStructureResult.maturityState),
                            sessionBars, sessionMinutes,
                            st->lastProfileStructureResult.pocDominance,
                            st->lastProfileStructureResult.vaWidthTicks,
                            st->lastProfileStructureResult.vaWidthRatio);
                        st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::VBP);
                    }
                }

                // === UPDATE DISPLAY LEVELS FROM PROFILE (SSOT) ===
                // This is the primary source - profile API avoids calculation order issues
                const double profPOC = st->sessionVolumeProfile.session_poc;
                const double profVAH = st->sessionVolumeProfile.session_vah;
                const double profVAL = st->sessionVolumeProfile.session_val;
                if (profPOC > 0.0 && profVAH > 0.0 && profVAL > 0.0) {
                    st->displayPOC = profPOC;
                    st->displayVAH = profVAH;
                    st->displayVAL = profVAL;
                    st->displayLevelsValid = true;
                }

                // Load native Peaks/Valleys for BOTH RTH and GLOBEX sessions
                // Uses negative profile indices (-1, -2, ...) to find most recent of each type
                st->sessionVolumeProfile.PopulateDualSessionPeaksValleys(sc, vbpStudyId, rthStartSec, rthEndSec, diagLevel);

                // Sync legacy session_hvn/session_lvn from current session for backward compatibility
                // Track HVN/LVN deltas before update
                const int oldHvnCount = static_cast<int>(st->sessionVolumeProfile.session_hvn.size());
                const int oldLvnCount = static_cast<int>(st->sessionVolumeProfile.session_lvn.size());

                const auto& currentPV = isCurRTH
                    ? st->sessionVolumeProfile.dualSessionPV.rth
                    : st->sessionVolumeProfile.dualSessionPV.globex;
                if (currentPV.valid)
                {
                    st->sessionVolumeProfile.session_hvn = currentPV.hvn;
                    st->sessionVolumeProfile.session_lvn = currentPV.lvn;

                    // Track deltas (net changes) for session statistics
                    const int newHvnCount = static_cast<int>(st->sessionVolumeProfile.session_hvn.size());
                    const int newLvnCount = static_cast<int>(st->sessionVolumeProfile.session_lvn.size());

                    if (newHvnCount > oldHvnCount)
                        st->sessionAccum.hvnAdded += (newHvnCount - oldHvnCount);
                    else if (newHvnCount < oldHvnCount)
                        st->sessionAccum.hvnRemoved += (oldHvnCount - newHvnCount);

                    if (newLvnCount > oldLvnCount)
                        st->sessionAccum.lvnAdded += (newLvnCount - oldLvnCount);
                    else if (newLvnCount < oldLvnCount)
                        st->sessionAccum.lvnRemoved += (oldLvnCount - newLvnCount);
                }

                // Success logging (verbose, only in log window)
                if (inLogWindow && st->logManager.ShouldEmit(LogChannel::EVENTS_CSV, LogLevel::VERBOSE))
                {
                    AMT::SessionEvent evt;
                    evt.type = AMT::SessionEventType::VBP_UPDATE;
                    evt.timestamp = probeBarTime;
                    evt.bar = curBarIdx;
                    evt.poc = st->sessionVolumeProfile.session_poc;
                    evt.vah = st->sessionVolumeProfile.session_vah;
                    evt.val = st->sessionVolumeProfile.session_val;
                    evt.message = "Profile updated";
                    st->logManager.LogSessionEvent(evt);
                }

                // ============================================================
                // SYNC SESSION CONTEXT ON EVERY PROFILE UPDATE
                // This ensures POC/VAH/VAL migration is immediately reflected
                // in phase detection and zone calculations.
                // ============================================================
                if (st->amtZonesInitialized)
                {
                    const double newPoc = st->sessionVolumeProfile.session_poc;
                    const double newVah = st->sessionVolumeProfile.session_vah;
                    const double newVal = st->sessionVolumeProfile.session_val;

                    // Validate new values before syncing
                    if (newVah > newVal && newVah > 0.0 && newVal > 0.0)
                    {
                        // Detect migration (read from SessionManager SSOT)
                        const double oldPoc = st->sessionMgr.GetPOC();
                        const bool pocMigrated = (oldPoc > 0.0) &&
                            (std::abs(newPoc - oldPoc) > sc.TickSize * 0.5);

                        // Update SessionManager (SSOT for POC/VAH/VAL)
                        st->sessionMgr.UpdateLevels(newPoc, newVah, newVal, sc.TickSize);

                        // Log migration event
                        if (pocMigrated && diagLevel >= 2 && isLiveBar)
                        {
                            SCString msg;
                            msg.Format("POC migrated: %.2f -> %.2f | VAH=%.2f VAL=%.2f | Range=%d ticks",
                                oldPoc, newPoc, newVah, newVal,
                                st->sessionMgr.GetVARangeTicks());
                            st->logManager.LogThrottled(ThrottleKey::VBP_DRIFT, curBarIdx, 1, msg.GetChars(), LogCategory::VBP);
                            st->sessionAccum.pocDriftCount++;
                        }

                        // ============================================================
                        // LEVEL ACCEPTANCE ENGINE: Register profile levels for tracking
                        // ============================================================
                        st->levelAcceptance.RegisterLevel(AMT::LevelType::POC, newPoc);
                        st->levelAcceptance.RegisterLevel(AMT::LevelType::VAH, newVah);
                        st->levelAcceptance.RegisterLevel(AMT::LevelType::VAL, newVal);

                        // Register HVN/LVN levels (nearest to current price)
                        st->levelAcceptance.RegisterHVNs(st->sessionVolumeProfile.session_hvn);
                        st->levelAcceptance.RegisterLVNs(st->sessionVolumeProfile.session_lvn);

                        // Register prior session levels if available
                        const double priorPOC = st->amtZoneManager.sessionCtx.prior_poc;
                        const double priorVAH = st->amtZoneManager.sessionCtx.prior_vah;
                        const double priorVAL = st->amtZoneManager.sessionCtx.prior_val;
                        if (priorPOC > 0.0) {
                            st->levelAcceptance.RegisterLevel(AMT::LevelType::PRIOR_POC, priorPOC);
                        }
                        if (priorVAH > 0.0) {
                            st->levelAcceptance.RegisterLevel(AMT::LevelType::PRIOR_VAH, priorVAH);
                        }
                        if (priorVAL > 0.0) {
                            st->levelAcceptance.RegisterLevel(AMT::LevelType::PRIOR_VAL, priorVAL);
                        }
                    }
                    // else: Keep existing valid values, don't overwrite with invalid
                }

                // === AMT ZONE INTEGRATION ===
                // Create/update zones from VBP profile data
                //
                // ZONE CLEARING POLICY:
                // - Session boundary (RTH <-> Globex): zones cleared in session boundary block
                //   via ResetForSession() BEFORE we reach here. This ensures session isolation.
                // - First init: zones cleared here (no prior zones exist)
                // - POC migration: zones are RECENTERED, not cleared (preserves stats)
                //
                // NOTE: Zones are always session-scoped. Cross-session zone persistence is
                // a correctness violation (ghost zones can be selected by current-session logic).
                const bool shouldClearZones = !st->amtZonesInitialized;

                // Zone creation needed on: first init OR session change
                // POC migration -> RECENTER existing zones (preserves stats), don't recreate
                const bool needsZoneCreation = !st->amtZonesInitialized || sessionChanged;

                // SSOT: Get current VbP values for either creation or recenter
                const double vbpPoc = st->sessionVolumeProfile.session_poc;
                const double vbpVah = st->sessionVolumeProfile.session_vah;
                const double vbpVal = st->sessionVolumeProfile.session_val;

                // DIAGNOSTIC: Log PRIOR VBP state using tri-state contract
                // Only log once at session change, using proper state classification
                if (sessionChanged)
                {
                    const AMT::PriorVBPState priorState = st->amtZoneManager.sessionCtx.priorVBPState;
                    SCString compMsg;

                    if (priorState == AMT::PriorVBPState::PRIOR_MISSING)
                    {
                        // Not a bug - just degraded mode (first session or insufficient history)
                        compMsg.Format("Bar %d | State:MISSING | Reason: insufficient history (profiles not built yet)",
                            curBarIdx);
                        st->logManager.LogInfo(curBarIdx, compMsg.GetChars(), LogCategory::VBP);
                    }
                    else if (priorState == AMT::PriorVBPState::PRIOR_DUPLICATES_CURRENT)
                    {
                        // TRUE DEFECT - log with diagnostic context for reproduction
                        const double priorPoc = st->amtZoneManager.sessionCtx.prior_poc;
                        const double priorVah = st->amtZoneManager.sessionCtx.prior_vah;
                        const double priorVal = st->amtZoneManager.sessionCtx.prior_val;
                        compMsg.Format("Bar %d | State:DUPLICATES_CURRENT (BUG!) | VBP: POC=%.2f VAH=%.2f VAL=%.2f | PRIOR: POC=%.2f VAH=%.2f VAL=%.2f",
                            curBarIdx, vbpPoc, vbpVah, vbpVal, priorPoc, priorVah, priorVal);
                        st->logManager.LogWarn(curBarIdx, compMsg.GetChars(), LogCategory::VBP);
                    }
                    else  // PRIOR_VALID
                    {
                        const double priorPoc = st->amtZoneManager.sessionCtx.prior_poc;
                        const double priorVah = st->amtZoneManager.sessionCtx.prior_vah;
                        const double priorVal = st->amtZoneManager.sessionCtx.prior_val;
                        compMsg.Format("Bar %d | State:VALID | VBP: POC=%.2f VAH=%.2f VAL=%.2f | PRIOR: POC=%.2f VAH=%.2f VAL=%.2f",
                            curBarIdx, vbpPoc, vbpVah, vbpVal, priorPoc, priorVah, priorVal);
                        st->logManager.LogInfo(curBarIdx, compMsg.GetChars(), LogCategory::VBP);
                    }
                }

                // === POC MIGRATION: Recenter existing zones (preserves stats) ===
                // Policy: small drift -> recenter; large jump -> retire+create
                if (pocMigratedThisUpdate && st->amtZonesInitialized && !needsZoneCreation)
                {
                    // Try to recenter zones to new VbP anchors
                    auto recenterResult = st->amtZoneManager.RecenterAnchorsEx(
                        vbpPoc, vbpVah, vbpVal, sc.TickSize);

                    // Handle large jumps: retire affected zones and mark for recreation
                    // (will be recreated on next zone creation pass)
                    if (recenterResult.anyLargeJump())
                    {
                        // Large structural change - remove affected zones
                        // They'll be recreated fresh with the new anchors
                        // INVARIANT: Force-finalize pending engagements BEFORE erasing each zone
                        if (recenterResult.pocLargeJump && st->amtZoneManager.pocId >= 0)
                        {
                            st->amtZoneManager.ForceFinalizeSingleZone(
                                st->amtZoneManager.pocId, curBarIdx, probeBarTime,
                                AMT::UnresolvedReason::ZONE_EXPIRY);
                            st->amtZoneManager.activeZones.erase(st->amtZoneManager.pocId);
                            st->amtZoneManager.pocId = -1;
                        }
                        if (recenterResult.vahLargeJump && st->amtZoneManager.vahId >= 0)
                        {
                            st->amtZoneManager.ForceFinalizeSingleZone(
                                st->amtZoneManager.vahId, curBarIdx, probeBarTime,
                                AMT::UnresolvedReason::ZONE_EXPIRY);
                            st->amtZoneManager.activeZones.erase(st->amtZoneManager.vahId);
                            st->amtZoneManager.vahId = -1;
                        }
                        if (recenterResult.valLargeJump && st->amtZoneManager.valId >= 0)
                        {
                            st->amtZoneManager.ForceFinalizeSingleZone(
                                st->amtZoneManager.valId, curBarIdx, probeBarTime,
                                AMT::UnresolvedReason::ZONE_EXPIRY);
                            st->amtZoneManager.activeZones.erase(st->amtZoneManager.valId);
                            st->amtZoneManager.valId = -1;
                        }

                        if (diagLevel >= 2 && isLiveBar)
                        {
                            SCString msg;
                            msg.Format("Large structural change - retiring zones: POC=%d VAH=%d VAL=%d",
                                recenterResult.pocLargeJump ? 1 : 0,
                                recenterResult.vahLargeJump ? 1 : 0,
                                recenterResult.valLargeJump ? 1 : 0);
                            st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::ZONE);
                        }

                        // Force zone recreation for retired zones
                        // (Recreate immediately below since zones are now missing)
                    }

                    if ((recenterResult.applied > 0 || recenterResult.latched > 0) &&
                        diagLevel >= 2 && isLiveBar)
                    {
                        SCString msg;
                        msg.Format("applied=%d latched=%d: POC=%.2f VAH=%.2f VAL=%.2f",
                            recenterResult.applied, recenterResult.latched, vbpPoc, vbpVah, vbpVal);
                        st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::ZONE);
                    }

                    // If any zones were retired due to large jump, recreate them now
                    if (recenterResult.anyLargeJump())
                    {
                        // Recreate only the missing anchor zones (not full clear)
                        if (st->amtZoneManager.pocId < 0 && vbpPoc > 0.0)
                        {
                            auto result = st->amtZoneManager.CreateZone(
                                AMT::ZoneType::VPB_POC, vbpPoc, probeBarTime, curBarIdx, true);
                            if (result.ok) st->amtZoneManager.pocId = result.zoneId;
                        }
                        if (st->amtZoneManager.vahId < 0 && vbpVah > 0.0)
                        {
                            auto result = st->amtZoneManager.CreateZone(
                                AMT::ZoneType::VPB_VAH, vbpVah, probeBarTime, curBarIdx, true);
                            if (result.ok) st->amtZoneManager.vahId = result.zoneId;
                        }
                        if (st->amtZoneManager.valId < 0 && vbpVal > 0.0)
                        {
                            auto result = st->amtZoneManager.CreateZone(
                                AMT::ZoneType::VPB_VAL, vbpVal, probeBarTime, curBarIdx, true);
                            if (result.ok) st->amtZoneManager.valId = result.zoneId;
                        }
                    }
                }

                // === ZONE CREATION: First init or session change ===
                if (needsZoneCreation)
                {
                    // Clear zones on first init only (session boundary clears are handled above)
                    // INVARIANT: Force-finalize before clear for safety, even though first init
                    // should have no pending engagements.
                    if (shouldClearZones)
                    {
                        SCString clrMsg;
                        clrMsg.Format("Bar %d | shouldClearZones=1 | BEFORE: activeZones=%d pocId=%d vahId=%d valId=%d",
                            curBarIdx,
                            static_cast<int>(st->amtZoneManager.activeZones.size()),
                            st->amtZoneManager.pocId, st->amtZoneManager.vahId, st->amtZoneManager.valId);
                        st->logManager.LogDebug(curBarIdx, clrMsg.GetChars(), LogCategory::ZONE);

                        // DRY: Use atomic helper for zone clearing
                        st->amtZoneManager.ClearZonesOnly(curBarIdx, probeBarTime, AMT::UnresolvedReason::CHART_RESET);
                    }

                    // SSOT: Set tickSize in config before creating zones
                    // This ensures AMT::ZoneRuntime can store anchorTicks as authoritative
                    st->amtZoneManager.config.tickSize = sc.TickSize;

                    // Create zones from the session volume profile
                    // The profile stores data as std::map<int, VolumeAtPrice> where key is tick price
                    // Convert to s_VolumeAtPriceV2 array format for CreateZonesFromProfile
                    std::vector<s_VolumeAtPriceV2> vapArray;
                    vapArray.reserve(st->sessionVolumeProfile.volume_profile.size());

                    for (const auto& [tickPrice, vap] : st->sessionVolumeProfile.volume_profile)
                    {
                        vapArray.push_back(vap);
                    }

                    // DIAGNOSTIC: Log zone creation attempt
                    if (diagLevel >= 1)
                    {
                        SCString msg;
                        msg.Format("needsZoneCreation=1 | vapArray.size=%d | POC=%.2f VAH=%.2f VAL=%.2f | bar=%d",
                            static_cast<int>(vapArray.size()), vbpPoc, vbpVah, vbpVal, curBarIdx);
                        st->logManager.LogThrottled(ThrottleKey::ZONE_UPDATE, curBarIdx, 1, msg.GetChars(), LogCategory::ZONE);
                    }

                    if (!vapArray.empty())
                    {
                        AMT::CreateZonesFromProfile(
                            st->amtZoneManager,
                            vapArray.data(),
                            static_cast<int>(vapArray.size()),
                            sc.TickSize,
                            probeBarTime,
                            curBarIdx,
                            vbpPoc,  // VbP study POC (authoritative)
                            vbpVah,  // VbP study VAH (authoritative)
                            vbpVal); // VbP study VAL (authoritative)

                        st->amtZonesInitialized = true;

                        // NOTE: onEngagementFinalized callback is now registered at INIT time
                        // (see needsStateInit block above) to fix the bug where early sessions
                        // had no callback registered. The callback persists across sessions.

                        // Always log zone creation result for debugging
                        {
                            SCString msg;
                            msg.Format("Zones created: count=%d | VBP: poc=%d vah=%d val=%d | POC=%.2f VAH=%.2f VAL=%.2f",
                                static_cast<int>(st->amtZoneManager.activeZones.size()),
                                st->amtZoneManager.pocId, st->amtZoneManager.vahId, st->amtZoneManager.valId,
                                vbpPoc, vbpVah, vbpVal);  // Use actual VbP values, not sessionMgr (may not be updated yet)
                            st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::ZONE);

                            // Log PRIOR zones using tri-state contract
                            {
                                const AMT::PriorVBPState priorState = st->amtZoneManager.sessionCtx.priorVBPState;
                                if (priorState == AMT::PriorVBPState::PRIOR_MISSING)
                                {
                                    // Not logged here - already logged once at session change
                                }
                                else if (priorState == AMT::PriorVBPState::PRIOR_DUPLICATES_CURRENT)
                                {
                                    // Warning: defect detected
                                    msg.Format("PRIOR zones SKIPPED | State:%s | P_POC=%.2f P_VAH=%.2f P_VAL=%.2f",
                                        AMT::to_string(priorState),
                                        st->amtZoneManager.sessionCtx.prior_poc,
                                        st->amtZoneManager.sessionCtx.prior_vah,
                                        st->amtZoneManager.sessionCtx.prior_val);
                                    st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::ZONE);
                                }
                                else  // PRIOR_VALID
                                {
                                    msg.Format("PRIOR zones: zoneIds=[%d,%d,%d] | State:%s | P_POC=%.2f P_VAH=%.2f P_VAL=%.2f",
                                        st->amtZoneManager.priorPocId,
                                        st->amtZoneManager.priorVahId,
                                        st->amtZoneManager.priorValId,
                                        AMT::to_string(priorState),
                                        st->amtZoneManager.sessionCtx.prior_poc,
                                        st->amtZoneManager.sessionCtx.prior_vah,
                                        st->amtZoneManager.sessionCtx.prior_val);
                                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::ZONE);
                                }
                            }

                            // ============================================================
                            // ZONE-DUMP: Machine-auditable dump of all active zones
                            // Proves posture correctness (no TPO_* when TPO disabled)
                            // ============================================================
                            {
                                int tpoCount = 0;
                                SCString dumpHeader;
                                dumpHeader.Format("count=%d |",
                                    static_cast<int>(st->amtZoneManager.activeZones.size()));
                                st->logManager.LogDebug(curBarIdx, dumpHeader.GetChars(), LogCategory::ZONE);

                                for (const auto& [id, zone] : st->amtZoneManager.activeZones) {
                                    const char* typeNameStr = AMT::ZoneTypeToString(zone.type);
                                    int typeVal = static_cast<int>(zone.type);
                                    const char* roleName = AMT::ZoneRoleToString(zone.role);

                                    SCString zoneEntry;
                                    zoneEntry.Format("  (id=%d, type=%s(%d), price=%.2f, role=%s)",
                                        id, typeNameStr, typeVal, zone.GetAnchorPrice(), roleName);
                                    st->logManager.LogDebug(curBarIdx, zoneEntry.GetChars(), LogCategory::ZONE);

                                    // Count TPO zones for assertion
                                    if (zone.type == AMT::ZoneType::TPO_POC ||
                                        zone.type == AMT::ZoneType::TPO_VAH ||
                                        zone.type == AMT::ZoneType::TPO_VAL) {
                                        tpoCount++;
                                    }
                                }

                                // POSTURE ENFORCEMENT: Assert no TPO zones when TPO disabled
                                if (!AMT::g_zonePosture.enableTPO && tpoCount > 0) {
                                    SCString errMsg;
                                    errMsg.Format("TPO disabled but %d TPO zones exist!", tpoCount);
                                    st->logManager.LogError(curBarIdx, errMsg.GetChars(), LogCategory::ZONE);
                                }
                            }
                        }
                    }
                    else
                    {
                        // vapArray was empty - log this
                        SCString msg;
                        msg.Format("FAILED: vapArray empty | profile_size=%d | session_poc=%.2f",
                            static_cast<int>(st->sessionVolumeProfile.volume_profile.size()),
                            st->sessionVolumeProfile.session_poc);
                        st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::ZONE);
                    }
                }
                else
                {
                    // SSOT: Session context updates now happen ONLY via CreateZonesFromProfile
                    // when zones are recreated. Continuous overwrites here were a SSOT breach
                    // that could cause stale/divergent data between VAP array and sessionCtx.
                    // See: Change Set A (session context triple-init fix)
                }

                // =========================================================================
                // SSOT: HVN/LVN CACHE REFRESH WITH HYSTERESIS
                // =========================================================================
                // NOTE: HVN/LVN (Peaks/Valleys) are SSOT from Sierra Chart VbP study via
                // GetStudyPeakValleyLine() API. They are loaded in PopulatePeaksValleysFromVbP()
                // called above after each successful VbP profile load. No periodic refresh
                // needed - Sierra Chart manages peak/valley calculation based on VbP settings.
                st->sessionAccum.profileRefreshCount++;
            }

            st->sessionVolumeProfile.bars_since_last_compute = 0;

#if PERF_TIMING_ENABLED
            if (sc.IsFullRecalculation) {
                st->perfStats.vbpMs += vbpTimer.ElapsedMs();
                st->perfStats.vbpCalls++;
            }
#endif
        }
        else
        {
            st->sessionVolumeProfile.bars_since_last_compute++;
        }

        // =====================================================================
        // LOUD DIAGNOSTIC: Why are zones not initialized?
        // NO FALLBACKS - just tell us exactly what's wrong
        // =====================================================================
        if (!st->amtZonesInitialized && isLiveBar)
        {
            SCString msg;
            msg.Format("[ZONE-ERROR] Zones NOT initialized on live bar %d! "
                       "poc=%.2f vah=%.2f val=%.2f profileSize=%d activeZones=%d "
                       "pocId=%d vahId=%d valId=%d",
                curBarIdx,
                st->sessionVolumeProfile.session_poc,
                st->sessionVolumeProfile.session_vah,
                st->sessionVolumeProfile.session_val,
                static_cast<int>(st->sessionVolumeProfile.volume_profile.size()),
                static_cast<int>(st->amtZoneManager.activeZones.size()),
                st->amtZoneManager.pocId,
                st->amtZoneManager.vahId,
                st->amtZoneManager.valId);
            st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::ZONE);
        }
    }

    // === AMT PHASE SNAPSHOT (declared outside block for later use) ===
    AMT::PhaseSnapshot amtSnapshot;  // Default: ROTATION phase
    AMT::CurrentPhase amtPhase = AMT::CurrentPhase::ROTATION;

    // === AMT ZONE UPDATE (runs every bar when zones are initialized) ===
#if PERF_TIMING_ENABLED
    PerfTimer zoneTimer;
    zoneTimer.Start();
#endif

    if (st->amtZonesInitialized && curBarIdx != st->amtLastZoneUpdateBar)
    {
        // Get current bar data
        const double currentPrice = probeClose;
        const double currentHigh = probeHigh;
        const double currentLow = probeLow;
        const double currentVolume = st->currentSnapshot.effort.totalVolume;
        const double currentDelta = st->currentSnapshot.effort.delta;
        const int currentBar = curBarIdx;

        // Dalton state/phase/reason/bias - computed in VA block, used by BuildPhaseSnapshot
        AMT::AMTMarketState daltonState = AMT::AMTMarketState::UNKNOWN;
        AMT::CurrentPhase daltonPhase = AMT::CurrentPhase::UNKNOWN;
        AMT::PhaseReason daltonReason = AMT::PhaseReason::NONE;
        AMT::TradingBias daltonBias = AMT::TradingBias::WAIT;
        AMT::VolumeConfirmation daltonVolConf = AMT::VolumeConfirmation::UNKNOWN;

        // Get prior bar OHLC for rotation metrics (with bounds check)
        const double priorHigh = (curBarIdx > 0) ? sc.High[curBarIdx - 1] : currentHigh;
        const double priorLow = (curBarIdx > 0) ? sc.Low[curBarIdx - 1] : currentLow;

        // Convert volume profile to array for update function
        std::vector<s_VolumeAtPriceV2> vapArray;
        vapArray.reserve(st->sessionVolumeProfile.volume_profile.size());
        for (const auto& [tickPrice, vap] : st->sessionVolumeProfile.volume_profile)
        {
            vapArray.push_back(vap);
        }

        // Update all zones - SSOT: Pass cached thresholds for sigma-based classification
        const AMT::VolumeThresholds* ssotThresholds =
            st->sessionVolumeProfile.cachedThresholds.valid
                ? &st->sessionVolumeProfile.cachedThresholds
                : nullptr;

        // Run engagement state machine (StartEngagement/FinalizeEngagement)
        // MUST run BEFORE UpdateZoneComplete() to detect transitions correctly
        const int zoneCountBefore = static_cast<int>(st->amtZoneManager.activeZones.size());
        st->amtZoneManager.UpdateZones(currentPrice, sc.TickSize, currentBar, probeBarTime, sc, diagLevel);
        const int zoneCountAfter = static_cast<int>(st->amtZoneManager.activeZones.size());

        // ========================================================================
        // TUNING TELEMETRY: Engagement Start (diagLevel >= 2)
        // TELEMETRY ONLY: Does NOT affect any behavioral logic
        // ========================================================================
        if (diagLevel >= 2 && !st->amtZoneManager.engagedThisBar.empty())
        {
            for (int engagedZoneId : st->amtZoneManager.engagedThisBar)
            {
                const AMT::ZoneRuntime* zone = st->amtZoneManager.GetZone(engagedZoneId);
                if (!zone) continue;

                // Build telemetry record
                AMT::EngagementTelemetryRecord rec;
                rec.zoneId = engagedZoneId;
                rec.zoneType = zone->type;
                rec.bar = currentBar;
                rec.price = currentPrice;

                // Friction (current bar - already populated)
                rec.friction = st->amtContext.friction;
                rec.frictionValid = st->amtContext.frictionValid;
                // Spread baseline info from domWarmup if ready
                if (st->domWarmup.IsSpreadReady()) {
                    rec.spreadBaselineReady = true;
                    // Note: spreadTicks/spreadPctile would need to be passed or recomputed
                    // For now, mark as available but don't recompute
                }

                // Volatility (prior bar's value - current bar computed after this)
                rec.volatility = st->amtContext.volatility;
                rec.volatilityValid = st->amtContext.volatilityValid;
                // 2D volatility info not available at this point (computed in ContextBuilder later)
                rec.closeChangeValid = false;

                // Market composition (current bar - already populated)
                rec.marketComposition = st->amtContext.confidence.marketComposition;
                rec.marketCompositionValid = st->amtContext.confidence.marketCompositionValid;

                // Compute advisories (TELEMETRY ONLY)
                rec.advisory.ComputeAdvisories(
                    rec.friction, rec.frictionValid,
                    0.0, 0.0, false  // No range/closeChange percentiles available here
                );

                // Emit structured telemetry log
                SCString msg;
                msg.Format("[TUNING-ENGAGE] bar=%d zone=%d %s price=%.2f | "
                           "FRIC=%s(v=%d) wouldBlock=%d threshOff=%.2f | "
                           "VOL=%s(v=%d) | COMP=%.2f(v=%d)",
                    rec.bar, rec.zoneId, AMT::ZoneTypeToString(rec.zoneType), rec.price,
                    AMT::to_string(rec.friction), rec.frictionValid ? 1 : 0,
                    rec.advisory.wouldBlockIfLocked ? 1 : 0, rec.advisory.thresholdOffset,
                    AMT::to_string(rec.volatility), rec.volatilityValid ? 1 : 0,
                    rec.marketComposition, rec.marketCompositionValid ? 1 : 0);
                sc.AddMessageToLog(msg, 0);
            }
        }

        // LOUD DIAGNOSTIC: Detect zone removal
        if (zoneCountAfter < zoneCountBefore) {
            SCString msg;
            msg.Format("Bar %d | UpdateZones removed %d zones! Before=%d After=%d | IDs: poc=%d vah=%d val=%d",
                currentBar, zoneCountBefore - zoneCountAfter, zoneCountBefore, zoneCountAfter,
                st->amtZoneManager.pocId, st->amtZoneManager.vahId, st->amtZoneManager.valId);
            st->logManager.LogWarn(currentBar, msg.GetChars(), LogCategory::ZONE);
        }

        for (auto& [id, zone] : st->amtZoneManager.activeZones)
        {
            AMT::UpdateZoneComplete(
                zone,
                currentPrice,
                currentHigh,
                currentLow,
                priorHigh,      // Prior bar high for rotation metrics
                priorLow,       // Prior bar low for rotation metrics
                currentVolume,
                currentDelta,
                sc.TickSize,
                currentBar,
                probeBarTime,
                vapArray.empty() ? nullptr : vapArray.data(),
                static_cast<int>(vapArray.size()),
                st->amtZoneManager.sessionCtx,
                st->amtZoneManager.config,
                st->sessionMgr.GetVAH(),  // SSOT: vah from SessionManager
                st->sessionMgr.GetVAL(),  // SSOT: val from SessionManager
                st->sessionMgr.GetSessionStartBar(),  // SSOT: sessionStartBar from SessionManager
                ssotThresholds);
        }

        // ========================================================================
        // STRUCTURE TRACKER UPDATE (SSOT for session extremes)
        // Updates session extremes (bar High/Low), IB levels, and range-adaptive thresholds
        // NOTE: ZoneSessionState.rth_high/low sync removed - StructureTracker is now SSOT
        // ========================================================================
        {
            AMT::StructureTracker& structure = st->amtZoneManager.structure;

            // Update session extremes
            structure.UpdateExtremes(probeHigh, probeLow, currentBar);

            // ================================================================
            // EXTREME ACCEPTANCE TRACKER UPDATE (AMT-aligned)
            // Tracks acceptance/rejection at session extremes using:
            //   - Tail ratio (bar structure)
            //   - Delta direction (volume intent)
            //   - Time at price (TPO-like duration)
            //   - Retest outcomes (returns that held/rejected)
            // ================================================================
            {
                // Update extreme levels when they change
                const double sessHi = structure.GetSessionHigh();
                const double sessLo = structure.GetSessionLow();

                // Notify tracker of new extremes
                st->extremeTracker.OnNewSessionHigh(currentBar, sessHi);
                st->extremeTracker.OnNewSessionLow(currentBar, sessLo);

                // Get delta and deltaConsistency for acceptance signals
                const double barDelta = st->currentSnapshot.effort.delta;
                const double deltaConsistency = st->amtContext.confidence.deltaConsistency;
                const int sessionRangeTicks = structure.GetSessionRangeTicks();

                // Update tracking for this bar (called EVERY bar)
                st->extremeTracker.UpdateBar(
                    currentBar,
                    probeHigh, probeLow, probeOpen, probeClose,
                    barDelta, deltaConsistency,
                    sessionRangeTicks, sc.TickSize);

                // Update volume concentration from VbP profile with sigma-based classification
                // AMT: Volume density at extremes distinguishes acceptance vs rejection
                // Note: sessHi/sessLo already defined above
                double highVolBand = 0.0, lowVolBand = 0.0, totalVol = 0.0;
                constexpr int EXTREME_VOLUME_BAND_TICKS = 2;  // 2 ticks around extreme

                if (st->sessionVolumeProfile.GetExtremeVolumeConcentration(
                        sessHi, sessLo, EXTREME_VOLUME_BAND_TICKS,
                        highVolBand, lowVolBand, totalVol))
                {
                    // Use VolumeThresholds for AMT-aligned HVN/LVN classification
                    st->extremeTracker.UpdateVolumeWithThresholds(
                        highVolBand, lowVolBand, totalVol,
                        st->sessionVolumeProfile.cachedThresholds);
                }

                // Compute acceptance state
                st->extremeTracker.ComputeAcceptance();
            }

            // ================================================================
            // DALTON SPIKE DETECTION (Late-Day Imbalance)
            // A spike is a breakout in final ~30 minutes that hasn't been
            // validated by time. Next-day opening relative to spike matters.
            // ================================================================
            {
                const double sessHi = structure.GetSessionHigh();
                const double sessLo = structure.GetSessionLow();

                // Check if we're in RTH and in final 30 minutes
                const int SPIKE_WINDOW_MINUTES = 30;
                const int rthDurationSec = rthEndSec - rthStartSec;
                const int minutesFromOpen = (tSec >= rthStartSec) ?
                    (tSec - rthStartSec) / 60 : 0;
                const int minutesToClose = (rthDurationSec / 60) - minutesFromOpen;

                const bool inSpikeWindow = (st->sessionMgr.IsRTH() &&
                                            minutesToClose <= SPIKE_WINDOW_MINUTES &&
                                            minutesToClose >= 0);

                if (inSpikeWindow)
                {
                    // Check if this bar made a new session extreme
                    const bool madeNewHigh = (probeHigh >= sessHi);
                    const bool madeNewLow = (probeLow <= sessLo);

                    if (madeNewHigh || madeNewLow)
                    {
                        // Price before the spike (approximate with previous close)
                        const double priceBeforeSpike = (curBarIdx > 0) ?
                            sc.Close[curBarIdx - 1] : probeOpen;

                        // Detect the spike
                        st->lastDaltonState.spikeContext.DetectSpike(
                            probeHigh, probeLow, priceBeforeSpike,
                            sessHi, sessLo,
                            currentBar, madeNewHigh, madeNewLow);

                        // Log spike detection
                        if (diagLevel >= 1 && st->lastDaltonState.spikeContext.hasSpike)
                        {
                            SCString spikeMsg;
                            spikeMsg.Format("Bar %d | SPIKE_DETECTED: dir=%s range=[%.2f-%.2f] origin=%.2f | %d min to close",
                                currentBar,
                                st->lastDaltonState.spikeContext.isUpSpike ? "UP" : "DOWN",
                                st->lastDaltonState.spikeContext.spikeLow,
                                st->lastDaltonState.spikeContext.spikeHigh,
                                st->lastDaltonState.spikeContext.spikeOrigin,
                                minutesToClose);
                            st->logManager.LogInfo(currentBar, spikeMsg.GetChars(), LogCategory::AMT);
                        }
                    }
                }
            }

            // ================================================================
            // AMT SIGNAL ENGINE UPDATE
            // Process bar through AMT signal engine for:
            //   - Activity classification (Intent × Participation → ActivityType)
            //   - State tracking (BALANCE/IMBALANCE with leaky accumulator)
            //   - Excess detection (poor high/low, confirmed excess)
            //   - Single print zone tracking (session-persistent)
            // ================================================================
            {
                // Get VbP SSOT values for value center
                const double poc = st->sessionVolumeProfile.session_poc;
                const double vah = st->sessionVolumeProfile.session_vah;
                const double val = st->sessionVolumeProfile.session_val;

                // Previous bar price for direction calculation
                const double prevPrice = (curBarIdx > 0) ? sc.Close[curBarIdx - 1] : probeClose;

                // Delta as fraction of volume (-1 to +1)
                const double barVolume = st->currentSnapshot.effort.totalVolume;
                const double barDeltaRaw = st->currentSnapshot.effort.delta;
                const double deltaPct = (barVolume > 0.0) ? barDeltaRaw / barVolume : 0.0;

                // Session extremes for excess detection (from StructureTracker SSOT)
                const double sessionHigh = structure.GetSessionHigh();
                const double sessionLow = structure.GetSessionLow();

                // ============================================================
                // VOLUME CONVICTION (Per Dalton: Volume confirms conviction)
                // ============================================================
                // volumeConviction = volumePercentile / 50.0, clamped to [0, 2]
                // - 0.0 = 0th percentile (VOLUME_VACUUM, no conviction)
                // - 1.0 = 50th percentile (normal conviction)
                // - 2.0 = 100th percentile (high conviction, strong signal)
                // This weights the strength contribution so low-volume bars
                // don't artificially drive BALANCE/IMBALANCE transitions.
                double volumeConviction = 1.0;  // Default to normal
                {
                    const AMT::SessionPhase signalPhase = st->phaseCoordinator.GetPhase();
                    const int bucketIdx = AMT::SessionPhaseToBucketIndex(signalPhase);
                    if (bucketIdx >= 0 && bucketIdx < AMT::EFFORT_BUCKET_COUNT) {
                        const auto& bucketDist = st->effortBaselines.Get(signalPhase);
                        // Compute volume-per-second for current bar
                        const double barIntervalSec = (sc.SecondsPerBar > 0) ?
                            static_cast<double>(sc.SecondsPerBar) : 60.0;
                        const double curVolSec = barVolume / barIntervalSec;
                        // Query percentile from phase-specific baseline
                        // TryPercentile returns valid=false if baseline not ready
                        const AMT::PercentileResult volResult = bucketDist.vol_sec.TryPercentile(curVolSec);
                        if (volResult.valid) {
                            // Convert percentile to conviction: 50th%ile = 1.0
                            volumeConviction = volResult.value / 50.0;
                            // Clamp to [0.0, 2.0]
                            volumeConviction = (std::max)(0.0, (std::min)(2.0, volumeConviction));
                        }
                    }
                }

                // Only process if we have valid VA data
                if (poc > 0.0 && vah > val && val > 0.0) {
                    // ============================================================
                    // STEP 1: DALTON ENGINE (1TF/2TF) - SSOT for Balance/Imbalance
                    // Per Dalton: 1TF/2TF is the DETECTION MECHANISM for state.
                    // Activity classification (below) determines WHO is in control.
                    // ============================================================
                    // daltonState and daltonPhase declared at outer scope (line ~5146)
                    {
                        // Calculate minutes from open (for IB window tracking)
                        const int minutesFromOpen = (tSec >= rthStartSec) ?
                            (tSec - rthStartSec) / 60 : 0;

                        // ============================================================
                        // EXTREME DELTA FROM DELTA ENGINE (SSOT - Dec 2024)
                        // DeltaEngine computes extreme delta; we just read it here.
                        // DeltaEngine.Compute() runs at line ~4488, before this point.
                        // ============================================================
                        const bool extremeDeltaBar = st->lastDeltaResult.isExtremeDeltaBar;
                        const bool extremeDeltaSession = st->lastDeltaResult.isExtremeDeltaSession;
                        const bool deltaCoherence = st->lastDeltaResult.directionalCoherence;

                        // Determine session type for Dalton processing
                        const bool isGlobexSession = st->sessionMgr.IsGlobex();

                        // Process bar through Dalton engine FIRST
                        st->lastDaltonState = st->daltonEngine.ProcessBar(
                            probeHigh,      // Bar high
                            probeLow,       // Bar low
                            probeClose,     // Bar close
                            prevPrice,      // Previous close
                            poc,            // Point of Control
                            vah,            // Value Area High
                            val,            // Value Area Low
                            deltaPct,       // Bar delta as fraction of volume
                            sc.TickSize,    // Tick size
                            minutesFromOpen,// Minutes since RTH open
                            currentBar,     // Bar index
                            extremeDeltaBar,     // SSOT: Per-bar extreme
                            extremeDeltaSession, // SSOT: Session extreme
                            deltaCoherence,      // SSOT: Directional coherence
                            isGlobexSession      // NEW: Session type flag (Jan 2025)
                        );

                        // ============================================================
                        // OPENING TYPE CLASSIFICATION (Jan 2025)
                        // Classify Dalton's 4 opening types in first 30 min of RTH.
                        // ============================================================
                        if (!isGlobexSession && minutesFromOpen <= 30) {
                            st->daltonEngine.UpdateOpeningClassification(
                                probeHigh, probeLow, probeClose, probeOpen,
                                minutesFromOpen, currentBar, sc.TickSize);
                        }

                        // Update gap fill status during RTH
                        if (!isGlobexSession) {
                            st->daltonEngine.UpdateGapFill(probeHigh, probeLow);
                        }

                        // Check HVN/LVN proximity (updates atHVN/atLVN in state)
                        AMT::DaltonEngine::CheckVolumeNodeProximity(
                            st->lastDaltonState,
                            probeClose,
                            sc.TickSize,
                            2,  // toleranceTicks
                            st->sessionVolumeProfile.session_hvn,
                            st->sessionVolumeProfile.session_lvn
                        );

                        // Compute volume confirmation from baseline percentile
                        // Use volume rate (vol_sec) percentile from effort baseline
                        double volumePctile = -1.0;
                        {
                            const AMT::SessionPhase volPhase = st->phaseCoordinator.GetPhase();
                            const auto& bucket = st->effortBaselines.Get(volPhase);
                            if (bucket.vol_sec.size() >= 10) {
                                const double volRate = st->currentSnapshot.effort.bidVolSec +
                                                       st->currentSnapshot.effort.askVolSec;
                                volumePctile = bucket.vol_sec.percentile(volRate);
                            }
                        }
                        st->lastDaltonState.volumeConf =
                            AMT::DaltonState::DeriveVolumeConfirmation(volumePctile);

                        // ========================================================
                        // DALTON ACCEPTANCE TRACKING
                        // "One hour of trading at a new level constitutes initial acceptance"
                        // Track time at current price level for acceptance validation.
                        // ========================================================
                        {
                            // Get bar interval for time calculation
                            const int barIntervalSec = sc.SecondsPerBar;

                            // Define "level" based on value area relationship
                            // If outside value area, we're at a "new" level to track
                            const bool outsideVA = (st->lastDaltonState.location == AMT::ValueLocation::ABOVE_VALUE ||
                                                    st->lastDaltonState.location == AMT::ValueLocation::BELOW_VALUE);

                            // Determine current level anchor (use close for simplicity)
                            // Reset tracking if level changed significantly (> 4 ticks)
                            const double levelDiff = std::abs(probeClose - st->lastDaltonState.levelAnchorPrice);
                            const double levelTolerance = 4.0 * sc.TickSize;

                            if (st->lastDaltonState.levelAnchorPrice <= 0.0 ||
                                levelDiff > levelTolerance)
                            {
                                // New level - reset tracking
                                st->lastDaltonState.levelAnchorPrice = probeClose;
                                st->lastDaltonState.barsAtCurrentLevel = 1;
                                st->lastDaltonState.tpoCountAtLevel = 1;
                            }
                            else
                            {
                                // Same level - increment counters
                                st->lastDaltonState.barsAtCurrentLevel++;

                                // TPO count approximation: count unique price touches
                                // For simplicity, increment if bar range overlaps level
                                if (probeHigh >= st->lastDaltonState.levelAnchorPrice &&
                                    probeLow <= st->lastDaltonState.levelAnchorPrice)
                                {
                                    st->lastDaltonState.tpoCountAtLevel++;
                                }
                            }

                            // Compute acceptance state
                            st->lastDaltonState.acceptance = AMT::DaltonState::ComputeAcceptance(
                                st->lastDaltonState.barsAtCurrentLevel,
                                barIntervalSec,
                                st->lastDaltonState.tpoCountAtLevel);

                            // Only accept if we're outside prior value (accepting NEW value)
                            if (!outsideVA)
                            {
                                // Inside value area - no acceptance of "new" value needed
                                st->lastDaltonState.acceptance = AMT::DaltonAcceptance::PROBING;
                            }
                        }

                        // ============================================================
                        // LEVEL ACCEPTANCE ENGINE: Process bar and update state
                        // ============================================================
                        {
                            // Register session extremes and IB levels
                            const AMT::StructureTracker& structure = st->amtZoneManager.structure;
                            st->levelAcceptance.RegisterLevel(AMT::LevelType::SESSION_HIGH, structure.GetSessionHigh());
                            st->levelAcceptance.RegisterLevel(AMT::LevelType::SESSION_LOW, structure.GetSessionLow());

                            if (structure.IsIBFrozen()) {
                                st->levelAcceptance.RegisterLevel(AMT::LevelType::IB_HIGH, structure.GetIBHigh());
                                st->levelAcceptance.RegisterLevel(AMT::LevelType::IB_LOW, structure.GetIBLow());
                            }

                            // Compute close strength (0 = weak/tail, 1 = strong close)
                            const double barRange = probeHigh - probeLow;
                            double closeStrength = 0.5;  // Default neutral
                            if (barRange > 0.0) {
                                // For bullish bar, close near high = strong; for bearish, close near low = strong
                                const double closePct = (probeClose - probeLow) / barRange;
                                const bool isBullish = probeClose > probeOpen;
                                closeStrength = isBullish ? closePct : (1.0 - closePct);
                            }

                            // Process bar through level acceptance engine
                            st->levelAcceptance.ProcessBar(
                                currentBar,
                                probeHigh,
                                probeLow,
                                probeClose,
                                st->currentSnapshot.effort.totalVolume,
                                st->currentSnapshot.effort.delta,
                                closeStrength);

                            // Populate DaltonState with level acceptance signals
                            st->lastDaltonState.hasLVNAcceptance = st->levelAcceptance.HasLVNAcceptance();
                            st->lastDaltonState.hasHVNRejection = st->levelAcceptance.HasHVNRejection();
                            st->lastDaltonState.hasIBBreak = st->levelAcceptance.HasIBBreak(&st->lastDaltonState.ibBreakIsUp);
                            st->lastDaltonState.levelDirectionSignal = st->levelAcceptance.GetNetDirectionalSignal();

                            // Get VAH/VAL outcomes
                            st->lastDaltonState.vahOutcome = st->levelAcceptance.GetOutcome(AMT::LevelType::VAH);
                            st->lastDaltonState.valOutcome = st->levelAcceptance.GetOutcome(AMT::LevelType::VAL);
                        }

                        // Derive trading bias (uses volumeConf, valueMigration, acceptance, spike, level acceptance)
                        st->lastDaltonState.bias = st->lastDaltonState.DeriveTradingBias();

                        // Extract the authoritative state, phase, reason, bias, volume from Dalton
                        daltonState = st->lastDaltonState.phase;
                        daltonPhase = st->lastDaltonState.DeriveCurrentPhase();
                        daltonReason = st->lastDaltonState.DerivePhaseReason();
                        daltonBias = st->lastDaltonState.bias;
                        daltonVolConf = st->lastDaltonState.volumeConf;

                        // Log Dalton state on transitions or periodically (diagLevel >= 2)
                        if (diagLevel >= 2 && (currentBar % 10 == 0)) {
                            const AMT::DaltonState& ds = st->lastDaltonState;
                            SCString daltonMsg;
                            daltonMsg.Format(
                                "Bar %d | DALTON: TF=%s phase=%s act=%s | "
                                "IB: %.2f-%.2f ext=%s ratio=%.1f | rot=%d day=%s",
                                currentBar,
                                AMT::TimeframePatternToString(ds.timeframe),
                                AMT::AMTMarketStateToString(ds.phase),
                                AMT::AMTActivityTypeToString(ds.activity),
                                ds.ibLow, ds.ibHigh,
                                AMT::RangeExtensionTypeToString(ds.extension),
                                ds.extensionRatio,
                                ds.rotationFactor,
                                AMT::DaltonDayTypeToString(ds.dayType));
                            st->logManager.LogInfo(currentBar, daltonMsg.GetChars(), LogCategory::AMT);
                        }
                    }

                    // ============================================================
                    // STEP 2: SIGNAL ENGINE - Activity + State Tracking
                    // Uses daltonState/daltonPhase as SSOT.
                    // Computes WHO is in control (INITIATIVE/RESPONSIVE).
                    // ============================================================
                    // Get tail sizes at extremes from volume profile (for excess detection)
                    const double tailAtHigh = st->sessionVolumeProfile.GetTailAtExtreme(sessionHigh, poc);
                    const double tailAtLow = st->sessionVolumeProfile.GetTailAtExtreme(sessionLow, poc);

                    // Process bar through signal engine (with Dalton state/phase as SSOT)
                    AMT::StateEvidence evidence = st->amtSignalEngine.ProcessBar(
                        probeClose,
                        prevPrice,
                        poc, vah, val,
                        deltaPct,
                        sc.TickSize,
                        sessionHigh,
                        sessionLow,
                        currentBar,
                        tailAtHigh,
                        tailAtLow,
                        volumeConviction,  // Weight strength by volume
                        daltonState,       // SSOT: Balance/Imbalance from Dalton (1TF/2TF)
                        daltonPhase        // SSOT: CurrentPhase from Dalton
                    );

                    // Store for logging and external access
                    st->lastStateEvidence = evidence;

                    // Set structure flags
                    evidence.rangeExtended = (probeHigh >= sessionHigh || probeLow <= sessionLow);
                    evidence.ibBroken = structure.IsIBFrozen() &&
                        (probeHigh > structure.GetIBHigh() || probeLow < structure.GetIBLow());

                    // ============================================================
                    // SINGLE PRINT DETECTION (profile-structural, session-persistent)
                    // Detect thin-volume zones that indicate one-sided aggressive moves
                    // ============================================================
                    {
                        std::vector<double> volumeArray;
                        double priceStart = 0.0;
                        double avgVolume = 0.0;

                        const int numLevels = st->sessionVolumeProfile.ExtractVolumeArray(
                            volumeArray, priceStart, avgVolume);

                        if (numLevels > 0 && avgVolume > 0.0) {
                            // Detect new single print zones from current profile
                            auto newZones = st->amtSignalEngine.DetectSinglePrints(
                                volumeArray.data(),
                                priceStart,
                                sc.TickSize,
                                numLevels,
                                avgVolume,
                                currentBar
                            );

                            // Merge new zones with existing (avoiding duplicates)
                            for (const auto& newZone : newZones) {
                                bool isDuplicate = false;
                                for (const auto& existingZone : st->singlePrintZones) {
                                    // Check if zones overlap significantly
                                    if (std::abs(newZone.GetCenter() - existingZone.GetCenter()) < sc.TickSize * 3) {
                                        isDuplicate = true;
                                        break;
                                    }
                                }
                                if (!isDuplicate) {
                                    st->singlePrintZones.push_back(newZone);

                                    // Log new single print zone detection
                                    if (diagLevel >= 2) {
                                        SCString spMsg;
                                        spMsg.Format("Bar %d | [SP-DETECT] New single print zone: %.2f-%.2f (%d ticks)",
                                            currentBar, newZone.lowPrice, newZone.highPrice, newZone.widthTicks);
                                        st->logManager.LogInfo(currentBar, spMsg.GetChars(), LogCategory::AMT);
                                    }
                                }
                            }

                            // Update fill progress for existing zones
                            st->amtSignalEngine.UpdateSinglePrintFill(
                                st->singlePrintZones,
                                volumeArray.data(),
                                priceStart,
                                sc.TickSize,
                                numLevels,
                                avgVolume
                            );

                            // Remove fully filled zones
                            st->singlePrintZones.erase(
                                std::remove_if(st->singlePrintZones.begin(), st->singlePrintZones.end(),
                                    [](const AMT::SinglePrintZone& z) { return !z.valid; }),
                                st->singlePrintZones.end()
                            );
                        }

                        // Update evidence with single print info
                        evidence.singlePrintZonePresent = !st->singlePrintZones.empty();

                        // Check if price is currently in a single print zone
                        for (const auto& spZone : st->singlePrintZones) {
                            if (spZone.Contains(probeClose, sc.TickSize)) {
                                // Price is in a single print zone - log if diagLevel high enough
                                if (diagLevel >= 2) {
                                    SCString spMsg;
                                    spMsg.Format("Bar %d | [SP-TOUCH] Price %.2f in single print zone %.2f-%.2f (fill=%.0f%%)",
                                        currentBar, probeClose, spZone.lowPrice, spZone.highPrice, spZone.fillProgress * 100.0);
                                    st->logManager.LogInfo(currentBar, spMsg.GetChars(), LogCategory::AMT);
                                }
                                break;
                            }
                        }
                    }

                    // Update stored evidence with final flags
                    st->lastStateEvidence = evidence;

                    // NOTE: Dalton engine processing moved to STEP 1 above (SSOT for Balance/Imbalance)
                    // The 1TF/2TF derived state is now passed to the signal engine.

                    // Log on state transitions (full snapshot) or periodically
                    if (evidence.IsTransition() || (diagLevel >= 2 && (currentBar % 10 == 0))) {
                        st->logManager.LogAMTStateEvidence(currentBar, evidence, probeClose);
                    }
                }
            }

            // Update IB levels (only during IB window)
            // SSOT: Use sessionMgr.IsRTH() - single source for RTH determination
            structure.UpdateIB(probeHigh, probeLow, probeBarTime, currentBar, st->sessionMgr.IsRTH());
            structure.CheckIBFreeze(probeBarTime, currentBar);

            // Update range-adaptive thresholds (logging removed - too verbose)
            structure.UpdateAdaptiveThresholds(sc.TickSize, currentBar);

            // ================================================================
            // DAY TYPE CLASSIFIER UPDATE (Phase 2)
            // ================================================================
            {
                AMT::DayTypeClassifier& dtc = st->dayTypeClassifier;

                // Notify IB complete when structure tracker freezes IB
                if (structure.IsIBFrozen() && !dtc.IsIBComplete()) {
                    dtc.NotifyIBComplete(currentBar, probeBarTime);
                    if (diagLevel >= 1) {
                        SCString ibMsg;
                        ibMsg.Format("Bar %d | IB complete: IB_HI=%.2f IB_LO=%.2f",
                            currentBar, structure.GetIBHigh(), structure.GetIBLow());
                        st->logManager.LogInfo(currentBar, ibMsg.GetChars(), LogCategory::DAYTYPE);
                    }
                }

                // Update RE tracking after IB complete
                if (dtc.IsIBComplete() && !dtc.IsClassified()) {
                    // Profile maturity: require minimum bars after IB
                    const int barsAfterIB = currentBar - st->sessionMgr.GetSessionStartBar();
                    const bool profileMature = (barsAfterIB >= 12);  // ~1 hour at 5-min bars
                    dtc.NotifyProfileMature(profileMature);

                    // Get values for RE tracking
                    const double ibHigh = structure.GetIBHigh();
                    const double ibLow = structure.GetIBLow();
                    const double barVolume = st->currentSnapshot.effort.totalVolume;
                    const double barDelta = st->currentSnapshot.effort.delta;
                    const double sessionVol = st->sessionAccum.sessionTotalVolume;

                    // Update RE tracking (use bar High/Low to detect extensions, Close for rejection)
                    AMT::RangeExtensionState reState = dtc.UpdateRETracking(
                        probeHigh, probeLow, probeClose,
                        ibHigh, ibLow,
                        barVolume, barDelta, sessionVol,
                        currentBar, probeBarTime, sc.TickSize);

                    // Log RE state changes (diagLevel >= 2)
                    if (diagLevel >= 2 && reState != AMT::RangeExtensionState::NONE) {
                        const AMT::REAttempt& attempt = dtc.GetCurrentAttempt();
                        const double volPct = sessionVol > 0.0 ?
                            (attempt.volumeOutsideIB / sessionVol) * 100.0 : 0.0;
                        SCString reMsg;
                        reMsg.Format("Bar %d | RE_%s %s | ext=%.2f bars=%d vol=%.0f (%.1f%%)",
                            currentBar,
                            AMT::to_string(attempt.direction),
                            AMT::to_string(reState),
                            attempt.furthestExtension,
                            attempt.barsOutsideIB,
                            attempt.volumeOutsideIB,
                            volPct);
                        st->logManager.LogDebug(currentBar, reMsg.GetChars(), LogCategory::DAYTYPE);
                    }

                    // Update VA migration tracking
                    dtc.UpdateVAMigration(
                        st->sessionMgr.GetVAH(), st->sessionMgr.GetVAL(),
                        ibHigh, ibLow, sc.TickSize);

                    // Try classification
                    const bool classified = dtc.TryClassify(currentBar, probeBarTime);
                    if (classified && diagLevel >= 1) {
                        char reSummary[64];
                        dtc.FormatRESummary(reSummary, sizeof(reSummary));
                        SCString classMsg;
                        classMsg.Format("Bar %d | DAYTYPE: %s | %s | VA_MIG=%s",
                            currentBar,
                            AMT::to_string(dtc.GetClassification()),
                            reSummary,
                            dtc.FormatVAMigration());
                        st->logManager.LogInfo(currentBar, classMsg.GetChars(), LogCategory::DAYTYPE);
                    }
                }
            }
        }

        // === DOM-AWARE DYNAMIC WIDTH UPDATE ===
        // Update POC zone widths based on DOM liquidity concentration
        AMT::ZoneRuntime* amtPOC = st->amtZoneManager.GetPOC();
        if (amtPOC && amtPOC->GetAnchorPrice() > 0.0)
        {
            const int maxDepthLevels = sc.Input[14].GetInt();   // Max Depth Levels
            const int maxBandTicks = sc.Input[15].GetInt();     // Max Band Ticks
            const double targetPct = sc.Input[16].GetFloat();   // Target Depth Mass %
            const double haloMult = sc.Input[17].GetFloat();    // Halo Multiplier

            const int liqTicks = ComputeLiquidityCoreTicks(
                sc,
                st->depthPointsCache,  // Reusable buffer
                amtPOC->GetAnchorPrice(),
                maxDepthLevels,
                maxBandTicks,
                targetPct,
                sc.TickSize
            );

            // Only update when liqTicks changes (avoid redundant updates)
            if (st->cachedAmtLiqTicks != liqTicks) {
                st->cachedAmtLiqTicks = liqTicks;
                AMT::UpdateZoneDynamicWidths(*amtPOC, liqTicks, haloMult);
            }
        }

        // Build phase snapshot using AMT framework (SINGLE AUTHORITATIVE LOCUS)
        // All downstream consumers use this snapshot; no recomputation allowed.
        // All Dalton outputs are REQUIRED (SSOT) - no fallback.
        amtSnapshot = AMT::BuildPhaseSnapshot(
            st->amtZoneManager,
            currentPrice,
            currentPrice,          // closePrice = currentPrice = probeClose
            sc.TickSize,
            curBarIdx,             // Current bar index
            st->amtPhaseTracker,
            daltonState,           // SSOT: from Dalton 1TF/2TF
            daltonPhase,           // SSOT: from DaltonState.DeriveCurrentPhase()
            daltonReason,          // SSOT: from DaltonState.DerivePhaseReason()
            daltonBias,            // SSOT: from DaltonState.DeriveTradingBias()
            daltonVolConf);        // SSOT: from volume baseline percentile
        // Extract primary phase for backward compatibility
        amtPhase = amtSnapshot.phase;

        // Track phase history for session stats
        st->amtPhaseHistory.push_back(amtPhase);

        // Track phase transitions (for session accumulators)
        if (amtPhase != st->lastLoggedPhase)
        {
            st->sessionAccum.phaseTransitionCount++;
            st->lastLoggedPhase = amtPhase;
        }

        // ================================================================
        // PHASE/REGIME INVARIANT VALIDATION (diagLevel >= 3)
        // Runtime observability for state machine invariants
        // ================================================================
        AMT::ValidatePhaseRegimeInvariants(amtSnapshot, st->amtPhaseTracker,
                                           curBarIdx, sc, diagLevel);

        st->amtLastZoneUpdateBar = curBarIdx;

#if PERF_TIMING_ENABLED
        if (sc.IsFullRecalculation) {
            st->perfStats.zoneMs += zoneTimer.ElapsedMs();
        }
#endif

        // ================================================================
        // PHASE 2: Log finalized engagements (live bar only to avoid spam)
        // ================================================================
        if (diagLevel >= 2 && isLiveBar && !st->amtZoneManager.finalizedThisBar.empty())
        {
            for (int zid : st->amtZoneManager.finalizedThisBar)
            {
                AMT::ZoneRuntime* z = st->amtZoneManager.GetZone(zid);
                if (z)
                {
                    const AMT::EngagementMetrics& eng = z->currentEngagement;
                    SCString msg;
                    msg.Format("zone=%d type=%d entry=%d exit=%d bars=%d escVel=%.2f",
                        z->zoneId, static_cast<int>(z->type),
                        eng.startBar, eng.endBar, eng.barsEngaged, eng.escapeVelocity);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::ZONE);
                }
            }
        }

#ifdef VALIDATE_ZONE_MIGRATION
        // ================================================================
        // PHASE 3: Validation logging - only log mismatches (low noise)
        // Compare most recent AMT episodes with their legacy matches
        // ================================================================
        if (!st->amtZoneManager.finalizedThisBar.empty())
        {
            // Check last N AMT episodes for mismatches
            const auto& amtEpisodes = st->validationState.amtEpisodes;
            for (size_t i = 0; i < amtEpisodes.size(); ++i)
            {
                const auto& amtEp = amtEpisodes[i];
                if (!amtEp.matched) continue;  // Skip unmatched for now

                // Find matching legacy episode
                const AMT::ValidationEpisode* legacyEp =
                    st->validationState.FindMatchingLegacy(amtEp, tickSize);
                if (!legacyEp) continue;

                // Compare and log mismatches
                AMT::ValidationMismatchReason reason =
                    st->validationState.CompareEpisodes(*legacyEp, amtEp);

                if (reason != AMT::ValidationMismatchReason::NONE)
                {
                    st->validationState.counters.mismatchCount++;
                    st->validationState.counters.IncrementForReason(reason);
                    st->sessionAccum.validationDivergenceCount++;

                    // Log mismatch (low noise - only on actual mismatch)
                    SCString msg;
                    msg.Format("[VAL-MISMATCH] reason=%s anchor=%.2f legEntry=%d amtEntry=%d "
                               "legBars=%d amtBars=%d legEscVel=%.4f amtEscVel=%.4f",
                        AMT::GetMismatchReasonString(reason),
                        amtEp.anchorPrice,
                        legacyEp->entryBar, amtEp.entryBar,
                        legacyEp->barsEngaged, amtEp.barsEngaged,
                        legacyEp->escapeVelocity, amtEp.escapeVelocity);
                    st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::SYSTEM);
                }
            }
        }
#endif

        // --- SUBGRAPH DRAWING ---
        AMT::ZoneRuntime* vah = st->amtZoneManager.GetVAH();
        AMT::ZoneRuntime* poc = st->amtZoneManager.GetPOC();
        AMT::ZoneRuntime* val = st->amtZoneManager.GetVAL();

        // DIAGNOSTIC: Log zone status on live bar if zones are missing
        if (isLiveBar && diagLevel >= 1)
        {
            if (!vah || !poc || !val)
            {
                SCString msg;
                msg.Format("MISSING ZONES: vah=%s poc=%s val=%s | IDs: vahId=%d pocId=%d valId=%d | initialized=%d | activeZones=%d | display: POC=%.2f VAH=%.2f VAL=%.2f",
                    vah ? "OK" : "NULL", poc ? "OK" : "NULL", val ? "OK" : "NULL",
                    st->amtZoneManager.vahId, st->amtZoneManager.pocId, st->amtZoneManager.valId,
                    st->amtZonesInitialized ? 1 : 0,
                    static_cast<int>(st->amtZoneManager.activeZones.size()),
                    st->displayPOC, st->displayVAH, st->displayVAL);
                st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::ZONE);
            }
        }

        // === DRAW ZONE LINES (DECOUPLED FROM ZONE OBJECTS) ===
        // Use decoupled display levels as SSOT - these persist even if zones are pruned.
        // This ensures display never goes to 0 due to zone cleanup/expiration.
        if (st->displayLevelsValid) {
            sc.Subgraph[0][curBarIdx] = static_cast<float>(st->displayVAH);
            sc.Subgraph[1][curBarIdx] = static_cast<float>(st->displayPOC);
            sc.Subgraph[2][curBarIdx] = static_cast<float>(st->displayVAL);
        } else if (vah && poc && val) {
            // Fallback to zone objects if display levels not yet initialized
            sc.Subgraph[0][curBarIdx] = static_cast<float>(vah->GetAnchorPrice());
            sc.Subgraph[1][curBarIdx] = static_cast<float>(poc->GetAnchorPrice());
            sc.Subgraph[2][curBarIdx] = static_cast<float>(val->GetAnchorPrice());
        }
        // If neither valid, leave subgraphs unchanged (preserves prior value)

        // Store phase and proximity as data
        sc.Subgraph[3][curBarIdx] = static_cast<float>(static_cast<int>(amtPhase));

        // Find nearest zone and store proximity
        // CONTRACT A: Uses halo-based tolerance (default) for consistency with FSM
        AMT::ZoneRuntime* nearest = st->amtZoneManager.GetStrongestZoneAtPrice(
            currentPrice, sc.TickSize);  // Uses halo width by default
        if (nearest)
        {
            sc.Subgraph[4][curBarIdx] = static_cast<float>(static_cast<int>(nearest->proximity));
            sc.Subgraph[5][curBarIdx] = static_cast<float>(nearest->strengthScore);
        }

        // =======================================================================
        // LOGGING FIX: Store zone/context values to logging subgraphs AT BAR CLOSE
        // This captures finalized values for accurate retrospective CSV logging.
        // Without this, the CSV lambda would use current-bar values for historical bars.
        // =======================================================================
        {
            // Detect bar close: previous bar just closed (or current bar closed for historical)
            const int prevBar = curBarIdx - 1;
            const bool prevBarJustClosed = (prevBar >= 0) &&
                (sc.GetBarHasClosedStatus(prevBar) == BHCS_BAR_HAS_CLOSED) &&
                (st->lastBarCloseStoredBar < prevBar);

            // Also handle the case where we're on the last bar and IT just closed
            const bool curBarJustClosed = (curBarIdx == sc.ArraySize - 1) &&
                (sc.GetBarHasClosedStatus(curBarIdx) == BHCS_BAR_HAS_CLOSED) &&
                (st->lastBarCloseStoredBar < curBarIdx);

            // Helper lambda: calculate proximity at a given price for a zone
            auto calcProximityAtPrice = [&](AMT::ZoneRuntime* zone, double price) -> int {
                if (!zone) return 0;
                const long long priceTicks = AMT::PriceToTicks(price, sc.TickSize);
                const long long anchorTicks = zone->GetAnchorTicks();
                const long long distTicks = std::abs(priceTicks - anchorTicks);
                const int coreWidth = st->amtZoneManager.config.GetCoreWidth();
                const int haloWidth = st->amtZoneManager.config.GetHaloWidth();
                if (distTicks <= coreWidth) return 2;  // AT_ZONE
                if (distTicks <= haloWidth) return 1;  // APPROACHING
                return 0;  // INACTIVE
            };

            if (prevBarJustClosed)
            {
                const double prevClose = sc.Close[prevBar];

                // Store zone anchor prices at bar close time
                sc.Subgraph[6][prevBar] = static_cast<float>(poc ? poc->GetAnchorPrice() : 0.0);
                sc.Subgraph[7][prevBar] = static_cast<float>(vah ? vah->GetAnchorPrice() : 0.0);
                sc.Subgraph[8][prevBar] = static_cast<float>(val ? val->GetAnchorPrice() : 0.0);

                // Store proximity calculated at bar close price
                sc.Subgraph[9][prevBar] = static_cast<float>(calcProximityAtPrice(poc, prevClose));
                sc.Subgraph[10][prevBar] = static_cast<float>(calcProximityAtPrice(vah, prevClose));
                sc.Subgraph[11][prevBar] = static_cast<float>(calcProximityAtPrice(val, prevClose));

                // Store facilitation/market state/delta at bar close
                // Phase 2: Use new AMT signal engine state as SSOT
                sc.Subgraph[12][prevBar] = static_cast<float>(static_cast<int>(st->amtContext.facilitation));
                sc.Subgraph[13][prevBar] = static_cast<float>(static_cast<int>(st->lastStateEvidence.currentState));
                sc.Subgraph[14][prevBar] = static_cast<float>(st->amtContext.confidence.deltaConsistency);

                st->lastBarCloseStoredBar = prevBar;
            }

            if (curBarJustClosed)
            {
                const double curClose = sc.Close[curBarIdx];

                // Store zone anchor prices at bar close time
                sc.Subgraph[6][curBarIdx] = static_cast<float>(poc ? poc->GetAnchorPrice() : 0.0);
                sc.Subgraph[7][curBarIdx] = static_cast<float>(vah ? vah->GetAnchorPrice() : 0.0);
                sc.Subgraph[8][curBarIdx] = static_cast<float>(val ? val->GetAnchorPrice() : 0.0);

                // Store proximity calculated at bar close price
                sc.Subgraph[9][curBarIdx] = static_cast<float>(calcProximityAtPrice(poc, curClose));
                sc.Subgraph[10][curBarIdx] = static_cast<float>(calcProximityAtPrice(vah, curClose));
                sc.Subgraph[11][curBarIdx] = static_cast<float>(calcProximityAtPrice(val, curClose));

                // Store facilitation/market state/delta at bar close
                // Phase 2: Use new AMT signal engine state as SSOT
                sc.Subgraph[12][curBarIdx] = static_cast<float>(static_cast<int>(st->amtContext.facilitation));
                sc.Subgraph[13][curBarIdx] = static_cast<float>(static_cast<int>(st->lastStateEvidence.currentState));
                sc.Subgraph[14][curBarIdx] = static_cast<float>(st->amtContext.confidence.deltaConsistency);

                st->lastBarCloseStoredBar = curBarIdx;

#if LOGGING_VALIDATION_ENABLED
                // Validation: Check for unexpected zero values when zones exist
                if (st->displayLevelsValid && sc.Subgraph[6][curBarIdx] <= 0.0f)
                {
                    SCString warnMsg;
                    warnMsg.Format("VALIDATE: Bar %d POC=0 stored but displayLevelsValid=true (poc=%s)",
                        curBarIdx, poc ? "exists" : "NULL");
                    st->logManager.LogWarn(curBarIdx, warnMsg.GetChars(), LogCategory::AUDIT);
                }

                // Validation: Check facilitation is not UNDEFINED when baseline is sufficient
                if (st->facilitationComputed &&
                    static_cast<AMT::AuctionFacilitation>(static_cast<int>(sc.Subgraph[12][curBarIdx])) == AMT::AuctionFacilitation::UNDEFINED)
                {
                    SCString warnMsg;
                    warnMsg.Format("VALIDATE: Bar %d FACIL=UNDEFINED stored but facilitationComputed=true",
                        curBarIdx);
                    st->logManager.LogWarn(curBarIdx, warnMsg.GetChars(), LogCategory::AUDIT);
                }
#endif
            }
        }

        // --- CSV LOGGING (via LogManager) ---
        // Log at BAR CLOSE: use SC proper API to detect when bar has closed
        const bool csvEnabled = sc.Input[116].GetYesNo() != 0;  // Log: AMT CSV

        // GUARD: Only log at the LAST bar (per SC documentation best practice)
        // SC recommends: if (sc.Index == sc.ArraySize - 1) for logging
        // Pattern: Log ALL unlogged closed bars when we reach the last bar
        const bool isAtLastBar = (curBarIdx == sc.ArraySize - 1);

        // Zone counts for logging (current snapshot - for live bar only)
        const int activeZoneCount = static_cast<int>(st->amtZoneManager.activeZones.size());
        const int totalTouches = st->amtZoneManager.GetTotalTouches();

        // =======================================================================
        // LOGGING FIX: logBarToCsv now reads from STORED SUBGRAPHS (bar-close values)
        // This ensures historical bars get their own close-time values, not current-bar values.
        // Phase/POC/VAH/VAL/proximities are read from Subgraph[3] and Subgraph[6-11].
        // =======================================================================
        auto logBarToCsv = [&](int barIdx) {
            const double barClose = sc.Close[barIdx];
            const double barHigh = sc.High[barIdx];
            const double barLow = sc.Low[barIdx];
            const double barVolume = sc.Volume[barIdx];
            const SCDateTime barTime = sc.BaseDateTimeIn[barIdx];

            AmtBarData data;
            data.timestamp = barTime;
            data.barIndex = barIdx;
            data.price = barClose;
            data.high = barHigh;
            data.low = barLow;
            data.volume = barVolume;
            data.delta = sc.AskVolume[barIdx] - sc.BidVolume[barIdx];

            // FIX: Read phase from stored subgraph (set at bar close in storage block)
            const int storedPhaseInt = static_cast<int>(sc.Subgraph[3][barIdx]);
            data.phase = AMT::CurrentPhaseToString(static_cast<AMT::CurrentPhase>(storedPhaseInt));

            // FIX: Read zone prices from stored subgraphs (set at bar close)
            const double storedPOC = sc.Subgraph[6][barIdx];
            const double storedVAH = sc.Subgraph[7][barIdx];
            const double storedVAL = sc.Subgraph[8][barIdx];

            // Zone existence: check if stored price is valid (non-zero)
            data.hasPOC = (storedPOC > 0.0) ? 1 : 0;
            data.hasVAH = (storedVAH > 0.0) ? 1 : 0;
            data.hasVAL = (storedVAL > 0.0) ? 1 : 0;

            data.pocPrice = storedPOC;
            data.vahPrice = storedVAH;
            data.valPrice = storedVAL;

            // FIX: Read proximity from stored subgraphs (calculated at bar close)
            data.pocProximity = static_cast<int>(sc.Subgraph[9][barIdx]);
            data.vahProximity = static_cast<int>(sc.Subgraph[10][barIdx]);
            data.valProximity = static_cast<int>(sc.Subgraph[11][barIdx]);

            // Touch counts: Cannot recover per-bar historical values without additional storage
            // Use 0 for historical bars (these are cumulative counters that weren't stored per-bar)
            // For the current bar only, we can use live values
            const bool isCurrentBar = (barIdx == curBarIdx);
            if (isCurrentBar && vah) data.vahTouches = vah->touchCount; else data.vahTouches = 0;
            if (isCurrentBar && poc) data.pocTouches = poc->touchCount; else data.pocTouches = 0;
            if (isCurrentBar && val) data.valTouches = val->touchCount; else data.valTouches = 0;

            // Strength scores: similarly, only accurate for current bar
            if (isCurrentBar && vah) data.vahStrength = vah->strengthScore; else data.vahStrength = 0.0;
            if (isCurrentBar && poc) data.pocStrength = poc->strengthScore; else data.pocStrength = 0.0;
            if (isCurrentBar && val) data.valStrength = val->strengthScore; else data.valStrength = 0.0;

            // CONTRACT: Zone counts only accurate for current bar (not stored per-bar)
            // For historical bars, use 0 to avoid misrepresenting context
            if (isCurrentBar) {
                data.activeZoneCount = activeZoneCount;
                data.totalTouches = totalTouches;
            } else {
                data.activeZoneCount = 0;  // Historical: unavailable
                data.totalTouches = 0;     // Historical: unavailable
            }

#if LOGGING_VALIDATION_ENABLED
            // Validation: Verify stored values were set (non-zero when expected)
            // Subgraph[3] (phase) should always have been set for closed bars
            if (sc.Subgraph[3][barIdx] == 0.0f && barIdx > st->sessionMgr.sessionStartBar + 10)
            {
                SCString warnMsg;
                warnMsg.Format("VALIDATE: CSV Bar %d has phase=0 in subgraph (may be uninitialized)",
                    barIdx);
                st->logManager.LogWarn(barIdx, warnMsg.GetChars(), LogCategory::AUDIT);
            }

            // For current bar, verify stored matches live (sanity check)
            if (isCurrentBar && poc)
            {
                const float storedPocPrice = sc.Subgraph[6][barIdx];
                const float livePocPrice = static_cast<float>(poc->GetAnchorPrice());
                if (std::abs(storedPocPrice - livePocPrice) > 0.01f)
                {
                    SCString warnMsg;
                    warnMsg.Format("VALIDATE: Bar %d POC mismatch stored=%.2f live=%.2f",
                        barIdx, storedPocPrice, livePocPrice);
                    st->logManager.LogWarn(barIdx, warnMsg.GetChars(), LogCategory::AUDIT);
                }
            }
#endif

            st->logManager.LogAmtBar(data);
        };

        // When at the last bar, log ALL unlogged closed bars in chronological order
        if (csvEnabled && isAtLastBar)
        {
            // Find the range of bars to log
            // Closed bars are from 0 to ArraySize-2 (last bar may still be open)
            const int lastClosedBar = sc.ArraySize - 2;
            const int firstBarToLog = st->lastAmtCsvLoggedBar + 1;

            // Log all unlogged closed bars in order (oldest to newest)
            int barsLogged = 0;
            for (int barIdx = firstBarToLog; barIdx <= lastClosedBar; ++barIdx)
            {
                if (sc.GetBarHasClosedStatus(barIdx) == BHCS_BAR_HAS_CLOSED)
                {
                    logBarToCsv(barIdx);
                    st->lastAmtCsvLoggedBar = barIdx;
                    ++barsLogged;
                }
            }

            // Also log the current bar if it's closed (historical playback final bar)
            const bool currBarClosed = (sc.GetBarHasClosedStatus(curBarIdx) == BHCS_BAR_HAS_CLOSED);
            if (currBarClosed && st->lastAmtCsvLoggedBar < curBarIdx)
            {
                logBarToCsv(curBarIdx);
                st->lastAmtCsvLoggedBar = curBarIdx;
                ++barsLogged;
            }

            // DEBUG: Log how many bars were logged
            if (diagLevel >= 1 && barsLogged > 0)
            {
                SCString logMsg;
                logMsg.Format("Logged %d bars (range %d-%d) | lastLogged=%d",
                    barsLogged, firstBarToLog, st->lastAmtCsvLoggedBar, st->lastAmtCsvLoggedBar);
                st->logManager.LogInfo(curBarIdx, logMsg.GetChars(), LogCategory::SYSTEM);
            }

            // Flush after batch logging
            st->logManager.MaybeFlush(curBarIdx, true);
        }

        // --- SESSION STATISTICS (every bar close) ---
        // Use GetBarHasClosedStatus to detect when previous bar has closed
        const int statsBarIdx = curBarIdx - 1;
        const bool statsBarClosed = (statsBarIdx >= 0) &&
            (sc.GetBarHasClosedStatus(statsBarIdx) == BHCS_BAR_HAS_CLOSED) &&
            (st->lastStatsLoggedBar < statsBarIdx);  // Not already logged
        const bool statsLastBarClosed = (curBarIdx == sc.ArraySize - 1) &&
            (sc.GetBarHasClosedStatus(curBarIdx) == BHCS_BAR_HAS_CLOSED) &&
            (st->lastStatsLoggedBar < curBarIdx);  // Not already logged

        if (diagLevel >= 1 && (statsBarClosed || statsLastBarClosed))
        {
            // Track which bar we're logging to prevent duplicates
            const int closedBarIdx = statsLastBarClosed ? curBarIdx : statsBarIdx;
            st->lastStatsLoggedBar = closedBarIdx;

            // Calculate session statistics
            AMT::SessionStatistics stats = AMT::CalculateSessionStats(
                st->amtZoneManager,
                st->engagementAccum,
                st->sessionMgr.GetPOC(),
                st->sessionMgr.GetVAH(),
                st->sessionMgr.GetVAL(),
                st->sessionMgr.GetVARangeTicks(),  // SSOT: from SessionManager
                amtPhase,
                currentBar,
                st->amtPhaseHistory);
            // --- MERGE SESSION ACCUMULATORS INTO STATS ---
            // Volume and Delta from internal accumulation (SSOT: native SC arrays)
            stats.totalVolume = st->sessionAccum.sessionTotalVolume;
            stats.netDelta = st->sessionAccum.sessionCumDelta;  // SSOT: internal accumulation (not NB)
            if (stats.totalBars > 0) {
                stats.avgVolumePerBar = stats.totalVolume / stats.totalBars;
                stats.avgDeltaPerBar = stats.netDelta / stats.totalBars;
            }

            // HVN/LVN metrics (now from SC native peaks/valleys)
            stats.hvnCount = static_cast<int>(st->sessionVolumeProfile.session_hvn.size());
            stats.lvnCount = static_cast<int>(st->sessionVolumeProfile.session_lvn.size());
            stats.hvnAdded = st->sessionAccum.hvnAdded;
            stats.hvnRemoved = st->sessionAccum.hvnRemoved;
            stats.lvnAdded = st->sessionAccum.lvnAdded;
            stats.lvnRemoved = st->sessionAccum.lvnRemoved;

            // Compute widest gap between adjacent LVN prices
            stats.widestLvnTicks = 0.0;
            if (st->sessionVolumeProfile.session_lvn.size() >= 2)
            {
                std::vector<double> sortedLvn = st->sessionVolumeProfile.session_lvn;
                std::sort(sortedLvn.begin(), sortedLvn.end());
                double maxGap = 0.0;
                for (size_t i = 1; i < sortedLvn.size(); ++i)
                {
                    const double gap = sortedLvn[i] - sortedLvn[i - 1];
                    if (gap > maxGap)
                        maxGap = gap;
                }
                stats.widestLvnTicks = maxGap / sc.TickSize;
            }

            // Zone engagement metrics
            stats.engagementCount = st->sessionAccum.engagementCount;
            stats.escapeCount = st->sessionAccum.escapeCount;
            if (stats.engagementCount > 0) {
                stats.avgEngagementBars = static_cast<double>(st->sessionAccum.totalEngagementBars) / stats.engagementCount;
            }
            if (stats.escapeCount > 0) {
                stats.avgEscapeVelocity = st->sessionAccum.totalEscapeVelocity / stats.escapeCount;
            }

            // Extreme condition counts
            stats.extremeVolumeCount = st->sessionAccum.extremeVolumeCount;
            stats.extremeDeltaCount = st->sessionAccum.extremeDeltaCount;
            stats.extremeTradesCount = st->sessionAccum.extremeTradesCount;
            stats.extremeStackCount = st->sessionAccum.extremeStackCount;
            stats.extremePullCount = st->sessionAccum.extremePullCount;
            stats.extremeDepthCount = st->sessionAccum.extremeDepthCount;
            stats.totalExtremeEvents = stats.extremeVolumeCount + stats.extremeDeltaCount +
                stats.extremeTradesCount + stats.extremeStackCount +
                stats.extremePullCount + stats.extremeDepthCount;

            // Probe metrics
            stats.probesFired = st->sessionAccum.probesFired;
            stats.probesResolved = st->sessionAccum.probesResolved;
            stats.probesHit = st->sessionAccum.probesHit;
            stats.probesMissed = st->sessionAccum.probesMissed;
            stats.probesExpired = st->sessionAccum.probesExpired;
            if (stats.probesResolved > 0) {
                stats.avgProbeScore = st->sessionAccum.totalProbeScore / stats.probesResolved;
                stats.probeHitRate = static_cast<double>(stats.probesHit) / stats.probesResolved * 100.0;
            }

            // Session/state transition metrics
            stats.sessionChangeCount = st->sessionAccum.sessionChangeCount;
            stats.phaseTransitionCount = st->sessionAccum.phaseTransitionCount;
            stats.intentChangeCount = st->sessionAccum.intentChangeCount;
            stats.marketStateChangeCount = st->sessionAccum.marketStateChangeCount;

            // Warning/error metrics
            stats.zoneWidthMismatchCount = st->sessionAccum.zoneWidthMismatchCount;
            stats.validationDivergenceCount = st->sessionAccum.validationDivergenceCount;
            stats.configErrorCount = st->sessionAccum.configErrorCount;
            stats.vbpWarningCount = st->sessionAccum.vbpWarningCount;

            // AMT Session Stats block (single throttle gate)
            // Force emit on session transition to capture first-bar SessionDelta
            // Only emit on last bar during recalc to avoid log spam (session change still forces emit)
            const bool forceStatsOnSessionChange = sessionChanged && diagLevel >= 1;
            const bool isLastBarStats = (curBarIdx == sc.ArraySize - 1);
            if ((st->logManager.ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MINIMAL, curBarIdx) &&
                st->logManager.ShouldLog(ThrottleKey::STATS_BLOCK, curBarIdx, 1) && isLastBarStats) || forceStatsOnSessionChange)
            {
                SCString msg;

                // Header
                st->logManager.LogToSC(LogCategory::AMT, "========== AMT SESSION STATISTICS ==========", false);

                // Zone summary
                msg.Format("Bar %d | Phase: %s | Zones: %d active",
                    curBarIdx,
                    AMT::CurrentPhaseToString(amtPhase),
                    stats.activeZones);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // AMT State/Phase telemetry
                msg.Format("STATE: %s | PHASE: %s | streak=%d/%d",
                    AMT::AMTMarketStateToString(amtSnapshot.marketState),
                    AMT::CurrentPhaseToString(amtSnapshot.phase),
                    st->amtPhaseTracker.candidateBars,
                    st->amtPhaseTracker.GetConfirmationBarsFor(amtSnapshot.rawPhase));
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // Phase primitives (at diagLevel >= 2)
                if (diagLevel >= 2) {
                    const AMT::PhasePrimitives& p = amtSnapshot.primitives;
                    msg.Format("Prim: P=%.2f POC=%.2f VAH=%.2f VAL=%.2f | inVA=%d atVAL=%d atVAH=%d | dPOC=%.1f vaRange=%.1f | outStreak=%d accepted=%d",
                        p.price, p.poc, p.vah, p.val,
                        p.insideVA ? 1 : 0, p.atVAL ? 1 : 0, p.atVAH ? 1 : 0,
                        p.dPOC_ticks, p.vaRangeTicks,
                        p.outsideCloseStreak, p.acceptanceOutsideVA ? 1 : 0);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Current state (zone, session, facilitation, confidence)
                // Note: Store std::string in locals to avoid dangling pointer from .c_str()
                // ZONE= shows nearest/strongest VBP or PRIOR profile zone within tolerance
                std::string zoneStrStorage = st->amtZoneManager.GetNearestZoneDescription(currentPrice, sc.TickSize);
                const char* zoneStr = zoneStrStorage.c_str();
                const char* sessStr = AMT::SessionPhaseToString(st->amtContext.session);
                // FACIL shows UNKNOWN until baseline is stable (truthful "known" semantics)
                const char* facilStr = st->facilitationComputed
                    ? AMT::to_string(st->amtContext.facilitation)
                    : "UNKNOWN";
                // deltaConsistency is computed from CLOSED bar (curBarIdx - 1)
                // Label as DELTA_FRAC to indicate closed-bar metric (more reliable than forming bar)
                const float deltaFrac = st->amtContext.confidence.deltaConsistency;  // [0,1] 0.5=neutral
                const bool deltaValid = st->amtContext.confidence.deltaConsistencyValid;

                // Kyle's 4-Component Liquidity Model (enhanced Dec 2024):
                // LIQ = DepthRank * (1 - StressRank) * ResRank * SpreadPenalty
                // D=depth(higher=better), S=stress(higher=worse), R=resilience(higher=better), T=tightness/spread(higher=wider=worse)
                const auto& liqSnap = st->lastLiqSnap;
                const char* liqStateStr = AMT::to_string(liqSnap.liqState);

                // Build spread rank string (only show if valid)
                char spreadStr[32] = "";
                if (liqSnap.spreadRankValid) {
                    snprintf(spreadStr, sizeof(spreadStr), " T=%.0f", liqSnap.spreadRank * 100.0);
                }

                msg.Format("State: ZONE=%s | SESS=%s | FACIL=%s | DELTA_FRAC=%.2f%s | LIQ=%.2f %s [D=%.0f S=%.0f R=%.0f%s]",
                    zoneStr,
                    sessStr ? sessStr : "NULL",
                    facilStr ? facilStr : "NULL",
                    static_cast<double>(deltaFrac),
                    deltaValid ? "" : "(thin)",
                    liqSnap.liqValid ? liqSnap.liq : 0.0,
                    liqStateStr,
                    liqSnap.depthRankValid ? liqSnap.depthRank * 100.0 : 0.0,
                    liqSnap.stressRankValid ? liqSnap.stressRank * 100.0 : 0.0,
                    liqSnap.resilienceRankValid ? liqSnap.resilienceRank * 100.0 : 0.0,
                    spreadStr);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // AMT Signal Engine state line
                // Shows: state + strength + location + activity + phase + excess + single prints
                // Per AMT: phase is derived from signals, not separately detected
                {
                    const AMT::StateEvidence& ev = st->lastStateEvidence;
                    const int spCount = static_cast<int>(st->singlePrintZones.size());
                    const AMT::CurrentPhase derivedPhase = ev.DerivePhase();
                    msg.Format("AMT: %s str=%.2f | loc=%s act=%s | phase=%s | ex=%s | SP=%d",
                        AMT::AMTMarketStateToString(ev.currentState),
                        ev.stateStrength,
                        AMT::ValueLocationToString(ev.location),
                        AMT::AMTActivityTypeToString(ev.activity.activityType),
                        AMT::CurrentPhaseToString(derivedPhase),
                        AMT::ExcessTypeToString(ev.excessDetected),
                        spCount);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Volatility Engine state line (context gate for trigger trustworthiness)
                // Condensed format: regime + ER/chop + trend/CV + pace + shock + stop + gap
                {
                    const AMT::VolatilityResult& vol = st->lastVolResult;
                    if (vol.IsReady()) {
                        // Build condensed log string
                        SCString volLine;
                        const char* transStr = vol.isTransitioning ? " TRANS" : "";

                        // Base: REGIME p=XX s=XX
                        volLine.Format("VOL: %s p=%.0f s=%d%s",
                            AMT::VolatilityRegimeToShortString(vol.regime),
                            vol.rangePercentile,
                            vol.stabilityBars,
                            transStr);

                        // Efficiency Ratio + chop (if valid)
                        if (vol.efficiencyValid) {
                            msg.Format(" | ER=%.2f(p%.0f) chop=%.2f",
                                vol.efficiencyRatio, vol.efficiencyPercentile, vol.chopSeverity);
                            volLine += msg;
                        }

                        // Volatility trend + CV (if valid)
                        if (vol.volMomentumValid || vol.stabilityValid) {
                            const char* trendStr = "?";
                            if (vol.volMomentumValid) {
                                switch (vol.volTrend) {
                                    case AMT::VolatilityTrend::EXPANDING:   trendStr = "EXPAND"; break;
                                    case AMT::VolatilityTrend::CONTRACTING: trendStr = "CONTRACT"; break;
                                    case AMT::VolatilityTrend::STABLE:      trendStr = "STABLE"; break;
                                    default: trendStr = "?"; break;
                                }
                            }
                            if (vol.stabilityValid) {
                                msg.Format(" | %s cv=%.2f", trendStr, vol.volCV);
                            } else {
                                msg.Format(" | %s", trendStr);
                            }
                            volLine += msg;
                        }

                        // Pace
                        if (vol.paceReady) {
                            msg.Format(" | %s", AMT::AuctionPaceToShortString(vol.pace));
                            volLine += msg;
                        }

                        // Shock/aftershock
                        if (vol.shockFlag) {
                            volLine += " | SHOCK=Y";
                        } else if (vol.aftershockActive) {
                            msg.Format(" | SHOCK=N after=%d", vol.barsSinceShock);
                            volLine += msg;
                        } else {
                            volLine += " | SHOCK=N";
                        }

                        // Stop guidance (if active)
                        if (vol.IsStopGuidanceReady()) {
                            msg.Format(" | stop>=%.1ft", vol.stopGuidance.minStopTicks);
                            volLine += msg;
                        }

                        // Gap context (diagnostic, if present)
                        if (vol.HasGapContext()) {
                            const char* respChar = "?";
                            if (vol.gapResponse == AMT::EarlyResponse::ACCEPTING) respChar = "A";
                            else if (vol.gapResponse == AMT::EarlyResponse::REJECTING) respChar = "R";
                            else if (vol.gapResponse == AMT::EarlyResponse::UNRESOLVED) respChar = "U";

                            if (vol.IsGapUp()) {
                                msg.Format(" | GAP=+%.0ft(%s)", vol.gapFromValueTicks, respChar);
                            } else if (vol.IsGapDown()) {
                                msg.Format(" | GAP=-%.0ft(%s)", vol.gapFromValueTicks, respChar);
                            } else {
                                msg.Format(" | GAP=IN(%s)", respChar);
                            }
                            volLine += msg;
                        }

                        st->logManager.LogToSC(LogCategory::AMT, volLine, false);
                    } else {
                        msg.Format("VOL: %s (reason=%s)",
                            vol.IsWarmup() ? "WARMUP" : "ERROR",
                            AMT::VolatilityErrorToString(vol.errorReason));
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }
                }

                // Delta Engine state line (participation pressure classification)
                // Shows: character + alignment + percentiles + confidence + warnings
                {
                    const AMT::DeltaResult& delta = st->lastDeltaResult;
                    if (delta.IsReady()) {
                        // Build warning flags string
                        char warnStr[64] = "";
                        if (delta.HasWarnings()) {
                            snprintf(warnStr, sizeof(warnStr), " [%s%s%s%s]",
                                delta.isThinTape ? "THIN" : "",
                                (delta.isThinTape && delta.isHighChop) ? "," : "",
                                delta.isHighChop ? "CHOP" : "",
                                delta.isExhaustion ? ",EXH" : "");
                        }
                        msg.Format("DELTA: %s/%s | bar=%.0f sess=%.0f vol=%.0f | conf=%s%s | cont=%s bkout=%s pos=%.2fx",
                            AMT::DeltaCharacterShort(delta.character),
                            AMT::DeltaAlignmentShort(delta.alignment),
                            delta.barDeltaPctile,
                            delta.sessionDeltaPctile,
                            delta.volumePctile,
                            AMT::DeltaConfidenceToString(delta.confidence),
                            warnStr,
                            delta.constraints.allowContinuation ? "OK" : "BLOCK",
                            delta.constraints.allowBreakout ? "OK" : "BLOCK",
                            delta.constraints.positionSizeMultiplier);
                    } else {
                        msg.Format("DELTA: %s (reason=%s)",
                            delta.IsWarmup() ? "WARMUP" : "ERROR",
                            AMT::DeltaErrorToString(delta.errorReason));
                    }
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Volume Acceptance Engine state line (move confirmation)
                // Shows: state + intensity + migration + scores + multiplier
                {
                    const AMT::VolumeAcceptanceResult& vol = st->lastVolumeResult;
                    if (vol.IsReady()) {
                        // Build rejection flags string
                        char rejStr[64] = "";
                        if (vol.lowVolumeBreakout || vol.fastReturn || vol.wickRejection || vol.deltaRejection) {
                            snprintf(rejStr, sizeof(rejStr), " [%s%s%s%s]",
                                vol.lowVolumeBreakout ? "LV" : "",
                                vol.fastReturn ? "FR" : "",
                                vol.wickRejection ? "WK" : "",
                                vol.deltaRejection ? "DV" : "");
                        }
                        msg.Format("VOLACC: %s/%s migr=%s | pct=%.0f acc=%.2f rej=%.2f | mult=%.2f%s",
                            AMT::AcceptanceStateToShortString(vol.confirmedState),
                            AMT::VolumeIntensityToShortString(vol.intensity),
                            AMT::ValueMigrationStateToString(vol.migration),
                            vol.volumePercentile,
                            vol.acceptanceScore,
                            vol.rejectionScore,
                            vol.confirmationMultiplier,
                            rejStr);
                    } else {
                        msg.Format("VOLACC: %s (reason=%s)",
                            vol.IsWarmup() ? "WARMUP" : "ERROR",
                            AMT::AcceptanceErrorToString(vol.errorReason));
                    }
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Bid/Ask Imbalance line (3-component model diagnostic)
                // IMB = (bidMass - askMass) / (bidMass + askMass) [-1, +1]
                // Positive = more bid support, Negative = more ask resistance
                if (diagLevel >= 2 && liqSnap.depth.valid) {
                    msg.Format("DOM: Depth bidMass=%.0f askMass=%.0f IMB=%.2f | Stress=%.2f | RefillRate=%.1f/s",
                        liqSnap.depth.bidMass,
                        liqSnap.depth.askMass,
                        liqSnap.depth.imbalance,
                        liqSnap.stress.stress,
                        liqSnap.resilience.refillRate);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);

                    // Peak liquidity line (maximum depth available during bar)
                    // Consumed = Peak - Ending = depth absorbed by aggressive orders
                    // Toxicity = |consumedBid - consumedAsk| / consumedTotal (asymmetric consumption)
                    if (liqSnap.peakValid) {
                        // Build toxicity string (only show if valid)
                        char toxStr[48] = "";
                        if (liqSnap.toxicityValid) {
                            snprintf(toxStr, sizeof(toxStr), " | TOX=%.2f", liqSnap.toxicityProxy);
                        }
                        msg.Format("DOM: Peak bidMass=%.0f askMass=%.0f total=%.0f | Consumed bid=%.0f ask=%.0f total=%.0f%s",
                            liqSnap.peakBidMass,
                            liqSnap.peakAskMass,
                            liqSnap.peakDepthMass,
                            liqSnap.consumedBidMass,
                            liqSnap.consumedAskMass,
                            liqSnap.consumedDepthMass,
                            toxStr);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }

                    // Direct Stack/Pull API values (no study dependency)
                    if (liqSnap.directStackPullValid) {
                        msg.Format("DOM: StackPull bid=%.0f ask=%.0f net=%.0f (direct API)",
                            liqSnap.directBidStackPull,
                            liqSnap.directAskStackPull,
                            liqSnap.directBidStackPull + liqSnap.directAskStackPull);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }

                    // DOM Staleness diagnostic (millisecond-level freshness)
                    if (st->domQualityTracker.IsTimingValid()) {
                        msg.Format("DOM: Freshness ageMs=%d staleByMs=%s staleByBars=%s combined=%s",
                            st->domQualityTracker.GetAgeMs(),
                            st->domQualityTracker.IsStaleByMs() ? "YES" : "no",
                            st->domQualityTracker.IsStaleByBars() ? "YES" : "no",
                            st->domQualityTracker.isStale ? "STALE" : "fresh");
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }

                    // Spatial Liquidity Profile (walls, voids, OBI, POLR)
                    if (st->lastSpatialProfile.valid) {
                        const auto& sp = st->lastSpatialProfile;
                        msg.Format("SPATIAL: OBI=%+.2f POLR=%s | WALLS: bid=%d ask=%d [%+.0ft,%+.0ft] | VOIDS: bid=%d ask=%d",
                            sp.direction.orderBookImbalance,
                            sp.GetPOLRString(),
                            sp.bidWallCount, sp.askWallCount,
                            sp.nearestBidWallTicks, sp.nearestAskWallTicks,
                            sp.bidVoidCount, sp.askVoidCount);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);

                        msg.Format("SPATIAL: GATE: long=%s short=%s | RISK: up=%.1ft down=%.1ft | mean=%.0f sigma=%.0f",
                            sp.gating.longBlocked ? "BLOCK" : "OK",
                            sp.gating.shortBlocked ? "BLOCK" : "OK",
                            sp.riskUp.estimatedSlippageTicks,
                            sp.riskDown.estimatedSlippageTicks,
                            sp.meanDepth,
                            sp.stddevDepth);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }
                }

                // Diagonal Delta and Average Trade Size (from Numbers Bars SG43/44/51/52)
                // SSOT: Read from liqSnap (Liq3Result), not snap.effort (legacy staging)
                if (diagLevel >= 2) {
                    if (liqSnap.diagonalDeltaValid || liqSnap.avgTradeSizeValid) {
                        msg.Format("NB: DiagDelta pos=%.0f neg=%.0f net=%.0f | AvgTrade bid=%.1f ask=%.1f ratio=%.2f",
                            liqSnap.diagonalPosDeltaSum,
                            liqSnap.diagonalNegDeltaSum,
                            liqSnap.diagonalNetDelta,
                            liqSnap.avgBidTradeSize,
                            liqSnap.avgAskTradeSize,
                            liqSnap.avgTradeSizeRatio);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }
                }

                // v1.2 SHADOW MODE: Compute shadow-adjusted confidence score (instrumentation only)
                // Invariant: baseScore unchanged, shadowScore = baseScore * confMult
                if (diagLevel >= 2)
                {
                    AMT::ConfidenceWeights defaultWeights;  // Uses default weights
                    AMT::ScoreResult baseResult = st->amtContext.confidence.calculate_score(defaultWeights);

                    // Get confMult from frozen shape (or 1.0 if not frozen)
                    float confMult = 1.0f;
                    AMT::ProfileShape shadowShape = AMT::ProfileShape::UNDEFINED;
                    if (st->behaviorMgr.frozen) {
                        shadowShape = st->behaviorMgr.observation.frozen.shape;
                        confMult = st->behaviorHistory.GetConfidenceMultiplier(shadowShape);
                    }

                    const float baseScore = baseResult.scoreValid ? baseResult.score : 0.0f;
                    const float shadowScore = baseScore * confMult;

                    msg.Format("SHADOW: shape=%s base=%.3f confMult=%.2f shadow=%.3f | "
                        "frozen=%d valid=%d SHADOW_ONLY=1",
                        AMT::ProfileShapeToString(shadowShape),
                        baseScore,
                        confMult,
                        shadowScore,
                        st->behaviorMgr.frozen ? 1 : 0,
                        baseResult.scoreValid ? 1 : 0);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Regime line - current bar phase + previous bar context fields
                // AUCTION= uses current-bar amtPhase (confirmed, post-hysteresis) - matches "Phase:" above
                // STATE/AGGR/SIDE use st->amtContext which is previous bar (newCtx not yet built)
                // Labels explicitly indicate timing: no label = current bar, (prev) = previous bar
                {
                    const char* barRegimeStr = st->amtContext.stateValid ? AMT::to_string(st->amtContext.state) : "UNK";
                    const char* phaseStr = AMT::CurrentPhaseToString(amtPhase);
                    const char* aggrStr = st->amtContext.aggressionValid ? AMT::to_string(st->amtContext.aggression) : "UNK";
                    const char* sideStr = st->amtContext.sideValid ? AMT::to_string(st->amtContext.side) : "UNK";
                    msg.Format("Regime: BAR_REGIME(prev)=%s | PHASE=%s | AGGR(prev)=%s | SIDE(prev)=%s",
                        barRegimeStr, phaseStr, aggrStr, sideStr);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Env line - AuctionContext environment fields
                // NOTE: LIQSTATE uses liqSnap (3-component model SSOT), not ctx.liquidity
                // because ctx is built later in the bar processing pipeline
                {
                    const char* volStr = st->amtContext.volatilityValid ? AMT::to_string(st->amtContext.volatility) : "UNK";
                    const char* liqStateStr = liqSnap.liqValid ? AMT::to_string(liqSnap.liqState) : "UNK";
                    const char* outcomeStr = st->amtContext.outcomeValid ? AMT::to_string(st->amtContext.outcome) : "UNK";
                    const char* transStr = st->amtContext.transitionValid ? AMT::to_string(st->amtContext.transition) : "UNK";
                    const char* intentStr = st->amtContext.intentValid ? AMT::to_string(st->amtContext.intent) : "UNK";
                    const char* fricStr = st->amtContext.frictionValid ? AMT::to_string(st->amtContext.friction) : "UNK";
                    msg.Format("Env: VOL=%s | LIQSTATE=%s | FRIC=%s | OUTCOME=%s | TRANS=%s | INTENT=%s",
                        volStr, liqStateStr, fricStr, outcomeStr, transStr, intentStr);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Tier2 Metrics line - new baseline-derived metrics (market composition, friction, volatility detail)
                if (diagLevel >= 2) {
                    const auto& conf = st->amtContext.confidence;
                    const char* mktCompStr = conf.marketCompositionValid ? "VALID" : "N/A";
                    const float mktComp = conf.marketCompositionValid ? conf.marketComposition : 0.0f;
                    msg.Format("Tier2: MktComp=%.2f (%s) | Friction=%s (valid=%s)",
                        mktComp, mktCompStr,
                        AMT::to_string(st->amtContext.friction),
                        st->amtContext.frictionValid ? "Y" : "N");
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);

                    // 2D Volatility refinement detail (when close-change baseline available)
                    // Query percentiles for logging (use pre-computed curBarRangeTicks - DRY)
                    const AMT::SessionPhase logPhase = st->phaseCoordinator.GetPhase();
                    const int logBucketIdx = AMT::SessionPhaseToBucketIndex(logPhase);
                    const double logRangeTicks = curBarRangeTicks;  // Local alias for logging
                    double logRangePctl = 50.0;
                    double logCCTicks = 0.0;
                    double logCCPctl = 50.0;
                    bool logCCValid = false;

                    if (logBucketIdx >= 0) {
                        const auto& logDist = st->effortBaselines.Get(logPhase);
                        const auto rangeRes = logDist.bar_range.TryPercentile(curBarRangeTicks);
                        if (rangeRes.valid) logRangePctl = rangeRes.value;

                        if (curBarIdx > 0 && sc.TickSize > 0.0 && sc.Close[curBarIdx - 1] > 0.0) {
                            logCCTicks = std::abs(sc.Close[curBarIdx] - sc.Close[curBarIdx - 1]) / sc.TickSize;
                            const auto ccRes = logDist.abs_close_change.TryPercentile(logCCTicks);
                            if (ccRes.valid) {
                                logCCValid = true;
                                logCCPctl = ccRes.value;
                            }
                        }
                    }

                    // Detect refinement character
                    const char* volCharacter = "NORMAL";
                    if (logCCValid) {
                        const bool highRange = (logRangePctl >= 75.0);
                        const bool lowRange = (logRangePctl <= 25.0);
                        const bool highTravel = (logCCPctl >= 75.0);
                        const bool lowTravel = (logCCPctl <= 25.0);
                        if (highRange && lowTravel) volCharacter = "INDECISIVE";
                        else if (lowRange && highTravel) volCharacter = "BREAKOUT_POTENTIAL";
                        else if (highRange && highTravel) volCharacter = "TRENDING";
                        else if (lowRange && lowTravel) volCharacter = "COMPRESSED";
                    }
                    msg.Format("Vol2D: range=%.0fT pctl=%.1f | travel=%.0fT pctl=%.1f (%s) | char=%s",
                        logRangeTicks, logRangePctl,
                        logCCTicks, logCCPctl,
                        logCCValid ? "VALID" : "N/A",
                        volCharacter);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // DayType line - AuctionContext day structure and profile shapes (per-bar)
                // Note: RAW_NOW/RESOLVED_NOW are per-bar instantaneous values
                // FINAL is only used for frozen session-level shape (see SHAPE_FREEZE log)
                {
                    msg.Format("DayType: STRUCT=%s | Shape: RAW_NOW=%s RESOLVED_NOW=%s%s%s",
                        AMT::to_string(st->amtContext.dayStructure),
                        AMT::ProfileShapeToString(st->amtContext.rawShape),
                        AMT::ProfileShapeToString(st->amtContext.resolvedShape),
                        st->amtContext.shapeConflict ? " [CONFLICT]" : "",
                        st->amtContext.shapeFrozen ? " [FROZEN]" : "");
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Structure line (always shows session/IB levels with distances)
                // This eliminates "ZONE=NONE at session high" confusion
                {
                    const AMT::StructureTracker& structure = st->amtZoneManager.structure;
                    const int distSessHi = structure.GetDistToSessionHighTicks(currentPrice, sc.TickSize);
                    const int distSessLo = structure.GetDistToSessionLowTicks(currentPrice, sc.TickSize);
                    const int distIBHi = structure.GetDistToIBHighTicks(currentPrice, sc.TickSize);
                    const int distIBLo = structure.GetDistToIBLowTicks(currentPrice, sc.TickSize);

                    msg.Format("Struct: SESS_HI=%.2f SESS_LO=%.2f DIST_HI_T=%d DIST_LO_T=%d | "
                               "IB_HI=%.2f IB_LO=%.2f DIST_IB_HI_T=%d DIST_IB_LO_T=%d IB=%s | RANGE_T=%d",
                        structure.GetSessionHigh(), structure.GetSessionLow(), distSessHi, distSessLo,
                        structure.GetIBHigh(), structure.GetIBLow(), distIBHi, distIBLo,
                        structure.IsIBFrozen() ? "FROZEN" : "OPEN",
                        structure.GetSessionRangeTicks());
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Dalton Session Bridge (overnight inventory, gap, opening type)
                // Only logged during RTH when session bridge is populated
                {
                    const AMT::SessionBridge& bridge = st->daltonEngine.GetSessionBridge();
                    if (bridge.valid) {
                        // Opening type line
                        msg.Format("DALTON: OPEN=%s (%s) | GAP=%s sz=%.0ft fill=%s",
                            AMT::OpeningTypeToString(st->lastDaltonState.openingType),
                            st->lastDaltonState.openingClassified ? "CLASSIFIED" : "pending",
                            AMT::GapTypeToString(bridge.gap.type),
                            bridge.gap.gapSize,
                            bridge.gap.gapFilled ? "YES" : "NO");
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);

                        // Overnight inventory line
                        msg.Format("DALTON: INV=%s score=%.2f | ON: HI=%.2f LO=%.2f MID=%.2f CL=%.2f",
                            AMT::InventoryPositionToString(bridge.inventory.position),
                            bridge.inventory.score,
                            bridge.overnight.onHigh,
                            bridge.overnight.onLow,
                            bridge.overnight.onMidpoint,
                            bridge.overnight.onClose);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);

                        // Prior RTH context (for gap calculation reference)
                        if (diagLevel >= 2) {
                            msg.Format("DALTON: PRIOR_RTH: HI=%.2f LO=%.2f CL=%.2f | POC=%.2f VAH=%.2f VAL=%.2f",
                                bridge.priorRTHHigh,
                                bridge.priorRTHLow,
                                bridge.priorRTHClose,
                                bridge.priorRTHPOC,
                                bridge.priorRTHVAH,
                                bridge.priorRTHVAL);
                            st->logManager.LogToSC(LogCategory::AMT, msg, false);
                        }
                    } else if (!st->lastDaltonState.isGlobexSession) {
                        // RTH but bridge not yet populated (early in session)
                        msg.Format("DALTON: OPEN=%s (%s) | Bridge: pending",
                            AMT::OpeningTypeToString(st->lastDaltonState.openingType),
                            st->lastDaltonState.openingClassified ? "CLASSIFIED" : "pending");
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    } else {
                        // GLOBEX session - show mini-IB tracking
                        msg.Format("DALTON: GLOBEX | mini-IB: %.2f-%.2f (%s) | TF=%s rot=%d",
                            bridge.overnight.miniIBLow,
                            bridge.overnight.miniIBHigh,
                            bridge.overnight.miniIBFrozen ? "FROZEN" : "OPEN",
                            AMT::TimeframePatternToString(st->lastDaltonState.timeframe),
                            st->lastDaltonState.rotationFactor);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }
                }

                // VA summary (uses decoupled display levels as SSOT)
                msg.Format("VA: POC=%.2f VAH=%.2f VAL=%.2f | Range=%d ticks",
                    st->displayPOC,
                    st->displayVAH,
                    st->displayVAL,
                    static_cast<int>((st->displayVAH - st->displayVAL) / sc.TickSize));
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // Volume summary - CLOSED bar only (session volume on SessionDelta line)
                const int closedBar = (curBarIdx > 0) ? (curBarIdx - 1) : 0;
                const double closedBarVol = sc.Volume[closedBar];
                const double closedBarAskVol = sc.AskVolume[closedBar];
                const double closedBarBidVol = sc.BidVolume[closedBar];
                const double closedBarDelta = closedBarAskVol - closedBarBidVol;
                const double closedBarDeltaPct = (closedBarVol > 0.0)
                    ? (closedBarDelta / closedBarVol * 100.0) : 0.0;
                msg.Format("Volume: ClosedBar[%d] Vol=%.0f Delta=%.0f (%.1f%%)",
                    closedBar,
                    closedBarVol,
                    closedBarDelta,
                    closedBarDeltaPct);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // Session-scoped delta (SSOT: internal accumulation)
                // sessionCumDelta = accumulated bar deltas (from sc.AskVolume - sc.BidVolume)
                // sessionDeltaRatio = sessionCumDelta / sessionTotalVolume (normalized)
                // sessionDeltaPctile = percentile rank vs SessionDeltaBaseline (phase-bucketed)
                {
                    const double sessionCumDelta = st->sessionAccum.sessionCumDelta;  // SSOT (internal)
                    const double sessionVol = st->sessionAccum.sessionTotalVolume;  // SSOT denominator
                    const double sessionDeltaRatio = (sessionVol > 1.0)
                        ? sessionCumDelta / sessionVol
                        : 0.0;
                    // Use phase-bucketed SessionDeltaBaseline (compares current phase delta to same-phase historical)
                    const AMT::SessionPhase logPhase = st->phaseCoordinator.GetPhase();
                    const auto& phaseBucket = st->sessionDeltaBaseline.Get(logPhase);
                    AMT::PercentileResult deltaPctile = st->sessionDeltaBaseline.TryGetPercentile(logPhase, sessionDeltaRatio);
                    if (deltaPctile.valid) {
                        msg.Format("SessionDelta: Cum=%.0f Ratio=%.4f Pctile=%.1f | Vol=%.0f | Phase=%s (n=%d)",
                            sessionCumDelta,
                            sessionDeltaRatio,
                            deltaPctile.value,
                            sessionVol,
                            AMT::SessionPhaseToString(logPhase),
                            phaseBucket.sessionsContributed);
                    } else {
                        msg.Format("SessionDelta: Cum=%.0f Ratio=%.4f Pctile=N/A | Vol=%.0f | Phase=%s (n=%d, need %d)",
                            sessionCumDelta,
                            sessionDeltaRatio,
                            sessionVol,
                            AMT::SessionPhaseToString(logPhase),
                            phaseBucket.sessionsContributed,
                            AMT::SessionDeltaBucket::REQUIRED_SESSIONS);
                    }
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // DeltaFlags line - AuctionContext derived delta flags + BarAbsPctl from EffortBaselines
                {
                    // Compute BarAbsPctl: percentile of |closedBarDeltaPctRaw| in bucket-specific baseline
                    // Uses closed bar (same scope as Volume line) for consistency
                    SCString barAbsPctlStr = "NA(not_ready)";
                    if (closedBarVol > 0.0) {
                        // Get closed bar time to determine bucket
                        const SCDateTime cbTime = sc.BaseDateTimeIn[closedBar];
                        int cbHour, cbMinute, cbSecond;
                        cbTime.GetTimeHMS(cbHour, cbMinute, cbSecond);
                        const int cbTimeSec = cbHour * 3600 + cbMinute * 60 + cbSecond;

                        // Determine phase from closed bar time using drift-proof wrapper
                        const AMT::SessionPhase cbPhase = AMT::DetermineSessionPhase(cbTimeSec, rthStartSec, rthEndSec);
                        const int cbBucketIdx = AMT::SessionPhaseToBucketIndex(cbPhase);

                        if (cbBucketIdx >= 0) {  // Tradeable phase
                            // Raw delta fraction (baseline stores [-1, 1], not percentage)
                            const double closedBarDeltaPctRaw = closedBarDelta / closedBarVol;

                            // Query baseline with absolute value (magnitude-based rarity)
                            const auto& bucketDist = st->effortBaselines.Get(cbPhase);
                            const AMT::PercentileResult barPctile = bucketDist.delta_pct.TryPercentile(std::abs(closedBarDeltaPctRaw));

                            if (barPctile.valid) {
                                barAbsPctlStr.Format("%.0f", barPctile.value);
                            }
                        } else {
                            barAbsPctlStr = "NA(non_tradeable)";
                        }
                    }

                    msg.Format("DeltaFlags: ExtBar=%c ExtSess=%c Extreme=%c Coherent=%c | Valid=%c | BarAbsPctl=%s",
                        st->amtContext.isExtremeDeltaBar ? 'Y' : 'N',
                        st->amtContext.isExtremeDeltaSession ? 'Y' : 'N',
                        st->amtContext.isExtremeDelta ? 'Y' : 'N',
                        st->amtContext.directionalCoherence ? 'Y' : 'N',
                        st->amtContext.sessionDeltaValid ? 'Y' : 'N',
                        barAbsPctlStr.GetChars());
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Zone touches
                msg.Format("Touches: VAH=%d POC=%d VAL=%d | Total=%d",
                    stats.vahTests,
                    stats.pocTouches,
                    stats.valTests,
                    stats.vahTests + stats.pocTouches + stats.valTests);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // Phase distribution (now includes PULLBACK)
                if (stats.totalBars > 0)
                {
                    msg.Format("Phase Distribution: ROT=%.1f%% TEST=%.1f%% DRIVE=%.1f%% EXT=%.1f%% PULL=%.1f%% FAIL=%.1f%%",
                        stats.GetPhasePercent(stats.rotationBars),
                        stats.GetPhasePercent(stats.testingBars),
                        stats.GetPhasePercent(stats.drivingBars),
                        stats.GetPhasePercent(stats.extensionBars),
                        stats.GetPhasePercent(stats.pullbackBars),
                        stats.GetPhasePercent(stats.failedAuctionBars));
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);

                    // Invariant check (warn if buckets don't sum to totalBars)
                    if (!stats.CheckInvariant())
                    {
                        std::string violation = stats.GetInvariantViolation();
                        msg.Format("WARNING: %s", violation.c_str());
                        st->logManager.LogToSC(LogCategory::AMT, msg, true);  // Warning level
                    }
                }

                // Market state (from AMTSignalEngine - leaky accumulator based)
                const AMT::StateEvidence& stateEv = st->lastStateEvidence;
                const char* stateStr = AMT::AMTMarketStateToString(stateEv.currentState);
                msg.Format("Market State: %s str=%.2f bars=%d",
                    stateStr, stateEv.stateStrength, stateEv.barsInState);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // HVN/LVN metrics
                msg.Format("HVN: %d (+%d/-%d) | LVN: %d (+%d/-%d) | WidestGap: %.0f ticks",
                    stats.hvnCount, stats.hvnAdded, stats.hvnRemoved,
                    stats.lvnCount, stats.lvnAdded, stats.lvnRemoved,
                    stats.widestLvnTicks);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // List HVN prices with volumes (for verification)
                if (!st->sessionVolumeProfile.session_hvn.empty())
                {
                    SCString hvnList;
                    hvnList.Format("  HVN prices:");
                    for (size_t i = 0; i < st->sessionVolumeProfile.session_hvn.size() && i < 10; ++i)
                    {
                        const double hvnPrice = st->sessionVolumeProfile.session_hvn[i];
                        const int hvnTick = static_cast<int>(std::llround(hvnPrice / sc.TickSize));
                        double vol = 0.0;
                        auto it = st->sessionVolumeProfile.volume_profile.find(hvnTick);
                        if (it != st->sessionVolumeProfile.volume_profile.end())
                            vol = it->second.Volume;
                        SCString entry;
                        entry.Format(" %.2f(%.0f)", hvnPrice, vol);
                        hvnList += entry;
                    }
                    if (st->sessionVolumeProfile.session_hvn.size() > 10)
                        hvnList += " ...";
                    st->logManager.LogToSC(LogCategory::AMT, hvnList, false);
                }

                // List LVN prices with volumes (for verification)
                if (!st->sessionVolumeProfile.session_lvn.empty())
                {
                    SCString lvnList;
                    lvnList.Format("  LVN prices:");
                    for (size_t i = 0; i < st->sessionVolumeProfile.session_lvn.size() && i < 10; ++i)
                    {
                        const double lvnPrice = st->sessionVolumeProfile.session_lvn[i];
                        const int lvnTick = static_cast<int>(std::llround(lvnPrice / sc.TickSize));
                        double vol = 0.0;
                        auto it = st->sessionVolumeProfile.volume_profile.find(lvnTick);
                        if (it != st->sessionVolumeProfile.volume_profile.end())
                            vol = it->second.Volume;
                        SCString entry;
                        entry.Format(" %.2f(%.0f)", lvnPrice, vol);
                        lvnList += entry;
                    }
                    if (st->sessionVolumeProfile.session_lvn.size() > 10)
                        lvnList += " ...";
                    st->logManager.LogToSC(LogCategory::AMT, lvnList, false);
                }

                // Zone engagement metrics
                msg.Format("Engagements: %d | Escapes: %d | AvgBars: %.1f | AvgVel: %.2f",
                    stats.engagementCount, stats.escapeCount,
                    stats.avgEngagementBars, stats.avgEscapeVelocity);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // Extreme conditions
                if (stats.totalExtremeEvents > 0)
                {
                    msg.Format("Extremes: Vol=%d Delta=%d Trades=%d Stack=%d Pull=%d Depth=%d (Total=%d)",
                        stats.extremeVolumeCount, stats.extremeDeltaCount, stats.extremeTradesCount,
                        stats.extremeStackCount, stats.extremePullCount, stats.extremeDepthCount,
                        stats.totalExtremeEvents);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Probe metrics
                if (stats.probesFired > 0)
                {
                    msg.Format("Probes: Fired=%d Resolved=%d | Hit=%d Miss=%d Exp=%d | HitRate=%.1f%% AvgScore=%.1f",
                        stats.probesFired, stats.probesResolved,
                        stats.probesHit, stats.probesMissed, stats.probesExpired,
                        stats.probeHitRate, stats.avgProbeScore);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Session/state transition metrics
                if (stats.sessionChangeCount > 0 || stats.phaseTransitionCount > 0 || stats.marketStateChangeCount > 0)
                {
                    msg.Format("Transitions: Session=%d Phase=%d State=%d",
                        stats.sessionChangeCount, stats.phaseTransitionCount, stats.marketStateChangeCount);
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // Warning/error metrics
                if (stats.zoneWidthMismatchCount > 0 || stats.validationDivergenceCount > 0 ||
                    stats.configErrorCount > 0 || stats.vbpWarningCount > 0)
                {
                    msg.Format("Warnings: WidthMismatch=%d ValDivergence=%d ConfigErr=%d VbPWarn=%d",
                        stats.zoneWidthMismatchCount, stats.validationDivergenceCount,
                        stats.configErrorCount, stats.vbpWarningCount);
                    st->logManager.LogToSC(LogCategory::AMT, msg, true);  // Warning level
                }

                // Snapshot reference for Effort/DOM signals
                const auto& s = st->currentSnapshot;

                // Effort signals with explicit units - SSOT contract per AMT_Snapshots.h
                // NOTE: SecondsPerBar == 0 for non-time bars (tick/range/volume) - rates are not meaningful
                const bool isTimeBased = (sc.SecondsPerBar > 0);
                const double barSec = isTimeBased ? static_cast<double>(sc.SecondsPerBar) : 0.0;
                const double bidEstVol = s.effort.bidVolSec * barSec;
                const double askEstVol = s.effort.askVolSec * barSec;
                const double estVolSum = bidEstVol + askEstVol;

                // Mismatch detector: warn if estimated volume diverges significantly from actual
                // Threshold: 25% divergence is suspicious (allows for rounding, timing jitter)
                // Only meaningful for COMPLETED time-based bars where rate × seconds should ≈ total
                // Partial/live bars (curBarIdx == ArraySize-1) are excluded to avoid false positives
                const bool isCompletedBar = (curBarIdx < sc.ArraySize - 1);
                const double mismatchPct = (s.effort.totalVolume > 1.0 && isTimeBased && isCompletedBar)
                    ? std::abs(estVolSum - s.effort.totalVolume) / s.effort.totalVolume * 100.0
                    : 0.0;
                const bool hasMismatch = (mismatchPct > 25.0);

                if (isTimeBased)
                {
                    msg.Format("Effort: SRC=NB | BidRate=%.2f AskRate=%.2f (vol/sec) | BarSec=%.0f | EstVol=%.0f/%.0f | TotVol=%.0f%s",
                        s.effort.bidVolSec, s.effort.askVolSec, barSec,
                        bidEstVol, askEstVol, s.effort.totalVolume,
                        hasMismatch ? " [MISMATCH]" : "");
                }
                else
                {
                    // Non-time bars: rates are not meaningful, show raw values only
                    msg.Format("Effort: SRC=NB | BidRate=%.2f AskRate=%.2f (vol/sec) | BarSec=N/A (non-time) | TotVol=%.0f",
                        s.effort.bidVolSec, s.effort.askVolSec, s.effort.totalVolume);
                }
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // Emit warning on mismatch (throttled - only in diagnostic mode)
                if (hasMismatch && diagLevel >= 2)
                {
                    msg.Format("[EFFORT-WARN] Bar %d: EstVol=%.0f vs TotVol=%.0f (%.1f%% divergence) - check NB rate subgraphs",
                        curBarIdx, estVolSum, s.effort.totalVolume, mismatchPct);
                    st->logManager.LogToSC(LogCategory::AMT, msg, true);
                }

                // NB cross-check: CumDelta from Numbers Bars (includes current forming bar)
                // This differs from SessionDelta.Cum by ~currentBarDelta on live bars
                msg.Format("Effort: NB_CumDelta=%.0f(tick) MaxDelta=%.0f",
                    s.effort.cumDelta, s.effort.maxDelta);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);
                msg.Format("Effort: DeltaSec=%.2f TradesSec=%.2f",
                    s.effort.deltaSec, s.effort.tradesSec);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                msg.Format("DOM: BidSz=%.0f AskSz=%.0f BidStk=%.0f AskStk=%.0f Bid=%.2f Ask=%.2f",
                    s.liquidity.domBidSize, s.liquidity.domAskSize,
                    s.liquidity.bidStackPull, s.liquidity.askStackPull,
                    s.liquidity.bestBid, s.liquidity.bestAsk);
                st->logManager.LogToSC(LogCategory::AMT, msg, false);

                // NEW BASELINE METRICS (Dec 2024)
                // EffortBaselineStore: phase-based effort distributions from prior sessions
                {
                    // Log current phase bucket status
                    const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
                    const int curIdx = AMT::SessionPhaseToBucketIndex(curPhase);
                    if (curIdx >= 0) {
                        const auto& curBucket = st->effortBaselines.Get(curPhase);
                        msg.Format("EffortBaselines: CurPhase=%s sessions=%d/%d bars=%d",
                            AMT::SessionPhaseToString(curPhase),
                            curBucket.sessionsContributed, AMT::EffortBucketDistribution::REQUIRED_SESSIONS,
                            curBucket.totalBarsPushed);
                        st->logManager.LogToSC(LogCategory::AMT, msg, false);
                    }
                }

                // SessionDeltaBaseline: phase-bucketed delta ratio distribution
                {
                    const auto& sdb = st->sessionDeltaBaseline;
                    const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
                    const auto& phaseBucket = sdb.Get(curPhase);
                    const char* stateStr = phaseBucket.IsReady() ? "READY" : "WARMUP";
                    auto meanResult = phaseBucket.delta_ratio.TryMean();
                    auto medResult = phaseBucket.delta_ratio.TryMedian();
                    if (meanResult.valid && medResult.valid) {
                        msg.Format("SessionDelta[%s]: %s sessions=%d size=%zu mean=%.4f median=%.4f mad=%.4f",
                            AMT::SessionPhaseToString(curPhase),
                            stateStr, phaseBucket.sessionsContributed,
                            phaseBucket.delta_ratio.size(),
                            meanResult.value, medResult.value, phaseBucket.delta_ratio.mad());
                    } else {
                        msg.Format("SessionDelta[%s]: %s sessions=%d size=%zu",
                            AMT::SessionPhaseToString(curPhase),
                            stateStr, phaseBucket.sessionsContributed, phaseBucket.delta_ratio.size());
                    }
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);
                }

                // DOMBaseline: phase-bucketed DOM baseline
                {
                    const auto& dw = st->domWarmup;
                    const AMT::SessionPhase domLogPhase = st->phaseCoordinator.GetPhase();
                    const auto& domBucket = dw.Get(domLogPhase);
                    const char* coreState = domBucket.IsReady() ? "READY" : "BUILDING";
                    const char* haloState = domBucket.IsHaloReady() ? "READY" : "BUILDING";
                    const char* spreadState = domBucket.IsSpreadReady() ? "READY" : "BUILDING";
                    msg.Format("DOMBaseline[%s]: core=%s n=%d depth=%zu | halo=%s mass=%zu imbal=%zu | spread=%s n=%zu",
                        AMT::SessionPhaseToString(domLogPhase),
                        coreState, domBucket.sessionsContributed, domBucket.depthMassCore.size(),
                        haloState, domBucket.depthMassHalo.size(), domBucket.haloImbalance.size(),
                        spreadState, domBucket.spreadTicks.size());
                    st->logManager.LogToSC(LogCategory::AMT, msg, false);

                    // Show current halo values if available for this phase
                    if (domBucket.IsHaloReady()) {
                        auto haloMedian = domBucket.depthMassHalo.TryMedian();
                        auto imbalMedian = domBucket.haloImbalance.TryMedian();
                        if (haloMedian.valid && imbalMedian.valid) {
                            msg.Format("DOMHalo[%s]: massMedian=%.0f imbalMedian=%.3f",
                                AMT::SessionPhaseToString(domLogPhase),
                                haloMedian.value, imbalMedian.value);
                            st->logManager.LogToSC(LogCategory::AMT, msg, false);
                        }
                    }
                }

                // NOTE: MarketStatePrior logging removed - AMTSignalEngine uses leaky accumulator
                // (no historical prior blending)

                st->logManager.LogToSC(LogCategory::AMT, "=============================================", false);
            }
        }
    }

    // --- 2. Derive State & Aggression from AMT Phase Snapshot (SSOT) ---
    // Phase is determined by AMT framework (amtSnapshot built above).
    // No parallel phase logic here - only derive secondary signals from snapshot.

    // ========================================================================
    // SESSION-SCOPED DELTA COMPUTATION (SSOT - First-Class Decision Input)
    // ========================================================================
    // sessionCumDelta := accumulated bar deltas (SSOT: internal, from sc.AskVolume - sc.BidVolume)
    // sessionDeltaRatio := sessionCumDelta / max(sessionTotalVolume, 1.0) [SIGNED]
    // sessionDeltaPctile := percentile rank of |sessionDeltaRatio| in rolling distribution
    //
    // MAGNITUDE-ONLY EXTREMENESS:
    //   - Distribution stores |sessionDeltaRatio| (absolute magnitude)
    //   - Query uses |sessionDeltaRatio| (absolute magnitude)
    //   - Threshold: pctile >= 85 means extreme magnitude in EITHER direction
    //   - Direction is handled separately by coherence check (sign comparison)
    //
    // CONTRACT INVARIANTS (C2 Safety):
    //   1. sessionVol source is ALWAYS sessionAccum.sessionTotalVolume
    //   2. sessionDeltaPctile stays within [0, 100] when distribution has data
    //   3. sessionCumDelta resets to 0 at session boundaries (internal accumulation)
    // ========================================================================
    double sessionDeltaPct = 0.0;  // Named "Pct" for API compatibility, but is actually ratio [-1, +1]
    double sessionDeltaPctile = 50.0;
    bool sessionDeltaValid = false;

    // Always valid when session has volume (no dependency on deprecated NB cumDelta latch)
    if (st->sessionAccum.sessionTotalVolume > 0.0)
    {
        const double sessionCumDelta = st->sessionAccum.sessionCumDelta;  // SSOT (internal)
        const double sessionVol = st->sessionAccum.sessionTotalVolume;  // SSOT denominator
        sessionDeltaPct = sessionCumDelta / (std::max)(sessionVol, 1.0);

        // PHASE 6: Use phase-bucketed SessionDeltaBaseline (prior sessions) for persistence-validated extreme delta
        // This compares current session's delta ratio against PRIOR sessions' SAME PHASE distribution (constraint #1)
        // Magnitude-only: stores |phaseDeltaRatio|, queries |phaseDeltaRatio|
        const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
        const AMT::PercentileResult priorSessionResult = st->sessionDeltaBaseline.TryGetPercentile(curPhase, sessionDeltaPct);
        if (priorSessionResult.valid)
        {
            sessionDeltaPctile = priorSessionResult.value;
            sessionDeltaValid = true;

            // C2 SAFETY INVARIANT: sessionDeltaPctile must be in [0, 100]
            if (sessionDeltaPctile < 0.0 || sessionDeltaPctile > 100.0)
            {
                // Log error but clamp to avoid downstream issues
                if (st->logManager.ShouldEmit(LogChannel::EVENTS_CSV, LogLevel::MINIMAL))
                {
                    AMT::SessionEvent evt;
                    evt.type = AMT::SessionEventType::EVENT_ERROR;
                    evt.timestamp = probeBarTime;
                    evt.bar = curBarIdx;
                    evt.sessDeltaPct = sessionDeltaPct;
                    evt.sessDeltaPctl = static_cast<int>(sessionDeltaPctile);
                    evt.message.Format("C2_INVARIANT: pctile=%.2f outside [0,100] - clamping",
                        sessionDeltaPctile);
                    st->logManager.LogSessionEvent(evt);
                }
                sessionDeltaPctile = (std::max)(0.0, (std::min)(100.0, sessionDeltaPctile));
            }
        }
        else
        {
            // NOTE: Legacy within-session fallback removed (Dec 2024)
            // SessionDeltaBaseline is the only source for session delta percentile
            // sessionDeltaValid remains false until enough prior sessions are collected
        }
    }

    // ========================================================================
    // AUCTION CONTEXT BUILDER (SSOT - Phases 1-3)
    // ========================================================================
    // All semantic interpretation is computed HERE and stored in amtContext.
    // Downstream arbitration, logging, and zone logic read from amtContext.
    // ========================================================================
    {
        AMT::ContextBuilderInput ctxInput;

        // --- Regime inputs ---
        ctxInput.sessionPhase = st->phaseCoordinator.GetPhase();
        ctxInput.currentPhase = amtSnapshot.phase;
        ctxInput.phaseSnapshotValid = st->amtZonesInitialized;
        ctxInput.phaseIsDirectional = amtSnapshot.IsDirectional();
        // Phase 3: Use AMT signal engine state directly (no legacy mapping needed)
        ctxInput.confirmedState = st->lastStateEvidence.currentState;
        ctxInput.priorConfirmedState = ctxInput.confirmedState;
        ctxInput.facilitation = st->amtContext.facilitation;
        ctxInput.facilitationComputed = st->facilitationComputed;

        // --- Control inputs (delta-based) ---
        ctxInput.deltaConsistency = st->amtContext.confidence.deltaConsistency;
        ctxInput.deltaConsistencyValid = st->amtContext.confidence.deltaConsistencyValid;
        ctxInput.sessionCumDelta = st->sessionAccum.sessionCumDelta;
        ctxInput.sessionTotalVolume = st->sessionAccum.sessionTotalVolume;
        ctxInput.sessionDeltaBaselineReady = sessionDeltaValid;
        ctxInput.sessionDeltaPctile = sessionDeltaPctile;

        // --- Extreme delta (SSOT: DeltaEngine - Dec 2024) ---
        ctxInput.isExtremeDeltaBar = st->lastDeltaResult.isExtremeDeltaBar;
        ctxInput.isExtremeDeltaSession = st->lastDeltaResult.isExtremeDeltaSession;
        ctxInput.isExtremeDelta = st->lastDeltaResult.isExtremeDelta;
        ctxInput.directionalCoherence = st->lastDeltaResult.directionalCoherence;

        // --- Activity classification (SSOT: AMT_Signals.h - Jan 2025) ---
        // Initiative vs Responsive is LOCATION-GATED per Dalton's Market Profile.
        // SSOT: AMTActivityType from AMTSignalEngine.ProcessBar(), mapped to AggressionType.
        ctxInput.ssotAggression = AMT::MapAMTActivityToLegacy(
            st->lastStateEvidence.activity.activityType);
        ctxInput.ssotAggressionValid = st->lastStateEvidence.activity.valid;

        // --- Environment inputs ---
        ctxInput.barRangeTicks = curBarRangeTicks;  // Use pre-computed value (DRY)

        // Range baseline: query EffortBaselineStore.bar_range for current SessionPhase
        // This provides volatility classification (COMPRESSED / NORMAL / EXPANDED)
        {
            const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
            const int bucketIdx = AMT::SessionPhaseToBucketIndex(curPhase);

            if (bucketIdx >= 0) {  // Tradeable phase
                const auto& bucketDist = st->effortBaselines.Get(curPhase);
                const AMT::PercentileResult rangeResult = bucketDist.bar_range.TryPercentile(ctxInput.barRangeTicks);

                if (rangeResult.valid) {
                    ctxInput.rangeBaselineReady = true;
                    ctxInput.rangePctile = rangeResult.value;
                } else {
                    ctxInput.rangeBaselineReady = false;
                }
            } else {
                // Non-tradeable phase - no effort baseline data
                ctxInput.rangeBaselineReady = false;
            }
        }

        // Close-change baseline: query EffortBaselineStore.abs_close_change for 2D volatility
        // Only valid when: bar > 0 (need prevClose), tickSize > 0, baseline ready
        {
            const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
            const int bucketIdx = AMT::SessionPhaseToBucketIndex(curPhase);

            if (bucketIdx >= 0 && curBarIdx > 0 && sc.TickSize > 0.0) {
                const double prevClose = sc.Close[curBarIdx - 1];
                const double curClose = sc.Close[curBarIdx];

                if (prevClose > 0.0) {
                    ctxInput.closeChangeTicks = std::abs(curClose - prevClose) / sc.TickSize;

                    const auto& bucketDist = st->effortBaselines.Get(curPhase);
                    const AMT::PercentileResult closeChangeResult =
                        bucketDist.abs_close_change.TryPercentile(ctxInput.closeChangeTicks);

                    if (closeChangeResult.valid) {
                        ctxInput.closeChangeBaselineReady = true;
                        ctxInput.closeChangePctile = closeChangeResult.value;
                    } else {
                        ctxInput.closeChangeBaselineReady = false;
                    }
                } else {
                    ctxInput.closeChangeBaselineReady = false;
                }
            } else {
                // Non-tradeable phase, first bar, or invalid tickSize
                ctxInput.closeChangeBaselineReady = false;
            }
        }

        // CLOSED BAR POLICY: Use closedBarDepth (computed earlier) for all depth metrics
        ctxInput.curDepth = closedBarDepth;

        // PHASE 6: Use DOMWarmup for depth percentile if ready (phase-bucketed)
        // Uses current phase to compare against same-phase historical data
        const AMT::SessionPhase domQueryPhase = ctxInput.sessionPhase;
        if (st->domWarmup.IsReady(domQueryPhase))
        {
            const AMT::PercentileResult depthResult = st->domWarmup.TryDepthPercentile(domQueryPhase, closedBarDepth);
            ctxInput.depthBaselineReady = depthResult.valid;
            if (depthResult.valid) {
                ctxInput.depthPctile = depthResult.value;
            }
        }
        else
        {
            ctxInput.depthBaselineReady = false;
        }
        ctxInput.domInputsConfigured = st->domInputsValid;

        // 3-Component Liquidity Model: Feed LiquidityEngine output to ContextBuilder
        // This bypasses the old ClassifyLiquidity(depthPctile) when available
        if (st->lastLiqSnap.liqValid) {
            ctxInput.liqState = st->lastLiqSnap.liqState;
            ctxInput.liqStateValid = true;
        } else {
            ctxInput.liqStateValid = false;  // Model not ready, liquidityValid will be false
        }

        // --- Narrative inputs (zone engagement) ---
        const AMT::ZoneRuntime* pocZone = st->amtZoneManager.GetPOC();
        const AMT::ZoneRuntime* vahZone = st->amtZoneManager.GetVAH();
        const AMT::ZoneRuntime* valZone = st->amtZoneManager.GetVAL();

        if (pocZone && pocZone->proximity == AMT::ZoneProximity::AT_ZONE) {
            ctxInput.engagedZoneId = pocZone->zoneId;
            ctxInput.engagedZoneType = pocZone->type;
            ctxInput.engagedZoneProximity = pocZone->proximity;
            ctxInput.engagementOutcome = pocZone->currentEngagement.outcome;
            ctxInput.atPOC = true;
        } else if (vahZone && vahZone->proximity == AMT::ZoneProximity::AT_ZONE) {
            ctxInput.engagedZoneId = vahZone->zoneId;
            ctxInput.engagedZoneType = vahZone->type;
            ctxInput.engagedZoneProximity = vahZone->proximity;
            ctxInput.engagementOutcome = vahZone->currentEngagement.outcome;
            ctxInput.atUpperBoundary = true;
        } else if (valZone && valZone->proximity == AMT::ZoneProximity::AT_ZONE) {
            ctxInput.engagedZoneId = valZone->zoneId;
            ctxInput.engagedZoneType = valZone->type;
            ctxInput.engagedZoneProximity = valZone->proximity;
            ctxInput.engagementOutcome = valZone->currentEngagement.outcome;
            ctxInput.atLowerBoundary = true;
        }

        ctxInput.barVolume = st->currentSnapshot.effort.totalVolume;

        // =====================================================================
        // PHASE 4 INPUTS (Pattern Evidence - for logging/diagnostics only)
        // =====================================================================

        // Volume baseline: query EffortBaselineStore.vol_sec for current SessionPhase
        // vol_sec baseline stores volume-per-second, so convert barVolume to rate
        {
            const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
            const int bucketIdx = AMT::SessionPhaseToBucketIndex(curPhase);

            if (bucketIdx >= 0) {  // Tradeable phase
                // Convert bar volume to volume-per-second (baseline stores rates, not totals)
                const double barIntervalSec = (sc.SecondsPerBar > 0) ? static_cast<double>(sc.SecondsPerBar) : 60.0;
                const double volSec = ctxInput.barVolume / barIntervalSec;

                const auto& bucketDist = st->effortBaselines.Get(curPhase);
                const AMT::PercentileResult volResult = bucketDist.vol_sec.TryPercentile(volSec);

                if (volResult.valid) {
                    ctxInput.volumeBaselineReady = true;
                    ctxInput.volumePctile = volResult.value;
                } else {
                    ctxInput.volumeBaselineReady = false;
                    ctxInput.volumePctile = 50.0;  // Neutral fallback
                }
            } else {
                // Non-tradeable phase - no effort baseline data
                ctxInput.volumeBaselineReady = false;
                ctxInput.volumePctile = 50.0;
            }
        }

        ctxInput.deltaPct = st->currentSnapshot.effort.deltaPct;
        ctxInput.bidStackPull = st->currentSnapshot.liquidity.bidStackPull;
        ctxInput.askStackPull = st->currentSnapshot.liquidity.askStackPull;
        ctxInput.domBidSize = st->currentSnapshot.liquidity.domBidSize;
        ctxInput.domAskSize = st->currentSnapshot.liquidity.domAskSize;
        ctxInput.currentBar = curBarIdx;

        // Build context (SINGLE AUTHORITATIVE CALL)
        AMT::AuctionContext newCtx = AMT::AuctionContextBuilder::Build(ctxInput);

        // Preserve existing confidence metrics (already computed earlier in study)
        newCtx.confidence = st->amtContext.confidence;

        // Track if session shape was already frozen (for one-shot SHAPE_FREEZE gate)
        // NOTE: Do NOT preserve rawShape/resolvedShape - those should compute fresh per-bar
        const bool wasAlreadyFrozen = st->amtContext.shapeFrozen;

        // =====================================================================
        // UNIFIED PROFILE SHAPE CLASSIFICATION (via ProfileStructureResult)
        // =====================================================================
        // Uses ComputeShape for:
        // - Feature extraction from volume profile
        // - Geometric classification (rawShape)
        // - DayStructure constraint resolution (resolvedShape)
        // - Shape freeze (session-level, preserved across bars)
        // =====================================================================

        // Set dayStructure from classifier (Phase 2) - needed before ComputeShape
        newCtx.dayStructure = st->dayTypeClassifier.GetClassification();
        newCtx.dayStructureValid = st->dayTypeClassifier.IsClassified();

        // Compute unified shape using ProfileStructureResult
        // ComputeShape handles: feature extraction, classification, resolution, freeze
        {
            // Get the result from ComputeStructure (computed earlier in bar)
            AMT::ProfileStructureResult& structResult = st->lastProfileStructureResult;

            // Call ComputeShape if profile is ready (thresholds computed)
            if (structResult.thresholdsComputed) {
                // Compute session minutes for shape gates
                const int tSecLocal = TimeToSeconds(sc.BaseDateTimeIn[curBarIdx]);
                const int shapeSessionMinutes = isCurRTH
                    ? (tSecLocal >= rthStartSec ? (tSecLocal - rthStartSec) / 60 : 0)
                    : (tSecLocal >= gbxStartSec ? (tSecLocal - gbxStartSec) / 60 : ((86400 - gbxStartSec + tSecLocal) / 60));

                // Get session extremes in ticks for volume distribution validation
                const int sessionHighTicks = static_cast<int>(std::round(st->amtZoneManager.structure.GetSessionHigh() / sc.TickSize));
                const int sessionLowTicks = static_cast<int>(std::round(st->amtZoneManager.structure.GetSessionLow() / sc.TickSize));

                // Wire volatility regime to shape break detector for adaptive thresholds
                // (scales POC drift, acceptance bars, etc. based on volatility environment)
                const AMT::VolatilityRegime volRegimeForShape = st->lastVolResult.IsReady()
                    ? st->lastVolResult.regime : AMT::VolatilityRegime::NORMAL;
                st->sessionVolumeProfile.SetBreakDetectorVolatilityRegime(volRegimeForShape);

                st->sessionVolumeProfile.ComputeShape(
                    structResult,
                    curBarIdx,
                    shapeSessionMinutes,
                    isCurRTH,
                    sessionHighTicks,
                    sessionLowTicks,
                    newCtx.dayStructure,
                    true);  // freezeOnResolve = true
            }

            // Copy shape fields from ProfileStructureResult to context
            newCtx.rawShape = structResult.rawShape;

            // Log shape classification failure (rate-limited, per-instance via StudyState)
            if (!structResult.rawShapeValid && structResult.thresholdsComputed && diagLevel >= 2) {
                if (curBarIdx - st->diagLastShapeFailLogBar >= 100) {
                    SCString failMsg;
                    failMsg.Format("ProfileShape: RAW=UNDEFINED (error=%s resolution=%s)",
                        AMT::ShapeErrorToString(structResult.shapeError),
                        structResult.shapeResolution);
                    st->logManager.LogDebug(curBarIdx, failMsg.GetChars(), LogCategory::AMT);
                    st->diagLastShapeFailLogBar = curBarIdx;
                }
            }
        }

        // =====================================================================
        // SHAPE RESOLUTION: Read from unified ProfileStructureResult
        // ComputeShape (called above) handles classification, resolution, and freeze
        // SHAPE_FREEZE log emits ONCE when first becoming frozen (session-level SSOT)
        // =====================================================================
        {
            const AMT::ProfileStructureResult& structResult = st->lastProfileStructureResult;

            // Copy resolved shape fields from ProfileStructureResult to context
            newCtx.resolvedShape = structResult.resolvedShape;
            newCtx.shapeConflict = structResult.shapeConflict;
            newCtx.shapeFrozen = structResult.shapeFrozen;

            // Map to legacy enums from resolvedShape (not rawShape)
            newCtx.balanceShape = AMT::ToBalanceProfileShape(structResult.resolvedShape);
            newCtx.imbalanceShape = AMT::ToImbalanceProfileShape(structResult.resolvedShape);

            // Log shape freeze (ONE-SHOT per session - only on first freeze)
            // This captures the session-level SSOT shape at freeze time
            if (!wasAlreadyFrozen && structResult.shapeFrozen && diagLevel >= 1)
            {
                SCString shapeMsg;
                shapeMsg.Format("SHAPE_FREEZE: t_freeze=%d | STRUCT=%s RAW_FROZEN=%s FINAL_FROZEN=%s | resolution=%s conflict=%d",
                    curBarIdx,
                    AMT::to_string(newCtx.dayStructure),
                    AMT::ProfileShapeToString(structResult.rawShape),
                    AMT::ProfileShapeToString(structResult.resolvedShape),
                    structResult.shapeResolution,
                    structResult.shapeConflict ? 1 : 0);
                st->logManager.LogInfo(curBarIdx, shapeMsg.GetChars(), LogCategory::SESSION);
            }

            // Freeze behavior references using RESOLVED shape (not raw) - ONE-SHOT
            if (!wasAlreadyFrozen && structResult.shapeFrozen &&
                !st->behaviorMgr.frozen && !structResult.shapeConflict)
            {
                // Only freeze behavior if no conflict (valid final shape)
                const auto& svp = st->sessionVolumeProfile;
                st->behaviorMgr.Freeze(
                    curBarIdx,
                    static_cast<float>(svp.session_poc),
                    static_cast<float>(svp.session_vah),
                    static_cast<float>(svp.session_val),
                    static_cast<float>(st->amtZoneManager.GetSessionHigh()),
                    static_cast<float>(st->amtZoneManager.GetSessionLow()),
                    structResult.resolvedShape,
                    structResult.asymmetry);

                if (diagLevel >= 1)
                {
                    const auto& frozen = st->behaviorMgr.observation.frozen;
                    const auto& hyp = st->behaviorMgr.hypothesis;
                    SCString freezeMsg;
                    freezeMsg.Format("BEHAVIOR-FREEZE: t_freeze=%d shape=%s hypothesis=%s | "
                        "POC_0=%.2f VAH_0=%.2f VAL_0=%.2f W_va=%.2f",
                        curBarIdx,
                        AMT::ProfileShapeToString(frozen.shape),
                        AMT::HypothesisTypeToString(hyp.hypothesis),
                        frozen.POC_0, frozen.VAH_0, frozen.VAL_0, frozen.W_va);
                    st->logManager.LogInfo(curBarIdx, freezeMsg.GetChars(), LogCategory::SESSION);
                }
            }
        }

        // =====================================================================
        // PHASE 3: SEMANTIC DAY TYPE MAPPING
        // Pure function: (structure, shape, RE metadata) -> semantic subtype
        // =====================================================================
        {
            const AMT::REDirection primaryRE = st->dayTypeClassifier.GetPrimaryREDirection();
            AMT::SemanticMappingResult semantic = AMT::MapStructureToSemantics(
                newCtx.dayStructure,
                newCtx.balanceShape,
                newCtx.imbalanceShape,
                primaryRE);

            newCtx.balanceType = semantic.balanceType;
            newCtx.imbalanceType = semantic.imbalanceType;

            // Log semantic mapping (one-time when classification changes)
            static AMT::BalanceStructure lastLoggedBalType = AMT::BalanceStructure::NONE;
            static AMT::ImbalanceStructure lastLoggedImbType = AMT::ImbalanceStructure::NONE;

            const bool subtypeChanged = (semantic.balanceType != lastLoggedBalType ||
                                         semantic.imbalanceType != lastLoggedImbType);

            if (diagLevel >= 1 && newCtx.dayStructureValid && subtypeChanged) {
                SCString p3Msg;
                if (newCtx.dayStructure == AMT::DayStructure::BALANCED) {
                    p3Msg.Format("[DAYTYPE-P3] BALANCED -> %s | Evidence: %s",
                        AMT::to_string(semantic.balanceType),
                        semantic.evidence);
                } else if (newCtx.dayStructure == AMT::DayStructure::IMBALANCED) {
                    p3Msg.Format("[DAYTYPE-P3] IMBALANCED -> %s | Evidence: %s",
                        AMT::to_string(semantic.imbalanceType),
                        semantic.evidence);
                } else {
                    p3Msg.Format("[DAYTYPE-P3] %s -> (no subtype) | %s",
                        AMT::to_string(newCtx.dayStructure),
                        semantic.evidence);
                }
                st->logManager.LogInfo(curBarIdx, p3Msg.GetChars(), LogCategory::DAYTYPE);

                lastLoggedBalType = semantic.balanceType;
                lastLoggedImbType = semantic.imbalanceType;
            }
        }

        // Write to SSOT
        st->amtContext = newCtx;

        // =====================================================================
        // v1.2: BEHAVIOR OUTCOME TRACKING (per-bar observation)
        // Invariant: ProcessBar only called if frozen (guard inside method)
        // =====================================================================
        st->behaviorMgr.ProcessBar(
            curBarIdx,
            static_cast<float>(probeHigh),
            static_cast<float>(probeLow),
            static_cast<float>(probeClose));

        // =====================================================================
        // PHASE 4: PATTERN EVENT LOGGING (Transition-Only, Semantic Facts)
        // =====================================================================
        // CONTRACT: Log each pattern ONCE per bar when first observed.
        // Contains: bar index, pattern name, high-level context (state, side)
        // Does NOT contain: thresholds, metric values, confidence weights
        // =====================================================================
        if (diagLevel >= 1)  // Only log if diagnostics enabled
        {
            st->patternLogger.ResetForNewBar(curBarIdx);

            // Helper: Get context string for pattern log
            const char* stateStr = newCtx.stateValid ? AMT::to_string(newCtx.state) : "UNK";
            const char* sideStr = newCtx.sideValid ? AMT::to_string(newCtx.side) : "NEUTRAL";

            // Volume patterns
            for (const auto& p : newCtx.volumePatterns) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.volumePatternsLogged & bit) == 0) {
                    st->patternLogger.volumePatternsLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }

            // TPO mechanics
            for (const auto& p : newCtx.tpoMechanics) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.tpoMechanicsLogged & bit) == 0) {
                    st->patternLogger.tpoMechanicsLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }

            // Balance DOM patterns
            for (const auto& p : newCtx.balanceDOMPatterns) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.balanceDOMLogged & bit) == 0) {
                    st->patternLogger.balanceDOMLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }

            // Imbalance DOM patterns
            for (const auto& p : newCtx.imbalanceDOMPatterns) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.imbalanceDOMLogged & bit) == 0) {
                    st->patternLogger.imbalanceDOMLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }

            // Balance delta patterns
            for (const auto& p : newCtx.balanceDeltaPatterns) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.balanceDeltaLogged & bit) == 0) {
                    st->patternLogger.balanceDeltaLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }

            // Imbalance delta patterns
            for (const auto& p : newCtx.imbalanceDeltaPatterns) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.imbalanceDeltaLogged & bit) == 0) {
                    st->patternLogger.imbalanceDeltaLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }

            // DOM control patterns
            for (const auto& p : newCtx.domControlPatterns) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.domControlLogged & bit) == 0) {
                    st->patternLogger.domControlLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }

            // DOM events
            for (const auto& p : newCtx.domEvents) {
                const uint32_t bit = 1u << static_cast<int>(p);
                if ((st->patternLogger.domEventsLogged & bit) == 0) {
                    st->patternLogger.domEventsLogged |= bit;
                    SCString msg;
                    msg.Format("[PATTERN] Bar=%d | Pattern=%s | Context=%s | Side=%s",
                        curBarIdx, AMT::to_string(p), stateStr, sideStr);
                    st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PATTERN);
                }
            }
        }
    }

    // ========================================================================
    // READ FROM CONTEXT (SSOT) - Replace inline computations
    // ========================================================================
    const bool isExtremeDeltaBar = st->amtContext.isExtremeDeltaBar;
    const bool isExtremeDeltaSession = st->amtContext.isExtremeDeltaSession;
    const bool isExtremeDelta = st->amtContext.isExtremeDelta;
    const bool directionalCoherence = st->amtContext.directionalCoherence;

    AMT::AuctionFacilitation detectedFacilitation = st->amtContext.facilitation;
    // SSOT: facilitationKnown reflects whether baseline had enough samples for valid computation
    // (see FACILITATION COMPUTATION block ~line 1678)
    bool facilitationKnown = st->facilitationComputed;

    // === M0 ARBITRATION LADDER ===
    // Determines controller (ZONES vs BASELINE) with reason-coded validity gating.
    // rawState uses persistence-validated isExtremeDelta (new definition)
    int arbReason = ARB_DEFAULT_BASELINE;
    bool useZones = false;
    int engagedZoneId = -1;

    // Cache zone IDs (avoid repeated member access)
    const int pocId = st->amtZoneManager.pocId;
    const int vahId = st->amtZoneManager.vahId;
    const int valId = st->amtZoneManager.valId;

    // Gate 0: INVALID_ANCHOR_IDS
    if (pocId < 0 || vahId < 0 || valId < 0) {
        arbReason = ARB_INVALID_ANCHOR_IDS;
    }
    else {
        // Gate 1: INVALID_ZONE_PTRS
        const AMT::ZoneRuntime* poc = st->amtZoneManager.GetZone(pocId);
        const AMT::ZoneRuntime* vah = st->amtZoneManager.GetZone(vahId);
        const AMT::ZoneRuntime* val = st->amtZoneManager.GetZone(valId);

        if (!poc || !vah || !val) {
            arbReason = ARB_INVALID_ZONE_PTRS;
        }
        // Gate 2: NOT_READY
        else if (!st->amtZonesInitialized) {
            arbReason = ARB_NOT_READY;
        }
        // Gate 3: INVALID_VBP_PRICES (use SessionManager SSOT)
        else if (st->sessionMgr.GetPOC() <= 0.0 ||
                 st->sessionMgr.GetVAH() <= 0.0 ||
                 st->sessionMgr.GetVAL() <= 0.0) {
            arbReason = ARB_INVALID_VBP_PRICES;
        }
        // Gate 4: INVALID_VA_ORDER (use SessionManager SSOT)
        else if (st->sessionMgr.GetVAH() <= st->sessionMgr.GetVAL()) {
            arbReason = ARB_INVALID_VA_ORDER;
        }
        // Gate 5: VBP_STALE
        else if (st->sessionVolumeProfile.bars_since_last_compute >= MAX_VBP_STALE_BARS) {
            arbReason = ARB_VBP_STALE;
        }
        // Gate 6: ENGAGED (any anchor AT_ZONE)
        else if (poc->proximity == AMT::ZoneProximity::AT_ZONE ||
                 vah->proximity == AMT::ZoneProximity::AT_ZONE ||
                 val->proximity == AMT::ZoneProximity::AT_ZONE) {
            arbReason = ARB_ENGAGED;
            useZones = true;
            // Record first engaged zone ID
            if (poc->proximity == AMT::ZoneProximity::AT_ZONE) engagedZoneId = pocId;
            else if (vah->proximity == AMT::ZoneProximity::AT_ZONE) engagedZoneId = vahId;
            else engagedZoneId = valId;
        }
        // Gate 7: DIRECTIONAL
        else if (amtSnapshot.IsDirectional()) {
            arbReason = ARB_DIRECTIONAL;
            useZones = true;
        }
        // Gate 8: BASELINE_EXTREME (uses persistence-validated isExtremeDelta)
        else if (isExtremeDelta) {
            arbReason = ARB_BASELINE_EXTREME;
        }
        // Gate 9: DEFAULT_BASELINE (arbReason already set)
    }

    // ========================================================================
    // RAW STATE FROM DALTON ENGINE (SSOT)
    // ========================================================================
    // SSOT UNIFICATION (Dec 2024): DaltonEngine.phase is the SINGLE authoritative
    // source for Balance/Imbalance. It incorporates:
    //   - 1TF/2TF pattern detection (rotation tracker)
    //   - Extreme delta persistence validation (bar + session)
    // The legacy rawState computation (isTrending || isExtremeDelta) is removed.
    // confirmedState uses 5-bar hysteresis via MarketStateBucket (unchanged).
    const AMT::AMTMarketState rawState = st->lastDaltonState.phase;

    // M0: Log-on-change arbitration decision (only in log window)
    // Enhanced with persistence-validated extreme delta decomposition
    // CONTRACT: Rule 1.1 (bar close), Rule 1.5 (session boundary deferral)
    {
        const bool curBarClosed = (sc.GetBarHasClosedStatus(curBarIdx) == BHCS_BAR_HAS_CLOSED);

        // CONTRACT Rule 1.5: Defer logging on session boundary (values transitional)
        if (inLogWindow && curBarClosed && !sessionChanged &&
            arbReason != st->lastLoggedArbReason &&
            st->logManager.ShouldEmit(LogChannel::EVENTS_CSV, LogLevel::MINIMAL))
        {
            // Get all anchor proximities for ENGAGED explainability
            const AMT::ZoneRuntime* pocForLog = st->amtZoneManager.GetZone(pocId);
            const AMT::ZoneRuntime* vahForLog = st->amtZoneManager.GetZone(vahId);
            const AMT::ZoneRuntime* valForLog = st->amtZoneManager.GetZone(valId);
            const int pocProx = pocForLog ? static_cast<int>(pocForLog->proximity) : -1;
            const int vahProx = vahForLog ? static_cast<int>(vahForLog->proximity) : -1;
            const int valProx = valForLog ? static_cast<int>(valForLog->proximity) : -1;

            // Derive engaged anchor type from zone ID
            const char* engagedAnchor = (engagedZoneId < 0) ? "NONE" :
                (engagedZoneId == pocId) ? "POC" :
                (engagedZoneId == vahId) ? "VAH" :
                (engagedZoneId == valId) ? "VAL" : "UNK";

            // Compute tick distances from current price to each anchor (use SessionManager SSOT)
            const double tickSize = sc.TickSize;
            const long long priceTicks = static_cast<long long>(std::round(probeClose / tickSize));
            const long long pocTicks = static_cast<long long>(std::round(st->sessionMgr.GetPOC() / tickSize));
            const long long vahTicks = static_cast<long long>(std::round(st->sessionMgr.GetVAH() / tickSize));
            const long long valTicks = static_cast<long long>(std::round(st->sessionMgr.GetVAL() / tickSize));
            const int distPoc = static_cast<int>(std::abs(priceTicks - pocTicks));
            const int distVah = static_cast<int>(std::abs(priceTicks - vahTicks));
            const int distVal = static_cast<int>(std::abs(priceTicks - valTicks));

            // Structured arbitration/phase log with ALL fields
            AMT::SessionEvent evt;
            evt.type = AMT::SessionEventType::PHASE_SNAPSHOT;
            evt.timestamp = probeBarTime;
            evt.bar = curBarIdx;
            evt.zoneId = engagedZoneId;
            evt.zoneType = engagedAnchor;
            evt.phase = AMT::PhaseReasonToString(amtSnapshot.phaseReason);

            // Intent/state fields - ALL populated
            evt.deltaConf = st->amtContext.confidence.deltaConsistency;
            evt.sessDeltaPct = sessionDeltaPct;
            evt.sessDeltaPctl = static_cast<int>(sessionDeltaPctile);
            evt.coherent = directionalCoherence ? 1 : 0;
            evt.facilitation = AMT::to_string(detectedFacilitation);
            evt.marketState = AMT::to_string(rawState);  // Use rawState (confirmedState not computed yet)

            // VBP fields
            evt.poc = st->sessionMgr.GetPOC();
            evt.vah = st->sessionMgr.GetVAH();
            evt.val = st->sessionMgr.GetVAL();

            // Volume/range (use pre-computed curBarRangeTicks - DRY)
            evt.volume = st->currentSnapshot.effort.totalVolume;
            evt.range = curBarRangeTicks;

            evt.message.Format("%s|Rsn:%d|Ext:%d(B:%d S:%d)|Prox:%d/%d/%d|Dist:%d/%d/%d",
                useZones ? "ZONES" : "BASE",
                arbReason,
                isExtremeDelta ? 1 : 0,
                isExtremeDeltaBar ? 1 : 0,
                isExtremeDeltaSession ? 1 : 0,
                pocProx, vahProx, valProx,
                distPoc, distVah, distVal);
            st->logManager.LogSessionEvent(evt);
            st->lastLoggedArbReason = arbReason;

            // ========================================================================
            // TUNING TELEMETRY: Arbitration Decision (diagLevel >= 2)
            // TELEMETRY ONLY: Does NOT affect arbitration outcome
            // ========================================================================
            if (diagLevel >= 2)
            {
                // Build arbitration telemetry record
                AMT::ArbitrationTelemetryRecord arbRec;
                arbRec.arbReason = arbReason;
                arbRec.useZones = useZones;
                arbRec.engagedZoneId = engagedZoneId;
                arbRec.bar = curBarIdx;
                arbRec.price = probeClose;

                // Friction (current bar)
                arbRec.friction = st->amtContext.friction;
                arbRec.frictionValid = st->amtContext.frictionValid;

                // Volatility (current bar - fully computed by now)
                arbRec.volatility = st->amtContext.volatility;
                arbRec.volatilityValid = st->amtContext.volatilityValid;

                // Market composition
                arbRec.marketComposition = st->amtContext.confidence.marketComposition;
                arbRec.marketCompositionValid = st->amtContext.confidence.marketCompositionValid;

                // Get range/closeChange percentiles from context builder (if available)
                // These were computed in the ContextBuilder block above
                // We can access them via the session baselines
                double rangePctile = 0.0;
                double closeChangePctile = 0.0;
                bool closeChangeValid = false;

                const AMT::SessionPhase curPhase = st->phaseCoordinator.GetPhase();
                const int bucketIdx = AMT::SessionPhaseToBucketIndex(curPhase);
                if (bucketIdx >= 0) {
                    const auto& bucketDist = st->effortBaselines.Get(curPhase);
                    // Use pre-computed curBarRangeTicks (DRY)
                    const auto rangeResult = bucketDist.bar_range.TryPercentile(curBarRangeTicks);
                    if (rangeResult.valid) {
                        rangePctile = rangeResult.value;
                    }

                    // Close-change percentile
                    if (curBarIdx > 0 && sc.Close[curBarIdx - 1] > 0.0) {
                        const double closeChangeTicks = std::abs(sc.Close[curBarIdx] - sc.Close[curBarIdx - 1]) / sc.TickSize;
                        const auto closeResult = bucketDist.abs_close_change.TryPercentile(closeChangeTicks);
                        if (closeResult.valid) {
                            closeChangePctile = closeResult.value;
                            closeChangeValid = true;
                        }
                    }
                }

                // Compute 2D volatility character
                arbRec.character = AMT::Classify2DVolatilityCharacter(rangePctile, closeChangePctile, closeChangeValid);

                // Compute advisories (TELEMETRY ONLY)
                arbRec.advisory.ComputeAdvisories(
                    arbRec.friction, arbRec.frictionValid,
                    rangePctile, closeChangePctile, closeChangeValid
                );

                // Emit structured telemetry log
                SCString tuningMsg;
                tuningMsg.Format("[TUNING-ARB] bar=%d rsn=%d zones=%d | "
                           "FRIC=%s(v=%d) wouldBlock=%d threshOff=%.2f | "
                           "VOL=%s(v=%d) char=%s confDelta=%d",
                    arbRec.bar, arbRec.arbReason, arbRec.useZones ? 1 : 0,
                    AMT::to_string(arbRec.friction), arbRec.frictionValid ? 1 : 0,
                    arbRec.advisory.wouldBlockIfLocked ? 1 : 0, arbRec.advisory.thresholdOffset,
                    AMT::to_string(arbRec.volatility), arbRec.volatilityValid ? 1 : 0,
                    AMT::to_string(arbRec.character), arbRec.advisory.confirmationDelta);
                sc.AddMessageToLog(tuningMsg, 0);
            }
        }
    }

    // ========================================================================
    // AGGRESSION FROM NEW AMT SIGNAL ENGINE (SSOT)
    // ========================================================================
    // Phase 2 Migration: Use new AMT activity classification as SSOT.
    // INITIATIVE: Away from value + Aggressive participation
    // RESPONSIVE: Toward value OR (Away + Absorptive)
    // NEUTRAL: At value with balanced participation
    AMT::AggressionType detectedAggression = AMT::MapAMTActivityToLegacy(
        st->lastStateEvidence.activity.activityType);
    st->amtContext.aggression = detectedAggression;
    st->amtContext.aggressionValid = st->lastStateEvidence.activity.valid;

    // ========================================================================
    // UPDATE CONTEXT WITH CONFIRMED STATE (AMT SIGNAL ENGINE SSOT)
    // ========================================================================
    // AMTSignalEngine.ProcessBar() is the authoritative source for state.
    // AMTMarketState is now the SSOT - no legacy mapping needed.
    st->amtContext.state = st->lastStateEvidence.currentState;
    st->amtContext.stateValid = (st->lastStateEvidence.currentState != AMT::AMTMarketState::UNKNOWN);

    // Update transition based on actual state change (from new AMT signal engine)
    if (st->lastStateEvidence.IsTransition()) {
        const AMT::AMTMarketState prevState = st->lastStateEvidence.previousState;
        const AMT::AMTMarketState currState = st->lastStateEvidence.currentState;

        if (prevState == AMT::AMTMarketState::BALANCE &&
            currState == AMT::AMTMarketState::IMBALANCE) {
            st->amtContext.transition = AMT::TransitionMechanic::BALANCE_TO_IMBALANCE;
        } else if (prevState == AMT::AMTMarketState::IMBALANCE &&
                   currState == AMT::AMTMarketState::BALANCE) {
            st->amtContext.transition = AMT::TransitionMechanic::IMBALANCE_TO_BALANCE;
        }
        st->amtContext.transitionValid = true;
        st->sessionAccum.marketStateChangeCount++;
    }
    st->lastLoggedState = st->lastStateEvidence.currentState;

    // =======================================================================
    // LOGGING FIX: Only log SessionEvents when bar is FINALIZED
    // CONTRACT: Rule 1.1 (bar close), Rule 1.5 (session boundary deferral)
    // Gate: Only log once per bar when bar has closed.
    // Values computed earlier in this iteration ARE correct at bar close.
    // =======================================================================
    {
        const bool curBarClosed = (sc.GetBarHasClosedStatus(curBarIdx) == BHCS_BAR_HAS_CLOSED);
        const bool notYetLogged = (st->lastSessionEventBar < curBarIdx);

        // CONTRACT Rule 1.5: Defer logging on session boundary (values transitional)
        if (curBarClosed && notYetLogged && !sessionChanged && inLogWindow &&
            st->logManager.ShouldEmit(LogChannel::EVENTS_CSV, LogLevel::MODERATE))
        {
            st->lastSessionEventBar = curBarIdx;

            // Single comprehensive event with ALL fields populated
            {
                AMT::SessionEvent evt;
                evt.type = AMT::SessionEventType::PHASE_SNAPSHOT;
                evt.timestamp = probeBarTime;
                evt.bar = curBarIdx;

                // Phase fields
                evt.phase = AMT::CurrentPhaseToString(amtSnapshot.phase);

                // Intent/state fields - ALL populated
                evt.deltaConf = st->amtContext.confidence.deltaConsistency;
                evt.sessDeltaPct = sessionDeltaPct;
                evt.sessDeltaPctl = static_cast<int>(sessionDeltaPctile);
                evt.coherent = directionalCoherence ? 1 : 0;
                evt.aggression = AMT::to_string(detectedAggression);
                evt.facilitation = AMT::to_string(detectedFacilitation);
                evt.marketState = AMT::to_string(st->amtContext.state);

                // VBP fields
                evt.poc = st->sessionMgr.GetPOC();
                evt.vah = st->sessionMgr.GetVAH();
                evt.val = st->sessionMgr.GetVAL();

                // Volume/range diagnostics (use pre-computed curBarRangeTicks - DRY)
                evt.volume = st->currentSnapshot.effort.totalVolume;
                evt.range = curBarRangeTicks;

                evt.message.Format("raw:%s|Rsn:%s|OutVA:%d",
                    AMT::CurrentPhaseToString(amtSnapshot.rawPhase),
                    amtSnapshot.phaseReason,
                    amtSnapshot.isOutsideVA ? 1 : 0);
                st->logManager.LogSessionEvent(evt);
            }

            // Flush EVENTS_CSV immediately after bar-close logging
            st->logManager.FlushAll();
        }
    }

    // --- 3. Update AuctionContextModule with AUTHORITATIVE AMT phase ---
    // NOTE: Using template version from AMT_Modules.h - passes SCENARIO_DATABASE explicitly
    st->auctionCtxModule.Update(
        st->amtContext,
        st->amtContext.state,  // Use state from AMT signal engine
        detectedAggression,
        detectedFacilitation,
        amtSnapshot.phase,  // Use AMT authoritative phase, not detectedPhase
        facilitationKnown,
        SCENARIO_DATABASE,
        SCENARIO_COUNT
    );

    const auto& validScenarios = st->auctionCtxModule.GetValidScenarios();
    const AuctionMode mode = st->auctionCtxModule.DetermineMode();

#if PERF_TIMING_ENABLED
    // === PERFORMANCE TIMING: Log end-of-recalc stats BEFORE early returns ===
    // This must be here because MODE_LOCKED, probe active, and probe blocked all return early
    if (sc.UpdateStartIndex == 0 && curBarIdx == sc.ArraySize - 1) {
        st->perfStats.barsProcessed++;
        st->perfStats.totalMs += st->perfTimer.ElapsedMs();

        const int estimatedGetStudyCalls = st->perfStats.snapshotCalls * 27;
        SCString endMsg;
        endMsg.Format("bars=%d | totalMs=%.1f | snap=%.1fms(%d) | sess=%.1fms | base=%.1fms | vbp=%.1fms(%d) | zone=%.1fms | studyCalls~%d",
            st->perfStats.barsProcessed,
            st->perfStats.totalMs,
            st->perfStats.snapshotMs, st->perfStats.snapshotCalls,
            st->perfStats.sessionDetectMs,
            st->perfStats.baselineMs,
            st->perfStats.vbpMs, st->perfStats.vbpCalls,
            st->perfStats.zoneMs,
            estimatedGetStudyCalls);
        st->logManager.LogInfo(curBarIdx, endMsg.GetChars(), LogCategory::PERF);
    } else if (sc.UpdateStartIndex == 0) {
        // Non-final bar: just accumulate
        st->perfStats.barsProcessed++;
        st->perfStats.totalMs += st->perfTimer.ElapsedMs();
    }
#endif

    // --- 4. Check Mode Lock ---
    if (mode == AuctionMode::MODE_LOCKED)
    {
        // CONTRACT: Rule 1.1 (bar close), Rule 1.5 (session boundary deferral)
        const bool curBarClosed = (sc.GetBarHasClosedStatus(curBarIdx) == BHCS_BAR_HAS_CLOSED);

        // CONTRACT Rule 1.5: Defer logging on session boundary (values transitional)
        if (curBarClosed && !sessionChanged &&
            st->logManager.ShouldEmit(LogChannel::EVENTS_CSV, LogLevel::MINIMAL) &&
            st->logManager.ShouldLog(ThrottleKey::MODE_LOCK, curBarIdx, 5) &&
            !st->lastLoggedModeLocked)
        {
            // Use LIVE values - ALL fields populated
            AMT::SessionEvent evt;
            evt.type = AMT::SessionEventType::MODE_LOCK;
            evt.timestamp = probeBarTime;
            evt.bar = curBarIdx;

            // Phase fields
            evt.phase = AMT::CurrentPhaseToString(amtSnapshot.phase);

            // Intent/state fields - ALL populated
            evt.deltaConf = st->amtContext.confidence.deltaConsistency;
            evt.sessDeltaPct = sessionDeltaPct;
            evt.sessDeltaPctl = static_cast<int>(sessionDeltaPctile);
            evt.coherent = directionalCoherence ? 1 : 0;
            evt.aggression = AMT::to_string(detectedAggression);
            evt.facilitation = AMT::to_string(detectedFacilitation);
            evt.marketState = AMT::to_string(st->amtContext.state);

            // VBP fields
            evt.poc = st->sessionMgr.GetPOC();
            evt.vah = st->sessionMgr.GetVAH();
            evt.val = st->sessionMgr.GetVAL();

            // Volume/range diagnostics (use pre-computed curBarRangeTicks - DRY)
            evt.volume = st->currentSnapshot.effort.totalVolume;
            evt.range = curBarRangeTicks;

            evt.message = "";  // No longer logging rawState (MarketStateTracker removed)
            st->logManager.LogSessionEvent(evt);
            st->lastLoggedModeLocked = true;
        }
        return;  // MODE_LOCKED - nothing more to do this bar
    }
    // Reset mode-locked flag when we exit MODE_LOCKED
    st->lastLoggedModeLocked = false;

    // --- 5. If Probe Active (via ProbeManager), Feed to MiniVP Only ---
    if (st->probeMgr.IsProbeActive())
    {
        st->miniVP.Update(probeHigh, probeLow, probeClose, probeBidVol, probeAskVol, curBarIdx, probeBarTime, tickSize);

        const ProbeResult result = st->miniVP.GetResult();
        if (result.status != ProbeStatus::OBSERVING)
        {
            const ProbeRequest& req = st->miniVP.GetActiveRequest();

            st->zoneStore.RecordProbeResult(req, result, st->sessionMgr.GetPOC());  // SSOT

            // Calculate probe duration
            const int firedBar = st->probeMgr.GetFiredBarIndex();
            const int resolutionBars = curBarIdx - firedBar;
            const double elapsedSec = static_cast<double>(result.observation_time_ms) / 1000.0;

            if (diagLevel >= 1)
            {
                SCString msg;
                msg.Format(
                    "#%d S%d RESOLVED: %s | MFE:%.1f MAE:%.1f | %s | Bars:%d Time:%.1fs",
                    req.probe_id,
                    req.scenario_id,
                    to_string(result.status),
                    result.mfe,
                    result.mae,
                    to_string(result.mechanism),
                    resolutionBars,
                    elapsedSec
                );
                st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PROBE);
            }

            if (st->logManager.ShouldEmit(LogChannel::PROBE_CSV, LogLevel::MINIMAL))
            {
                st->logManager.LogProbeResolved(req, result, resolutionBars, probeBarTime);
                st->logManager.FlushAll();  // Flush probe CSV immediately
            }

            // Record for replay validation (using deterministic signature: fired_bar + scenario + direction)
            const int prevDivergences = st->replayValidator.GetDivergenceCount();
            st->replayValidator.RecordOutcome(req, result, firedBar, curBarIdx);
            if (st->replayValidator.IsValidating() &&
                st->replayValidator.GetDivergenceCount() > prevDivergences)
            {
                SCString msg;
                msg.Format("Probe @bar%d S%d %s: outcome differs from expected",
                    firedBar, req.scenario_id,
                    req.direction == ProbeDirection::LONG ? "LONG" : "SHORT");
                st->logManager.LogWarn(curBarIdx, msg.GetChars(), LogCategory::PROBE);
                st->sessionAccum.validationDivergenceCount++;
            }

            // --- ACCUMULATE PROBE STATS ---
            st->sessionAccum.probesResolved++;
            st->sessionAccum.totalProbeScore += req.score;
            switch (result.status) {
                case ProbeStatus::ACCEPTED:
                    st->sessionAccum.probesHit++;
                    break;
                case ProbeStatus::REJECTED:
                    st->sessionAccum.probesMissed++;
                    break;
                case ProbeStatus::TIMEOUT:
                    st->sessionAccum.probesExpired++;
                    break;
                default:
                    break;
            }

            st->miniVP.Clear();
            st->probeMgr.OnProbeResolved(curBarIdx);
            st->active_probe_count = 0;
        }
        return;  // Probe active - handled, nothing more to do this bar
    }

    // --- 6. Update warmup status and check if we can fire a new probe ---
    // Warmup status is updated each bar after backfill (baselines need N bars of live data)
    if (st->probeMgr.IsBackfillComplete()) {
        st->probeMgr.SetBaselineWarmedUp(st->drift.isWarmedUp());
    }

    if (!st->probeMgr.CanFireProbe(curBarIdx, isLiveBar))
    {
        // Log block reason only on state change (low-noise logging)
        if (st->logManager.ShouldEmit(LogChannel::EVENTS_CSV, LogLevel::MODERATE) &&
            st->probeMgr.ShouldLogBlockChange(curBarIdx))
        {
            const ProbeBlockReason reason = st->probeMgr.GetLastBlockReason();
            AMT::SessionEvent evt;
            evt.type = AMT::SessionEventType::PROBE_FIRED;  // Using PROBE_FIRED for gate events
            evt.timestamp = probeBarTime;
            evt.bar = curBarIdx;
            evt.message.Format("GATE_BLOCKED:%s|isLast=%d",
                ProbeBlockReasonToString(reason), isLiveBar ? 1 : 0);
            st->logManager.LogSessionEvent(evt);
        }
        return;  // Probe blocked - nothing more to do this bar
    }

    // --- 7. Run DynamicGauge ---
    st->dynamicGauge.SetThreshold(probeThreshold);

    // Select timeout based on session phase (from SSOT coordinator)
    const int activeTimeout = st->sessionMgr.IsRTH() ? probeTimeoutRTH : probeTimeoutGBX;
    st->dynamicGauge.SetTimeout(activeTimeout);

    // NOTE: Legacy BaselineEngine probe code removed (Dec 2024)
    // Probe system baselines pending migration to EffortBaselineStore
    // For now, skip probe gauge update until migration complete

    // Use neutral values for gauge (baselines not available)
    const double volPct = 50.0;
    const double deltaPct = 50.0;

    // Get session value area levels from SessionManager (SSOT)
    const double sessionPoc = st->sessionMgr.GetPOC();
    const double sessionVah = st->sessionMgr.GetVAH();
    const double sessionVal = st->sessionMgr.GetVAL();

    st->dynamicGauge.Update(
        volPct,
        deltaPct,
        probeClose,
        sessionPoc,
        sessionVah,
        sessionVal,
        validScenarios,
        curBarIdx,
        probeBarTime
    );

    if (st->dynamicGauge.ShouldFireProbe())
    {
        ProbeRequest req = st->dynamicGauge.CreateProbeRequest();

        // Get VbP context at the probe's price level
        // SSOT: Pass config coefficients for fallback classification
        VbPLevelContext vbpCtx = GetVbPContextAtPrice(
            st->sessionVolumeProfile,
            req.price,
            tickSize,
            st->amtZoneManager.config.hvnSigmaCoeff,
            st->amtZoneManager.config.lvnSigmaCoeff);

        st->miniVP.StartProbeWithContext(req, tickSize, vbpCtx, probeBarTime);
        st->probeMgr.OnProbeStarted(req.probe_id, curBarIdx, probeBarTime);
        st->active_probe_count = 1;
        st->sessionAccum.probesFired++;  // Accumulate probe fired count

        if (diagLevel >= 1)
        {
            SCString msg;
            msg.Format(
                "FIRED #%d S%d Score:%.1f | %s | %s | VbP:%s%s%s",
                req.probe_id,
                req.scenario_id,
                req.score,
                (req.direction == ProbeDirection::LONG) ? "LONG" : "SHORT",
                req.hypothesis,
                vbpCtx.isHVN ? "HVN " : "",
                vbpCtx.isLVN ? "LVN " : "",
                vbpCtx.atPOC ? "POC " : (vbpCtx.insideValueArea ? "VA" : "OUT")
            );
            st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::PROBE);
        }

        if (st->logManager.ShouldEmit(LogChannel::PROBE_CSV, LogLevel::MINIMAL))
        {
            st->logManager.LogProbeFired(req, probeBarTime);
            st->logManager.FlushAll();  // Flush probe CSV immediately
        }
    }

#if PERF_TIMING_ENABLED
    // === PERFORMANCE TIMING: Progress logging only ===
    // NOTE: Stats accumulation and PERF-END are handled earlier (before MODE_LOCKED check)
    // to ensure they run even when early returns are taken.
    // This block is only for progress logging during full recalc.
    if (sc.UpdateStartIndex == 0 && st->perfStats.barsProcessed % 500 == 0) {
        SCString perfMsg;
        perfMsg.Format("Bar %d/%d | elapsed=%.1fms | snapshot=%.1fms | vbp=%.1fms | zone=%.1fms",
            curBarIdx, sc.ArraySize, st->perfStats.totalMs,
            st->perfStats.snapshotMs, st->perfStats.vbpMs, st->perfStats.zoneMs);
        st->logManager.LogInfo(curBarIdx, perfMsg.GetChars(), LogCategory::PERF);
    }
#endif

#if USE_MANUAL_LOOP
    } // end for BarIndex
#endif
}
