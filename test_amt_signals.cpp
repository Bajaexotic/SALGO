// ============================================================================
// test_amt_signals.cpp
// Unit tests for AMT Signal Processing (AMT_Signals.h)
// ============================================================================

#include <iostream>
#include <cmath>
#include <cassert>
#include <vector>

// Include the header under test (from Sierra Chart ACS_Source)
#include "amt_core.h"
#include "AMT_Signals.h"

using namespace AMT;

// Test utilities
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
        g_testsFailed++; \
    } else { \
        std::cout << "[PASS] " << msg << "\n"; \
        g_testsPassed++; \
    } \
} while(0)

#define TEST_SECTION(name) std::cout << "\n=== " << name << " ===\n"

// ============================================================================
// ACTIVITY CLASSIFIER TESTS
// ============================================================================

void test_activity_classifier_intent() {
    TEST_SECTION("ActivityClassifier - Intent Detection");

    ActivityClassifier classifier;
    const double tickSize = 0.25;  // ES tick size
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;

    // Test 1: Moving toward POC (was above, now closer)
    {
        double prevPrice = 6108.00;  // 32 ticks from POC
        double price = 6105.00;      // 20 ticks from POC (closer)
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, 0.0, tickSize);
        TEST_ASSERT(result.valid, "Result should be valid");
        TEST_ASSERT(result.intent == ValueIntent::TOWARD_VALUE,
            "Moving from 6108 to 6105 should be TOWARD_VALUE (closer to POC at 6100)");
    }

    // Test 2: Moving away from POC (was close, now farther)
    {
        double prevPrice = 6102.00;  // 8 ticks from POC
        double price = 6108.00;      // 32 ticks from POC (farther)
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, 0.0, tickSize);
        TEST_ASSERT(result.intent == ValueIntent::AWAY_FROM_VALUE,
            "Moving from 6102 to 6108 should be AWAY_FROM_VALUE (farther from POC)");
    }

    // Test 3: At POC (within tolerance)
    {
        double price = 6100.25;  // 1 tick from POC
        double prevPrice = 6100.00;
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, 0.0, tickSize);
        TEST_ASSERT(result.intent == ValueIntent::AT_VALUE,
            "Price at 6100.25 (1 tick from POC) should be AT_VALUE");
    }
}

void test_activity_classifier_participation() {
    TEST_SECTION("ActivityClassifier - Participation Mode");

    ActivityClassifier classifier;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;

    // Test 1: Price up + positive delta = AGGRESSIVE
    {
        double prevPrice = 6100.00;
        double price = 6105.00;  // Price went up
        double deltaPct = 0.30;  // Positive delta (buyers winning)
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.participation == ParticipationMode::AGGRESSIVE,
            "Price up + positive delta should be AGGRESSIVE");
    }

    // Test 2: Price up + negative delta = ABSORPTIVE
    {
        double prevPrice = 6100.00;
        double price = 6105.00;  // Price went up
        double deltaPct = -0.30; // Negative delta (sellers absorbing)
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.participation == ParticipationMode::ABSORPTIVE,
            "Price up + negative delta should be ABSORPTIVE (absorption)");
    }

    // Test 3: Price down + negative delta = AGGRESSIVE
    {
        double prevPrice = 6105.00;
        double price = 6100.00;  // Price went down
        double deltaPct = -0.30; // Negative delta (sellers winning)
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.participation == ParticipationMode::AGGRESSIVE,
            "Price down + negative delta should be AGGRESSIVE");
    }

    // Test 4: Neutral delta = BALANCED
    {
        double prevPrice = 6100.00;
        double price = 6105.00;
        double deltaPct = 0.05;  // Near-neutral delta
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.participation == ParticipationMode::BALANCED,
            "Neutral delta should be BALANCED");
    }
}

void test_activity_classifier_activity_type() {
    TEST_SECTION("ActivityClassifier - Activity Type Derivation");

    ActivityClassifier classifier;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;

    // Test 1: Away + Aggressive = INITIATIVE
    {
        // Moving away from POC with aggressive participation
        double prevPrice = 6102.00;
        double price = 6115.00;    // Outside VAH, moving away
        double deltaPct = 0.40;    // Strong positive delta
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.activityType == AMTActivityType::INITIATIVE,
            "Away from value + aggressive should be INITIATIVE");
    }

    // Test 2: Toward value = RESPONSIVE (regardless of participation)
    {
        // Moving toward POC
        double prevPrice = 6115.00;  // Was far from POC
        double price = 6105.00;      // Now closer to POC
        double deltaPct = 0.40;      // Even with aggressive delta
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.activityType == AMTActivityType::RESPONSIVE,
            "Toward value should be RESPONSIVE (even with aggressive delta)");
    }

    // Test 3: Away + Absorptive = RESPONSIVE
    {
        // Moving away but with absorption
        double prevPrice = 6102.00;
        double price = 6115.00;    // Moving away
        double deltaPct = -0.40;   // Negative delta = absorption on up move
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.activityType == AMTActivityType::RESPONSIVE,
            "Away + absorptive should be RESPONSIVE");
    }

    // Test 4: At POC + balanced = NEUTRAL
    {
        double prevPrice = 6100.25;
        double price = 6100.00;    // At POC
        double deltaPct = 0.05;    // Neutral
        auto result = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        TEST_ASSERT(result.activityType == AMTActivityType::NEUTRAL,
            "At value + balanced should be NEUTRAL");
    }
}

void test_activity_classifier_location() {
    TEST_SECTION("ActivityClassifier - Location Detection");

    ActivityClassifier classifier;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;

    // Test locations
    auto r1 = classifier.Classify(6100.25, 6100.00, poc, vah, val, 0.0, tickSize);
    TEST_ASSERT(r1.location == ValueLocation::AT_POC, "6100.25 should be AT_POC");

    auto r2 = classifier.Classify(6110.25, 6110.00, poc, vah, val, 0.0, tickSize);
    TEST_ASSERT(r2.location == ValueLocation::AT_VAH, "6110.25 should be AT_VAH");

    auto r3 = classifier.Classify(6089.75, 6090.00, poc, vah, val, 0.0, tickSize);
    TEST_ASSERT(r3.location == ValueLocation::AT_VAL, "6089.75 should be AT_VAL");

    auto r4 = classifier.Classify(6120.00, 6115.00, poc, vah, val, 0.0, tickSize);
    TEST_ASSERT(r4.location == ValueLocation::ABOVE_VALUE, "6120 should be ABOVE_VALUE");

    auto r5 = classifier.Classify(6080.00, 6085.00, poc, vah, val, 0.0, tickSize);
    TEST_ASSERT(r5.location == ValueLocation::BELOW_VALUE, "6080 should be BELOW_VALUE");

    auto r6 = classifier.Classify(6105.00, 6103.00, poc, vah, val, 0.0, tickSize);
    TEST_ASSERT(r6.location == ValueLocation::INSIDE_VALUE, "6105 (between VAL and VAH) should be INSIDE_VALUE");
}

// ============================================================================
// AMT STATE TRACKER TESTS
// ============================================================================

void test_state_tracker_basic() {
    TEST_SECTION("AMTStateTracker - Basic State Transitions");

    AMTStateTracker tracker;
    ActivityClassifier classifier;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;

    // Initial state should be UNKNOWN
    TEST_ASSERT(tracker.GetCurrentState() == AMTMarketState::UNKNOWN,
        "Initial state should be UNKNOWN");

    // Feed initiative bars (away from value + aggressive)
    // This should drive strength up toward IMBALANCE threshold
    double price = 6115.00;
    double prevPrice = 6105.00;
    double deltaPct = 0.40;

    for (int i = 0; i < 10; ++i) {
        auto activity = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        auto evidence = tracker.Update(activity, i);
        prevPrice = price;
        price += 2.0;  // Keep moving away
    }

    // After sustained initiative, should be in IMBALANCE
    TEST_ASSERT(tracker.GetCurrentState() == AMTMarketState::IMBALANCE,
        "After sustained initiative bars, state should be IMBALANCE");

    // Now feed responsive bars (toward value)
    price = 6130.00;  // Start far
    for (int i = 10; i < 25; ++i) {
        prevPrice = price;
        price -= 3.0;  // Moving toward POC
        auto activity = classifier.Classify(price, prevPrice, poc, vah, val, 0.05, tickSize);
        auto evidence = tracker.Update(activity, i);
    }

    // After sustained responsive activity, should flip back to BALANCE
    TEST_ASSERT(tracker.GetCurrentState() == AMTMarketState::BALANCE,
        "After sustained responsive bars, state should return to BALANCE");
}

void test_state_tracker_strength_decay() {
    TEST_SECTION("AMTStateTracker - Strength Decay");

    AMTStateTracker tracker;
    ActivityClassifier classifier;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;

    // Feed initiative bars to build strength
    double price = 6115.00;
    double prevPrice = 6105.00;
    double deltaPct = 0.40;

    for (int i = 0; i < 5; ++i) {
        auto activity = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        tracker.Update(activity, i);
        prevPrice = price;
        price += 2.0;
    }

    double strengthAfterInitiative = tracker.GetStrength();
    TEST_ASSERT(strengthAfterInitiative > 0.5,
        "Strength should be above 0.5 after initiative bars");

    // Now feed neutral bars (at POC, balanced)
    price = 6100.00;
    prevPrice = 6100.25;
    deltaPct = 0.0;

    for (int i = 5; i < 20; ++i) {
        auto activity = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        tracker.Update(activity, i);
    }

    double strengthAfterDecay = tracker.GetStrength();
    TEST_ASSERT(strengthAfterDecay < strengthAfterInitiative,
        "Strength should decay over neutral bars");
}

void test_state_tracker_transitions() {
    TEST_SECTION("AMTStateTracker - Transition Detection");

    AMTStateTracker tracker;
    ActivityClassifier classifier;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;

    bool sawTransitionToImbalance = false;
    bool sawImbalanceToBalance = false;

    // Feed initiative bars
    double price = 6115.00;
    double prevPrice = 6105.00;

    for (int i = 0; i < 30; ++i) {
        double deltaPct = (i < 15) ? 0.40 : -0.05;  // First half initiative, second half responsive

        if (i >= 15) {
            prevPrice = price;
            price -= 2.0;  // Moving back toward POC
        } else {
            prevPrice = price;
            price += 2.0;  // Moving away
        }

        auto activity = classifier.Classify(price, prevPrice, poc, vah, val, deltaPct, tickSize);
        auto evidence = tracker.Update(activity, i);

        if (evidence.IsTransition()) {
            // Note: First transition is UNKNOWN->IMBALANCE (not BALANCE->IMBALANCE)
            // because we start at UNKNOWN and go directly to IMBALANCE
            if (evidence.currentState == AMTMarketState::IMBALANCE) {
                sawTransitionToImbalance = true;
            }
            if (evidence.previousState == AMTMarketState::IMBALANCE &&
                evidence.currentState == AMTMarketState::BALANCE) {
                sawImbalanceToBalance = true;
            }
        }
    }

    // We see transition TO IMBALANCE (from UNKNOWN, since we never passed through BALANCE first)
    // The IsTransition() returns false when previousState is UNKNOWN, so we check the state change differently
    TEST_ASSERT(tracker.GetCurrentState() == AMTMarketState::BALANCE,
        "Final state should be BALANCE after responsive bars");
    TEST_ASSERT(sawImbalanceToBalance,
        "Should have seen IMBALANCE->BALANCE transition");
}

// ============================================================================
// SINGLE PRINT DETECTOR TESTS
// ============================================================================

void test_single_print_detector() {
    TEST_SECTION("SinglePrintDetector - Basic Detection");

    SinglePrintDetector detector;
    const double tickSize = 0.25;
    const double priceStart = 6090.00;
    const int numLevels = 100;  // 100 ticks = 25 points
    const double avgVolume = 1000.0;

    // Create volume data with a single print zone
    std::vector<double> volumeData(numLevels);

    // Fill with average volume
    for (int i = 0; i < numLevels; ++i) {
        volumeData[i] = avgVolume;
    }

    // Create a thin zone (single print) from tick 40-47 (8 ticks)
    for (int i = 40; i < 48; ++i) {
        volumeData[i] = avgVolume * 0.05;  // Very thin
    }

    auto zones = detector.DetectFromProfile(
        volumeData.data(), priceStart, tickSize, numLevels, avgVolume, 100);

    TEST_ASSERT(zones.size() == 1, "Should detect exactly one single print zone");

    if (!zones.empty()) {
        TEST_ASSERT(zones[0].widthTicks >= 3, "Single print zone should be at least 3 ticks wide");
        TEST_ASSERT(zones[0].valid, "Single print zone should be valid");

        // Check price range
        double expectedLow = priceStart + 40 * tickSize;   // 6100.00
        double expectedHigh = priceStart + 47 * tickSize;  // 6101.75

        TEST_ASSERT(std::abs(zones[0].lowPrice - expectedLow) < 0.01,
            "Single print zone low price should be correct");
        TEST_ASSERT(std::abs(zones[0].highPrice - expectedHigh) < 0.01,
            "Single print zone high price should be correct");
    }
}

void test_single_print_fill_progress() {
    TEST_SECTION("SinglePrintDetector - Fill Progress");

    SinglePrintDetector detector;
    const double tickSize = 0.25;
    const double priceStart = 6090.00;
    const int numLevels = 100;
    const double avgVolume = 1000.0;

    // Create initial thin zone
    std::vector<double> volumeData(numLevels);
    for (int i = 0; i < numLevels; ++i) {
        volumeData[i] = avgVolume;
    }
    for (int i = 40; i < 48; ++i) {
        volumeData[i] = avgVolume * 0.05;
    }

    auto zones = detector.DetectFromProfile(
        volumeData.data(), priceStart, tickSize, numLevels, avgVolume, 100);

    TEST_ASSERT(zones.size() == 1, "Should detect single print zone initially");
    TEST_ASSERT(zones[0].fillProgress == 0.0, "Initial fill progress should be 0");

    // Partially fill the zone
    volumeData[40] = avgVolume;
    volumeData[41] = avgVolume;
    volumeData[42] = avgVolume;
    volumeData[43] = avgVolume;  // 4 of 8 ticks filled = 50%

    detector.UpdateFillProgress(zones, volumeData.data(), priceStart, tickSize, numLevels, avgVolume);

    TEST_ASSERT(zones[0].fillProgress >= 0.4 && zones[0].fillProgress <= 0.6,
        "Fill progress should be around 50% after partial fill");
    TEST_ASSERT(zones[0].valid, "Zone should still be valid at 50% fill");

    // Fully fill the zone
    for (int i = 44; i < 48; ++i) {
        volumeData[i] = avgVolume;
    }

    detector.UpdateFillProgress(zones, volumeData.data(), priceStart, tickSize, numLevels, avgVolume);

    TEST_ASSERT(!zones[0].valid, "Zone should be invalid after full fill");
}

// ============================================================================
// EXCESS DETECTOR TESTS
// ============================================================================

void test_excess_detector_basic() {
    TEST_SECTION("ExcessDetector - Basic Detection");

    ExcessDetector detector;
    const double tickSize = 0.25;
    const double sessionHigh = 6120.00;
    const double sessionLow = 6080.00;

    // Simulate price at high, then moving away
    ActivityClassification activity;
    activity.valid = true;
    activity.activityType = AMTActivityType::RESPONSIVE;  // Responsive at extreme

    // Touch high
    ExcessType result = detector.UpdateHigh(sessionHigh, sessionHigh, tickSize, 100, activity, 3.0);
    TEST_ASSERT(result == ExcessType::NONE, "No excess immediately at touch");

    // Move away
    for (int i = 0; i < 5; ++i) {
        result = detector.UpdateHigh(sessionHigh, sessionHigh - (i + 1) * 2.0, tickSize, 101 + i, activity, 3.0);
    }

    // Should now detect excess (tail + responsive + multi-bar away)
    TEST_ASSERT(result == ExcessType::EXCESS_HIGH,
        "Should detect EXCESS_HIGH after sustained move away with tail and responsive activity");
}

void test_excess_detector_poor_high() {
    TEST_SECTION("ExcessDetector - Poor High (No Tail)");

    ExcessDetector detector;
    const double tickSize = 0.25;
    const double sessionHigh = 6120.00;

    ActivityClassification activity;
    activity.valid = true;
    activity.activityType = AMTActivityType::INITIATIVE;  // Initiative at extreme (not responsive)

    // Touch high with no tail
    detector.UpdateHigh(sessionHigh, sessionHigh, tickSize, 100, activity, 0.0);  // No tail

    // Move away
    ExcessType result = ExcessType::NONE;
    for (int i = 0; i < 5; ++i) {
        result = detector.UpdateHigh(sessionHigh, sessionHigh - (i + 1) * 2.0, tickSize, 101 + i, activity, 0.0);
    }

    // Should detect poor high (rejected but no tail/responsive)
    TEST_ASSERT(result == ExcessType::POOR_HIGH,
        "Should detect POOR_HIGH when rejected without tail/responsive activity");
}

// ============================================================================
// AMT SIGNAL ENGINE TESTS
// ============================================================================

void test_signal_engine_integration() {
    TEST_SECTION("AMTSignalEngine - Integration Test");

    AMTSignalEngine engine;
    const double tickSize = 0.25;
    const double poc = 6100.00;
    const double vah = 6110.00;
    const double val = 6090.00;
    double sessionHigh = 6115.00;
    double sessionLow = 6085.00;

    // Simulate a trading sequence
    double prices[] = {6100.00, 6102.00, 6105.00, 6108.00, 6112.00, 6115.00, 6118.00, 6120.00};
    double deltas[] = {0.05, 0.15, 0.25, 0.35, 0.40, 0.45, 0.50, 0.30};

    StateEvidence lastEvidence;
    for (int i = 1; i < 8; ++i) {
        sessionHigh = (std::max)(sessionHigh, prices[i]);

        lastEvidence = engine.ProcessBar(
            prices[i],
            prices[i-1],
            poc, vah, val,
            deltas[i],
            tickSize,
            sessionHigh,
            sessionLow,
            i,
            0.0, 0.0  // No tail info
        );
    }

    // After sustained initiative moves, should be in IMBALANCE
    TEST_ASSERT(lastEvidence.currentState == AMTMarketState::IMBALANCE,
        "After sustained initiative bars, engine should report IMBALANCE");

    TEST_ASSERT(lastEvidence.activity.activityType == AMTActivityType::INITIATIVE ||
                lastEvidence.activity.activityType == AMTActivityType::RESPONSIVE,
        "Activity type should be valid");

    TEST_ASSERT(lastEvidence.pocPrice == poc, "POC price should be preserved in evidence");
}

// ============================================================================
// STATE EVIDENCE STRUCT TESTS
// ============================================================================

void test_state_evidence_reset() {
    TEST_SECTION("StateEvidence - Reset");

    StateEvidence evidence;
    evidence.currentState = AMTMarketState::IMBALANCE;
    evidence.stateStrength = 0.75;
    evidence.barsInState = 10;
    evidence.singlePrintZonePresent = true;

    evidence.Reset();

    TEST_ASSERT(evidence.currentState == AMTMarketState::UNKNOWN, "State should reset to UNKNOWN");
    TEST_ASSERT(evidence.stateStrength == 0.0, "Strength should reset to 0");
    TEST_ASSERT(evidence.barsInState == 0, "Bars in state should reset to 0");
    TEST_ASSERT(!evidence.singlePrintZonePresent, "Single print flag should reset to false");
}

void test_state_evidence_transition_detection() {
    TEST_SECTION("StateEvidence - Transition Detection");

    StateEvidence evidence;

    // No transition when previous is UNKNOWN
    evidence.previousState = AMTMarketState::UNKNOWN;
    evidence.currentState = AMTMarketState::BALANCE;
    TEST_ASSERT(!evidence.IsTransition(), "Should not be transition when previous is UNKNOWN");

    // Transition when previous differs from current
    evidence.previousState = AMTMarketState::BALANCE;
    evidence.currentState = AMTMarketState::IMBALANCE;
    TEST_ASSERT(evidence.IsTransition(), "Should be transition when state changes");

    // No transition when same state
    evidence.previousState = AMTMarketState::BALANCE;
    evidence.currentState = AMTMarketState::BALANCE;
    TEST_ASSERT(!evidence.IsTransition(), "Should not be transition when same state");
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "AMT Signals Test Suite\n";
    std::cout << "======================\n";

    // Activity Classifier tests
    test_activity_classifier_intent();
    test_activity_classifier_participation();
    test_activity_classifier_activity_type();
    test_activity_classifier_location();

    // State Tracker tests
    test_state_tracker_basic();
    test_state_tracker_strength_decay();
    test_state_tracker_transitions();

    // Single Print Detector tests
    test_single_print_detector();
    test_single_print_fill_progress();

    // Excess Detector tests
    test_excess_detector_basic();
    test_excess_detector_poor_high();

    // Signal Engine tests
    test_signal_engine_integration();

    // State Evidence tests
    test_state_evidence_reset();
    test_state_evidence_transition_detection();

    // Summary
    std::cout << "\n=== SUMMARY ===\n";
    std::cout << "Tests passed: " << g_testsPassed << "\n";
    std::cout << "Tests failed: " << g_testsFailed << "\n";

    if (g_testsFailed == 0) {
        std::cout << "\n[SUCCESS] All tests passed!\n";
        return 0;
    } else {
        std::cout << "\n[FAILURE] Some tests failed.\n";
        return 1;
    }
}
