// test_dom_events.cpp - Verify DOM event detection logic
// Tests the pure detection functions without Sierra runtime dependency
#include <iostream>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>

// Include required headers
#include "test_sierrachart_mock.h"
#include "../AMT_Patterns.h"
#include "../AMT_DomEvents.h"

using namespace AMT;

// ============================================================================
// TEST HELPERS
// ============================================================================

constexpr float EPSILON = 0.001f;
constexpr double TICK_SIZE = 0.25;  // ES tick size

bool approx_equal(float a, float b, float eps = EPSILON)
{
    return std::abs(a - b) < eps;
}

// Create a sample with sensible defaults
DomObservationSample MakeSample(
    int64_t timestampMs,
    int barIndex,
    int bestBidTick = 24400,
    int bestAskTick = 24401,
    double domBidSize = 1000.0,
    double domAskSize = 1000.0,
    double bidStackPull = 0.0,
    double askStackPull = 0.0,
    double haloDepthImbalance = 0.0,
    bool haloDepthValid = true,
    double askVolSec = 50.0,
    double bidVolSec = 50.0,
    double deltaSec = 0.0,
    double tradesSec = 10.0)
{
    DomObservationSample s;
    s.timestampMs = timestampMs;
    s.barIndex = barIndex;
    s.bestBidTick = bestBidTick;
    s.bestAskTick = bestAskTick;
    s.domBidSize = domBidSize;
    s.domAskSize = domAskSize;
    s.bidStackPull = bidStackPull;
    s.askStackPull = askStackPull;
    s.haloDepthImbalance = haloDepthImbalance;
    s.haloDepthValid = haloDepthValid;
    s.askVolSec = askVolSec;
    s.bidVolSec = bidVolSec;
    s.deltaSec = deltaSec;
    s.tradesSec = tradesSec;
    return s;
}

// ============================================================================
// TEST: DomHistoryBuffer basic operations
// ============================================================================

void test_dom_history_buffer_basics()
{
    std::cout << "=== Test: DomHistoryBuffer basics ===" << std::endl;

    DomHistoryBuffer buffer;

    // Initially empty
    assert(buffer.Size() == 0);
    assert(!buffer.HasMinSamples());

    // Push samples
    for (int i = 0; i < 10; ++i)
    {
        buffer.Push(MakeSample(1000 + i * 100, i));
    }

    assert(buffer.Size() == 10);
    assert(buffer.HasMinSamples());  // MIN_SAMPLES = 6

    // Reset clears buffer
    buffer.Reset();
    assert(buffer.Size() == 0);
    assert(!buffer.HasMinSamples());

    std::cout << "  PASSED" << std::endl;
}

void test_dom_history_buffer_window()
{
    std::cout << "=== Test: DomHistoryBuffer window retrieval ===" << std::endl;

    DomHistoryBuffer buffer;

    // Push samples 100ms apart
    for (int i = 0; i < 20; ++i)
    {
        buffer.Push(MakeSample(1000 + i * 100, i));
    }

    // Get 500ms window (should get last 5-6 samples)
    auto window = buffer.GetWindow(500);
    assert(window.size() >= 5);
    assert(window.size() <= 6);

    // Verify window contains most recent samples
    assert(window.back().barIndex == 19);

    std::cout << "  PASSED" << std::endl;
}

void test_dom_history_buffer_invalid_sample()
{
    std::cout << "=== Test: DomHistoryBuffer rejects invalid samples ===" << std::endl;

    DomHistoryBuffer buffer;

    // Invalid sample (timestamp = 0)
    DomObservationSample invalid;
    invalid.timestampMs = 0;
    invalid.barIndex = 0;
    buffer.Push(invalid);

    // Should not be added
    assert(buffer.Size() == 0);

    // Invalid sample (barIndex = -1)
    invalid.timestampMs = 1000;
    invalid.barIndex = -1;
    buffer.Push(invalid);

    // Should not be added
    assert(buffer.Size() == 0);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Feature extraction
// ============================================================================

void test_feature_extraction_eligibility()
{
    std::cout << "=== Test: Feature extraction eligibility ===" << std::endl;

    // Too few samples
    std::vector<DomObservationSample> tooFew;
    for (int i = 0; i < 3; ++i)
    {
        tooFew.push_back(MakeSample(1000 + i * 100, i));
    }

    auto f = ExtractFeatures(tooFew, DomEventConfig::DEFAULT_WINDOW_MS);
    assert(!f.isEligible);
    assert(std::string(f.ineligibleReason) == "INSUFFICIENT_SAMPLES");

    // Window too short
    std::vector<DomObservationSample> enough;
    for (int i = 0; i < 10; ++i)
    {
        enough.push_back(MakeSample(1000 + i * 100, i));
    }

    f = ExtractFeatures(enough, 500);  // Less than MIN_WINDOW_MS (1000)
    assert(!f.isEligible);
    assert(std::string(f.ineligibleReason) == "WINDOW_TOO_SHORT");

    // Valid
    f = ExtractFeatures(enough, DomEventConfig::DEFAULT_WINDOW_MS);
    assert(f.isEligible);
    assert(f.ineligibleReason == nullptr);

    std::cout << "  PASSED" << std::endl;
}

void test_feature_extraction_stack_pull()
{
    std::cout << "=== Test: Feature extraction stack/pull dominance ===" << std::endl;

    std::vector<DomObservationSample> samples;

    // Create samples with stacking (positive stackPull)
    for (int i = 0; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bidStackPull = 50.0;  // Net stacking
        s.askStackPull = 30.0;
        samples.push_back(s);
    }

    auto f = ExtractFeatures(samples, DomEventConfig::DEFAULT_WINDOW_MS);

    // Last sample has stackDominance = max(50,0) + max(30,0) = 80
    assert(f.stackDominance > 0.0);
    assert(f.pullDominance == 0.0);  // No negative values

    // Now test pulling
    samples.clear();
    for (int i = 0; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bidStackPull = -40.0;  // Net pulling
        s.askStackPull = -20.0;
        samples.push_back(s);
    }

    f = ExtractFeatures(samples, DomEventConfig::DEFAULT_WINDOW_MS);

    // pullDominance = -min(-40,0) - min(-20,0) = 40 + 20 = 60
    assert(f.pullDominance > 0.0);
    assert(f.stackDominance == 0.0);

    std::cout << "  PASSED" << std::endl;
}

void test_feature_extraction_price_movement()
{
    std::cout << "=== Test: Feature extraction price movement ===" << std::endl;

    std::vector<DomObservationSample> samples;

    // Price moving up (bestBid/Ask increasing)
    int baseBid = 24400;
    for (int i = 0; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bestBidTick = baseBid + i;
        s.bestAskTick = baseBid + i + 1;
        samples.push_back(s);
    }

    auto f = ExtractFeatures(samples, DomEventConfig::DEFAULT_WINDOW_MS);

    // Should detect upward movement
    assert(f.bestBidMoveTicks == 9);  // 24409 - 24400
    assert(f.bestAskMoveTicks == 9);

    std::cout << "  PASSED" << std::endl;
}

void test_feature_extraction_delta_sign_flip()
{
    std::cout << "=== Test: Feature extraction delta sign flip ===" << std::endl;

    std::vector<DomObservationSample> samples;

    // First half: positive delta
    for (int i = 0; i < 5; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.deltaSec = 10.0;  // Positive
        samples.push_back(s);
    }

    // Second half: negative delta (sign flip)
    for (int i = 5; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.deltaSec = -10.0;  // Negative
        samples.push_back(s);
    }

    auto f = ExtractFeatures(samples, DomEventConfig::DEFAULT_WINDOW_MS);

    assert(f.deltaSignFlipped);
    assert(f.consecutiveNegativeDelta == 5);  // Last 5 are negative

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Detector - LIQUIDITY_PULLING
// ============================================================================

void test_detect_liquidity_pulling()
{
    std::cout << "=== Test: Detect LIQUIDITY_PULLING ===" << std::endl;

    DomHistoryBuffer buffer;

    // Create samples with pulling + sudden halo depth drop
    // First 12 samples: stable halo depth with small variance (needed for MAD > 0)
    for (int i = 0; i < 12; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bidStackPull = -100.0;  // Strong pulling
        s.askStackPull = -50.0;
        // Small variance around 0.5 so MAD > 0
        s.haloDepthImbalance = 0.45 + (i % 3) * 0.05;  // 0.45, 0.50, 0.55, 0.45, ...
        s.haloDepthValid = true;
        buffer.Push(s);
    }

    // Last 3 samples: sharp drop in halo depth (well below median)
    for (int i = 12; i < 15; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bidStackPull = -100.0;  // Strong pulling
        s.askStackPull = -50.0;
        s.haloDepthImbalance = -0.9;  // Sharp drop (far from median ~0.5)
        s.haloDepthValid = true;
        buffer.Push(s);
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    bool found = false;
    for (const auto& p : result.controlPatterns)
    {
        if (p == DOMControlPattern::LIQUIDITY_PULLING)
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Detector - LIQUIDITY_STACKING
// ============================================================================

void test_detect_liquidity_stacking()
{
    std::cout << "=== Test: Detect LIQUIDITY_STACKING ===" << std::endl;

    DomHistoryBuffer buffer;

    // Create samples with stacking + sudden halo depth increase
    // First 12 samples: stable halo depth with small variance (needed for MAD > 0)
    for (int i = 0; i < 12; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bidStackPull = 100.0;  // Strong stacking
        s.askStackPull = 50.0;
        // Small variance around -0.5 so MAD > 0
        s.haloDepthImbalance = -0.55 + (i % 3) * 0.05;  // -0.55, -0.50, -0.45, ...
        s.haloDepthValid = true;
        buffer.Push(s);
    }

    // Last 3 samples: sharp increase in halo depth (well above median)
    for (int i = 12; i < 15; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bidStackPull = 100.0;  // Strong stacking
        s.askStackPull = 50.0;
        s.haloDepthImbalance = 0.9;  // Sharp increase (far from median ~-0.5)
        s.haloDepthValid = true;
        buffer.Push(s);
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    bool found = false;
    for (const auto& p : result.controlPatterns)
    {
        if (p == DOMControlPattern::LIQUIDITY_STACKING)
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Detector - BUYERS_LIFTING_ASKS
// ============================================================================

void test_detect_buyers_lifting_asks()
{
    std::cout << "=== Test: Detect BUYERS_LIFTING_ASKS ===" << std::endl;

    DomHistoryBuffer buffer;

    // Create samples with aggressive buying + price advancing
    int baseBid = 24400;
    for (int i = 0; i < 15; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.askVolSec = 100.0;  // High ask volume (buying)
        s.bidVolSec = 30.0;   // Low bid volume
        s.bestBidTick = baseBid + i / 3;  // Gradual upward move
        s.bestAskTick = baseBid + i / 3 + 1;
        buffer.Push(s);
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    bool found = false;
    for (const auto& p : result.controlPatterns)
    {
        if (p == DOMControlPattern::BUYERS_LIFTING_ASKS)
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Detector - SELLERS_HITTING_BIDS
// ============================================================================

void test_detect_sellers_hitting_bids()
{
    std::cout << "=== Test: Detect SELLERS_HITTING_BIDS ===" << std::endl;

    DomHistoryBuffer buffer;

    // Create samples with aggressive selling + price dropping
    int baseBid = 24400;
    for (int i = 0; i < 15; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bidVolSec = 100.0;  // High bid volume (selling)
        s.askVolSec = 30.0;   // Low ask volume
        s.bestBidTick = baseBid - i / 3;  // Gradual downward move
        s.bestAskTick = baseBid - i / 3 + 1;
        buffer.Push(s);
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    bool found = false;
    for (const auto& p : result.controlPatterns)
    {
        if (p == DOMControlPattern::SELLERS_HITTING_BIDS)
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Detector - LIQUIDITY_DISAPPEARANCE
// ============================================================================

void test_detect_liquidity_disappearance()
{
    std::cout << "=== Test: Detect LIQUIDITY_DISAPPEARANCE ===" << std::endl;

    DomHistoryBuffer buffer;

    // Create samples with stable halo (with variance for MAD > 0), then sudden drop
    for (int i = 0; i < 12; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        // Small variance around 0.5 so MAD > 0
        s.haloDepthImbalance = 0.45 + (i % 3) * 0.05;  // 0.45, 0.50, 0.55, ...
        s.haloDepthValid = true;
        buffer.Push(s);
    }

    // Sudden drop in halo depth (well below median)
    for (int i = 12; i < 15; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.haloDepthImbalance = -0.9;  // Sharp drop (far from median ~0.5)
        s.haloDepthValid = true;
        buffer.Push(s);
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    bool found = false;
    for (const auto& e : result.events)
    {
        if (e == DOMEvent::LIQUIDITY_DISAPPEARANCE)
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Detector - ORDER_FLOW_REVERSAL
// ============================================================================

void test_detect_order_flow_reversal()
{
    std::cout << "=== Test: Detect ORDER_FLOW_REVERSAL ===" << std::endl;

    DomHistoryBuffer buffer;

    // Strong positive delta for first half
    for (int i = 0; i < 8; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.deltaSec = 50.0;  // Strong buying
        buffer.Push(s);
    }

    // Strong negative delta for second half (reversal)
    for (int i = 8; i < 15; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.deltaSec = -50.0;  // Strong selling
        buffer.Push(s);
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    bool found = false;
    for (const auto& e : result.events)
    {
        if (e == DOMEvent::ORDER_FLOW_REVERSAL)
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Detector - SWEEP_LIQUIDATION
// ============================================================================

void test_detect_sweep_liquidation()
{
    std::cout << "=== Test: Detect SWEEP_LIQUIDATION ===" << std::endl;

    DomHistoryBuffer buffer;

    // Setup: stable conditions with variance
    int baseBid = 24400;
    for (int i = 0; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bestBidTick = baseBid;
        s.bestAskTick = baseBid + 1;
        // Small variance around 0.5 so MAD > 0
        s.haloDepthImbalance = 0.45 + (i % 3) * 0.05;
        s.haloDepthValid = true;
        s.tradesSec = 10.0 + (i % 3);  // Small variance
        buffer.Push(s);
    }

    // Sweep: rapid price move + depth collapse + trade spike
    for (int i = 10; i < 15; ++i)
    {
        auto s = MakeSample(1000 + i * 100, i);
        s.bestBidTick = baseBid + (i - 10) * 2;  // Price moving up rapidly
        s.bestAskTick = baseBid + (i - 10) * 2 + 1;
        s.haloDepthImbalance = -0.9;  // Depth collapsed (far from median)
        s.haloDepthValid = true;
        s.tradesSec = 100.0;  // Trade spike
        buffer.Push(s);
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    bool found = false;
    for (const auto& e : result.events)
    {
        if (e == DOMEvent::SWEEP_LIQUIDATION)
        {
            found = true;
            break;
        }
    }
    assert(found);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: LARGE_LOT_EXECUTION returns nothing (deferred)
// ============================================================================

void test_large_lot_returns_nothing()
{
    std::cout << "=== Test: LARGE_LOT_EXECUTION returns nothing (deferred) ===" << std::endl;

    DomHistoryBuffer buffer;

    // Add any samples
    for (int i = 0; i < 15; ++i)
    {
        buffer.Push(MakeSample(1000 + i * 100, i));
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    // Verify LARGE_LOT_EXECUTION is never emitted
    for (const auto& e : result.events)
    {
        assert(e != DOMEvent::LARGE_LOT_EXECUTION);
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Eligibility - too few samples returns nothing
// ============================================================================

void test_too_few_samples_returns_nothing()
{
    std::cout << "=== Test: Too few samples returns nothing ===" << std::endl;

    DomHistoryBuffer buffer;

    // Add only 3 samples (less than MIN_SAMPLES = 6)
    for (int i = 0; i < 3; ++i)
    {
        buffer.Push(MakeSample(1000 + i * 100, i));
    }

    auto result = DetectDomEventsAndControl(buffer, DomEventConfig::DEFAULT_WINDOW_MS);

    assert(!result.wasEligible);
    assert(result.controlPatterns.empty());
    assert(result.events.empty());

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Log state deduplication
// ============================================================================

void test_log_state_deduplication()
{
    std::cout << "=== Test: Log state deduplication ===" << std::endl;

    DomEventLogState logState;

    DomDetectionResult result1;
    result1.controlPatterns.push_back(DOMControlPattern::LIQUIDITY_PULLING);
    result1.events.push_back(DOMEvent::LIQUIDITY_DISAPPEARANCE);

    // First call should log
    assert(logState.ShouldLog(result1, 100));

    // Immediate repeat with same patterns should not log (throttle)
    assert(!logState.ShouldLog(result1, 101));

    // After throttle period, same patterns should still not log (unchanged)
    assert(!logState.ShouldLog(result1, 120));

    // Different patterns should log
    DomDetectionResult result2;
    result2.controlPatterns.push_back(DOMControlPattern::LIQUIDITY_STACKING);
    assert(logState.ShouldLog(result2, 121));

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: Log message builder
// ============================================================================

void test_log_message_builder()
{
    std::cout << "=== Test: Log message builder ===" << std::endl;

    DomDetectionResult result;
    result.windowMs = 5000;

    DOMControlHit ctrlHit;
    ctrlHit.type = DOMControlPattern::LIQUIDITY_PULLING;
    ctrlHit.strength01 = 0.85f;
    ctrlHit.windowMs = 5000;
    result.controlPatterns.push_back(ctrlHit.type);
    result.controlHits.push_back(ctrlHit);

    DOMEventHit evtHit;
    evtHit.type = DOMEvent::LIQUIDITY_DISAPPEARANCE;
    evtHit.strength01 = 0.72f;
    evtHit.windowMs = 5000;
    result.events.push_back(evtHit.type);
    result.eventHits.push_back(evtHit);

    std::string msg = BuildDomEventLogMessage(result, 35000000);

    // Verify message contains expected elements
    assert(msg.find("[DOM-EVENT]") != std::string::npos);
    assert(msg.find("window=5000ms") != std::string::npos);
    assert(msg.find("LIQ_PULLING") != std::string::npos);
    assert(msg.find("LIQ_DISAPPEAR") != std::string::npos);

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// TEST: to_string functions
// ============================================================================

void test_to_string_functions()
{
    std::cout << "=== Test: to_string functions ===" << std::endl;

    assert(std::string(to_string(DOMControlPattern::BUYERS_LIFTING_ASKS)) == "BUYERS_LIFTING");
    assert(std::string(to_string(DOMControlPattern::SELLERS_HITTING_BIDS)) == "SELLERS_HITTING");
    assert(std::string(to_string(DOMControlPattern::LIQUIDITY_PULLING)) == "LIQ_PULLING");
    assert(std::string(to_string(DOMControlPattern::LIQUIDITY_STACKING)) == "LIQ_STACKING");
    assert(std::string(to_string(DOMControlPattern::EXHAUSTION_DIVERGENCE)) == "EXHAUST_DIV");

    assert(std::string(to_string(DOMEvent::LIQUIDITY_DISAPPEARANCE)) == "LIQ_DISAPPEAR");
    assert(std::string(to_string(DOMEvent::ORDER_FLOW_REVERSAL)) == "FLOW_REVERSAL");
    assert(std::string(to_string(DOMEvent::SWEEP_LIQUIDATION)) == "SWEEP_LIQ");
    assert(std::string(to_string(DOMEvent::LARGE_LOT_EXECUTION)) == "LARGE_LOT");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// MAIN
// ============================================================================

int main()
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "DOM Events Detection Tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Buffer tests
    test_dom_history_buffer_basics();
    test_dom_history_buffer_window();
    test_dom_history_buffer_invalid_sample();

    // Feature extraction tests
    test_feature_extraction_eligibility();
    test_feature_extraction_stack_pull();
    test_feature_extraction_price_movement();
    test_feature_extraction_delta_sign_flip();

    // DOMControlPattern detector tests
    test_detect_liquidity_pulling();
    test_detect_liquidity_stacking();
    test_detect_buyers_lifting_asks();
    test_detect_sellers_hitting_bids();

    // DOMEvent detector tests
    test_detect_liquidity_disappearance();
    test_detect_order_flow_reversal();
    test_detect_sweep_liquidation();

    // Deferred/edge case tests
    test_large_lot_returns_nothing();
    test_too_few_samples_returns_nothing();

    // Observability tests
    test_log_state_deduplication();
    test_log_message_builder();
    test_to_string_functions();

    std::cout << "\n========================================" << std::endl;
    std::cout << "All DOM Events tests PASSED!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
