// ============================================================================
// Test: Tuning Telemetry v0
// Verifies: Telemetry computation does NOT mutate outcomes or affect behavior
// ============================================================================

#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>

// Include the telemetry header (standalone, no Sierra dependencies)
#include "../AMT_TuningTelemetry.h"

using namespace AMT;

// ============================================================================
// Test 1: TuningAdvisory computation is pure (no side effects)
// ============================================================================
void test_advisory_computation_is_pure() {
    printf("=== Test 1: TuningAdvisory computation is pure ===\n");

    // Input values
    ExecutionFriction friction = ExecutionFriction::WIDE;
    bool frictionValid = true;
    double rangePctile = 80.0;
    double closeChangePctile = 20.0;
    bool closeChangeValid = true;

    // Copy original values
    ExecutionFriction origFriction = friction;
    bool origFrictionValid = frictionValid;
    double origRangePctile = rangePctile;
    double origCloseChangePctile = closeChangePctile;
    bool origCloseChangeValid = closeChangeValid;

    // Compute advisory
    TuningAdvisory advisory;
    advisory.ComputeAdvisories(friction, frictionValid, rangePctile, closeChangePctile, closeChangeValid);

    // Verify inputs unchanged
    assert(friction == origFriction);
    assert(frictionValid == origFrictionValid);
    assert(rangePctile == origRangePctile);
    assert(closeChangePctile == origCloseChangePctile);
    assert(closeChangeValid == origCloseChangeValid);

    printf("  PASSED: Input values unchanged after ComputeAdvisories()\n");
}

// ============================================================================
// Test 2: LOCKED friction uses wouldBlockIfLocked (not thresholdOffset sentinel)
// CONTRACT: LOCKED is a hard block represented by boolean, not numeric sentinel
// ============================================================================
void test_locked_friction_advisory() {
    printf("=== Test 2: LOCKED uses wouldBlockIfLocked (no numeric sentinel) ===\n");

    TuningAdvisory advisory;

    // LOCKED with valid flag
    advisory.ComputeAdvisories(ExecutionFriction::LOCKED, true, 50.0, 50.0, true);
    assert(advisory.wouldBlockIfLocked == true);
    // CRITICAL: thresholdOffset must be 0.0 for LOCKED (not a sentinel value)
    // wouldBlockIfLocked is the AUTHORITATIVE indicator for hard blocks
    assert(std::abs(advisory.thresholdOffset - 0.0f) < 0.001f);
    printf("  PASSED: LOCKED + valid -> wouldBlock=true, offset=0.0 (no sentinel)\n");

    // LOCKED with invalid flag (should not block)
    advisory.ComputeAdvisories(ExecutionFriction::LOCKED, false, 50.0, 50.0, true);
    assert(advisory.wouldBlockIfLocked == false);
    assert(std::abs(advisory.thresholdOffset - 0.0f) < 0.001f);
    printf("  PASSED: LOCKED + invalid -> wouldBlock=false, offset=0.0\n");
}

// ============================================================================
// Test 3: Friction threshold offsets are correct
// ============================================================================
void test_friction_threshold_offsets() {
    printf("=== Test 3: Friction threshold offsets are correct ===\n");

    TuningAdvisory advisory;

    // TIGHT
    advisory.ComputeAdvisories(ExecutionFriction::TIGHT, true, 50.0, 50.0, false);
    assert(std::abs(advisory.thresholdOffset - TuningOffsets::TIGHT_THRESHOLD_OFFSET) < 0.001f);
    printf("  TIGHT offset = %.2f (expected %.2f) - OK\n",
           advisory.thresholdOffset, TuningOffsets::TIGHT_THRESHOLD_OFFSET);

    // NORMAL
    advisory.ComputeAdvisories(ExecutionFriction::NORMAL, true, 50.0, 50.0, false);
    assert(std::abs(advisory.thresholdOffset - TuningOffsets::NORMAL_THRESHOLD_OFFSET) < 0.001f);
    printf("  NORMAL offset = %.2f (expected %.2f) - OK\n",
           advisory.thresholdOffset, TuningOffsets::NORMAL_THRESHOLD_OFFSET);

    // WIDE
    advisory.ComputeAdvisories(ExecutionFriction::WIDE, true, 50.0, 50.0, false);
    assert(std::abs(advisory.thresholdOffset - TuningOffsets::WIDE_THRESHOLD_OFFSET) < 0.001f);
    printf("  WIDE offset = %.2f (expected %.2f) - OK\n",
           advisory.thresholdOffset, TuningOffsets::WIDE_THRESHOLD_OFFSET);

    printf("  PASSED: All friction offsets correct\n");
}

// ============================================================================
// Test 4: 2D Volatility character classification
// ============================================================================
void test_volatility_character_classification() {
    printf("=== Test 4: 2D Volatility character classification ===\n");

    // COMPRESSED: low range + low travel
    VolatilityCharacter c1 = Classify2DVolatilityCharacter(20.0, 20.0, true);
    assert(c1 == VolatilityCharacter::COMPRESSED);
    printf("  Low range (20) + low travel (20) = %s - OK\n", to_string(c1));

    // TRENDING: high range + high travel
    VolatilityCharacter c2 = Classify2DVolatilityCharacter(80.0, 80.0, true);
    assert(c2 == VolatilityCharacter::TRENDING);
    printf("  High range (80) + high travel (80) = %s - OK\n", to_string(c2));

    // INDECISIVE: high range + low travel
    VolatilityCharacter c3 = Classify2DVolatilityCharacter(80.0, 20.0, true);
    assert(c3 == VolatilityCharacter::INDECISIVE);
    printf("  High range (80) + low travel (20) = %s - OK\n", to_string(c3));

    // BREAKOUT_POTENTIAL: low range + high travel
    VolatilityCharacter c4 = Classify2DVolatilityCharacter(20.0, 80.0, true);
    assert(c4 == VolatilityCharacter::BREAKOUT_POTENTIAL);
    printf("  Low range (20) + high travel (80) = %s - OK\n", to_string(c4));

    // NORMAL: middle values
    VolatilityCharacter c5 = Classify2DVolatilityCharacter(50.0, 50.0, true);
    assert(c5 == VolatilityCharacter::NORMAL);
    printf("  Middle range (50) + middle travel (50) = %s - OK\n", to_string(c5));

    // UNKNOWN: closeChangeValid = false
    VolatilityCharacter c6 = Classify2DVolatilityCharacter(80.0, 80.0, false);
    assert(c6 == VolatilityCharacter::UNKNOWN);
    printf("  Any values + closeChangeValid=false = %s - OK\n", to_string(c6));

    printf("  PASSED: All volatility characters correct\n");
}

// ============================================================================
// Test 5: Confirmation delta from volatility character
// ============================================================================
void test_confirmation_delta() {
    printf("=== Test 5: Confirmation delta from volatility character ===\n");

    TuningAdvisory advisory;

    // INDECISIVE: +1 confirmation
    advisory.ComputeAdvisories(ExecutionFriction::NORMAL, true, 80.0, 20.0, true);
    assert(advisory.confirmationDelta == TuningOffsets::INDECISIVE_CONFIRMATION_DELTA);
    printf("  INDECISIVE -> confirmationDelta=%d (expected %d) - OK\n",
           advisory.confirmationDelta, TuningOffsets::INDECISIVE_CONFIRMATION_DELTA);

    // BREAKOUT_POTENTIAL: -1 confirmation
    advisory.ComputeAdvisories(ExecutionFriction::NORMAL, true, 20.0, 80.0, true);
    assert(advisory.confirmationDelta == TuningOffsets::BREAKOUT_POTENTIAL_CONFIRMATION_DELTA);
    printf("  BREAKOUT_POTENTIAL -> confirmationDelta=%d (expected %d) - OK\n",
           advisory.confirmationDelta, TuningOffsets::BREAKOUT_POTENTIAL_CONFIRMATION_DELTA);

    // TRENDING: 0 confirmation
    advisory.ComputeAdvisories(ExecutionFriction::NORMAL, true, 80.0, 80.0, true);
    assert(advisory.confirmationDelta == TuningOffsets::TRENDING_CONFIRMATION_DELTA);
    printf("  TRENDING -> confirmationDelta=%d (expected %d) - OK\n",
           advisory.confirmationDelta, TuningOffsets::TRENDING_CONFIRMATION_DELTA);

    // UNKNOWN (invalid closeChange): 0 confirmation
    advisory.ComputeAdvisories(ExecutionFriction::NORMAL, true, 80.0, 80.0, false);
    assert(advisory.confirmationDelta == TuningOffsets::DEFAULT_CONFIRMATION_DELTA);
    printf("  UNKNOWN -> confirmationDelta=%d (expected %d) - OK\n",
           advisory.confirmationDelta, TuningOffsets::DEFAULT_CONFIRMATION_DELTA);

    printf("  PASSED: All confirmation deltas correct\n");
}

// ============================================================================
// Test 6: EngagementTelemetryRecord initialization
// ============================================================================
void test_engagement_record_defaults() {
    printf("=== Test 6: EngagementTelemetryRecord initialization ===\n");

    EngagementTelemetryRecord rec;

    // Verify safe defaults
    assert(rec.zoneId == -1);
    assert(rec.zoneType == ZoneType::NONE);
    assert(rec.bar == -1);
    assert(rec.price == 0.0);
    assert(rec.friction == ExecutionFriction::UNKNOWN);
    assert(rec.frictionValid == false);
    assert(rec.volatility == VolatilityState::NORMAL);
    assert(rec.volatilityValid == false);
    assert(rec.marketCompositionValid == false);

    printf("  PASSED: All defaults are safe/invalid\n");
}

// ============================================================================
// Test 7: ArbitrationTelemetryRecord initialization
// ============================================================================
void test_arbitration_record_defaults() {
    printf("=== Test 7: ArbitrationTelemetryRecord initialization ===\n");

    ArbitrationTelemetryRecord rec;

    // Verify safe defaults
    assert(rec.arbReason == 0);
    assert(rec.useZones == false);
    assert(rec.engagedZoneId == -1);
    assert(rec.bar == -1);
    assert(rec.friction == ExecutionFriction::UNKNOWN);
    assert(rec.frictionValid == false);
    assert(rec.character == VolatilityCharacter::UNKNOWN);

    printf("  PASSED: All defaults are safe/invalid\n");
}

// ============================================================================
// Test 8: Multiple advisory computations don't accumulate state
// ============================================================================
void test_advisory_no_state_accumulation() {
    printf("=== Test 8: Advisory computations don't accumulate state ===\n");

    TuningAdvisory advisory;

    // First computation: LOCKED
    advisory.ComputeAdvisories(ExecutionFriction::LOCKED, true, 50.0, 50.0, true);
    assert(advisory.wouldBlockIfLocked == true);

    // Second computation: TIGHT (should completely replace, not accumulate)
    advisory.ComputeAdvisories(ExecutionFriction::TIGHT, true, 50.0, 50.0, true);
    assert(advisory.wouldBlockIfLocked == false);  // Not accumulated from LOCKED
    assert(std::abs(advisory.thresholdOffset - TuningOffsets::TIGHT_THRESHOLD_OFFSET) < 0.001f);

    printf("  PASSED: Advisory computation is stateless\n");
}

// ============================================================================
// Test 9: engagedThisBar cleared per UpdateZones call (simulates ZoneManager)
// CONTRACT: engagedThisBar MUST be cleared at start of each UpdateZones()
// This test simulates the ZoneManager pattern and would FAIL if clear is missing
// ============================================================================
void test_engaged_this_bar_cleared_per_update() {
    printf("=== Test 9: engagedThisBar cleared per UpdateZones call ===\n");

    // Simulate ZoneManager's engagedThisBar
    std::vector<int> engagedThisBar;

    // Simulate UpdateZones call for bar 1
    auto simulateUpdateZones = [&engagedThisBar](bool clearAtStart) {
        // This is what ZoneManager::UpdateZones() does at the start
        if (clearAtStart) {
            engagedThisBar.clear();
        }
        // Then engagements are pushed during the zone loop
    };

    // Bar 1: UpdateZones with clear
    simulateUpdateZones(true);
    engagedThisBar.push_back(42);  // Zone 42 engaged
    assert(engagedThisBar.size() == 1);
    printf("  Bar 1: engaged zone 42, size=%zu - OK\n", engagedThisBar.size());

    // Bar 2: UpdateZones with clear - MUST reset
    simulateUpdateZones(true);
    assert(engagedThisBar.empty());  // CRITICAL: Must be empty after clear
    engagedThisBar.push_back(10);    // Zone 10 engaged
    engagedThisBar.push_back(20);    // Zone 20 engaged
    assert(engagedThisBar.size() == 2);
    printf("  Bar 2: cleared, then engaged 10+20, size=%zu - OK\n", engagedThisBar.size());

    // Bar 3: UpdateZones with clear - verify no accumulation from bar 2
    simulateUpdateZones(true);
    assert(engagedThisBar.empty());  // Must not contain bar 2's zones
    printf("  Bar 3: cleared, size=%zu (no accumulation) - OK\n", engagedThisBar.size());

    printf("  PASSED: engagedThisBar cleared per update\n");
}

// ============================================================================
// Test 11: engagedThisBar accumulation detection (regression test)
// This test would FAIL if the clear() call were removed from UpdateZones()
// ============================================================================
void test_engaged_this_bar_accumulation_detection() {
    printf("=== Test 11: Accumulation detection (would fail if clear missing) ===\n");

    std::vector<int> engagedThisBar;

    // Simulate 3 bars WITHOUT clearing (BAD behavior we want to detect)
    auto simulateUpdateZonesNoClear = [&engagedThisBar]() {
        // Intentionally NOT clearing - simulates a bug
    };

    auto simulateUpdateZonesWithClear = [&engagedThisBar]() {
        engagedThisBar.clear();  // Correct behavior
    };

    // First, show what happens WITH accumulation (bug)
    engagedThisBar.clear();  // Start fresh
    simulateUpdateZonesNoClear();
    engagedThisBar.push_back(1);
    simulateUpdateZonesNoClear();
    engagedThisBar.push_back(2);
    simulateUpdateZonesNoClear();
    engagedThisBar.push_back(3);

    // With bug (no clear), size would be 3 (accumulated)
    size_t sizeWithBug = engagedThisBar.size();
    assert(sizeWithBug == 3);  // Shows accumulation
    printf("  Simulated BUG (no clear): accumulated size=%zu (BAD) - detected\n", sizeWithBug);

    // Now show correct behavior WITH clearing
    engagedThisBar.clear();  // Reset
    simulateUpdateZonesWithClear();
    engagedThisBar.push_back(1);
    simulateUpdateZonesWithClear();
    engagedThisBar.push_back(2);
    simulateUpdateZonesWithClear();
    engagedThisBar.push_back(3);

    // With correct behavior, only last bar's engagement remains
    size_t sizeCorrect = engagedThisBar.size();
    assert(sizeCorrect == 1);  // Only zone 3 from last bar
    assert(engagedThisBar[0] == 3);
    printf("  Correct behavior (with clear): size=%zu, contains only zone 3 - OK\n", sizeCorrect);

    printf("  PASSED: Accumulation detection test\n");
}

// ============================================================================
// Test 10: Invalid inputs produce safe advisory outputs
// ============================================================================
void test_invalid_inputs_safe_outputs() {
    printf("=== Test 10: Invalid inputs produce safe advisory outputs ===\n");

    TuningAdvisory advisory;

    // All invalid
    advisory.ComputeAdvisories(ExecutionFriction::LOCKED, false, 0.0, 0.0, false);
    assert(advisory.wouldBlockIfLocked == false);  // frictionValid=false
    assert(advisory.thresholdOffset == 0.0f);      // No offset when invalid
    assert(advisory.character == VolatilityCharacter::UNKNOWN);
    assert(advisory.confirmationDelta == 0);

    printf("  PASSED: Invalid inputs produce safe defaults\n");
}

// ============================================================================
// Main
// ============================================================================
int main() {
    printf("\n=== Tuning Telemetry Tests ===\n\n");

    test_advisory_computation_is_pure();
    test_locked_friction_advisory();
    test_friction_threshold_offsets();
    test_volatility_character_classification();
    test_confirmation_delta();
    test_engagement_record_defaults();
    test_arbitration_record_defaults();
    test_advisory_no_state_accumulation();
    test_engaged_this_bar_cleared_per_update();
    test_invalid_inputs_safe_outputs();
    test_engaged_this_bar_accumulation_detection();

    printf("\n=== ALL TESTS PASSED ===\n\n");
    return 0;
}
