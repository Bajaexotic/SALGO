#!/usr/bin/env python3
"""
Phase System v2 Calibration Data Extractor

Parses Sierra Chart AMT study logs and extracts per-session calibration metrics.

Usage:
    python extract_session_stats.py <logfile.txt> [--output calibration_report.txt]

Input: Sierra Chart message log export (copy/paste from Log window)
Output: Structured calibration report with metrics per session

Requirements: Python 3.6+
"""

import re
import sys
from dataclasses import dataclass, field
from typing import List, Dict, Optional
from datetime import datetime

@dataclass
class SessionStats:
    """Holds calibration metrics for one session."""
    session_date: str = ""
    session_type: str = ""  # RTH_AM, MID_SESS, RTH_PM, etc.
    first_bar: int = 0
    last_bar: int = 0
    total_bars: int = 0

    # Phase distribution (final values)
    pct_rotation: float = 0.0
    pct_testing: float = 0.0
    pct_trending: float = 0.0
    pct_extension: float = 0.0
    pct_pullback: float = 0.0
    pct_failed: float = 0.0
    pct_outside_balance: float = 0.0  # Derived: 100 - sum of others

    # Transition counts
    session_transitions: int = 0
    phase_transitions: int = 0
    state_transitions: int = 0

    # Regime observations (AuctionRegime - legacy four-phase)
    regime_balance_count: int = 0
    regime_transition_count: int = 0
    regime_imbalance_count: int = 0
    regime_failed_count: int = 0
    regime_excess_count: int = 0      # EXCESS regime
    regime_rebalance_count: int = 0   # REBALANCE regime

    # Market State observations (AMTMarketState - SSOT)
    market_state_balance_count: int = 0
    market_state_imbalance_count: int = 0

    # Key events
    pullback_events: int = 0
    acceptance_events: int = 0  # IMBALANCE transitions
    range_ext_events: int = 0

    # Structure
    session_range_ticks: int = 0
    va_range_ticks: int = 0

    # Market state
    market_state: str = ""
    balance_pct: float = 0.0

    # Shape freeze (session-level SSOT - from SHAPE_FREEZE log)
    shape_freeze_bar: int = 0           # Bar where shape was frozen
    shape_day_structure: str = ""       # BALANCED or IMBALANCED
    shape_raw_frozen: str = ""          # Geometric shape at freeze time
    shape_final_frozen: str = ""        # After family constraint (SSOT)
    shape_conflict: bool = False        # True if raw conflicted with structure
    shape_frozen: bool = False          # True if SHAPE_FREEZE was found


def parse_shape_freeze(line: str) -> Dict[str, any]:
    """Parse: SHAPE_FREEZE: t_freeze=123 | STRUCT=BALANCED RAW_FROZEN=D_SHAPED FINAL_FROZEN=D_SHAPED | resolution=ACCEPTED conflict=0"""
    result = {}

    # Extract freeze bar
    match = re.search(r't_freeze=(\d+)', line)
    if match:
        result['freeze_bar'] = int(match.group(1))

    # Extract structure
    match = re.search(r'STRUCT=(\w+)', line)
    if match:
        result['structure'] = match.group(1)

    # Extract raw frozen shape
    match = re.search(r'RAW_FROZEN=(\w+)', line)
    if match:
        result['raw_frozen'] = match.group(1)

    # Extract final frozen shape
    match = re.search(r'FINAL_FROZEN=(\w+)', line)
    if match:
        result['final_frozen'] = match.group(1)

    # Extract conflict flag
    match = re.search(r'conflict=(\d)', line)
    if match:
        result['conflict'] = match.group(1) == '1'

    return result


def parse_phase_distribution(line: str) -> Dict[str, float]:
    """Parse: Phase Distribution: ROT=68.4% TEST=7.9% TREND=7.9% EXT=3.9% PULL=0.0% FAIL=10.5%"""
    result = {}
    patterns = [
        (r'ROT=(\d+\.?\d*)%', 'rotation'),
        (r'TEST=(\d+\.?\d*)%', 'testing'),
        (r'TREND=(\d+\.?\d*)%', 'trending'),
        (r'EXT=(\d+\.?\d*)%', 'extension'),
        (r'PULL=(\d+\.?\d*)%', 'pullback'),
        (r'FAIL=(\d+\.?\d*)%', 'failed'),
    ]
    for pattern, key in patterns:
        match = re.search(pattern, line)
        if match:
            result[key] = float(match.group(1))
    return result


def parse_transitions(line: str) -> Dict[str, int]:
    """Parse: Transitions: Session=1 Phase=9 State=0"""
    result = {}
    patterns = [
        (r'Session=(\d+)', 'session'),
        (r'Phase=(\d+)', 'phase'),
        (r'State=(\d+)', 'state'),
    ]
    for pattern, key in patterns:
        match = re.search(pattern, line)
        if match:
            result[key] = int(match.group(1))
    return result


def parse_regime_phase(line: str) -> Dict[str, str]:
    """Parse: REGIME: RAW=FAILED_AUCTION CONF=TRANSITION | PHASE: RAW=... CONF=..."""
    result = {}
    # Use non-greedy .*? to capture the FIRST CONF= after each marker
    patterns = [
        (r'REGIME:.*?CONF=(\w+)', 'regime_conf'),
        (r'PHASE:.*?CONF=(\w+)', 'phase_conf'),
    ]
    for pattern, key in patterns:
        match = re.search(pattern, line)
        if match:
            result[key] = match.group(1)
    return result


def parse_struct(line: str) -> Dict[str, int]:
    """Parse: Struct: SESS_HI=... RANGE_T=97"""
    result = {}
    match = re.search(r'RANGE_T=(\d+)', line)
    if match:
        result['range_ticks'] = int(match.group(1))
    return result


def parse_va(line: str) -> Dict[str, int]:
    """Parse: VA: POC=... Range=40 ticks"""
    result = {}
    match = re.search(r'Range=(\d+)', line)
    if match:
        result['va_range'] = int(match.group(1))
    return result


def parse_bar_number(line: str) -> Optional[int]:
    """Extract bar number from: Bar 6333 |"""
    match = re.search(r'Bar\s+(\d+)', line)
    if match:
        return int(match.group(1))
    return None


def parse_session_phase(line: str) -> Optional[str]:
    """Extract session phase from: SESS=MID_SESS"""
    match = re.search(r'SESS=(\w+)', line)
    if match:
        return match.group(1)
    return None


def parse_market_state(line: str) -> Dict[str, any]:
    """Parse: Market State: IMBALANCE str=0.20 bars=1
       or legacy: Market State: BALANCE (sess=4771, bal=100%)"""
    result = {}
    # New format: Market State: IMBALANCE str=0.20 bars=1
    match = re.search(r'Market State:\s+(\w+)\s+str=([\d.]+)\s+bars=(\d+)', line)
    if match:
        result['state'] = match.group(1)
        result['strength'] = float(match.group(2))
        result['consecutive_bars'] = int(match.group(3))
        # Derive balance_pct from state (for backward compat)
        result['balance_pct'] = 100.0 if match.group(1) == 'BALANCE' else 0.0
        return result
    # Legacy format: Market State: BALANCE (sess=4771, bal=100%)
    match = re.search(r'Market State:\s+(\w+).*bal=(\d+)%', line)
    if match:
        result['state'] = match.group(1)
        result['balance_pct'] = float(match.group(2))
    return result


def parse_date(line: str) -> Optional[str]:
    """Extract date from log line: 2025-12-29  10:35:59.598"""
    match = re.match(r'(\d{4}-\d{2}-\d{2})', line)
    if match:
        return match.group(1)
    return None


def detect_session_boundaries(lines: List[str]) -> List[tuple]:
    """
    Detect session boundaries based on:
    1. Date changes
    2. Session phase changes (RTH_AM -> RTH_PM etc)
    3. Bar number resets

    Returns list of (start_line_idx, end_line_idx) tuples
    """
    sessions = []
    current_session_start = 0
    current_date = None
    last_bar = -1

    for i, line in enumerate(lines):
        date = parse_date(line)
        bar = parse_bar_number(line)

        # Detect session boundary
        is_boundary = False

        if date and date != current_date:
            if current_date is not None:
                is_boundary = True
            current_date = date

        if bar is not None and bar < last_bar - 100:  # Bar reset detection
            is_boundary = True

        if is_boundary and i > current_session_start + 10:  # Min 10 lines per session
            sessions.append((current_session_start, i - 1))
            current_session_start = i

        if bar is not None:
            last_bar = bar

    # Add final session
    if current_session_start < len(lines) - 1:
        sessions.append((current_session_start, len(lines) - 1))

    return sessions


def extract_session_stats(lines: List[str], start_idx: int, end_idx: int) -> SessionStats:
    """Extract stats from a session's log lines."""
    stats = SessionStats()

    # Track regime/phase counts (AuctionRegime - all 6 values)
    regime_counts = {'BALANCE': 0, 'TRANSITION': 0, 'IMBALANCE': 0,
                     'FAILED_AUCTION': 0, 'EXCESS': 0, 'REBALANCE': 0}
    # Track Market State counts (AMTMarketState - SSOT)
    market_state_counts = {'BALANCE': 0, 'IMBALANCE': 0}
    phase_counts = {'PULLBACK': 0, 'RANGE_EXTENSION': 0}
    prev_regime = None

    for i in range(start_idx, end_idx + 1):
        line = lines[i]

        # Get date
        if not stats.session_date:
            date = parse_date(line)
            if date:
                stats.session_date = date

        # Get bar range
        bar = parse_bar_number(line)
        if bar is not None:
            if stats.first_bar == 0:
                stats.first_bar = bar
            stats.last_bar = bar

        # Get session type
        sess_phase = parse_session_phase(line)
        if sess_phase:
            stats.session_type = sess_phase

        # Phase distribution (take last value)
        if 'Phase Distribution:' in line:
            dist = parse_phase_distribution(line)
            stats.pct_rotation = dist.get('rotation', 0)
            stats.pct_testing = dist.get('testing', 0)
            stats.pct_trending = dist.get('trending', 0)
            stats.pct_extension = dist.get('extension', 0)
            stats.pct_pullback = dist.get('pullback', 0)
            stats.pct_failed = dist.get('failed', 0)

        # Transitions (take last value)
        if 'Transitions:' in line:
            trans = parse_transitions(line)
            stats.session_transitions = trans.get('session', 0)
            stats.phase_transitions = trans.get('phase', 0)
            stats.state_transitions = trans.get('state', 0)

        # Regime/Phase counts
        if 'REGIME:' in line and 'CONF=' in line:
            rp = parse_regime_phase(line)
            regime = rp.get('regime_conf', '')
            phase = rp.get('phase_conf', '')

            if regime in regime_counts:
                regime_counts[regime] += 1

            # Track IMBALANCE transitions (acceptance events)
            if regime == 'IMBALANCE' and prev_regime != 'IMBALANCE':
                stats.acceptance_events += 1
            prev_regime = regime

            if phase in phase_counts:
                phase_counts[phase] += 1

        # Structure
        if 'Struct:' in line and 'RANGE_T=' in line:
            struct = parse_struct(line)
            stats.session_range_ticks = struct.get('range_ticks', 0)

        # VA
        if 'VA:' in line and 'Range=' in line:
            va = parse_va(line)
            stats.va_range_ticks = va.get('va_range', 0)

        # Market state (AMTMarketState - SSOT)
        if 'Market State:' in line:
            ms = parse_market_state(line)
            state = ms.get('state', '')
            stats.market_state = state
            stats.balance_pct = ms.get('balance_pct', 0)
            # Count Market State occurrences
            if state in market_state_counts:
                market_state_counts[state] += 1

        # Shape freeze (ONE per session - this is the SSOT)
        # Only take the first SHAPE_FREEZE found (should be unique per session)
        if 'SHAPE_FREEZE:' in line and not stats.shape_frozen:
            sf = parse_shape_freeze(line)
            stats.shape_freeze_bar = sf.get('freeze_bar', 0)
            stats.shape_day_structure = sf.get('structure', '')
            stats.shape_raw_frozen = sf.get('raw_frozen', '')
            stats.shape_final_frozen = sf.get('final_frozen', '')
            stats.shape_conflict = sf.get('conflict', False)
            stats.shape_frozen = True

    # Set regime counts (AuctionRegime - legacy)
    stats.regime_balance_count = regime_counts['BALANCE']
    stats.regime_transition_count = regime_counts['TRANSITION']
    stats.regime_imbalance_count = regime_counts['IMBALANCE']
    stats.regime_failed_count = regime_counts['FAILED_AUCTION']
    stats.regime_excess_count = regime_counts['EXCESS']
    stats.regime_rebalance_count = regime_counts['REBALANCE']

    # Set Market State counts (AMTMarketState - SSOT)
    stats.market_state_balance_count = market_state_counts['BALANCE']
    stats.market_state_imbalance_count = market_state_counts['IMBALANCE']

    # Set event counts
    stats.pullback_events = phase_counts['PULLBACK']
    stats.range_ext_events = phase_counts['RANGE_EXTENSION']

    # Calculate total bars
    if stats.last_bar > stats.first_bar:
        stats.total_bars = stats.last_bar - stats.first_bar + 1

    # Calculate outside_balance (what's left after other phases)
    accounted = (stats.pct_rotation + stats.pct_testing + stats.pct_trending +
                 stats.pct_extension + stats.pct_pullback + stats.pct_failed)
    stats.pct_outside_balance = max(0, 100.0 - accounted)

    return stats


def classify_session(stats: SessionStats) -> str:
    """Classify session as Balance, Trend, or Mixed based on metrics."""
    # High rotation + low trending = balance
    # Low rotation + high trending/extension = trend
    # Mixed metrics = mixed

    if stats.pct_rotation >= 60 and stats.pct_trending < 10:
        return "BALANCE"
    elif stats.pct_rotation < 50 and (stats.pct_trending >= 15 or stats.pct_extension >= 8):
        return "TREND"
    elif stats.regime_imbalance_count > stats.regime_balance_count:
        return "TREND"
    else:
        return "MIXED"


def generate_report(sessions: List[SessionStats]) -> str:
    """Generate calibration report."""
    lines = []
    lines.append("=" * 80)
    lines.append("PHASE SYSTEM v2 - CALIBRATION REPORT")
    lines.append("=" * 80)
    lines.append("")
    lines.append(f"Sessions analyzed: {len(sessions)}")
    lines.append("")

    # Per-session summary
    lines.append("-" * 80)
    lines.append("PER-SESSION METRICS")
    lines.append("-" * 80)
    lines.append("")

    for i, s in enumerate(sessions, 1):
        classification = classify_session(s)
        lines.append(f"SESSION {i}: {s.session_date} ({classification})")
        lines.append(f"  Bars: {s.first_bar}-{s.last_bar} ({s.total_bars} bars)")
        lines.append(f"  Range: {s.session_range_ticks}t | VA: {s.va_range_ticks}t")
        lines.append("")
        lines.append(f"  PHASE DISTRIBUTION:")
        lines.append(f"    ROT={s.pct_rotation:5.1f}%  TEST={s.pct_testing:5.1f}%  TREND={s.pct_trending:5.1f}%")
        lines.append(f"    EXT={s.pct_extension:5.1f}%  PULL={s.pct_pullback:5.1f}%  FAIL={s.pct_failed:5.1f}%")
        lines.append(f"    OUT_BAL={s.pct_outside_balance:5.1f}% (derived)")
        lines.append("")
        # Market State (AMTMarketState - SSOT)
        lines.append(f"  MARKET STATE (AMTMarketState):")
        total_ms = s.market_state_balance_count + s.market_state_imbalance_count
        if total_ms > 0:
            lines.append(f"    BALANCE={s.market_state_balance_count} ({100*s.market_state_balance_count/total_ms:.1f}%)")
            lines.append(f"    IMBALANCE={s.market_state_imbalance_count} ({100*s.market_state_imbalance_count/total_ms:.1f}%)")
        else:
            lines.append(f"    (no Market State data)")

        # Regime (AuctionRegime - legacy four-phase)
        lines.append(f"  REGIME (AuctionRegime - legacy):")
        total_regime = (s.regime_balance_count + s.regime_transition_count + s.regime_imbalance_count +
                       s.regime_failed_count + s.regime_excess_count + s.regime_rebalance_count)
        if total_regime > 0:
            lines.append(f"    BALANCE={s.regime_balance_count} ({100*s.regime_balance_count/total_regime:.1f}%)")
            lines.append(f"    TRANSITION={s.regime_transition_count} ({100*s.regime_transition_count/total_regime:.1f}%)")
            lines.append(f"    IMBALANCE={s.regime_imbalance_count} ({100*s.regime_imbalance_count/total_regime:.1f}%)")
            lines.append(f"    EXCESS={s.regime_excess_count} ({100*s.regime_excess_count/total_regime:.1f}%)")
            lines.append(f"    REBALANCE={s.regime_rebalance_count} ({100*s.regime_rebalance_count/total_regime:.1f}%)")
            lines.append(f"    FAILED={s.regime_failed_count} ({100*s.regime_failed_count/total_regime:.1f}%)")
        lines.append("")
        lines.append(f"  TRANSITIONS: Phase={s.phase_transitions}")
        lines.append(f"  EVENTS: Pullback={s.pullback_events} | Acceptance={s.acceptance_events} | RangeExt={s.range_ext_events}")
        lines.append(f"  STATE: {s.market_state} (bal={s.balance_pct:.0f}%)")
        # Shape freeze (session-level SSOT)
        if s.shape_frozen:
            conflict_str = " [CONFLICT]" if s.shape_conflict else ""
            lines.append(f"  SHAPE_FREEZE: t={s.shape_freeze_bar} STRUCT={s.shape_day_structure} "
                        f"RAW={s.shape_raw_frozen} FINAL={s.shape_final_frozen}{conflict_str}")
        else:
            lines.append(f"  SHAPE_FREEZE: (not frozen)")
        lines.append("")

    # Aggregate metrics
    lines.append("-" * 80)
    lines.append("AGGREGATE METRICS")
    lines.append("-" * 80)
    lines.append("")

    n = len(sessions)
    if n > 0:
        avg_rot = sum(s.pct_rotation for s in sessions) / n
        avg_test = sum(s.pct_testing for s in sessions) / n
        avg_trend = sum(s.pct_trending for s in sessions) / n
        avg_ext = sum(s.pct_extension for s in sessions) / n
        avg_pull = sum(s.pct_pullback for s in sessions) / n
        avg_fail = sum(s.pct_failed for s in sessions) / n

        lines.append(f"  AVG PHASE DISTRIBUTION:")
        lines.append(f"    ROT={avg_rot:5.1f}%  TEST={avg_test:5.1f}%  TREND={avg_trend:5.1f}%")
        lines.append(f"    EXT={avg_ext:5.1f}%  PULL={avg_pull:5.1f}%  FAIL={avg_fail:5.1f}%")
        lines.append("")

        avg_phase_trans = sum(s.phase_transitions for s in sessions) / n
        pullback_sessions = sum(1 for s in sessions if s.pullback_events > 0)
        acceptance_sessions = sum(1 for s in sessions if s.acceptance_events > 0)

        lines.append(f"  AVG PHASE TRANSITIONS: {avg_phase_trans:.1f} per session")
        lines.append(f"  PULLBACK REACHABILITY: {pullback_sessions}/{n} sessions ({100*pullback_sessions/n:.0f}%)")
        lines.append(f"  ACCEPTANCE EVENTS: {acceptance_sessions}/{n} sessions ({100*acceptance_sessions/n:.0f}%)")
        lines.append("")

        # Classification breakdown
        balance_count = sum(1 for s in sessions if classify_session(s) == "BALANCE")
        trend_count = sum(1 for s in sessions if classify_session(s) == "TREND")
        mixed_count = sum(1 for s in sessions if classify_session(s) == "MIXED")

        lines.append(f"  SESSION TYPES: Balance={balance_count} Trend={trend_count} Mixed={mixed_count}")
        lines.append("")

        # Shape distribution (SESSION-LEVEL - counts one per session, NOT per bar)
        lines.append("  SHAPE DISTRIBUTION (session-level, 1 per session):")
        frozen_sessions = [s for s in sessions if s.shape_frozen]
        if frozen_sessions:
            # Count final shapes
            shape_counts = {}
            conflict_count = 0
            for s in frozen_sessions:
                shape = s.shape_final_frozen if s.shape_final_frozen else "UNDEFINED"
                shape_counts[shape] = shape_counts.get(shape, 0) + 1
                if s.shape_conflict:
                    conflict_count += 1

            for shape, count in sorted(shape_counts.items(), key=lambda x: -x[1]):
                pct = 100 * count / len(frozen_sessions)
                lines.append(f"    {shape}={count} ({pct:.1f}%)")
            lines.append(f"    CONFLICT_RATE={conflict_count}/{len(frozen_sessions)} ({100*conflict_count/len(frozen_sessions):.1f}%)")
        else:
            lines.append(f"    (no SHAPE_FREEZE events found)")

    lines.append("")
    lines.append("-" * 80)
    lines.append("CALIBRATION CONCERNS")
    lines.append("-" * 80)
    lines.append("")

    # Flag issues
    concerns = []

    if n > 0:
        if avg_pull < 1.0:
            concerns.append("- PULLBACK never/rarely fires. Consider reducing directionalAfterglowBars or approachingPOCLookback.")

        if avg_fail > 10.0:
            concerns.append("- FAILED_AUCTION is high (>10%). Consider reducing failedAuctionRecencyBars.")

        if avg_trend < 3.0 and trend_count > 0:
            concerns.append("- DRIVING is low on trend days. Consider checking directional phase detection.")

        if avg_ext < 2.0 and trend_count > 0:
            concerns.append("- RANGE_EXTENSION is rare. Consider increasing nearExtremeTicks or extremeUpdateWindowBars.")

        if acceptance_sessions == 0:
            concerns.append("- No IMBALANCE/acceptance events. acceptanceClosesRequired may be too high.")

    if not concerns:
        concerns.append("- No major concerns detected. Review individual sessions for edge cases.")

    for c in concerns:
        lines.append(c)

    lines.append("")
    lines.append("=" * 80)

    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_session_stats.py <logfile.txt> [--output report.txt]")
        print("")
        print("Input: Copy Sierra Chart log window contents to a text file")
        print("       (Select all in Log window, Ctrl+C, paste to Notepad, save)")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = None

    if '--output' in sys.argv:
        idx = sys.argv.index('--output')
        if idx + 1 < len(sys.argv):
            output_file = sys.argv[idx + 1]

    # Read input
    try:
        with open(input_file, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: File not found: {input_file}")
        sys.exit(1)

    print(f"Read {len(lines)} lines from {input_file}")

    # Filter to AMT and SESSION lines (SHAPE_FREEZE uses [SESSION] tag)
    amt_lines = [l.strip() for l in lines if '[AMT]' in l or '[SESSION]' in l]
    print(f"Found {len(amt_lines)} AMT/SESSION log lines")

    if len(amt_lines) < 10:
        print("Warning: Very few AMT log lines found. Make sure the log contains AMT study output.")
        # Try to process anyway with all lines
        amt_lines = [l.strip() for l in lines]

    # Detect sessions
    sessions_bounds = detect_session_boundaries(amt_lines)
    print(f"Detected {len(sessions_bounds)} session(s)")

    # Extract stats per session
    all_stats = []
    for start, end in sessions_bounds:
        stats = extract_session_stats(amt_lines, start, end)
        if stats.total_bars > 0:  # Only include non-empty sessions
            all_stats.append(stats)

    print(f"Extracted stats for {len(all_stats)} session(s)")

    # Generate report
    report = generate_report(all_stats)

    # Output
    if output_file:
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write(report)
        print(f"Report written to: {output_file}")
    else:
        print("")
        print(report)


if __name__ == "__main__":
    main()
