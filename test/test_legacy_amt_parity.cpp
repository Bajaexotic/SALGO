// Define standalone test mode to exclude SC-dependent functions
#define AMT_STANDALONE_TEST

// test_legacy_amt_parity.cpp
// Simulates legacy and AMT zone engagement logic side-by-side
// Verifies they produce identical engagement episodes
// Compile: g++ -std=c++17 -I. -o test_legacy_amt_parity.exe test_legacy_amt_parity.cpp

#include <iostream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <string>
#include <functional>

// Mock Sierra Chart types
#ifndef SIERRACHART_H
#define SIERRACHART_H
struct SCDateTime {
    double m_dt = 0.0;
    SCDateTime() = default;
    SCDateTime(double d) : m_dt(d) {}
    double GetAsDouble() const { return m_dt; }
    SCDateTime operator+(double d) const { return SCDateTime(m_dt + d); }
};
#endif

#include "AMT_Zones.h"

// =============================================================================
// LEGACY ZONE STATE SIMULATION (mirrors pre-Phase6 AuctionSensor behavior)
// =============================================================================
// NOTE: LegacyZoneState is intentionally preserved here for PARITY TESTING.
// This enum simulates the old 3-state FSM to verify AMT produces identical
// engagement episodes. Production code should only use AMT::ZoneProximity.

enum LegacyZoneState { ST_INACTIVE = 0, ST_APPROACH = 1, ST_ENGAGED = 2 };

struct LegacyZoneRuntime {
    LegacyZoneState state = ST_INACTIVE;
    int barsOutsideHalo = 0;
    int zoneEntryIndex = -1;
    double zoneEntryPrice = 0.0;
    double cachedAnchor = 0.0;
    int cachedCoreTicks = 3;
    int cachedHaloTicks = 5;
};

struct LegacyEngagement {
    int entryBar = -1;
    int exitBar = -1;
    int barsEngaged = 0;
    double entryPrice = 0.0;
    double exitPrice = 0.0;
    double escapeVelocity = 0.0;
    int coreWidthTicks = 0;
    int haloWidthTicks = 0;
};

// =============================================================================
// AMT ENGAGEMENT TRACKER (simplified - matches ZoneRuntime behavior)
// =============================================================================

struct AmtEngagement {
    int startBar = -1;
    int endBar = -1;
    int barsEngaged = 0;
    double entryPrice = 0.0;
    double exitPrice = 0.0;
    double escapeVelocity = 0.0;
};

struct AmtZoneTracker {
    AMT::ZoneProximity proximity = AMT::ZoneProximity::INACTIVE;
    int coreWidthTicks = 3;
    int haloWidthTicks = 5;
    int barsOutsideZone = 0;

    // Current engagement
    bool inEngagement = false;
    int engagementStartBar = -1;
    double engagementEntryPrice = 0.0;

    std::vector<AmtEngagement> engagements;

    void UpdateProximity(double price, double anchor, double tickSize, int bar, int timeoutBars) {
        double dist = std::fabs(price - anchor) / tickSize;

        AMT::ZoneProximity newProx;
        if (dist <= coreWidthTicks) {
            newProx = AMT::ZoneProximity::AT_ZONE;
        } else if (dist <= haloWidthTicks) {
            newProx = AMT::ZoneProximity::APPROACHING;
        } else {
            newProx = AMT::ZoneProximity::INACTIVE;
        }

        bool inHalo = (newProx == AMT::ZoneProximity::AT_ZONE || newProx == AMT::ZoneProximity::APPROACHING);

        // Handle timeout when outside halo
        if (!inHalo && proximity != AMT::ZoneProximity::INACTIVE) {
            barsOutsideZone++;

            if (barsOutsideZone >= timeoutBars) {
                // Finalize engagement
                if (inEngagement && engagementStartBar >= 0) {
                    AmtEngagement eng;
                    eng.startBar = engagementStartBar;
                    eng.endBar = bar;
                    eng.barsEngaged = bar - engagementStartBar;
                    eng.entryPrice = engagementEntryPrice;
                    eng.exitPrice = price;
                    eng.escapeVelocity = (eng.barsEngaged > 0)
                        ? std::fabs(eng.exitPrice - eng.entryPrice) / tickSize / eng.barsEngaged
                        : 0.0;
                    engagements.push_back(eng);
                }

                inEngagement = false;
                engagementStartBar = -1;
                engagementEntryPrice = 0.0;
                barsOutsideZone = 0;
                proximity = AMT::ZoneProximity::INACTIVE;
            }
            return;
        }

        if (!inHalo) return;

        barsOutsideZone = 0;

        // Start engagement when entering HALO (matches legacy behavior)
        if (proximity == AMT::ZoneProximity::INACTIVE && inHalo) {
            if (!inEngagement) {
                inEngagement = true;
                engagementStartBar = bar;
                engagementEntryPrice = price;
            }
        }

        proximity = newProx;
    }
};

// =============================================================================
// SIMULATED BAR DATA
// =============================================================================

struct SimBar {
    int index;
    double close;
    double anchorPrice;
    int liqTicks;
};

std::vector<SimBar> GenerateTestBars(double anchor, double tickSize, int scenario) {
    std::vector<SimBar> bars;

    if (scenario == 1) {
        // Scenario 1: Clean engagement - approach, engage for 5 bars, exit
        for (int i = 0; i < 5; i++) {
            bars.push_back({i, anchor + 20 * tickSize, anchor, 3});
        }
        for (int i = 5; i < 8; i++) {
            bars.push_back({i, anchor + 4 * tickSize, anchor, 3});
        }
        for (int i = 8; i < 13; i++) {
            bars.push_back({i, anchor + 1 * tickSize, anchor, 3});
        }
        for (int i = 13; i < 16; i++) {
            bars.push_back({i, anchor + 10 * tickSize, anchor, 3});
        }
        for (int i = 16; i < 21; i++) {
            bars.push_back({i, anchor + 25 * tickSize, anchor, 3});
        }
    }
    else if (scenario == 2) {
        // Scenario 2: Two separate engagements
        for (int i = 0; i < 5; i++) {
            bars.push_back({i, anchor + 20 * tickSize, anchor, 3});
        }
        for (int i = 5; i < 11; i++) {
            bars.push_back({i, anchor + 1 * tickSize, anchor, 3});
        }
        for (int i = 11; i < 16; i++) {
            bars.push_back({i, anchor + 15 * tickSize, anchor, 3});
        }
        for (int i = 16; i < 21; i++) {
            bars.push_back({i, anchor + 2 * tickSize, anchor, 3});
        }
        for (int i = 21; i < 26; i++) {
            bars.push_back({i, anchor + 20 * tickSize, anchor, 3});
        }
    }
    else if (scenario == 3) {
        // Scenario 3: Brief touch then exit
        for (int i = 0; i < 5; i++) {
            bars.push_back({i, anchor + 20 * tickSize, anchor, 3});
        }
        // Quick touch
        bars.push_back({5, anchor + 1 * tickSize, anchor, 3});
        bars.push_back({6, anchor + 1 * tickSize, anchor, 3});
        // Exit immediately
        for (int i = 7; i < 15; i++) {
            bars.push_back({i, anchor + 20 * tickSize, anchor, 3});
        }
    }

    return bars;
}

// =============================================================================
// LEGACY PROCESSING
// =============================================================================

std::vector<LegacyEngagement> RunLegacySimulation(
    const std::vector<SimBar>& bars,
    double tickSize,
    double haloMult,
    int timeoutBars)
{
    std::vector<LegacyEngagement> engagements;
    LegacyZoneRuntime st;

    for (const auto& bar : bars) {
        const double currentPrice = bar.close;
        const double anchor = bar.anchorPrice;
        const int liqTicks = bar.liqTicks;
        const int idx = bar.index;

        int bestDist = static_cast<int>(std::ceil(std::fabs(currentPrice - anchor) / tickSize));

        if (st.cachedAnchor == 0.0 || std::fabs(anchor - st.cachedAnchor) > tickSize * 0.5) {
            const int coreTicks = std::max(2, liqTicks);
            const int haloTicks = std::max(coreTicks + 1, static_cast<int>(std::round(coreTicks * haloMult)));
            st.cachedAnchor = anchor;
            st.cachedCoreTicks = coreTicks;
            st.cachedHaloTicks = haloTicks;
        }

        const bool inHalo = bestDist <= st.cachedHaloTicks;
        const bool inCore = bestDist <= st.cachedCoreTicks;
        const LegacyZoneState newState = inCore ? ST_ENGAGED : (inHalo ? ST_APPROACH : ST_INACTIVE);

        if (!inHalo && st.state != ST_INACTIVE) {
            st.barsOutsideHalo++;

            if (st.barsOutsideHalo >= timeoutBars) {
                if (st.zoneEntryIndex >= 0) {
                    const int timeInZone = idx - st.zoneEntryIndex;
                    const double exitPrice = currentPrice;
                    const double escapeVel = (st.zoneEntryPrice > 0.0 && timeInZone > 0)
                        ? std::fabs(exitPrice - st.zoneEntryPrice) / tickSize / timeInZone
                        : 0.0;

                    LegacyEngagement eng;
                    eng.entryBar = st.zoneEntryIndex;
                    eng.exitBar = idx;
                    eng.barsEngaged = timeInZone;
                    eng.entryPrice = st.zoneEntryPrice;
                    eng.exitPrice = exitPrice;
                    eng.escapeVelocity = escapeVel;
                    eng.coreWidthTicks = st.cachedCoreTicks;
                    eng.haloWidthTicks = st.cachedHaloTicks;
                    engagements.push_back(eng);
                }

                st.state = ST_INACTIVE;
                st.barsOutsideHalo = 0;
                st.zoneEntryIndex = -1;
                st.zoneEntryPrice = 0.0;
            }
            continue;
        }

        if (!inHalo) continue;

        st.barsOutsideHalo = 0;

        if (st.state == ST_INACTIVE && (inHalo || inCore)) {
            st.zoneEntryIndex = idx;
            st.zoneEntryPrice = currentPrice;
        }

        st.state = newState;
    }

    return engagements;
}

// =============================================================================
// AMT PROCESSING
// =============================================================================

std::vector<AmtEngagement> RunAmtSimulation(
    const std::vector<SimBar>& bars,
    double tickSize,
    double haloMult,
    int timeoutBars)
{
    AmtZoneTracker tracker;
    tracker.coreWidthTicks = std::max(2, bars[0].liqTicks);
    tracker.haloWidthTicks = std::max(tracker.coreWidthTicks + 1,
        static_cast<int>(std::round(tracker.coreWidthTicks * haloMult)));

    for (const auto& bar : bars) {
        tracker.UpdateProximity(bar.close, bar.anchorPrice, tickSize, bar.index, timeoutBars);
    }

    return tracker.engagements;
}

// =============================================================================
// COMPARISON
// =============================================================================

bool CompareEngagements(
    const std::vector<LegacyEngagement>& legacy,
    const std::vector<AmtEngagement>& amt)
{
    if (legacy.size() != amt.size()) {
        std::cout << "  COUNT MISMATCH: legacy=" << legacy.size() << " amt=" << amt.size() << std::endl;
        return false;
    }

    bool allMatch = true;
    for (size_t i = 0; i < legacy.size(); i++) {
        const auto& leg = legacy[i];
        const auto& a = amt[i];

        bool match = true;
        std::string diff;

        if (leg.entryBar != a.startBar) {
            match = false;
            diff += " entryBar(" + std::to_string(leg.entryBar) + "!=" + std::to_string(a.startBar) + ")";
        }
        if (leg.barsEngaged != a.barsEngaged) {
            match = false;
            diff += " barsEngaged(" + std::to_string(leg.barsEngaged) + "!=" + std::to_string(a.barsEngaged) + ")";
        }
        if (std::fabs(leg.escapeVelocity - a.escapeVelocity) > 1e-6) {
            match = false;
            diff += " escVel(" + std::to_string(leg.escapeVelocity) + "!=" + std::to_string(a.escapeVelocity) + ")";
        }

        if (!match) {
            std::cout << "  Episode " << i << " MISMATCH:" << diff << std::endl;
            allMatch = false;
        }
    }

    return allMatch;
}

void PrintEngagement(const char* prefix, int idx, int entry, int exit, int bars, double escVel) {
    std::cout << "  " << prefix << "[" << idx << "]: entry=" << entry
              << " exit=" << exit
              << " bars=" << bars
              << " escVel=" << std::fixed << std::setprecision(4) << escVel << std::endl;
}

// =============================================================================
// MAIN TEST
// =============================================================================

int main() {
    std::cout << "=== Legacy vs AMT Parity Test ===" << std::endl;
    std::cout << "Simulates both systems on identical bar data" << std::endl << std::endl;

    const double anchor = 5000.0;
    const double tickSize = 0.25;
    const double haloMult = 1.5;
    const int timeoutBars = 3;

    int passed = 0;
    int failed = 0;

    // Scenario 1
    {
        std::cout << "Scenario 1: Clean single engagement..." << std::endl;
        auto bars = GenerateTestBars(anchor, tickSize, 1);
        auto legacy = RunLegacySimulation(bars, tickSize, haloMult, timeoutBars);
        auto amt = RunAmtSimulation(bars, tickSize, haloMult, timeoutBars);

        std::cout << "  Legacy engagements: " << legacy.size() << std::endl;
        std::cout << "  AMT engagements: " << amt.size() << std::endl;

        for (size_t i = 0; i < legacy.size(); i++) {
            PrintEngagement("Legacy", i, legacy[i].entryBar, legacy[i].exitBar,
                legacy[i].barsEngaged, legacy[i].escapeVelocity);
        }
        for (size_t i = 0; i < amt.size(); i++) {
            PrintEngagement("AMT", i, amt[i].startBar, amt[i].endBar,
                amt[i].barsEngaged, amt[i].escapeVelocity);
        }

        if (CompareEngagements(legacy, amt)) {
            std::cout << "  [PASS]" << std::endl;
            passed++;
        } else {
            std::cout << "  [FAIL]" << std::endl;
            failed++;
        }
        std::cout << std::endl;
    }

    // Scenario 2
    {
        std::cout << "Scenario 2: Two separate engagements..." << std::endl;
        auto bars = GenerateTestBars(anchor, tickSize, 2);
        auto legacy = RunLegacySimulation(bars, tickSize, haloMult, timeoutBars);
        auto amt = RunAmtSimulation(bars, tickSize, haloMult, timeoutBars);

        std::cout << "  Legacy engagements: " << legacy.size() << std::endl;
        std::cout << "  AMT engagements: " << amt.size() << std::endl;

        for (size_t i = 0; i < legacy.size(); i++) {
            PrintEngagement("Legacy", i, legacy[i].entryBar, legacy[i].exitBar,
                legacy[i].barsEngaged, legacy[i].escapeVelocity);
        }
        for (size_t i = 0; i < amt.size(); i++) {
            PrintEngagement("AMT", i, amt[i].startBar, amt[i].endBar,
                amt[i].barsEngaged, amt[i].escapeVelocity);
        }

        if (CompareEngagements(legacy, amt)) {
            std::cout << "  [PASS]" << std::endl;
            passed++;
        } else {
            std::cout << "  [FAIL]" << std::endl;
            failed++;
        }
        std::cout << std::endl;
    }

    // Scenario 3
    {
        std::cout << "Scenario 3: Brief touch then exit..." << std::endl;
        auto bars = GenerateTestBars(anchor, tickSize, 3);
        auto legacy = RunLegacySimulation(bars, tickSize, haloMult, timeoutBars);
        auto amt = RunAmtSimulation(bars, tickSize, haloMult, timeoutBars);

        std::cout << "  Legacy engagements: " << legacy.size() << std::endl;
        std::cout << "  AMT engagements: " << amt.size() << std::endl;

        for (size_t i = 0; i < legacy.size(); i++) {
            PrintEngagement("Legacy", i, legacy[i].entryBar, legacy[i].exitBar,
                legacy[i].barsEngaged, legacy[i].escapeVelocity);
        }
        for (size_t i = 0; i < amt.size(); i++) {
            PrintEngagement("AMT", i, amt[i].startBar, amt[i].endBar,
                amt[i].barsEngaged, amt[i].escapeVelocity);
        }

        if (CompareEngagements(legacy, amt)) {
            std::cout << "  [PASS]" << std::endl;
            passed++;
        } else {
            std::cout << "  [FAIL]" << std::endl;
            failed++;
        }
        std::cout << std::endl;
    }

    // Summary
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;

    if (failed == 0) {
        std::cout << std::endl << "Legacy and AMT produce IDENTICAL results!" << std::endl;
    } else {
        std::cout << std::endl << "PARITY FAILURES - these show semantic drift to investigate" << std::endl;
    }

    return failed;
}
