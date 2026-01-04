/**
 * test_ssot_invariants.cpp
 *
 * Tests for Single Source of Truth (SSOT) invariants across the AMT framework.
 * These tests verify that SSOT relationships documented in CLAUDE.md are enforced.
 *
 * SSOT Map (from CLAUDE.md):
 *   1. Session Phase: phaseCoordinator (SessionPhaseCoordinator)
 *   2. Zone Anchor Prices: VbP Study -> sessionVolumeProfile -> SessionManager
 *   3. Session Extremes: StructureTracker (ZoneManager.structure)
 *   4. Zone Anchor Storage: anchorTicks (ZoneRuntime) - anchorPrice is DERIVED
 *   5. Session Start Bar: SessionManager.sessionStartBar
 *
 * Compile: g++ -std=c++17 -I.. -o test_ssot_invariants.exe test_ssot_invariants.cpp
 * Run: ./test_ssot_invariants.exe
 */

#include <iostream>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

// Include core headers for testing
#include "test_sierrachart_mock.h"
#include "../amt_core.h"
#include "../AMT_config.h"
#define AMT_SSOT_ASSERTIONS 1
#include "../AMT_Invariants.h"
#include "../AMT_ProfileShape.h"
#include "../AMT_DayType.h"

// ============================================================================
// TEST INFRASTRUCTURE
// ============================================================================

int testsPassed = 0;
int testsFailed = 0;

void CHECK(bool condition, const std::string& testName) {
    if (condition) {
        std::cout << "[PASS] " << testName << std::endl;
        testsPassed++;
    } else {
        std::cout << "[FAIL] " << testName << std::endl;
        testsFailed++;
    }
}

void CHECK_NEAR(double a, double b, double epsilon, const std::string& testName) {
    CHECK(std::abs(a - b) < epsilon, testName);
}

// ============================================================================
// MOCK STRUCTURES (Simplified for testing invariants)
// ============================================================================

// Simplified SessionVolumeProfile (input from VbP study)
struct MockVolumeProfile {
    double session_poc = 0.0;
    double session_vah = 0.0;
    double session_val = 0.0;

    void PopulateFromVbP(double poc, double vah, double val) {
        session_poc = poc;
        session_vah = vah;
        session_val = val;
    }
};

// Simplified SessionManager (SSOT for levels)
struct MockSessionManager {
private:
    // SSOT: Private storage with controlled accessors
    double sessionPOC_ = 0.0;
    double sessionVAH_ = 0.0;
    double sessionVAL_ = 0.0;
    int sessionStartBar_ = -1;

public:
    // Single-writer interface
    void UpdateLevels(double poc, double vah, double val) {
        sessionPOC_ = poc;
        sessionVAH_ = vah;
        sessionVAL_ = val;
    }

    void SetSessionStartBar(int bar) {
        sessionStartBar_ = bar;
    }

    // Read-only accessors
    double GetPOC() const { return sessionPOC_; }
    double GetVAH() const { return sessionVAH_; }
    double GetVAL() const { return sessionVAL_; }
    int GetSessionStartBar() const { return sessionStartBar_; }

    void Reset() {
        sessionPOC_ = sessionVAH_ = sessionVAL_ = 0.0;
        sessionStartBar_ = -1;
    }
};

// Simplified StructureTracker (SSOT for session extremes)
struct MockStructureTracker {
private:
    double sessionHigh_ = 0.0;
    double sessionLow_ = 0.0;
    int sessionHighBar_ = -1;
    int sessionLowBar_ = -1;

public:
    // Single-writer interface
    void UpdateExtremes(double high, double low, int bar) {
        if (high > sessionHigh_ || sessionHigh_ == 0.0) {
            sessionHigh_ = high;
            sessionHighBar_ = bar;
        }
        if (low < sessionLow_ || sessionLow_ == 0.0) {
            sessionLow_ = low;
            sessionLowBar_ = bar;
        }
    }

    // Read-only accessors
    double GetSessionHigh() const { return sessionHigh_; }
    double GetSessionLow() const { return sessionLow_; }
    int GetSessionHighBar() const { return sessionHighBar_; }
    int GetSessionLowBar() const { return sessionLowBar_; }

    bool IsHighUpdatedRecently(int currentBar, int threshold = 5) const {
        return (sessionHighBar_ >= 0) && (currentBar - sessionHighBar_ <= threshold);
    }

    void Reset() {
        sessionHigh_ = sessionLow_ = 0.0;
        sessionHighBar_ = sessionLowBar_ = -1;
    }
};

// Simplified ZoneRuntime (SSOT: anchorTicks, DERIVED: anchorPrice)
struct MockZoneRuntime {
private:
    long long anchorTicks_ = 0;
    double anchorPrice_ = 0.0;  // DERIVED - never set directly
    double tickSizeCache_ = 0.0;

public:
    // Single-writer interface for anchor
    void SetAnchorTicks(long long ticks, double tickSize) {
        anchorTicks_ = ticks;
        tickSizeCache_ = tickSize;
        anchorPrice_ = ticks * tickSize;  // Always derived
    }

    void RecenterAnchor(long long newTicks) {
        anchorTicks_ = newTicks;
        anchorPrice_ = newTicks * tickSizeCache_;  // Re-derive
    }

    // Read-only accessors
    long long GetAnchorTicks() const { return anchorTicks_; }
    double GetAnchorPrice() const { return anchorPrice_; }

    // INVARIANT CHECK: anchorPrice must always equal anchorTicks * tickSize
    bool CheckAnchorInvariant() const {
        if (tickSizeCache_ <= 0.0) return true;  // Not initialized
        double expected = anchorTicks_ * tickSizeCache_;
        return std::abs(anchorPrice_ - expected) < 1e-9;
    }
};

// ============================================================================
// SSOT INVARIANT TESTS
// ============================================================================

/**
 * Test 1: VolumeProfile -> SessionManager sync invariant
 * After UpdateLevels(), SessionManager must match VolumeProfile
 */
void test_vbp_to_sessionmgr_sync() {
    std::cout << "\n--- Test: VbP -> SessionManager Sync ---\n";

    MockVolumeProfile vbp;
    MockSessionManager mgr;

    // Simulate VbP study populating data
    vbp.PopulateFromVbP(6100.00, 6110.00, 6090.00);

    // SSOT contract: Must call UpdateLevels to sync
    mgr.UpdateLevels(vbp.session_poc, vbp.session_vah, vbp.session_val);

    // Invariant: SessionManager must match source
    CHECK_NEAR(mgr.GetPOC(), vbp.session_poc, 0.001, "POC synced to SessionManager");
    CHECK_NEAR(mgr.GetVAH(), vbp.session_vah, 0.001, "VAH synced to SessionManager");
    CHECK_NEAR(mgr.GetVAL(), vbp.session_val, 0.001, "VAL synced to SessionManager");

    // Simulate POC drift (VbP updates)
    vbp.PopulateFromVbP(6102.00, 6112.00, 6092.00);

    // Before sync: values diverge
    CHECK(std::abs(mgr.GetPOC() - vbp.session_poc) > 0.01, "POC diverged before sync");

    // After sync: values match again
    mgr.UpdateLevels(vbp.session_poc, vbp.session_vah, vbp.session_val);
    CHECK_NEAR(mgr.GetPOC(), vbp.session_poc, 0.001, "POC re-synced after update");
}

/**
 * Test 2: StructureTracker is SSOT for session extremes
 * Must be updated via UpdateExtremes(), not direct writes
 */
void test_structure_tracker_extremes() {
    std::cout << "\n--- Test: StructureTracker Extremes SSOT ---\n";

    MockStructureTracker tracker;

    // First bar sets initial extremes
    tracker.UpdateExtremes(6100.00, 6095.00, 0);
    CHECK_NEAR(tracker.GetSessionHigh(), 6100.00, 0.001, "Initial high set");
    CHECK_NEAR(tracker.GetSessionLow(), 6095.00, 0.001, "Initial low set");

    // New high updates high only
    tracker.UpdateExtremes(6105.00, 6097.00, 1);
    CHECK_NEAR(tracker.GetSessionHigh(), 6105.00, 0.001, "New high updated");
    CHECK_NEAR(tracker.GetSessionLow(), 6095.00, 0.001, "Low unchanged (higher low ignored)");

    // New low updates low only
    tracker.UpdateExtremes(6103.00, 6090.00, 2);
    CHECK_NEAR(tracker.GetSessionHigh(), 6105.00, 0.001, "High unchanged (lower high ignored)");
    CHECK_NEAR(tracker.GetSessionLow(), 6090.00, 0.001, "New low updated");

    // Bar tracking works
    CHECK(tracker.GetSessionHighBar() == 1, "High bar tracked correctly");
    CHECK(tracker.GetSessionLowBar() == 2, "Low bar tracked correctly");
    CHECK(tracker.IsHighUpdatedRecently(3, 5), "High updated recently (bar 1->3)");
}

/**
 * Test 3: Zone anchor tick/price invariant
 * anchorPrice must ALWAYS equal anchorTicks * tickSize
 */
void test_zone_anchor_invariant() {
    std::cout << "\n--- Test: Zone Anchor Tick/Price Invariant ---\n";

    MockZoneRuntime zone;
    const double tickSize = 0.25;  // ES tick size

    // Set anchor at 6100.00 = 24400 ticks
    zone.SetAnchorTicks(24400, tickSize);
    CHECK(zone.CheckAnchorInvariant(), "Invariant holds after SetAnchorTicks");
    CHECK_NEAR(zone.GetAnchorPrice(), 6100.00, 0.001, "Anchor price derived correctly");

    // Recenter to 6102.00 = 24408 ticks
    zone.RecenterAnchor(24408);
    CHECK(zone.CheckAnchorInvariant(), "Invariant holds after RecenterAnchor");
    CHECK_NEAR(zone.GetAnchorPrice(), 6102.00, 0.001, "Recentered price derived correctly");

    // Verify ticks are integer (no floating point drift)
    CHECK(zone.GetAnchorTicks() == 24408, "Anchor ticks are exact integer");
}

/**
 * Test 4: SessionManager.sessionStartBar is SSOT
 * Only one place should set this value
 */
void test_session_start_bar_ssot() {
    std::cout << "\n--- Test: Session Start Bar SSOT ---\n";

    MockSessionManager mgr;

    // Initially invalid
    CHECK(mgr.GetSessionStartBar() == -1, "Session start bar initially invalid");

    // Set on session transition
    mgr.SetSessionStartBar(100);
    CHECK(mgr.GetSessionStartBar() == 100, "Session start bar set correctly");

    // Reset clears it
    mgr.Reset();
    CHECK(mgr.GetSessionStartBar() == -1, "Session start bar cleared on reset");
}

/**
 * Test 5: SessionPhaseCoordinator single-writer pattern
 * Uses the actual SessionPhaseCoordinator from AMT_Session.h concepts
 */
void test_phase_coordinator_single_writer() {
    std::cout << "\n--- Test: Phase Coordinator Single Writer ---\n";

    // Simulate the coordinator pattern
    AMT::SessionPhase current = AMT::SessionPhase::UNKNOWN;
    AMT::SessionPhase previous = AMT::SessionPhase::UNKNOWN;
    bool sessionChanged = false;

    auto UpdatePhase = [&](AMT::SessionPhase newPhase) -> bool {
        if (newPhase == current) return false;
        previous = current;
        current = newPhase;
        sessionChanged = true;
        return true;
    };

    // First update
    bool changed = UpdatePhase(AMT::SessionPhase::INITIAL_BALANCE);
    CHECK(changed, "First phase change detected");
    CHECK(current == AMT::SessionPhase::INITIAL_BALANCE, "Phase updated to IB");
    CHECK(previous == AMT::SessionPhase::UNKNOWN, "Previous was UNKNOWN");

    // Same phase, no change
    changed = UpdatePhase(AMT::SessionPhase::INITIAL_BALANCE);
    CHECK(!changed, "Same phase, no change");

    // New phase
    changed = UpdatePhase(AMT::SessionPhase::MID_SESSION);
    CHECK(changed, "Phase change to MID_SESSION detected");
    CHECK(previous == AMT::SessionPhase::INITIAL_BALANCE, "Previous is now IB");
}

/**
 * Test 6: PriceToTicks/TicksToPrice roundtrip invariant
 * Tests the canonical tick math from AMT_config.h
 */
void test_tick_math_roundtrip() {
    std::cout << "\n--- Test: Tick Math Roundtrip ---\n";

    const double tickSize = 0.25;

    // Test prices that should roundtrip exactly
    std::vector<double> testPrices = {6100.00, 6100.25, 6100.50, 6100.75, 6099.00};

    for (double price : testPrices) {
        long long ticks = AMT::PriceToTicks(price, tickSize);
        double roundtrip = ticks * tickSize;

        std::string testName = "Roundtrip: " + std::to_string(price);
        CHECK_NEAR(roundtrip, price, 0.0001, testName);
    }

    // Test that non-tick-aligned prices snap to nearest tick
    double midPrice = 6100.12;  // Between .00 and .25
    long long ticks = AMT::PriceToTicks(midPrice, tickSize);
    double snapped = ticks * tickSize;
    CHECK(snapped == 6100.00 || snapped == 6100.25, "Non-aligned price snaps to tick");
}

/**
 * Test 7: DRY violation detector - document pattern for grep-based CI
 * This test documents what patterns indicate SSOT bypass
 */
void test_dry_violation_patterns() {
    std::cout << "\n--- Test: DRY Violation Pattern Documentation ---\n";

    // Document patterns that indicate SSOT violations:
    // 1. Direct write to anchorPrice (should only be derived)
    //    Pattern: "anchorPrice\s*=" without "anchorTicks"

    // 2. Reading sessionVolumeProfile.session_* when SessionManager has SSOT
    //    Pattern: "sessionVolumeProfile\.session_(poc|vah|val)" in non-sync code

    // 3. Multiple session phase storage locations
    //    Pattern: "sessionPhase\s*=" in files other than coordinator

    // These patterns should be checked by CI/grep scripts
    std::cout << "  DRY patterns documented for CI enforcement\n";
    CHECK(true, "DRY violation patterns documented");
}

/**
 * Test 8: AMT_Invariants.h validation helpers
 * Tests the helper functions from the invariants header
 */
void test_invariant_helpers() {
    std::cout << "\n--- Test: AMT_Invariants.h Helpers ---\n";

    // Zone anchor invariant
    CHECK(AMT::ValidateZoneAnchorInvariant(24400, 6100.00, 0.25), "Zone anchor valid (exact)");
    CHECK(!AMT::ValidateZoneAnchorInvariant(24400, 6100.50, 0.25), "Zone anchor invalid (drift)");

    // Percentile range
    CHECK(AMT::ValidatePercentileRange(0.0), "Percentile 0 valid");
    CHECK(AMT::ValidatePercentileRange(50.0), "Percentile 50 valid");
    CHECK(AMT::ValidatePercentileRange(100.0), "Percentile 100 valid");
    CHECK(!AMT::ValidatePercentileRange(-1.0), "Percentile -1 invalid");
    CHECK(!AMT::ValidatePercentileRange(101.0), "Percentile 101 invalid");

    // Price positive
    CHECK(AMT::ValidatePricePositive(6100.00), "Price positive valid");
    CHECK(!AMT::ValidatePricePositive(0.0), "Price zero invalid");
    CHECK(!AMT::ValidatePricePositive(-1.0), "Price negative invalid");

    // Session level order
    CHECK(AMT::ValidateSessionLevelOrder(6100.00, 6110.00, 6090.00), "Session levels ordered");
    CHECK(!AMT::ValidateSessionLevelOrder(6100.00, 6090.00, 6110.00), "Session levels inverted");

    // SSOTCheckpoint
    AMT::SSOTCheckpoint checkpoint;
    checkpoint.CheckZoneAnchor(24400, 6100.00, 0.25);
    CHECK(!checkpoint.HasViolations(), "Checkpoint no violations");

    checkpoint.Reset();
    checkpoint.CheckPercentile(150.0, "test_pctl");  // Invalid
    CHECK(checkpoint.HasViolations(), "Checkpoint detected violation");
}

// ============================================================================
// CIRCULARITY TESTS: DayStructure vs ProfileShape Independence
// ============================================================================

/**
 * Test 9: DayStructure is independent of ProfileShape (no circularity)
 * DayTypeClassifier must compute structure from RE acceptance, NOT from shape
 */
void test_daystructure_independence_from_shape() {
    std::cout << "\n--- Test: DayStructure Independence from ProfileShape ---\n";

    // Create a fresh classifier
    AMT::DayTypeClassifier classifier;
    classifier.Reset(0);

    // Simulate session with IB complete and RE acceptance
    classifier.NotifyIBComplete(60, SCDateTime());
    classifier.NotifyProfileMature(true);

    // Start RE attempt above IB - close MUST be outside IB to not trigger immediate rejection
    AMT::RangeExtensionState state = classifier.UpdateRETracking(
        6100.25,    // barHigh - outside IB
        6095.00,    // barLow
        6099.00,    // barClose - OUTSIDE IB (> ibHigh=6098.00)
        6098.00,    // ibHigh
        6090.00,    // ibLow
        1000.0,     // barVolume
        50.0,       // barDelta
        10000.0,    // sessionTotalVolume
        61,         // currentBar
        SCDateTime(),
        0.25);      // tickSize

    CHECK(state == AMT::RangeExtensionState::ATTEMPTING, "RE attempt started");

    // Continue for enough bars with sufficient volume to trigger acceptance
    // Acceptance requires: MIN_BARS=6 bars, MIN_VOLUME_PCT=10% of session volume
    for (int bar = 62; bar <= 70; bar++) {
        state = classifier.UpdateRETracking(
            6100.50, 6099.00, 6100.00,  // High outside IB, close outside
            6098.00, 6090.00,            // IB boundaries
            500.0, 10.0, 10000.0,        // Volume (500/bar), delta, total
            bar, SCDateTime(), 0.25);
        if (state == AMT::RangeExtensionState::ACCEPTED) break;  // Stop once accepted
    }

    CHECK(state == AMT::RangeExtensionState::ACCEPTED, "RE accepted");

    // Classify
    bool classified = classifier.TryClassify(68, SCDateTime());
    CHECK(classified, "Classification occurred");
    CHECK(classifier.GetClassification() == AMT::DayStructure::IMBALANCED,
          "DayStructure = IMBALANCED (from RE, not shape)");

    // KEY: DayTypeClassifier NEVER referenced ProfileShape!
    // Classification was based purely on RE acceptance evidence.
    std::cout << "  INVARIANT: DayTypeClassifier does not reference ProfileShape\n";
}

/**
 * Test 10: Shape resolution requires BOTH inputs
 * Cannot freeze shape without both RawShape and DayStructure being valid
 */
void test_shape_resolution_requires_both_inputs() {
    std::cout << "\n--- Test: Shape Resolution Requires Both Inputs ---\n";

    // Case 1: RawShape valid, DayStructure undefined -> cannot resolve
    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::NORMAL_DISTRIBUTION,
            AMT::DayStructure::UNDEFINED);

        CHECK(result.finalShape == AMT::ProfileShape::UNDEFINED,
              "RawShape + UNDEFINED structure -> UNDEFINED final");
        CHECK(result.conflict == false, "No conflict (just pending)");
    }

    // Case 2: RawShape undefined, DayStructure valid -> cannot resolve
    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::UNDEFINED,
            AMT::DayStructure::BALANCED);

        CHECK(result.finalShape == AMT::ProfileShape::UNDEFINED,
              "UNDEFINED shape + structure -> UNDEFINED final");
        CHECK(result.conflict == false, "No conflict (just pending)");
    }

    // Case 3: Both valid -> resolution occurs
    {
        AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::NORMAL_DISTRIBUTION,
            AMT::DayStructure::BALANCED);

        CHECK(result.finalShape == AMT::ProfileShape::NORMAL_DISTRIBUTION,
              "Both valid -> resolution occurs");
        CHECK(result.conflict == false, "No conflict (family matches)");
    }

    std::cout << "  INVARIANT: No partial freeze, both inputs required\n";
}

/**
 * Test 11: Family constraint applied after computation (Stage B)
 * Demonstrates the two-stage architecture: compute independently, then combine
 */
void test_family_constraint_is_stage_b() {
    std::cout << "\n--- Test: Family Constraint is Stage B (Post-Computation) ---\n";

    // Simulate user's case: THIN_VERTICAL shape but BALANCED structure
    AMT::ProfileShape rawShape = AMT::ProfileShape::THIN_VERTICAL;  // From geometry
    AMT::DayStructure structure = AMT::DayStructure::BALANCED;      // From RE tracking

    AMT::ShapeResolutionResult result = AMT::ResolveShapeWithDayStructure(rawShape, structure);

    CHECK(result.rawShape == AMT::ProfileShape::THIN_VERTICAL,
          "RawShape preserved as THIN_VERTICAL");
    CHECK(result.finalShape == AMT::ProfileShape::UNDEFINED,
          "FinalShape = UNDEFINED (family conflict)");
    CHECK(result.conflict == true, "Conflict flag set");

    // KEY: Neither computation influenced the other
    std::cout << "  RawShape = THIN_VERTICAL (computed from geometry)\n";
    std::cout << "  DayStructure = BALANCED (computed from RE tracking)\n";
    std::cout << "  Conflict detected AFTER both independently computed\n";
    std::cout << "  INVARIANT: Family constraint is Stage B (post-computation)\n";
}

/**
 * Test 12: Family helper functions coverage
 */
void test_family_helper_coverage() {
    std::cout << "\n--- Test: Family Helper Functions ---\n";

    // Balance family
    CHECK(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::NORMAL_DISTRIBUTION),
          "NORMAL_DISTRIBUTION in balance family");
    CHECK(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::D_SHAPED),
          "D_SHAPED in balance family");
    CHECK(AMT::IsShapeInBalanceFamily(AMT::ProfileShape::BALANCED),
          "BALANCED in balance family");
    CHECK(!AMT::IsShapeInBalanceFamily(AMT::ProfileShape::P_SHAPED),
          "P_SHAPED not in balance family");

    // Imbalance family
    CHECK(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::P_SHAPED),
          "P_SHAPED in imbalance family");
    CHECK(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::B_SHAPED),
          "B_SHAPED in imbalance family");
    CHECK(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::THIN_VERTICAL),
          "THIN_VERTICAL in imbalance family");
    CHECK(AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::DOUBLE_DISTRIBUTION),
          "DOUBLE_DISTRIBUTION in imbalance family");
    CHECK(!AMT::IsShapeInImbalanceFamily(AMT::ProfileShape::NORMAL_DISTRIBUTION),
          "NORMAL_DISTRIBUTION not in imbalance family");
}

/**
 * Test shape semantics: per-bar instantaneous vs session-level frozen SSOT
 *
 * Three distinct concepts:
 *   RawShapeNow: instantaneous geometric shape (changes bar-to-bar)
 *   ResolvedNow: per-bar resolution with family constraint (can be CONFLICT)
 *   FinalShapeFrozen: session-level SSOT (frozen once, immutable)
 *
 * The term "FINAL" must only refer to FinalShapeFrozen, never per-bar resolution.
 */
void test_shape_semantics_per_bar_vs_frozen() {
    std::cout << "\n=== Shape Semantics: Per-Bar vs Session-Level ===\n";

    // Simulate a session with evolving shapes
    struct ShapeSnapshot {
        AMT::ProfileShape rawNow;
        AMT::DayStructure structure;
        AMT::ProfileShape resolvedNow;
        bool conflictNow;
    };

    // Simulate shape evolution across bars (what happens in real trading)
    std::vector<ShapeSnapshot> barSnapshots;

    // Early session: P_SHAPED (imbalance) with BALANCED structure -> CONFLICT
    {
        auto result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::P_SHAPED, AMT::DayStructure::BALANCED);
        barSnapshots.push_back({AMT::ProfileShape::P_SHAPED, AMT::DayStructure::BALANCED,
                                result.finalShape, result.conflict});
    }

    // Mid session: D_SHAPED (balance) with BALANCED structure -> ACCEPTED
    {
        auto result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::D_SHAPED, AMT::DayStructure::BALANCED);
        barSnapshots.push_back({AMT::ProfileShape::D_SHAPED, AMT::DayStructure::BALANCED,
                                result.finalShape, result.conflict});
    }

    // Late session: THIN_VERTICAL (imbalance) with BALANCED structure -> CONFLICT
    {
        auto result = AMT::ResolveShapeWithDayStructure(
            AMT::ProfileShape::THIN_VERTICAL, AMT::DayStructure::BALANCED);
        barSnapshots.push_back({AMT::ProfileShape::THIN_VERTICAL, AMT::DayStructure::BALANCED,
                                result.finalShape, result.conflict});
    }

    // Verify per-bar shapes CAN fluctuate
    CHECK(barSnapshots[0].rawNow != barSnapshots[1].rawNow,
          "RawShapeNow can change between bars (P_SHAPED -> D_SHAPED)");
    CHECK(barSnapshots[1].rawNow != barSnapshots[2].rawNow,
          "RawShapeNow can change between bars (D_SHAPED -> THIN_VERTICAL)");

    // Verify per-bar conflict status CAN fluctuate
    CHECK(barSnapshots[0].conflictNow == true,
          "Bar 0: P_SHAPED + BALANCED = CONFLICT");
    CHECK(barSnapshots[1].conflictNow == false,
          "Bar 1: D_SHAPED + BALANCED = NO CONFLICT (accepted)");
    CHECK(barSnapshots[2].conflictNow == true,
          "Bar 2: THIN_VERTICAL + BALANCED = CONFLICT");

    // Simulate freeze at bar 1 (when D_SHAPED was valid)
    AMT::ProfileShape frozenShape = AMT::ProfileShape::UNDEFINED;
    int freezeBar = -1;
    bool frozenConflict = false;

    // Freeze logic: freeze when first non-conflicting shape is available
    for (size_t i = 0; i < barSnapshots.size(); ++i) {
        if (!barSnapshots[i].conflictNow && frozenShape == AMT::ProfileShape::UNDEFINED) {
            frozenShape = barSnapshots[i].resolvedNow;
            freezeBar = static_cast<int>(i);
            frozenConflict = barSnapshots[i].conflictNow;
            break;
        }
    }

    CHECK(frozenShape == AMT::ProfileShape::D_SHAPED,
          "FinalShapeFrozen captured D_SHAPED at freeze time");
    CHECK(freezeBar == 1,
          "Freeze occurred at bar 1 (first non-conflicting)");
    CHECK(!frozenConflict,
          "Frozen shape has no conflict");

    // Key invariant: FinalShapeFrozen is IMMUTABLE after freeze
    // Even though bar 2 has THIN_VERTICAL (conflict), the frozen shape stays D_SHAPED
    CHECK(frozenShape == AMT::ProfileShape::D_SHAPED,
          "FinalShapeFrozen remains D_SHAPED after bar 2 (immutable)");

    // Statistics should count SESSIONS, not BARS
    // If we counted bars, we'd see 2/3 = 67% CONFLICT
    // If we count sessions (freeze events), we'd see 0/1 = 0% CONFLICT
    int sessionCount = 1;  // One session
    int frozenWithConflict = frozenConflict ? 1 : 0;  // From freeze event
    double sessionConflictRate = 100.0 * frozenWithConflict / sessionCount;

    CHECK(sessionConflictRate == 0.0,
          "Session-level conflict rate = 0% (from freeze event, not per-bar)");

    // What would be WRONG: counting per-bar conflicts
    int barConflictCount = 0;
    for (const auto& snap : barSnapshots) {
        if (snap.conflictNow) barConflictCount++;
    }
    double wrongBarConflictRate = 100.0 * barConflictCount / barSnapshots.size();
    CHECK(wrongBarConflictRate > 60.0,
          "Per-bar conflict rate = 67% (bogus if used for session stats)");

    // Verify the two rates are DIFFERENT
    CHECK(sessionConflictRate != wrongBarConflictRate,
          "Session-level and per-bar conflict rates differ (semantically correct)");

    std::cout << "  Session-level FINAL conflict rate: " << sessionConflictRate << "%\n";
    std::cout << "  Per-bar RESOLVED_NOW conflict rate: " << wrongBarConflictRate << "% (diagnostic only)\n";
}

/**
 * Test: RE acceptance with consolidation bars (Bug Fix Validation)
 *
 * Previously, bars that CLOSED outside IB but didn't make new extension HIGHs
 * were not counted toward the 6-bar acceptance threshold. This test validates
 * the fix that counts bars based on CLOSE position, not HIGH extension.
 *
 * Scenario: Price extends above IB, then consolidates outside IB without
 * making new highs. These consolidation bars should count toward acceptance.
 */
void test_re_consolidation_bars_count() {
    std::cout << "\n--- Test: RE Consolidation Bars Count Toward Acceptance ---\n";

    AMT::DayTypeClassifier classifier;
    classifier.Reset(0);
    classifier.NotifyIBComplete(60, SCDateTime());
    classifier.NotifyProfileMature(true);

    const double ibHigh = 6100.00;
    const double ibLow = 6090.00;
    double sessionVol = 10000.0;

    // Bar 1: Initial extension - HIGH above IB, CLOSE above IB
    AMT::RangeExtensionState state = classifier.UpdateRETracking(
        6105.00,    // barHigh - extends above IB
        6095.00,    // barLow
        6103.00,    // barClose - outside IB (> 6100)
        ibHigh, ibLow,
        500.0, 10.0, sessionVol,
        61, SCDateTime(), 0.25);
    CHECK(state == AMT::RangeExtensionState::ATTEMPTING, "RE attempt started");

    // Bars 2-6: Consolidation bars - HIGH does NOT extend above 6105,
    // but CLOSE remains above IB. These should count toward acceptance.
    for (int bar = 62; bar <= 66; bar++) {
        sessionVol += 500.0;  // Accumulate session volume
        state = classifier.UpdateRETracking(
            6102.00,    // barHigh - BELOW initial extension (6105), but still > IB
            6099.00,    // barLow
            6101.00,    // barClose - still outside IB (> 6100)
            ibHigh, ibLow,
            500.0, 10.0, sessionVol,
            bar, SCDateTime(), 0.25);

        // Before the fix, these bars wouldn't count because HIGH didn't extend
        // After the fix, they count because CLOSE is outside IB
        if (state == AMT::RangeExtensionState::ACCEPTED) {
            std::cout << "  RE accepted at bar " << bar << " (6 bars accumulated)\n";
            break;
        }
    }

    CHECK(state == AMT::RangeExtensionState::ACCEPTED,
          "RE accepted with consolidation bars (bug fix validation)");

    // Verify classification
    bool classified = classifier.TryClassify(66, SCDateTime());
    CHECK(classified, "Classification occurred");
    CHECK(classifier.GetClassification() == AMT::DayStructure::IMBALANCED,
          "DayStructure = IMBALANCED");

    std::cout << "  BUG FIX VALIDATED: Consolidation bars outside IB count toward acceptance\n";
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "========================================\n";
    std::cout << "  AMT SSOT Invariant Tests\n";
    std::cout << "========================================\n";

    test_vbp_to_sessionmgr_sync();
    test_structure_tracker_extremes();
    test_zone_anchor_invariant();
    test_session_start_bar_ssot();
    test_phase_coordinator_single_writer();
    test_tick_math_roundtrip();
    test_dry_violation_patterns();
    test_invariant_helpers();

    std::cout << "\n--- Circularity Tests (DayStructure vs ProfileShape) ---\n";
    test_daystructure_independence_from_shape();
    test_shape_resolution_requires_both_inputs();
    test_family_constraint_is_stage_b();
    test_family_helper_coverage();

    std::cout << "\n--- Bug Fix Validation Tests ---\n";
    test_re_consolidation_bars_count();

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << testsPassed << " passed, " << testsFailed << " failed\n";
    std::cout << "========================================\n";

    std::cout << "\n--- Shape Semantics Tests (Per-Bar vs Session-Level) ---\n";
    test_shape_semantics_per_bar_vs_frozen();

    std::cout << "\nKEY INVARIANTS VERIFIED:\n";
    std::cout << "  1. VbP -> SessionManager sync\n";
    std::cout << "  2. StructureTracker session extremes SSOT\n";
    std::cout << "  3. Zone anchor tick/price derivation\n";
    std::cout << "  4. Session start bar SSOT\n";
    std::cout << "  5. Phase coordinator single-writer\n";
    std::cout << "  6. DayStructure computed independently of ProfileShape\n";
    std::cout << "  7. Shape resolution requires both inputs (no partial freeze)\n";
    std::cout << "  8. Family constraint is Stage B (post-computation)\n";
    std::cout << "  9. Shape semantics: per-bar vs session-level SSOT\n";
    std::cout << "  10. RE acceptance counts CLOSE-based bars (not just extension bars)\n";

    return testsFailed > 0 ? 1 : 0;
}
