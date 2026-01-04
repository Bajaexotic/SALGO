// ============================================================================
// AMT_Bridge.h
// Zone type derivation utilities for AMT framework
// ============================================================================

#ifndef AMT_BRIDGE_H
#define AMT_BRIDGE_H

#include "amt_core.h"

namespace AMT {

// ============================================================================
// ZONE ROLE DERIVATION
// Derive ZoneRole from ZoneType (for zones created from VBP data)
// ============================================================================

inline ZoneRole DeriveRoleFromType(ZoneType type) {
    switch (type) {
        case ZoneType::VPB_VAH:
        case ZoneType::VPB_VAL:
        case ZoneType::TPO_VAH:
        case ZoneType::TPO_VAL:
        case ZoneType::PRIOR_VAH:
        case ZoneType::PRIOR_VAL:
            return ZoneRole::VALUE_BOUNDARY;

        case ZoneType::VPB_POC:
        case ZoneType::TPO_POC:
        case ZoneType::PRIOR_POC:
            return ZoneRole::VALUE_CORE;

        case ZoneType::IB_HIGH:
        case ZoneType::IB_LOW:
        case ZoneType::SESSION_HIGH:
        case ZoneType::SESSION_LOW:
            return ZoneRole::RANGE_BOUNDARY;

        case ZoneType::VWAP:
            return ZoneRole::MEAN_REFERENCE;

        default:
            return ZoneRole::MEAN_REFERENCE;
    }
}

// ============================================================================
// ANCHOR MECHANISM DERIVATION
// ============================================================================

inline AnchorMechanism DeriveMechanismFromType(ZoneType type) {
    switch (type) {
        case ZoneType::VPB_POC:
        case ZoneType::VPB_VAH:
        case ZoneType::VPB_VAL:
        case ZoneType::TPO_POC:
        case ZoneType::TPO_VAH:
        case ZoneType::TPO_VAL:
        case ZoneType::PRIOR_POC:
        case ZoneType::PRIOR_VAH:
        case ZoneType::PRIOR_VAL:
            return AnchorMechanism::VOLUME_PROFILE;

        case ZoneType::IB_HIGH:
        case ZoneType::IB_LOW:
        case ZoneType::SESSION_HIGH:
        case ZoneType::SESSION_LOW:
            return AnchorMechanism::TIME_RANGE;

        case ZoneType::VWAP:
            return AnchorMechanism::WEIGHTED_MEAN;

        default:
            return AnchorMechanism::FIXED_LEVEL;
    }
}

// ============================================================================
// ZONE SOURCE DERIVATION
// ============================================================================

inline ZoneSource DeriveSourceFromType(ZoneType type, bool isRTH) {
    // Prior session types always come from prior session
    if (type == ZoneType::PRIOR_POC ||
        type == ZoneType::PRIOR_VAH ||
        type == ZoneType::PRIOR_VAL) {
        return ZoneSource::PRIOR_RTH;
    }

    // VWAP is always intraday calculated
    if (type == ZoneType::VWAP) {
        return ZoneSource::INTRADAY_CALC;
    }

    // Everything else depends on current session
    return isRTH ? ZoneSource::CURRENT_RTH : ZoneSource::CURRENT_GLOBEX;
}

} // namespace AMT

#endif // AMT_BRIDGE_H
