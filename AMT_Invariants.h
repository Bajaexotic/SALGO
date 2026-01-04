// ============================================================================
// AMT_Invariants.h
// Runtime assertions for SSOT (Single Source of Truth) invariants
// ============================================================================
//
// This header provides macros and utilities for runtime validation of SSOT
// relationships documented in CLAUDE.md. These assertions help catch bugs
// where derived values drift from their source of truth.
//
// USAGE:
//   #define AMT_SSOT_ASSERTIONS 1  // Enable in debug builds
//   #include "AMT_Invariants.h"
//
//   // In code that updates SSOT:
//   AMT_SSOT_ASSERT(zone.anchorPrice == zone.anchorTicks * tickSize,
//                   "anchorPrice drift");
//
// ============================================================================

#ifndef AMT_INVARIANTS_H
#define AMT_INVARIANTS_H

#include <cmath>
#include <cstdio>

namespace AMT {

// ============================================================================
// CONFIGURATION
// ============================================================================

// Enable/disable SSOT assertions (define before including this header)
#ifndef AMT_SSOT_ASSERTIONS
#ifdef _DEBUG
#define AMT_SSOT_ASSERTIONS 1
#else
#define AMT_SSOT_ASSERTIONS 0
#endif
#endif

// ============================================================================
// ASSERTION MACROS
// ============================================================================

#if AMT_SSOT_ASSERTIONS

// Basic SSOT assertion - logs and optionally halts on violation
// NOTE: Handler functions are in global namespace (not AMT::)
#define AMT_SSOT_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            ::InvariantViolation(__FILE__, __LINE__, #condition, message); \
        } \
    } while (0)

// SSOT assertion with value logging
#define AMT_SSOT_ASSERT_EQ(actual, expected, epsilon, message) \
    do { \
        if (std::abs((actual) - (expected)) > (epsilon)) { \
            ::InvariantViolationWithValues(__FILE__, __LINE__, \
                #actual, static_cast<double>(actual), \
                #expected, static_cast<double>(expected), message); \
        } \
    } while (0)

// SSOT range assertion
#define AMT_SSOT_ASSERT_RANGE(value, min_val, max_val, message) \
    do { \
        if ((value) < (min_val) || (value) > (max_val)) { \
            ::InvariantRangeViolation(__FILE__, __LINE__, \
                #value, static_cast<double>(value), \
                static_cast<double>(min_val), static_cast<double>(max_val), message); \
        } \
    } while (0)

#else

// No-op when assertions are disabled
#define AMT_SSOT_ASSERT(condition, message) ((void)0)
#define AMT_SSOT_ASSERT_EQ(actual, expected, epsilon, message) ((void)0)
#define AMT_SSOT_ASSERT_RANGE(value, min_val, max_val, message) ((void)0)

#endif

} // namespace AMT (temporarily closed for SC integration)

// ============================================================================
// VIOLATION HANDLERS (SC-Compliant)
// These functions are outside AMT namespace to use global s_sc type
// ============================================================================

// Forward declare SC interface for optional SC logging (global namespace)
struct s_sc;

// Global SC pointer for violation logging (set by study on init)
// When set, violations route to SC message log; otherwise stderr fallback
inline s_sc*& GetSSOTLogContext() {
    static s_sc* g_ssotSC = nullptr;
    return g_ssotSC;
}

inline void SetSSOTLogContext(s_sc* sc) {
    GetSSOTLogContext() = sc;
}

// SC-compliant violation handler - routes to SC message log when available
inline void InvariantViolation(const char* file, int line,
                               const char* condition, const char* message) {
    // Extract just the filename from full path
    const char* filename = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') filename = p + 1;
    }

    s_sc* sc = GetSSOTLogContext();
    if (sc) {
        // Use SC logging - format via static buffer to avoid SCString in header
        char buf[512];
        snprintf(buf, sizeof(buf), "[SSOT-VIOLATION] %s:%d - %s | Condition: %s",
                 filename, line, message, condition);
        sc->AddMessageToLog(buf, 1);  // 1 = warning
    } else {
        // Fallback for unit tests or when SC not available
        fprintf(stderr, "[SSOT-VIOLATION] %s:%d - %s\n  Condition: %s\n",
                filename, line, message, condition);
    }

#ifdef _DEBUG
    // In debug builds, you might want to break here
    // __debugbreak();  // Windows
#endif
}

inline void InvariantViolationWithValues(const char* file, int line,
                                         const char* actualName, double actualValue,
                                         const char* expectedName, double expectedValue,
                                         const char* message) {
    const char* filename = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') filename = p + 1;
    }

    s_sc* sc = GetSSOTLogContext();
    if (sc) {
        char buf[512];
        snprintf(buf, sizeof(buf), "[SSOT-VIOLATION] %s:%d - %s | %s=%.6f, %s=%.6f",
                 filename, line, message, actualName, actualValue, expectedName, expectedValue);
        sc->AddMessageToLog(buf, 1);
    } else {
        fprintf(stderr, "[SSOT-VIOLATION] %s:%d - %s\n  %s=%.6f, %s=%.6f\n",
                filename, line, message, actualName, actualValue, expectedName, expectedValue);
    }
}

inline void InvariantRangeViolation(const char* file, int line,
                                    const char* valueName, double value,
                                    double minVal, double maxVal,
                                    const char* message) {
    const char* filename = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') filename = p + 1;
    }

    s_sc* sc = GetSSOTLogContext();
    if (sc) {
        char buf[512];
        snprintf(buf, sizeof(buf), "[SSOT-VIOLATION] %s:%d - %s | %s=%.6f not in [%.6f, %.6f]",
                 filename, line, message, valueName, value, minVal, maxVal);
        sc->AddMessageToLog(buf, 1);
    } else {
        fprintf(stderr, "[SSOT-VIOLATION] %s:%d - %s\n  %s=%.6f not in [%.6f, %.6f]\n",
                filename, line, message, valueName, value, minVal, maxVal);
    }
}

namespace AMT { // Reopen namespace for remaining AMT content

// ============================================================================
// SSOT VALIDATION HELPERS
// ============================================================================

/**
 * Validate zone anchor invariant: anchorPrice == anchorTicks * tickSize
 */
inline bool ValidateZoneAnchorInvariant(long long anchorTicks, double anchorPrice,
                                        double tickSize, double epsilon = 1e-9) {
    if (tickSize <= 0.0) return true;  // Can't validate without tick size
    double expected = anchorTicks * tickSize;
    return std::abs(anchorPrice - expected) < epsilon;
}

/**
 * Validate percentile is in valid range [0, 100]
 */
inline bool ValidatePercentileRange(double percentile) {
    return percentile >= 0.0 && percentile <= 100.0;
}

/**
 * Validate price is positive (for non-nullable price fields)
 */
inline bool ValidatePricePositive(double price) {
    return price > 0.0;
}

/**
 * Validate session levels are ordered: VAL < POC < VAH
 */
inline bool ValidateSessionLevelOrder(double poc, double vah, double val) {
    return val < poc && poc < vah;
}

// ============================================================================
// SSOT CHECKPOINT (for periodic validation)
// ============================================================================

/**
 * SSOT checkpoint that validates multiple invariants at once.
 * Call periodically (e.g., every N bars) in debug builds.
 */
struct SSOTCheckpoint {
    int violationCount = 0;

    void CheckZoneAnchor(long long ticks, double price, double tickSize) {
        if (!ValidateZoneAnchorInvariant(ticks, price, tickSize)) {
            violationCount++;
            AMT_SSOT_ASSERT_EQ(price, ticks * tickSize, 1e-9, "Zone anchor drift");
        }
    }

    void CheckPercentile(double value, const char* name) {
        if (!ValidatePercentileRange(value)) {
            violationCount++;
            AMT_SSOT_ASSERT_RANGE(value, 0.0, 100.0, name);
        }
    }

    void CheckSessionLevels(double poc, double vah, double val) {
        if (!ValidateSessionLevelOrder(poc, vah, val)) {
            violationCount++;
            AMT_SSOT_ASSERT(val < poc && poc < vah, "Session level order: VAL < POC < VAH");
        }
    }

    bool HasViolations() const { return violationCount > 0; }
    void Reset() { violationCount = 0; }
};

} // namespace AMT

#endif // AMT_INVARIANTS_H
