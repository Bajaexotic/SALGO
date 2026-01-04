// ============================================================================
// test_spatial_liquidity.cpp - Unit tests for SpatialLiquidityProfile
// ============================================================================
//
// Tests for spatial liquidity analysis:
// - Wall detection (depth > 2.5σ)
// - Void detection (depth < 10% mean)
// - OBI calculation
// - POLR direction
// - Kyle's Lambda slippage estimation
// - Trade gating logic
//
// Build: g++ -std=c++17 -o test_spatial_liquidity.exe test_spatial_liquidity.cpp
// Run: ./test_spatial_liquidity.exe
// ============================================================================

#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <string>
#include <algorithm>
#include <utility>

// ============================================================================
// COPY OF SPATIAL STRUCTS FOR STANDALONE TESTING
// (These mirror the definitions in AMT_Liquidity.h)
// ============================================================================

enum class SpatialErrorReason : int {
    NONE = 0,
    ERR_NO_LEVEL_DATA = 1,
    ERR_INVALID_REF_PRICE = 2,
    ERR_INVALID_TICK_SIZE = 3,
    WARMUP_DEPTH_BASELINE = 10,
    INSUFFICIENT_LEVELS = 20,
    ONE_SIDED_BOOK = 21
};

struct LevelInfo {
    double priceTicks = 0.0;
    double volume = 0.0;
    double distanceTicks = 0.0;
    double weight = 0.0;
    bool isBid = true;
};

struct WallInfo {
    double priceTicks = 0.0;
    double volume = 0.0;
    double sigmaScore = 0.0;
    int distanceFromRef = 0;
    bool isBid = true;
    bool isIceberg = false;

    bool IsSignificant() const { return sigmaScore >= 2.5; }
    bool IsStrong() const { return sigmaScore >= 3.0; }
    bool IsExtreme() const { return sigmaScore >= 4.0; }
};

struct VoidInfo {
    double startTicks = 0.0;
    double endTicks = 0.0;
    int gapTicks = 0;
    double avgDepthRatio = 0.0;
    bool isAboveRef = true;

    bool IsVoid() const { return avgDepthRatio < 0.10; }
    bool IsThin() const { return avgDepthRatio < 0.25 && avgDepthRatio >= 0.10; }
};

struct DirectionalResistance {
    double bidDepthWithinN = 0.0;
    double askDepthWithinN = 0.0;
    int rangeTicksUsed = 0;
    double orderBookImbalance = 0.0;
    double polrRatio = 0.0;
    bool polrIsUp = true;
    bool valid = false;

    double GetDirectionalBias() const {
        if (!valid) return 0.0;
        const double total = bidDepthWithinN + askDepthWithinN;
        if (total < 1.0) return 0.0;
        return (bidDepthWithinN - askDepthWithinN) / total;
    }
};

struct ExecutionRiskEstimate {
    int targetTicks = 0;
    double estimatedSlippageTicks = 0.0;
    double cumulativeDepth = 0.0;
    double kyleLambda = 0.0;
    int wallsTraversed = 0;
    int voidsTraversed = 0;
    bool isHighRisk = false;
    bool hasWallBlock = false;
    bool hasVoidAcceleration = false;
    bool valid = false;
};

struct SpatialTradeGating {
    bool longBlocked = false;
    double longRiskMultiplier = 1.0;
    bool shortBlocked = false;
    double shortRiskMultiplier = 1.0;
    bool blockedByBidWall = false;
    bool blockedByAskWall = false;
    bool acceleratedByBidVoid = false;
    bool acceleratedByAskVoid = false;
    bool valid = false;

    bool AnyBlocked() const { return longBlocked || shortBlocked; }
    bool HasAcceleration() const { return acceleratedByBidVoid || acceleratedByAskVoid; }
};

struct SpatialLiquidityProfile {
    std::vector<LevelInfo> bidLevels;
    std::vector<LevelInfo> askLevels;
    double referencePrice = 0.0;
    double tickSize = 0.0;

    double meanDepth = 0.0;
    double stddevDepth = 0.0;
    bool statsValid = false;

    std::vector<WallInfo> walls;
    int bidWallCount = 0;
    int askWallCount = 0;
    double nearestBidWallTicks = -1.0;
    double nearestAskWallTicks = -1.0;

    std::vector<VoidInfo> voids;
    int bidVoidCount = 0;
    int askVoidCount = 0;
    double nearestBidVoidTicks = -1.0;
    double nearestAskVoidTicks = -1.0;

    DirectionalResistance direction;

    ExecutionRiskEstimate riskUp;
    ExecutionRiskEstimate riskDown;

    SpatialTradeGating gating;

    bool valid = false;
    SpatialErrorReason errorReason = SpatialErrorReason::NONE;
    int errorBar = -1;
    bool wallBaselineReady = false;

    bool IsReady() const { return valid; }
    bool HasWalls() const { return !walls.empty(); }
    bool HasVoids() const { return !voids.empty(); }
    bool HasBidWall() const { return bidWallCount > 0; }
    bool HasAskWall() const { return askWallCount > 0; }
    bool HasBidVoid() const { return bidVoidCount > 0; }
    bool HasAskVoid() const { return askVoidCount > 0; }

    int GetPOLRDirection() const {
        if (!direction.valid) return 0;
        const double bias = direction.GetDirectionalBias();
        if (bias > 0.15) return 1;
        if (bias < -0.15) return -1;
        return 0;
    }

    const char* GetPOLRString() const {
        const int dir = GetPOLRDirection();
        if (dir > 0) return "UP";
        if (dir < 0) return "DOWN";
        return "BAL";
    }
};

struct SpatialConfig {
    int analysisRangeTicks = 10;
    int riskTargetTicks = 4;
    double wallSigmaThreshold = 2.5;
    double voidDepthRatio = 0.10;
    double thinDepthRatio = 0.25;
    size_t minLevelsForStats = 3;
    double polrBiasThreshold = 0.15;
    double highRiskSlippage = 2.0;
    double wallBlockDistance = 3;
};

// ============================================================================
// STANDALONE COMPUTE FUNCTION (COPY OF LOGIC FROM AMT_Liquidity.h)
// ============================================================================

SpatialLiquidityProfile ComputeSpatialProfile(
    const std::vector<std::pair<double, double>>& bidLevels,
    const std::vector<std::pair<double, double>>& askLevels,
    double referencePrice,
    double tickSize,
    const SpatialConfig& config,
    int barIndex = -1
) {
    SpatialLiquidityProfile result;
    result.referencePrice = referencePrice;
    result.tickSize = tickSize;
    result.errorBar = barIndex;

    if (referencePrice <= 0.0) {
        result.errorReason = SpatialErrorReason::ERR_INVALID_REF_PRICE;
        return result;
    }
    if (tickSize <= 0.0) {
        result.errorReason = SpatialErrorReason::ERR_INVALID_TICK_SIZE;
        return result;
    }
    if (bidLevels.empty() && askLevels.empty()) {
        result.errorReason = SpatialErrorReason::ERR_NO_LEVEL_DATA;
        return result;
    }

    const int analysisRange = config.analysisRangeTicks;
    std::vector<double> allDepths;

    // Process bid levels
    for (const auto& level : bidLevels) {
        const double price = level.first;
        const double volume = level.second;
        if (price <= 0.0 || volume <= 0.0) continue;

        const double distTicks = (referencePrice - price) / tickSize;
        if (distTicks < 0.0 || distTicks > static_cast<double>(analysisRange)) continue;

        LevelInfo info;
        info.priceTicks = price / tickSize;
        info.volume = volume;
        info.distanceTicks = distTicks;
        info.weight = 1.0 / (1.0 + distTicks);
        info.isBid = true;
        result.bidLevels.push_back(info);
        allDepths.push_back(volume);
    }

    // Process ask levels
    for (const auto& level : askLevels) {
        const double price = level.first;
        const double volume = level.second;
        if (price <= 0.0 || volume <= 0.0) continue;

        const double distTicks = (price - referencePrice) / tickSize;
        if (distTicks < 0.0 || distTicks > static_cast<double>(analysisRange)) continue;

        LevelInfo info;
        info.priceTicks = price / tickSize;
        info.volume = volume;
        info.distanceTicks = distTicks;
        info.weight = 1.0 / (1.0 + distTicks);
        info.isBid = false;
        result.askLevels.push_back(info);
        allDepths.push_back(volume);
    }

    if (result.bidLevels.size() < config.minLevelsForStats &&
        result.askLevels.size() < config.minLevelsForStats) {
        result.errorReason = SpatialErrorReason::INSUFFICIENT_LEVELS;
        return result;
    }

    // Compute mean and stddev
    if (allDepths.size() >= config.minLevelsForStats) {
        double sum = 0.0;
        for (double d : allDepths) sum += d;
        result.meanDepth = sum / static_cast<double>(allDepths.size());

        double sumSq = 0.0;
        for (double d : allDepths) {
            const double diff = d - result.meanDepth;
            sumSq += diff * diff;
        }
        result.stddevDepth = std::sqrt(sumSq / static_cast<double>(allDepths.size()));
        result.statsValid = (result.stddevDepth > 0.0);
    }

    // Detect walls
    if (result.statsValid && result.stddevDepth > 0.0) {
        result.wallBaselineReady = true;

        for (const auto& level : result.bidLevels) {
            const double sigmaScore = (level.volume - result.meanDepth) / result.stddevDepth;
            if (sigmaScore >= config.wallSigmaThreshold) {
                WallInfo wall;
                wall.priceTicks = level.priceTicks;
                wall.volume = level.volume;
                wall.sigmaScore = sigmaScore;
                wall.distanceFromRef = static_cast<int>(level.distanceTicks);
                wall.isBid = true;
                result.walls.push_back(wall);
                result.bidWallCount++;

                if (result.nearestBidWallTicks < 0.0 ||
                    level.distanceTicks < result.nearestBidWallTicks) {
                    result.nearestBidWallTicks = level.distanceTicks;
                }
            }
        }

        for (const auto& level : result.askLevels) {
            const double sigmaScore = (level.volume - result.meanDepth) / result.stddevDepth;
            if (sigmaScore >= config.wallSigmaThreshold) {
                WallInfo wall;
                wall.priceTicks = level.priceTicks;
                wall.volume = level.volume;
                wall.sigmaScore = sigmaScore;
                wall.distanceFromRef = static_cast<int>(level.distanceTicks);
                wall.isBid = false;
                result.walls.push_back(wall);
                result.askWallCount++;

                if (result.nearestAskWallTicks < 0.0 ||
                    level.distanceTicks < result.nearestAskWallTicks) {
                    result.nearestAskWallTicks = level.distanceTicks;
                }
            }
        }
    }

    // Detect voids
    if (result.statsValid && result.meanDepth > 0.0) {
        const double voidThreshold = result.meanDepth * config.voidDepthRatio;

        for (const auto& level : result.bidLevels) {
            if (level.volume < voidThreshold) {
                VoidInfo voidArea;
                voidArea.startTicks = level.priceTicks;
                voidArea.endTicks = level.priceTicks;
                voidArea.gapTicks = 1;
                voidArea.avgDepthRatio = level.volume / result.meanDepth;
                voidArea.isAboveRef = false;
                result.voids.push_back(voidArea);
                result.bidVoidCount++;

                if (result.nearestBidVoidTicks < 0.0 ||
                    level.distanceTicks < result.nearestBidVoidTicks) {
                    result.nearestBidVoidTicks = level.distanceTicks;
                }
            }
        }

        for (const auto& level : result.askLevels) {
            if (level.volume < voidThreshold) {
                VoidInfo voidArea;
                voidArea.startTicks = level.priceTicks;
                voidArea.endTicks = level.priceTicks;
                voidArea.gapTicks = 1;
                voidArea.avgDepthRatio = level.volume / result.meanDepth;
                voidArea.isAboveRef = true;
                result.voids.push_back(voidArea);
                result.askVoidCount++;

                if (result.nearestAskVoidTicks < 0.0 ||
                    level.distanceTicks < result.nearestAskVoidTicks) {
                    result.nearestAskVoidTicks = level.distanceTicks;
                }
            }
        }
    }

    // Compute OBI and POLR
    double bidDepthTotal = 0.0;
    double askDepthTotal = 0.0;

    for (const auto& level : result.bidLevels) {
        bidDepthTotal += level.volume * level.weight;
    }
    for (const auto& level : result.askLevels) {
        askDepthTotal += level.volume * level.weight;
    }

    result.direction.bidDepthWithinN = bidDepthTotal;
    result.direction.askDepthWithinN = askDepthTotal;
    result.direction.rangeTicksUsed = analysisRange;

    const double totalDepth = bidDepthTotal + askDepthTotal;
    if (totalDepth > 0.0) {
        result.direction.orderBookImbalance = (bidDepthTotal - askDepthTotal) / totalDepth;
        result.direction.polrIsUp = (bidDepthTotal > askDepthTotal);

        const double minDepth = std::min(bidDepthTotal, askDepthTotal);
        const double maxDepth = std::max(bidDepthTotal, askDepthTotal);
        result.direction.polrRatio = (maxDepth > 0.0) ? minDepth / maxDepth : 1.0;
        result.direction.valid = true;
    }

    // Execution risk
    const int riskTarget = config.riskTargetTicks;

    result.riskUp.targetTicks = riskTarget;
    double askDepthInTarget = 0.0;
    for (const auto& level : result.askLevels) {
        if (level.distanceTicks <= static_cast<double>(riskTarget)) {
            askDepthInTarget += level.volume;
        }
    }
    result.riskUp.cumulativeDepth = askDepthInTarget;
    if (askDepthInTarget > 0.0) {
        result.riskUp.kyleLambda = 1.0 / askDepthInTarget;
        result.riskUp.estimatedSlippageTicks = riskTarget * result.riskUp.kyleLambda * 100.0;
        result.riskUp.estimatedSlippageTicks = std::min(result.riskUp.estimatedSlippageTicks, 10.0);
    }
    result.riskUp.valid = true;

    result.riskDown.targetTicks = riskTarget;
    double bidDepthInTarget = 0.0;
    for (const auto& level : result.bidLevels) {
        if (level.distanceTicks <= static_cast<double>(riskTarget)) {
            bidDepthInTarget += level.volume;
        }
    }
    result.riskDown.cumulativeDepth = bidDepthInTarget;
    if (bidDepthInTarget > 0.0) {
        result.riskDown.kyleLambda = 1.0 / bidDepthInTarget;
        result.riskDown.estimatedSlippageTicks = riskTarget * result.riskDown.kyleLambda * 100.0;
        result.riskDown.estimatedSlippageTicks = std::min(result.riskDown.estimatedSlippageTicks, 10.0);
    }
    result.riskDown.valid = true;

    // Trade gating
    result.gating.valid = true;

    if (result.nearestAskWallTicks >= 0.0 &&
        result.nearestAskWallTicks <= config.wallBlockDistance) {
        for (const auto& wall : result.walls) {
            if (!wall.isBid && wall.IsStrong() &&
                wall.distanceFromRef <= static_cast<int>(config.wallBlockDistance)) {
                result.gating.longBlocked = true;
                result.gating.blockedByAskWall = true;
                break;
            }
        }
    }

    if (result.nearestBidWallTicks >= 0.0 &&
        result.nearestBidWallTicks <= config.wallBlockDistance) {
        for (const auto& wall : result.walls) {
            if (wall.isBid && wall.IsStrong() &&
                wall.distanceFromRef <= static_cast<int>(config.wallBlockDistance)) {
                result.gating.shortBlocked = true;
                result.gating.blockedByBidWall = true;
                break;
            }
        }
    }

    result.valid = true;
    result.errorReason = SpatialErrorReason::NONE;

    return result;
}

// ============================================================================
// TEST FRAMEWORK
// ============================================================================

static int g_testsRun = 0;
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running: " << #name << "... "; \
    try { test_##name(); std::cout << "PASS\n"; g_testsPassed++; } \
    catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; g_testsFailed++; } \
    g_testsRun++; \
} while(0)

#define ASSERT_TRUE(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)

#define ASSERT_FALSE(cond) \
    if (cond) throw std::runtime_error("Assertion failed: NOT " #cond)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b)

#define ASSERT_NEAR(a, b, tol) \
    if (std::abs((a) - (b)) > (tol)) throw std::runtime_error("Assertion failed: " #a " near " #b)

// ============================================================================
// TEST CASES
// ============================================================================

TEST(EmptyLevels_ReturnsNoLevelDataError) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> emptyBids;
    std::vector<std::pair<double, double>> emptyAsks;

    auto result = ComputeSpatialProfile(emptyBids, emptyAsks, 6000.0, 0.25, config, 100);

    ASSERT_FALSE(result.valid);
    ASSERT_EQ(static_cast<int>(result.errorReason), static_cast<int>(SpatialErrorReason::ERR_NO_LEVEL_DATA));
}

TEST(InvalidRefPrice_ReturnsError) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {{5999.75, 100}};
    std::vector<std::pair<double, double>> asks = {{6000.25, 100}};

    auto result = ComputeSpatialProfile(bids, asks, 0.0, 0.25, config, 100);

    ASSERT_FALSE(result.valid);
    ASSERT_EQ(static_cast<int>(result.errorReason), static_cast<int>(SpatialErrorReason::ERR_INVALID_REF_PRICE));
}

TEST(InvalidTickSize_ReturnsError) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {{5999.75, 100}};
    std::vector<std::pair<double, double>> asks = {{6000.25, 100}};

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.0, config, 100);

    ASSERT_FALSE(result.valid);
    ASSERT_EQ(static_cast<int>(result.errorReason), static_cast<int>(SpatialErrorReason::ERR_INVALID_TICK_SIZE));
}

TEST(BalancedBook_OBINearZero) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {
        {5999.75, 100}, {5999.50, 100}, {5999.25, 100}, {5999.00, 100}
    };
    std::vector<std::pair<double, double>> asks = {
        {6000.25, 100}, {6000.50, 100}, {6000.75, 100}, {6001.00, 100}
    };

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.25, config, 100);

    ASSERT_TRUE(result.valid);
    ASSERT_NEAR(result.direction.orderBookImbalance, 0.0, 0.05);
    ASSERT_EQ(result.GetPOLRDirection(), 0);
}

TEST(BidHeavyBook_PositiveOBI) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {
        {5999.75, 500}, {5999.50, 500}, {5999.25, 500}, {5999.00, 500}
    };
    std::vector<std::pair<double, double>> asks = {
        {6000.25, 100}, {6000.50, 100}, {6000.75, 100}, {6001.00, 100}
    };

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.25, config, 100);

    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.direction.orderBookImbalance > 0.15);
    ASSERT_EQ(result.GetPOLRDirection(), 1);
}

TEST(AskHeavyBook_NegativeOBI) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {
        {5999.75, 100}, {5999.50, 100}, {5999.25, 100}, {5999.00, 100}
    };
    std::vector<std::pair<double, double>> asks = {
        {6000.25, 500}, {6000.50, 500}, {6000.75, 500}, {6001.00, 500}
    };

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.25, config, 100);

    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.direction.orderBookImbalance < -0.15);
    ASSERT_EQ(result.GetPOLRDirection(), -1);
}

TEST(WallDetection_HighSigmaLevelIsWall) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {
        {5999.75, 100}, {5999.50, 100}, {5999.25, 100}, {5999.00, 1000}
    };
    std::vector<std::pair<double, double>> asks = {
        {6000.25, 100}, {6000.50, 100}, {6000.75, 100}, {6001.00, 100}
    };

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.25, config, 100);

    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.HasWalls());
    ASSERT_TRUE(result.bidWallCount >= 1);
}

TEST(VoidDetection_LowDepthLevelIsVoid) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {
        {5999.75, 1000}, {5999.50, 10}, {5999.25, 1000}, {5999.00, 1000}
    };
    std::vector<std::pair<double, double>> asks = {
        {6000.25, 1000}, {6000.50, 1000}, {6000.75, 1000}, {6001.00, 1000}
    };

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.25, config, 100);

    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.HasVoids());
    ASSERT_TRUE(result.bidVoidCount >= 1);
}

TEST(ExecutionRisk_ThinBookHighSlippage) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {
        {5999.75, 10}, {5999.50, 10}, {5999.25, 10}, {5999.00, 10}
    };
    std::vector<std::pair<double, double>> asks = {
        {6000.25, 10}, {6000.50, 10}, {6000.75, 10}, {6001.00, 10}
    };

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.25, config, 100);

    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.riskUp.estimatedSlippageTicks > 0.0);
    ASSERT_TRUE(result.riskDown.estimatedSlippageTicks > 0.0);
}

TEST(ExecutionRisk_DeepBookLowSlippage) {
    SpatialConfig config;
    std::vector<std::pair<double, double>> bids = {
        {5999.75, 10000}, {5999.50, 10000}, {5999.25, 10000}, {5999.00, 10000}
    };
    std::vector<std::pair<double, double>> asks = {
        {6000.25, 10000}, {6000.50, 10000}, {6000.75, 10000}, {6001.00, 10000}
    };

    auto result = ComputeSpatialProfile(bids, asks, 6000.0, 0.25, config, 100);

    ASSERT_TRUE(result.valid);
    ASSERT_TRUE(result.riskUp.estimatedSlippageTicks < 1.0);
    ASSERT_TRUE(result.riskDown.estimatedSlippageTicks < 1.0);
}

TEST(WallInfo_SigmaClassification) {
    WallInfo wall;

    wall.sigmaScore = 2.0;
    ASSERT_FALSE(wall.IsSignificant());
    ASSERT_FALSE(wall.IsStrong());
    ASSERT_FALSE(wall.IsExtreme());

    wall.sigmaScore = 2.7;
    ASSERT_TRUE(wall.IsSignificant());
    ASSERT_FALSE(wall.IsStrong());

    wall.sigmaScore = 3.5;
    ASSERT_TRUE(wall.IsSignificant());
    ASSERT_TRUE(wall.IsStrong());
    ASSERT_FALSE(wall.IsExtreme());

    wall.sigmaScore = 4.5;
    ASSERT_TRUE(wall.IsExtreme());
}

TEST(VoidInfo_Classification) {
    VoidInfo voidArea;

    voidArea.avgDepthRatio = 0.05;
    ASSERT_TRUE(voidArea.IsVoid());
    ASSERT_FALSE(voidArea.IsThin());

    voidArea.avgDepthRatio = 0.15;
    ASSERT_FALSE(voidArea.IsVoid());
    ASSERT_TRUE(voidArea.IsThin());

    voidArea.avgDepthRatio = 0.50;
    ASSERT_FALSE(voidArea.IsVoid());
    ASSERT_FALSE(voidArea.IsThin());
}

TEST(DirectionalResistance_BiasCalculation) {
    DirectionalResistance dir;

    dir.valid = false;
    ASSERT_NEAR(dir.GetDirectionalBias(), 0.0, 0.01);

    dir.valid = true;
    dir.bidDepthWithinN = 1000.0;
    dir.askDepthWithinN = 1000.0;
    ASSERT_NEAR(dir.GetDirectionalBias(), 0.0, 0.01);

    dir.bidDepthWithinN = 1500.0;
    dir.askDepthWithinN = 500.0;
    ASSERT_TRUE(dir.GetDirectionalBias() > 0.4);

    dir.bidDepthWithinN = 500.0;
    dir.askDepthWithinN = 1500.0;
    ASSERT_TRUE(dir.GetDirectionalBias() < -0.4);
}

TEST(SpatialTradeGating_Helpers) {
    SpatialTradeGating gating;

    ASSERT_FALSE(gating.AnyBlocked());
    ASSERT_FALSE(gating.HasAcceleration());

    gating.longBlocked = true;
    ASSERT_TRUE(gating.AnyBlocked());

    gating.longBlocked = false;
    gating.acceleratedByAskVoid = true;
    ASSERT_TRUE(gating.HasAcceleration());
}

TEST(HelperMethods_WorkCorrectly) {
    SpatialLiquidityProfile profile;

    ASSERT_FALSE(profile.IsReady());
    ASSERT_FALSE(profile.HasWalls());
    ASSERT_FALSE(profile.HasVoids());
    ASSERT_FALSE(profile.HasBidWall());
    ASSERT_FALSE(profile.HasAskWall());

    profile.valid = true;
    profile.walls.push_back(WallInfo{});
    profile.bidWallCount = 1;

    ASSERT_TRUE(profile.IsReady());
    ASSERT_TRUE(profile.HasWalls());
    ASSERT_TRUE(profile.HasBidWall());
    ASSERT_FALSE(profile.HasAskWall());
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "  Spatial Liquidity Profile Tests\n";
    std::cout << "========================================\n\n";

    RUN_TEST(EmptyLevels_ReturnsNoLevelDataError);
    RUN_TEST(InvalidRefPrice_ReturnsError);
    RUN_TEST(InvalidTickSize_ReturnsError);
    RUN_TEST(BalancedBook_OBINearZero);
    RUN_TEST(BidHeavyBook_PositiveOBI);
    RUN_TEST(AskHeavyBook_NegativeOBI);
    RUN_TEST(WallDetection_HighSigmaLevelIsWall);
    RUN_TEST(VoidDetection_LowDepthLevelIsVoid);
    RUN_TEST(ExecutionRisk_ThinBookHighSlippage);
    RUN_TEST(ExecutionRisk_DeepBookLowSlippage);
    RUN_TEST(WallInfo_SigmaClassification);
    RUN_TEST(VoidInfo_Classification);
    RUN_TEST(DirectionalResistance_BiasCalculation);
    RUN_TEST(SpatialTradeGating_Helpers);
    RUN_TEST(HelperMethods_WorkCorrectly);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << g_testsPassed << " passed, "
              << g_testsFailed << " failed, " << g_testsRun << " total\n";
    std::cout << "========================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
