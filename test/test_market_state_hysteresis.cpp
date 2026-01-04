// ============================================================================
// test_market_state_hysteresis.cpp
// Unit tests for P0 fix: MarketState hysteresis integration
// Tests: flicker prevention, confirmed transitions, mode stability
// ============================================================================

#include "test_sierrachart_mock.h"
#include "AMT_Analytics.h"
#include "amt_core.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace AMT;

// ============================================================================
// MOCK: Simulate live detection path inputs
// ============================================================================

struct MockPhaseSnapshot {
    CurrentPhase phase = CurrentPhase::ROTATION;

    bool IsDirectional() const {
        return phase == CurrentPhase::DRIVING_UP ||
               phase == CurrentPhase::DRIVING_DOWN ||
               phase == CurrentPhase::RANGE_EXTENSION ||
               phase == CurrentPhase::FAILED_AUCTION;
    }
};

// Simulate the live detection logic from AuctionSensor_v1.cpp:6729-6736
AMTMarketState ComputeRawState(const MockPhaseSnapshot& snapshot, double deltaConsistency) {
    bool isTrending = snapshot.IsDirectional();
    bool isExtremeDelta = (deltaConsistency > 0.7 || deltaConsistency < 0.3);

    return (isTrending || isExtremeDelta)
        ? AMTMarketState::IMBALANCE
        : AMTMarketState::BALANCE;
}

// ============================================================================
// TEST 1: FLICKER PREVENTION
// Raw state oscillates but confirmed state stays stable
// ============================================================================

void TestFlickerPrevention() {
    std::cout << "Testing flicker prevention..." << std::endl;

    MarketStateBucket tracker;
    MockPhaseSnapshot snapshot;
    snapshot.phase = CurrentPhase::ROTATION;  // Not directional

    int marketStateChangeCount = 0;
    AMTMarketState priorConfirmed = AMTMarketState::UNKNOWN;

    // Simulate 20 bars of oscillating delta (flicker scenario)
    std::vector<double> deltaSequence = {
        0.5, 0.75, 0.5, 0.8, 0.5, 0.72, 0.5, 0.71, 0.5, 0.73,
        0.5, 0.69, 0.5, 0.68, 0.5, 0.65, 0.5, 0.55, 0.5, 0.5
    };

    int rawFlips = 0;
    AMTMarketState lastRaw = AMTMarketState::UNKNOWN;

    for (size_t i = 0; i < deltaSequence.size(); i++) {
        AMTMarketState rawState = ComputeRawState(snapshot, deltaSequence[i]);
        AMTMarketState confirmedState = tracker.Update(rawState);

        // Count raw flips
        if (lastRaw != AMTMarketState::UNKNOWN && rawState != lastRaw) {
            rawFlips++;
        }
        lastRaw = rawState;

        // Count confirmed transitions (the metric we care about)
        if (confirmedState != priorConfirmed && priorConfirmed != AMTMarketState::UNKNOWN) {
            marketStateChangeCount++;
        }
        priorConfirmed = confirmedState;
    }

    std::cout << "  Raw flips: " << rawFlips << std::endl;
    std::cout << "  Confirmed transitions: " << marketStateChangeCount << std::endl;

    // Key assertion: confirmed transitions should be much fewer than raw flips
    assert(rawFlips >= 8);  // There were many raw oscillations
    assert(marketStateChangeCount <= 1);  // But confirmed state was stable

    std::cout << "  Flicker suppressed: " << rawFlips << " raw -> "
              << marketStateChangeCount << " confirmed [PASS]" << std::endl;
}

// ============================================================================
// TEST 2: LEGITIMATE TRANSITION DETECTION
// 5 consecutive bars of new state should cause transition
// ============================================================================

void TestLegitimateTransition() {
    std::cout << "\nTesting legitimate transition detection..." << std::endl;

    MarketStateBucket tracker;
    MockPhaseSnapshot snapshot;

    // Start in ROTATION (BALANCE)
    snapshot.phase = CurrentPhase::ROTATION;
    AMTMarketState s1 = tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(s1 == AMTMarketState::BALANCE);
    std::cout << "  Initial state: BALANCE [PASS]" << std::endl;

    // Transition to DRIVING_UP (IMBALANCE) - need 5 consecutive bars
    snapshot.phase = CurrentPhase::DRIVING_UP;

    AMTMarketState s2 = tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(s2 == AMTMarketState::BALANCE);  // Still BALANCE (1 bar)

    AMTMarketState s3 = tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(s3 == AMTMarketState::BALANCE);  // Still BALANCE (2 bars)

    AMTMarketState s4 = tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(s4 == AMTMarketState::BALANCE);  // Still BALANCE (3 bars)

    AMTMarketState s5 = tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(s5 == AMTMarketState::BALANCE);  // Still BALANCE (4 bars)

    AMTMarketState s6 = tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(s6 == AMTMarketState::IMBALANCE);  // NOW IMBALANCE (5 bars)

    std::cout << "  Transition after 5 bars: IMBALANCE [PASS]" << std::endl;
}

// ============================================================================
// TEST 3: TRANSITION INTERRUPTED BY NOISE
// Partial transition resets if noise interrupts
// ============================================================================

void TestTransitionInterrupted() {
    std::cout << "\nTesting transition interrupted by noise..." << std::endl;

    MarketStateBucket tracker;
    MockPhaseSnapshot snapshot;

    // Start in BALANCE
    snapshot.phase = CurrentPhase::ROTATION;
    tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(tracker.confirmedState == AMTMarketState::BALANCE);

    // Start transitioning to IMBALANCE
    snapshot.phase = CurrentPhase::DRIVING_UP;
    tracker.Update(ComputeRawState(snapshot, 0.5));  // 1
    tracker.Update(ComputeRawState(snapshot, 0.5));  // 2
    tracker.Update(ComputeRawState(snapshot, 0.5));  // 3

    assert(tracker.IsTransitioning() == true);
    assert(tracker.candidateBars == 3);
    std::cout << "  Building transition: 3 bars [PASS]" << std::endl;

    // Noise interrupts - single bar of BALANCE resets candidate
    snapshot.phase = CurrentPhase::ROTATION;
    tracker.Update(ComputeRawState(snapshot, 0.5));

    assert(tracker.confirmedState == AMTMarketState::BALANCE);
    assert(tracker.candidateBars == 0);  // Reset
    std::cout << "  Transition interrupted, counter reset [PASS]" << std::endl;
}

// ============================================================================
// TEST 4: DELTA-DRIVEN IMBALANCE
// Extreme delta alone triggers IMBALANCE (without directional phase)
// ============================================================================

void TestDeltaDrivenImbalance() {
    std::cout << "\nTesting delta-driven IMBALANCE..." << std::endl;

    MarketStateBucket tracker;
    MockPhaseSnapshot snapshot;
    snapshot.phase = CurrentPhase::ROTATION;  // Not directional

    // Establish BALANCE first
    tracker.Update(ComputeRawState(snapshot, 0.5));
    assert(tracker.confirmedState == AMTMarketState::BALANCE);

    // 5 bars of extreme delta (> 0.7) should trigger IMBALANCE
    for (int i = 0; i < 5; i++) {
        tracker.Update(ComputeRawState(snapshot, 0.85));  // Extreme delta
    }

    assert(tracker.confirmedState == AMTMarketState::IMBALANCE);
    std::cout << "  Extreme delta (0.85) for 5 bars -> IMBALANCE [PASS]" << std::endl;

    // Reset and test low delta
    tracker.Reset();
    tracker.Update(ComputeRawState(snapshot, 0.5));  // BALANCE first

    for (int i = 0; i < 5; i++) {
        tracker.Update(ComputeRawState(snapshot, 0.15));  // Low extreme delta
    }

    assert(tracker.confirmedState == AMTMarketState::IMBALANCE);
    std::cout << "  Extreme delta (0.15) for 5 bars -> IMBALANCE [PASS]" << std::endl;
}

// ============================================================================
// TEST 5: COUNT INTEGRITY
// marketStateChangeCount only increments on confirmed transitions
// ============================================================================

void TestCountIntegrity() {
    std::cout << "\nTesting count integrity..." << std::endl;

    MarketStateBucket tracker;
    MockPhaseSnapshot snapshot;

    int marketStateChangeCount = 0;

    // Simulate the exact counting logic from AuctionSensor_v1.cpp
    auto simulateBar = [&](CurrentPhase phase, double delta) {
        snapshot.phase = phase;
        AMTMarketState rawState = ComputeRawState(snapshot, delta);
        AMTMarketState priorConfirmed = tracker.confirmedState;
        AMTMarketState confirmedState = tracker.Update(rawState);

        // Count only BALANCE <-> IMBALANCE transitions (not to/from UNDEFINED)
        if (confirmedState != priorConfirmed &&
            priorConfirmed != AMTMarketState::UNKNOWN &&
            confirmedState != AMTMarketState::UNKNOWN) {
            marketStateChangeCount++;
        }
    };

    // Initial bar (UNDEFINED -> BALANCE, doesn't count)
    simulateBar(CurrentPhase::ROTATION, 0.5);
    assert(marketStateChangeCount == 0);

    // Flicker bars (no confirmed change)
    for (int i = 0; i < 10; i++) {
        simulateBar(i % 2 == 0 ? CurrentPhase::DRIVING_UP : CurrentPhase::ROTATION, 0.5);
    }
    assert(marketStateChangeCount == 0);
    std::cout << "  10 flicker bars: 0 transitions [PASS]" << std::endl;

    // Legitimate transition (5 consecutive DRIVING_UP)
    for (int i = 0; i < 5; i++) {
        simulateBar(CurrentPhase::DRIVING_UP, 0.5);
    }
    assert(marketStateChangeCount == 1);
    std::cout << "  5 consecutive DRIVING_UP: 1 transition [PASS]" << std::endl;

    // Another flicker burst (no change)
    for (int i = 0; i < 8; i++) {
        simulateBar(i % 2 == 0 ? CurrentPhase::ROTATION : CurrentPhase::DRIVING_UP, 0.5);
    }
    assert(marketStateChangeCount == 1);  // Still 1
    std::cout << "  8 more flicker bars: still 1 transition [PASS]" << std::endl;

    // Transition back to BALANCE
    for (int i = 0; i < 5; i++) {
        simulateBar(CurrentPhase::ROTATION, 0.5);
    }
    assert(marketStateChangeCount == 2);
    std::cout << "  5 consecutive ROTATION: 2 transitions total [PASS]" << std::endl;
}

// ============================================================================
// TEST 6: UNDEFINED HANDLING
// UNDEFINED immediately propagates and resets counters
// ============================================================================

void TestUndefinedHandling() {
    std::cout << "\nTesting UNDEFINED handling..." << std::endl;

    MarketStateBucket tracker;

    // Establish BALANCE
    tracker.Update(AMTMarketState::BALANCE);
    assert(tracker.confirmedState == AMTMarketState::BALANCE);

    // Build partial transition
    tracker.Update(AMTMarketState::IMBALANCE);
    tracker.Update(AMTMarketState::IMBALANCE);
    assert(tracker.candidateBars == 2);

    // UNDEFINED interrupts everything
    AMTMarketState result = tracker.Update(AMTMarketState::UNKNOWN);
    assert(result == AMTMarketState::UNKNOWN);
    assert(tracker.confirmedState == AMTMarketState::UNKNOWN);
    assert(tracker.candidateBars == 0);

    std::cout << "  UNDEFINED propagates immediately [PASS]" << std::endl;
}

// ============================================================================
// TEST 7: MODE STABILITY UNDER FLICKER
// AuctionMode should stay stable when raw state flickers
// ============================================================================

// Mock AuctionContextModule behavior
enum class MockAuctionMode { ROTATIONAL, DIRECTIONAL, LOCKED };

MockAuctionMode DetermineMode(AMTMarketState state) {
    if (state == AMTMarketState::UNKNOWN) return MockAuctionMode::LOCKED;
    return (state == AMTMarketState::BALANCE)
        ? MockAuctionMode::ROTATIONAL
        : MockAuctionMode::DIRECTIONAL;
}

void TestModeStabilityUnderFlicker() {
    std::cout << "\nTesting mode stability under flicker..." << std::endl;

    MarketStateBucket tracker;
    MockPhaseSnapshot snapshot;

    // Track mode changes
    std::vector<MockAuctionMode> modeHistory;

    // Simulate flicker scenario with hysteresis
    std::vector<CurrentPhase> phaseSequence = {
        CurrentPhase::ROTATION,   // BALANCE
        CurrentPhase::DRIVING_UP,   // IMBALANCE
        CurrentPhase::ROTATION,   // BALANCE
        CurrentPhase::DRIVING_UP,   // IMBALANCE
        CurrentPhase::ROTATION,   // BALANCE
        CurrentPhase::DRIVING_UP,   // IMBALANCE
        CurrentPhase::ROTATION,   // BALANCE
        CurrentPhase::ROTATION,   // BALANCE
        CurrentPhase::ROTATION,   // BALANCE
        CurrentPhase::ROTATION,   // BALANCE
    };

    for (auto phase : phaseSequence) {
        snapshot.phase = phase;
        AMTMarketState rawState = ComputeRawState(snapshot, 0.5);
        AMTMarketState confirmedState = tracker.Update(rawState);
        MockAuctionMode mode = DetermineMode(confirmedState);
        modeHistory.push_back(mode);
    }

    // Count mode changes
    int modeChanges = 0;
    for (size_t i = 1; i < modeHistory.size(); i++) {
        if (modeHistory[i] != modeHistory[i-1]) {
            modeChanges++;
        }
    }

    std::cout << "  Mode changes: " << modeChanges << std::endl;
    assert(modeChanges == 0);  // Mode should stay ROTATIONAL throughout
    assert(modeHistory.back() == MockAuctionMode::ROTATIONAL);

    std::cout << "  Mode stayed ROTATIONAL despite flicker [PASS]" << std::endl;
}

// ============================================================================
// TEST 8: CONFIRMATION PROGRESS VISIBILITY
// GetConfirmationProgress() should track partial transitions
// ============================================================================

void TestConfirmationProgress() {
    std::cout << "\nTesting confirmation progress visibility..." << std::endl;

    MarketStateBucket tracker;

    // Establish BALANCE
    tracker.Update(AMTMarketState::BALANCE);
    assert(tracker.GetConfirmationProgress() == 0.0);

    // Start transitioning
    tracker.Update(AMTMarketState::IMBALANCE);
    assert(tracker.GetConfirmationProgress() == 0.2);  // 1/5
    std::cout << "  1 bar: progress = 20% [PASS]" << std::endl;

    tracker.Update(AMTMarketState::IMBALANCE);
    assert(tracker.GetConfirmationProgress() == 0.4);  // 2/5
    std::cout << "  2 bars: progress = 40% [PASS]" << std::endl;

    tracker.Update(AMTMarketState::IMBALANCE);
    assert(tracker.GetConfirmationProgress() == 0.6);  // 3/5

    tracker.Update(AMTMarketState::IMBALANCE);
    assert(tracker.GetConfirmationProgress() == 0.8);  // 4/5

    tracker.Update(AMTMarketState::IMBALANCE);
    // After promotion, progress resets
    assert(tracker.confirmedState == AMTMarketState::IMBALANCE);
    assert(tracker.GetConfirmationProgress() == 0.0);
    std::cout << "  5 bars: promoted, progress = 0% [PASS]" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=== MarketState Hysteresis Integration Tests (P0 Fix) ===" << std::endl;
    std::cout << "Tests flicker prevention, confirmed transitions, mode stability\n" << std::endl;

    TestFlickerPrevention();
    TestLegitimateTransition();
    TestTransitionInterrupted();
    TestDeltaDrivenImbalance();
    TestCountIntegrity();
    TestUndefinedHandling();
    TestModeStabilityUnderFlicker();
    TestConfirmationProgress();

    std::cout << "\n=== All P0 hysteresis tests passed! ===" << std::endl;
    return 0;
}
