// ============================================================================
// test_analytics.cpp
// Unit tests for AMT_Analytics.h
// Tests: PULLBACK counting, bucket-sum invariant, MarketState guardrails
// ============================================================================

#include "test_sierrachart_mock.h"
#include "AMT_Analytics.h"
#include "AMT_Session.h"  // For SessionEngagementAccumulator
#include <iostream>
#include <cassert>
#include <vector>

using namespace AMT;

// ============================================================================
// TEST 1: PULLBACK COUNTING
// Previously PULLBACK fell through to default:break and was silently dropped
// ============================================================================

void TestPullbackIsCounted() {
    std::cout << "Testing PULLBACK phase is counted..." << std::endl;

    // Create a phase history with PULLBACK bars
    std::vector<CurrentPhase> history;
    history.push_back(CurrentPhase::ROTATION);
    history.push_back(CurrentPhase::ROTATION);
    history.push_back(CurrentPhase::PULLBACK);  // This was previously dropped!
    history.push_back(CurrentPhase::PULLBACK);
    history.push_back(CurrentPhase::DRIVING_UP);

    // Create minimal ZoneManager and empty accumulator for stats calculation
    ZoneManager zm;
    zm.config = ZoneConfig();
    SessionEngagementAccumulator accum;

    // Updated API: CalculateSessionStats(zm, accum, poc, vah, val, vaRangeTicks, currentPhase, currentBar, history)
    SessionStatistics stats = CalculateSessionStats(zm, accum, 5000.0, 5010.0, 4990.0, 8, CurrentPhase::ROTATION, 5, history);

    // Verify PULLBACK is counted
    assert(stats.pullbackBars == 2);
    std::cout << "  pullbackBars == 2 [PASS]" << std::endl;

    // Verify other buckets
    assert(stats.rotationBars == 2);
    assert(stats.drivingBars == 1);
    assert(stats.totalBars == 5);
    std::cout << "  Other buckets correct [PASS]" << std::endl;
}

void TestUnknownPhaseIsCounted() {
    std::cout << "\nTesting unknown phase values are counted..." << std::endl;

    // Simulate a future enum value by casting an int
    std::vector<CurrentPhase> history;
    history.push_back(CurrentPhase::ROTATION);
    history.push_back(static_cast<CurrentPhase>(99));  // Unknown future value
    history.push_back(CurrentPhase::ROTATION);

    ZoneManager zm;
    zm.config = ZoneConfig();
    SessionEngagementAccumulator accum;

    SessionStatistics stats = CalculateSessionStats(zm, accum, 5000.0, 5010.0, 4990.0, 8, CurrentPhase::ROTATION, 3, history);

    assert(stats.unknownBars == 1);
    assert(stats.rotationBars == 2);
    assert(stats.totalBars == 3);
    std::cout << "  unknownBars == 1 [PASS]" << std::endl;
}

// ============================================================================
// TEST 2: BUCKET-SUM INVARIANT
// Sum of all phase buckets must equal totalBars
// ============================================================================

void TestInvariantHolds() {
    std::cout << "\nTesting bucket-sum invariant holds..." << std::endl;

    std::vector<CurrentPhase> history;
    // Add variety of phases
    for (int i = 0; i < 10; i++) history.push_back(CurrentPhase::ROTATION);
    for (int i = 0; i < 5; i++) history.push_back(CurrentPhase::DRIVING_UP);
    for (int i = 0; i < 3; i++) history.push_back(CurrentPhase::PULLBACK);
    for (int i = 0; i < 7; i++) history.push_back(CurrentPhase::TESTING_BOUNDARY);
    for (int i = 0; i < 4; i++) history.push_back(CurrentPhase::RANGE_EXTENSION);
    for (int i = 0; i < 2; i++) history.push_back(CurrentPhase::FAILED_AUCTION);

    ZoneManager zm;
    zm.config = ZoneConfig();
    SessionEngagementAccumulator accum;

    SessionStatistics stats = CalculateSessionStats(zm, accum, 5000.0, 5010.0, 4990.0, 8, CurrentPhase::ROTATION, 31, history);

    // Verify invariant
    assert(stats.CheckInvariant() == true);
    assert(stats.GetBucketSum() == stats.totalBars);
    assert(stats.GetInvariantViolation().empty());
    std::cout << "  CheckInvariant() == true [PASS]" << std::endl;
    std::cout << "  GetBucketSum() == totalBars (31) [PASS]" << std::endl;

    // Verify individual counts
    assert(stats.rotationBars == 10);
    assert(stats.drivingBars == 5);
    assert(stats.pullbackBars == 3);
    assert(stats.testingBars == 7);
    assert(stats.extensionBars == 4);
    assert(stats.failedAuctionBars == 2);
    assert(stats.unknownBars == 0);
    std::cout << "  All bucket counts correct [PASS]" << std::endl;
}

void TestInvariantViolationDetected() {
    std::cout << "\nTesting invariant violation is detected..." << std::endl;

    // Manually create a stats object with broken invariant
    SessionStatistics stats;
    stats.rotationBars = 10;
    stats.drivingBars = 5;
    stats.totalBars = 20;  // Should be 15!

    assert(stats.CheckInvariant() == false);
    assert(stats.GetBucketSum() == 15);
    assert(!stats.GetInvariantViolation().empty());
    std::cout << "  Violation detected: " << stats.GetInvariantViolation() << " [PASS]" << std::endl;
}

// ============================================================================
// TEST 3: MARKETSTATE MINIMUM SAMPLE SIZE
// Must return UNDEFINED when totalBars < MIN_SAMPLE_SIZE (30)
// ============================================================================

void TestMarketStateMinSampleSize() {
    std::cout << "\nTesting MarketState minimum sample size..." << std::endl;

    SessionStatistics stats;

    // Below minimum (29 bars)
    stats.totalBars = 29;
    stats.rotationBars = 29;  // 100% rotation - would be BALANCE if not for guard
    assert(stats.HasSufficientSample() == false);
    assert(stats.GetMarketState() == AMTMarketState::UNKNOWN);
    std::cout << "  29 bars: UNDEFINED (insufficient sample) [PASS]" << std::endl;

    // At minimum (30 bars)
    stats.totalBars = 30;
    stats.rotationBars = 30;  // 100% rotation
    assert(stats.HasSufficientSample() == true);
    assert(stats.GetMarketState() == AMTMarketState::BALANCE);
    std::cout << "  30 bars: BALANCE (sufficient sample) [PASS]" << std::endl;

    // Above minimum with imbalance
    stats.totalBars = 100;
    stats.rotationBars = 50;  // 50% rotation
    assert(stats.GetMarketState() == AMTMarketState::IMBALANCE);
    std::cout << "  100 bars, 50% rotation: IMBALANCE [PASS]" << std::endl;
}

void TestMarketStateThreshold() {
    std::cout << "\nTesting MarketState 60% threshold..." << std::endl;

    SessionStatistics stats;
    stats.totalBars = 100;

    // Exactly 60% rotation - should be IMBALANCE (> 60 required)
    stats.rotationBars = 60;
    assert(stats.GetMarketState() == AMTMarketState::IMBALANCE);
    std::cout << "  60% rotation: IMBALANCE (boundary) [PASS]" << std::endl;

    // 61% rotation - should be BALANCE
    stats.rotationBars = 61;
    assert(stats.GetMarketState() == AMTMarketState::BALANCE);
    std::cout << "  61% rotation: BALANCE [PASS]" << std::endl;
}

// ============================================================================
// TEST 4: MARKETSTATE HYSTERESIS TRACKER
// State changes require 5 consecutive bars of the new state
// ============================================================================

void TestMarketStateHysteresis() {
    std::cout << "\nTesting MarketState hysteresis..." << std::endl;

    // Use MarketStateBucket directly for hysteresis testing
    MarketStateBucket bucket;
    bucket.minConfirmationBars = 5;

    // Start unknown
    assert(bucket.confirmedState == AMTMarketState::UNKNOWN);

    // First valid state promotes immediately
    AMTMarketState r1 = bucket.Update(AMTMarketState::BALANCE);
    assert(r1 == AMTMarketState::BALANCE);
    assert(bucket.confirmedState == AMTMarketState::BALANCE);
    std::cout << "  First BALANCE: Immediate promotion [PASS]" << std::endl;

    // One bar of IMBALANCE (not enough)
    AMTMarketState r2 = bucket.Update(AMTMarketState::IMBALANCE);
    assert(r2 == AMTMarketState::BALANCE);  // Still BALANCE
    std::cout << "  1 bar IMBALANCE: Still BALANCE [PASS]" << std::endl;

    // 4 more bars of IMBALANCE (total 5, now confirmed)
    bucket.Update(AMTMarketState::IMBALANCE);
    bucket.Update(AMTMarketState::IMBALANCE);
    bucket.Update(AMTMarketState::IMBALANCE);
    AMTMarketState r6 = bucket.Update(AMTMarketState::IMBALANCE);
    assert(r6 == AMTMarketState::IMBALANCE);  // Now promoted
    std::cout << "  5 bars IMBALANCE: Promoted to IMBALANCE [PASS]" << std::endl;

    // One bar of BALANCE flicker (rejected)
    AMTMarketState r7 = bucket.Update(AMTMarketState::BALANCE);
    assert(r7 == AMTMarketState::IMBALANCE);  // Stay IMBALANCE
    std::cout << "  1 bar BALANCE flicker: Stay IMBALANCE [PASS]" << std::endl;
}

void TestMarketStateUnknownPropagates() {
    std::cout << "\nTesting UNKNOWN propagates immediately..." << std::endl;

    MarketStateBucket bucket;

    // Get to BALANCE
    bucket.Update(AMTMarketState::BALANCE);
    assert(bucket.confirmedState == AMTMarketState::BALANCE);

    // Build up partial confirmation for IMBALANCE
    bucket.Update(AMTMarketState::IMBALANCE);
    bucket.Update(AMTMarketState::IMBALANCE);
    assert(bucket.candidateBars == 2);

    // UNKNOWN interrupts everything
    AMTMarketState r = bucket.Update(AMTMarketState::UNKNOWN);
    assert(r == AMTMarketState::UNKNOWN);
    assert(bucket.confirmedState == AMTMarketState::UNKNOWN);
    assert(bucket.candidateBars == 0);
    std::cout << "  UNKNOWN propagates immediately [PASS]" << std::endl;
}

void TestMarketStateTransitioning() {
    std::cout << "\nTesting IsTransitioning() detection..." << std::endl;

    MarketStateBucket bucket;
    bucket.Update(AMTMarketState::BALANCE);

    assert(bucket.IsTransitioning() == false);

    bucket.Update(AMTMarketState::IMBALANCE);
    assert(bucket.IsTransitioning() == true);
    std::cout << "  Transitioning detected [PASS]" << std::endl;

    // Complete the transition
    for (int i = 0; i < 4; i++) {
        bucket.Update(AMTMarketState::IMBALANCE);
    }
    assert(bucket.IsTransitioning() == false);
    std::cout << "  Transition complete [PASS]" << std::endl;
}

// ============================================================================
// TEST 5: PERCENTAGE CALCULATIONS
// Verify GetPhasePercent works correctly
// ============================================================================

void TestPhasePercentages() {
    std::cout << "\nTesting phase percentage calculations..." << std::endl;

    SessionStatistics stats;
    stats.totalBars = 100;
    stats.rotationBars = 45;
    stats.pullbackBars = 10;
    stats.drivingBars = 15;
    stats.extensionBars = 10;
    stats.failedAuctionBars = 5;
    stats.testingBars = 15;
    stats.unknownBars = 0;

    assert(stats.CheckInvariant());

    assert(stats.GetPhasePercent(stats.rotationBars) == 45.0);
    assert(stats.GetPhasePercent(stats.pullbackBars) == 10.0);
    assert(stats.GetRotationPercent() == 45.0);
    std::cout << "  Percentages calculated correctly [PASS]" << std::endl;

    // Edge case: zero total bars
    SessionStatistics empty;
    assert(empty.GetPhasePercent(0) == 0.0);
    assert(empty.GetPhasePercent(10) == 0.0);  // Division by zero guarded
    std::cout << "  Zero totalBars handled [PASS]" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=== AMT Analytics Unit Tests ===" << std::endl;
    std::cout << "Tests bucket-sum invariant, PULLBACK counting, MarketState guardrails\n" << std::endl;

    // PULLBACK counting tests
    TestPullbackIsCounted();
    TestUnknownPhaseIsCounted();

    // Invariant tests
    TestInvariantHolds();
    TestInvariantViolationDetected();

    // MarketState guardrail tests
    TestMarketStateMinSampleSize();
    TestMarketStateThreshold();
    TestMarketStateHysteresis();
    TestMarketStateUnknownPropagates();
    TestMarketStateTransitioning();

    // Percentage calculation tests
    TestPhasePercentages();

    std::cout << "\n=== All tests passed! ===" << std::endl;
    return 0;
}
