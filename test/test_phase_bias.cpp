// test_phase_bias.cpp - CurrentPhase Bias Audit (ValueZone 9-state, Dec 2024)
#include <iostream>
#include <string>
#include <map>
#include <vector>
#define SIERRACHART_H
#include "test_sierrachart_mock.h"
#include "amt_core.h"
using namespace AMT;

enum class TimeframePattern : int { UNKNOWN=0, ONE_TIME_FRAMING_UP=1, ONE_TIME_FRAMING_DOWN=2, TWO_TIME_FRAMING=3 };
enum class RangeExtensionType : int { NONE=0, RANGE_EXT_HIGH=1, RANGE_EXT_LOW=2, RANGE_EXT_BOTH=3 };

const char* PhaseStr(CurrentPhase p) {
    switch(p) { case CurrentPhase::FAILED_AUCTION: return "FAILED_AUCTION"; case CurrentPhase::TESTING_BOUNDARY: return "TESTING_BOUNDARY";
    case CurrentPhase::ROTATION: return "ROTATION"; case CurrentPhase::RANGE_EXTENSION: return "RANGE_EXTENSION";
    case CurrentPhase::PULLBACK: return "PULLBACK"; case CurrentPhase::DRIVING_UP: return "DRIVING_UP";
    case CurrentPhase::DRIVING_DOWN: return "DRIVING_DOWN"; default: return "UNKNOWN"; }
}

struct MockDaltonState {
    AMTMarketState marketState = AMTMarketState::UNKNOWN;
    ValueZone location = ValueZone::UNKNOWN;
    AMTActivityType activity = AMTActivityType::NEUTRAL;
    ExcessType excess = ExcessType::NONE;
    RangeExtensionType extension = RangeExtensionType::NONE;
    TimeframePattern timeframe = TimeframePattern::TWO_TIME_FRAMING;
    bool failedAuctionAbove = false, failedAuctionBelow = false;

    CurrentPhase DeriveCurrentPhase() const {
        if (failedAuctionAbove || failedAuctionBelow) return CurrentPhase::FAILED_AUCTION;
        if (excess != ExcessType::NONE) return CurrentPhase::FAILED_AUCTION;
        if (marketState == AMTMarketState::BALANCE) {
            if (IsAtBoundary(location)) return CurrentPhase::TESTING_BOUNDARY;
            return CurrentPhase::ROTATION;
        }
        if (marketState == AMTMarketState::IMBALANCE) {
            if (IsAtBoundary(location) && activity == AMTActivityType::RESPONSIVE) return CurrentPhase::FAILED_AUCTION;
            if (extension != RangeExtensionType::NONE && activity == AMTActivityType::INITIATIVE) return CurrentPhase::RANGE_EXTENSION;
            if (activity == AMTActivityType::RESPONSIVE) return CurrentPhase::PULLBACK;
            if (timeframe == TimeframePattern::ONE_TIME_FRAMING_UP) return CurrentPhase::DRIVING_UP;
            if (timeframe == TimeframePattern::ONE_TIME_FRAMING_DOWN) return CurrentPhase::DRIVING_DOWN;
            return CurrentPhase::DRIVING_UP;
        }
        return CurrentPhase::UNKNOWN;
    }
};

int main() {
    std::cout << "=== CurrentPhase Bias Audit (ValueZone 9-state) ===\n";
    
    // Test critical case: IMBALANCE + AT_VAH + RESPONSIVE = FAILED_AUCTION
    MockDaltonState d; d.marketState = AMTMarketState::IMBALANCE; d.location = ValueZone::AT_VAH; d.activity = AMTActivityType::RESPONSIVE;
    CurrentPhase daltonR = d.DeriveCurrentPhase();
    CurrentPhase standaloneR = AMT::DeriveCurrentPhase(AMTMarketState::IMBALANCE, ValueZone::AT_VAH, AMTActivityType::RESPONSIVE, ExcessType::NONE, false);
    
    std::cout << "Input: IMBALANCE + AT_VAH + RESPONSIVE\nExpected: FAILED_AUCTION\n";
    std::cout << "Dalton: " << PhaseStr(daltonR) << "\nStandalone: " << PhaseStr(standaloneR) << "\n";
    
    bool pass = (daltonR == CurrentPhase::FAILED_AUCTION) && (standaloneR == CurrentPhase::FAILED_AUCTION);
    std::cout << "Result: " << (pass ? "PASS" : "FAIL") << "\n";
    
    // Quick enumeration
    int total=0, matches=0;
    ValueZone zones[] = {ValueZone::UPPER_VALUE, ValueZone::AT_VAH, ValueZone::AT_VAL, ValueZone::NEAR_ABOVE_VALUE};
    AMTMarketState states[] = {AMTMarketState::BALANCE, AMTMarketState::IMBALANCE};
    AMTActivityType acts[] = {AMTActivityType::NEUTRAL, AMTActivityType::INITIATIVE, AMTActivityType::RESPONSIVE};
    for (auto st : states) for (auto z : zones) for (auto a : acts) {
        total++; MockDaltonState m; m.marketState=st; m.location=z; m.activity=a;
        if (AMT::DeriveCurrentPhase(st,z,a,ExcessType::NONE,false) == m.DeriveCurrentPhase()) matches++;
    }
    std::cout << "Enumeration: " << matches << "/" << total << " match\n";
    return pass ? 0 : 1;
}
