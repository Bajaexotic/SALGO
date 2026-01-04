#!/usr/bin/env python3
"""
Phase Attribution Analysis - PULLBACK and FAILED_AUCTION diagnostics

Analyzes Phase System v2 logs to determine why PULLBACK never confirms
and why FAILED_AUCTION is elevated.
"""

import re
import sys
from collections import defaultdict, Counter
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple

@dataclass
class BarState:
    """Parsed state for a single bar."""
    bar_num: int = 0
    # Primitives
    price: float = 0.0
    poc: float = 0.0
    vah: float = 0.0
    val: float = 0.0
    inVA: bool = None  # None = unknown, derive from reasons
    atVAL: bool = False
    atVAH: bool = False
    dPOC: float = 0.0
    vaRange: float = 0.0
    outStreak: int = 0
    accepted: int = 0
    # Phase/Regime
    raw_regime: str = ""
    conf_regime: str = ""
    raw_phase: str = ""
    conf_phase: str = ""
    streak: int = 0
    min_confirm: int = 3
    # Reasons
    phase_reason: str = ""
    regime_reason: str = ""


def parse_bar_number(line: str) -> Optional[int]:
    """Extract bar number from log line."""
    match = re.search(r'Bar\s+(\d+)', line)
    return int(match.group(1)) if match else None


def parse_primitives(line: str) -> Dict:
    """Parse: Prim: P=6969.00 POC=6975.00 VAH=6979.50 VAL=6971.75 | inVA=0 atVAL=0 atVAH=0 | dPOC=24.0 vaRange=31.0 | outStreak=13 accepted=1"""
    result = {}
    patterns = [
        (r'P=([0-9.]+)', 'price', float),
        (r'POC=([0-9.]+)', 'poc', float),
        (r'VAH=([0-9.]+)', 'vah', float),
        (r'VAL=([0-9.]+)', 'val', float),
        (r'inVA=(\d)', 'inVA', lambda x: x == '1'),
        (r'atVAL=(\d)', 'atVAL', lambda x: x == '1'),
        (r'atVAH=(\d)', 'atVAH', lambda x: x == '1'),
        (r'dPOC=([0-9.]+)', 'dPOC', float),
        (r'vaRange=([0-9.]+)', 'vaRange', float),
        (r'outStreak=(\d+)', 'outStreak', int),
        (r'accepted=(\d)', 'accepted', int),
    ]
    for pattern, key, converter in patterns:
        match = re.search(pattern, line)
        if match:
            result[key] = converter(match.group(1))
    return result


def parse_regime_phase(line: str) -> Dict:
    """Parse: REGIME: RAW=X CONF=Y | PHASE: RAW=A CONF=B | streak=N/M"""
    result = {}
    patterns = [
        (r'REGIME:.*?RAW=(\w+)', 'raw_regime'),
        (r'REGIME:.*?CONF=(\w+)', 'conf_regime'),
        (r'PHASE:.*?RAW=(\w+)', 'raw_phase'),
        (r'PHASE:.*?CONF=(\w+)', 'conf_phase'),
        (r'streak=(\d+)/(\d+)', 'streak'),
    ]
    for pattern, key in patterns:
        match = re.search(pattern, line)
        if match:
            if key == 'streak':
                result['streak'] = int(match.group(1))
                result['min_confirm'] = int(match.group(2))
            else:
                result[key] = match.group(1)
    return result


def parse_reasons(line: str) -> Dict:
    """Parse: Reasons: Ph=PULLBACK_TO_VALUE Rg=ACCEPTED_BELOW_VA"""
    result = {}
    match = re.search(r'Ph=(\w+)', line)
    if match:
        result['phase_reason'] = match.group(1)
    match = re.search(r'Rg=(\w+)', line)
    if match:
        result['regime_reason'] = match.group(1)
    return result


def infer_location_from_reasons(phase_reason: str, regime_reason: str) -> Tuple[Optional[bool], bool, bool]:
    """
    Infer (inVA, atVAL, atVAH) from phase/regime reason strings.
    Returns: (inVA or None if unknown, atVAL, atVAH)
    """
    inVA = None
    atVAL = False
    atVAH = False

    # Phase reasons that indicate location
    if phase_reason in ('AT_VAL', 'TESTING_VAL'):
        atVAL = True
        inVA = True  # At boundary is considered "in VA" for logging
    elif phase_reason in ('AT_VAH', 'TESTING_VAH'):
        atVAH = True
        inVA = True
    elif phase_reason in ('INSIDE_VALUE', 'INSIDE_VALUE_DEFAULT'):
        inVA = True
    elif phase_reason in ('OUTSIDE_ABOVE_VA', 'OUTSIDE_BELOW_VA', 'FAR_FROM_POC',
                          'FAR_FROM_POC_CONTINUING', 'RANGE_EXT_HIGH', 'RANGE_EXT_LOW',
                          'PULLBACK_TO_VALUE'):
        inVA = False

    # Regime reasons can also indicate location
    if regime_reason in ('PROBE_ABOVE_VA', 'PROBE_BELOW_VA',
                         'ACCEPTED_ABOVE_VA', 'ACCEPTED_BELOW_VA'):
        inVA = False
    elif regime_reason in ('INSIDE_VALUE', 'TESTING_VAH', 'TESTING_VAL'):
        inVA = True

    return inVA, atVAL, atVAH


def analyze_logs(filepath: str) -> Tuple[List[BarState], Dict]:
    """Parse log file and extract bar states."""
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()

    # Filter to AMT lines
    amt_lines = [l for l in lines if '[AMT]' in l]

    # Build bar states
    bar_states = []
    current_bar = BarState()

    for line in amt_lines:
        bar_num = parse_bar_number(line)
        if bar_num and bar_num != current_bar.bar_num:
            if current_bar.bar_num > 0:
                bar_states.append(current_bar)
            current_bar = BarState(bar_num=bar_num)

        if 'Prim:' in line:
            prims = parse_primitives(line)
            for k, v in prims.items():
                setattr(current_bar, k, v)

        if 'REGIME:' in line and 'CONF=' in line:
            rp = parse_regime_phase(line)
            for k, v in rp.items():
                setattr(current_bar, k, v)

        if 'Reasons:' in line:
            reasons = parse_reasons(line)
            for k, v in reasons.items():
                setattr(current_bar, k, v)
            # Infer location from reasons if not already set from primitives
            if current_bar.inVA is None:
                inVA, atVAL, atVAH = infer_location_from_reasons(
                    current_bar.phase_reason, current_bar.regime_reason)
                if inVA is not None:
                    current_bar.inVA = inVA
                if atVAL:
                    current_bar.atVAL = atVAL
                if atVAH:
                    current_bar.atVAH = atVAH

    # Add last bar
    if current_bar.bar_num > 0:
        bar_states.append(current_bar)

    return bar_states, {}


def compute_pullback_predicates(bars: List[BarState]) -> Dict:
    """Compute PULLBACK predicate breakdown."""
    stats = {
        'total_bars': len(bars),
        'outsideVA': 0,
        'approachingPOC': 0,  # dPOC decreasing toward POC
        'wasDirectionalRecently': 0,  # outStreak > 0 indicates prior movement
        'all_three': 0,
        'raw_pullback': 0,
        'conf_pullback': 0,
        # Suppression analysis for bars where all_three
        'suppressed_by_testingBoundary': 0,
        'suppressed_by_extension': 0,
        'suppressed_by_trending': 0,
        'suppressed_by_other': 0,
        'eligible_for_pullback': 0,
        # Streak analysis
        'pullback_streak_distribution': Counter(),
        'pullback_interrupted_by': Counter(),
    }

    prev_dPOC = None
    prev_phase = None
    directional_window = []  # Track last N bars for afterglow
    afterglow_bars = 5  # Default directionalAfterglowBars

    for i, bar in enumerate(bars):
        # outsideVA: only true if we definitively know bar is outside (inVA=False)
        outsideVA = (bar.inVA is False)

        # approachingPOC: dPOC is decreasing (moving toward POC)
        approachingPOC = False
        if prev_dPOC is not None and bar.dPOC < prev_dPOC:
            approachingPOC = True

        # wasDirectionalRecently: had directional movement in last N bars
        # Using outStreak as proxy - if recently outside VA with movement
        directional_window.append(outsideVA)
        if len(directional_window) > afterglow_bars:
            directional_window.pop(0)
        wasDirectionalRecently = any(directional_window[:-1]) if len(directional_window) > 1 else False

        if outsideVA:
            stats['outsideVA'] += 1
        if approachingPOC:
            stats['approachingPOC'] += 1
        if wasDirectionalRecently:
            stats['wasDirectionalRecently'] += 1

        all_three = outsideVA and approachingPOC and wasDirectionalRecently
        if all_three:
            stats['all_three'] += 1

            # Check what phase was actually assigned
            if bar.conf_phase == 'TESTING_BOUNDARY':
                stats['suppressed_by_testingBoundary'] += 1
            elif bar.conf_phase == 'RANGE_EXTENSION':
                stats['suppressed_by_extension'] += 1
            elif bar.conf_phase in ('DRIVING_UP', 'DRIVING_DOWN'):
                stats['suppressed_by_driving'] += 1
            elif bar.conf_phase == 'PULLBACK':
                stats['eligible_for_pullback'] += 1
            else:
                stats['suppressed_by_other'] += 1

        if bar.raw_phase == 'PULLBACK':
            stats['raw_pullback'] += 1
            stats['pullback_streak_distribution'][bar.streak] += 1
            # Track what interrupted pullback
            if prev_phase and prev_phase != 'PULLBACK' and bar.streak == 1:
                stats['pullback_interrupted_by'][bar.conf_phase] += 1

        if bar.conf_phase == 'PULLBACK':
            stats['conf_pullback'] += 1

        prev_dPOC = bar.dPOC
        prev_phase = bar.raw_phase

    return stats


def compute_failed_auction_analysis(bars: List[BarState]) -> Dict:
    """Analyze FAILED_AUCTION phase occurrences."""
    stats = {
        'total_bars': len(bars),
        'raw_failed': 0,
        'conf_failed': 0,
        'failed_reasons': Counter(),
        'failed_regime_context': Counter(),
        'failed_when_at_boundary': 0,
        'failed_when_outside_va': 0,
        'failed_when_inside_va': 0,
        'failed_when_unknown': 0,
        'failed_streak_distribution': Counter(),
        'conf_failed_examples': [],
    }

    for bar in bars:
        if bar.raw_phase == 'FAILED_AUCTION':
            stats['raw_failed'] += 1

        if bar.conf_phase == 'FAILED_AUCTION':
            stats['conf_failed'] += 1
            stats['failed_reasons'][bar.phase_reason] += 1
            stats['failed_regime_context'][bar.conf_regime] += 1

            if bar.atVAL or bar.atVAH:
                stats['failed_when_at_boundary'] += 1
            elif bar.inVA is None:
                stats['failed_when_unknown'] += 1
            elif bar.inVA is False:
                stats['failed_when_outside_va'] += 1
            else:
                stats['failed_when_inside_va'] += 1

            stats['failed_streak_distribution'][bar.streak] += 1

            if len(stats['conf_failed_examples']) < 10:
                stats['conf_failed_examples'].append({
                    'bar': bar.bar_num,
                    'price': bar.price,
                    'dPOC': bar.dPOC,
                    'inVA': bar.inVA,
                    'atVAL': bar.atVAL,
                    'atVAH': bar.atVAH,
                    'reason': bar.phase_reason,
                    'regime': bar.conf_regime,
                })

    return stats


def get_pullback_examples(filepath: str, count: int = 5) -> List[str]:
    """Get representative log excerpts for PULLBACK analysis."""
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()

    examples = []
    i = 0
    while i < len(lines) and len(examples) < count:
        if 'RAW=PULLBACK' in lines[i]:
            # Get context (2 lines before, this line, 2 lines after)
            start = max(0, i - 2)
            end = min(len(lines), i + 3)
            excerpt = ''.join(lines[start:end])
            examples.append(excerpt.strip())
            i = end  # Skip ahead
        else:
            i += 1

    return examples


def get_failed_examples(filepath: str, count: int = 5) -> List[str]:
    """Get representative log excerpts for FAILED_AUCTION analysis."""
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()

    examples = []
    i = 0
    while i < len(lines) and len(examples) < count:
        if 'CONF=FAILED_AUCTION' in lines[i]:
            start = max(0, i - 2)
            end = min(len(lines), i + 3)
            excerpt = ''.join(lines[start:end])
            examples.append(excerpt.strip())
            i = end
        else:
            i += 1

    return examples


def generate_report(bars: List[BarState], pullback_stats: Dict, failed_stats: Dict,
                   pullback_examples: List[str], failed_examples: List[str]) -> str:
    """Generate attribution report."""
    lines = []
    lines.append("=" * 90)
    lines.append("PHASE SYSTEM v2 - ATTRIBUTION REPORT")
    lines.append("=" * 90)
    lines.append("")

    # TABLE 1: PULLBACK Predicates
    lines.append("-" * 90)
    lines.append("TABLE 1: PULLBACK PREDICATE ANALYSIS")
    lines.append("-" * 90)
    lines.append("")
    total = pullback_stats['total_bars']
    lines.append(f"  {'Predicate':<30} {'Count':>10} {'Percent':>10}")
    lines.append(f"  {'-'*30} {'-'*10} {'-'*10}")
    lines.append(f"  {'Total bars analyzed':<30} {total:>10} {'100.0%':>10}")
    lines.append(f"  {'outsideVA':<30} {pullback_stats['outsideVA']:>10} {100*pullback_stats['outsideVA']/total:>9.1f}%")
    lines.append(f"  {'approachingPOC':<30} {pullback_stats['approachingPOC']:>10} {100*pullback_stats['approachingPOC']/total:>9.1f}%")
    lines.append(f"  {'wasDirectionalRecently':<30} {pullback_stats['wasDirectionalRecently']:>10} {100*pullback_stats['wasDirectionalRecently']/total:>9.1f}%")
    lines.append(f"  {'all_three (conjunctive)':<30} {pullback_stats['all_three']:>10} {100*pullback_stats['all_three']/total:>9.1f}%")
    lines.append("")
    lines.append(f"  {'RAW=PULLBACK detected':<30} {pullback_stats['raw_pullback']:>10} {100*pullback_stats['raw_pullback']/total:>9.1f}%")
    lines.append(f"  {'CONF=PULLBACK confirmed':<30} {pullback_stats['conf_pullback']:>10} {100*pullback_stats['conf_pullback']/total:>9.1f}%")
    lines.append("")

    # TABLE 2: PULLBACK Suppression
    lines.append("-" * 90)
    lines.append("TABLE 2: PULLBACK SUPPRESSION ANALYSIS (for bars where all_three=true)")
    lines.append("-" * 90)
    lines.append("")
    all3 = pullback_stats['all_three']
    if all3 > 0:
        lines.append(f"  {'Suppression Cause':<40} {'Count':>10} {'Percent':>10}")
        lines.append(f"  {'-'*40} {'-'*10} {'-'*10}")
        lines.append(f"  {'suppressed_by_testingBoundary':<40} {pullback_stats['suppressed_by_testingBoundary']:>10} {100*pullback_stats['suppressed_by_testingBoundary']/all3:>9.1f}%")
        lines.append(f"  {'suppressed_by_extension':<40} {pullback_stats['suppressed_by_extension']:>10} {100*pullback_stats['suppressed_by_extension']/all3:>9.1f}%")
        lines.append(f"  {'suppressed_by_trending':<40} {pullback_stats['suppressed_by_trending']:>10} {100*pullback_stats['suppressed_by_trending']/all3:>9.1f}%")
        lines.append(f"  {'suppressed_by_other':<40} {pullback_stats['suppressed_by_other']:>10} {100*pullback_stats['suppressed_by_other']/all3:>9.1f}%")
        lines.append(f"  {'eligible_for_pullback':<40} {pullback_stats['eligible_for_pullback']:>10} {100*pullback_stats['eligible_for_pullback']/all3:>9.1f}%")
    else:
        lines.append("  No bars with all three predicates true.")
    lines.append("")

    # Streak distribution
    lines.append("  PULLBACK Streak Distribution (RAW=PULLBACK bars):")
    for streak, count in sorted(pullback_stats['pullback_streak_distribution'].items()):
        lines.append(f"    streak={streak}: {count} bars")
    lines.append("")

    lines.append("  PULLBACK Interrupted By (when streak resets to 1):")
    for phase, count in pullback_stats['pullback_interrupted_by'].most_common():
        lines.append(f"    {phase}: {count}")
    lines.append("")

    # TABLE 3: FAILED_AUCTION Analysis
    lines.append("-" * 90)
    lines.append("TABLE 3: FAILED_AUCTION CAUSE ANALYSIS")
    lines.append("-" * 90)
    lines.append("")
    lines.append(f"  {'Metric':<40} {'Count':>10} {'Percent':>10}")
    lines.append(f"  {'-'*40} {'-'*10} {'-'*10}")
    lines.append(f"  {'RAW=FAILED_AUCTION':<40} {failed_stats['raw_failed']:>10} {100*failed_stats['raw_failed']/total:>9.1f}%")
    lines.append(f"  {'CONF=FAILED_AUCTION':<40} {failed_stats['conf_failed']:>10} {100*failed_stats['conf_failed']/total:>9.1f}%")
    lines.append("")

    conf_fail = failed_stats['conf_failed']
    if conf_fail > 0:
        lines.append("  FAILED_AUCTION by Phase Reason:")
        for reason, count in failed_stats['failed_reasons'].most_common():
            lines.append(f"    {reason}: {count} ({100*count/conf_fail:.1f}%)")
        lines.append("")

        lines.append("  FAILED_AUCTION by Regime Context:")
        for regime, count in failed_stats['failed_regime_context'].most_common():
            lines.append(f"    {regime}: {count} ({100*count/conf_fail:.1f}%)")
        lines.append("")

        lines.append("  FAILED_AUCTION Location (inferred from reasons):")
        lines.append(f"    At VA boundary (atVAL/atVAH): {failed_stats['failed_when_at_boundary']} ({100*failed_stats['failed_when_at_boundary']/conf_fail:.1f}%)")
        lines.append(f"    Outside VA: {failed_stats['failed_when_outside_va']} ({100*failed_stats['failed_when_outside_va']/conf_fail:.1f}%)")
        lines.append(f"    Inside VA: {failed_stats['failed_when_inside_va']} ({100*failed_stats['failed_when_inside_va']/conf_fail:.1f}%)")
        if failed_stats['failed_when_unknown'] > 0:
            lines.append(f"    Unknown: {failed_stats['failed_when_unknown']} ({100*failed_stats['failed_when_unknown']/conf_fail:.1f}%)")
        lines.append("")

        lines.append("  FAILED_AUCTION Streak Distribution:")
        for streak, count in sorted(failed_stats['failed_streak_distribution'].items()):
            lines.append(f"    streak={streak}: {count} bars")
    lines.append("")

    # DIAGNOSIS
    lines.append("-" * 90)
    lines.append("DIAGNOSIS")
    lines.append("-" * 90)
    lines.append("")

    # PULLBACK diagnosis
    lines.append("PULLBACK UNREACHABILITY CAUSE:")
    if pullback_stats['raw_pullback'] == 0:
        lines.append("  (a) One predicate never true: RAW=PULLBACK never detected")
        if pullback_stats['outsideVA'] == 0:
            lines.append("      -> outsideVA is always false")
        if pullback_stats['approachingPOC'] == 0:
            lines.append("      -> approachingPOC is always false")
        if pullback_stats['wasDirectionalRecently'] == 0:
            lines.append("      -> wasDirectionalRecently is always false")
    elif pullback_stats['conf_pullback'] == 0 and pullback_stats['raw_pullback'] > 0:
        max_streak = max(pullback_stats['pullback_streak_distribution'].keys()) if pullback_stats['pullback_streak_distribution'] else 0
        if max_streak < 3:
            lines.append(f"  (c) Confirmation/hysteresis: RAW=PULLBACK detected {pullback_stats['raw_pullback']} times")
            lines.append(f"      but max streak={max_streak} < minConfirmationBars=3")
            lines.append("      -> PULLBACK gets interrupted before reaching confirmation threshold")
        else:
            lines.append("  (b) Priority suppression: Higher-priority phases override PULLBACK")
    lines.append("")

    # FAILED_AUCTION diagnosis
    lines.append("FAILED_AUCTION ELEVATION CAUSE:")
    if conf_fail > 0:
        top_reason = failed_stats['failed_reasons'].most_common(1)[0] if failed_stats['failed_reasons'] else ('UNKNOWN', 0)
        lines.append(f"  Primary cause: {top_reason[0]} ({top_reason[1]} bars, {100*top_reason[1]/conf_fail:.1f}%)")

        if 'EXTREME' in str(top_reason[0]).upper() or 'NEW_EXTREME' in str(top_reason[0]).upper():
            lines.append("  -> FAIL IS being set by 'new extreme recently' logic")
        else:
            lines.append("  -> FAIL is NOT being set by 'new extreme recently' logic")
    lines.append("")

    # EXAMPLES
    lines.append("-" * 90)
    lines.append("REPRESENTATIVE LOG EXCERPTS - PULLBACK")
    lines.append("-" * 90)
    for i, ex in enumerate(pullback_examples, 1):
        lines.append(f"\n[PULLBACK Example {i}]")
        lines.append(ex)
    lines.append("")

    lines.append("-" * 90)
    lines.append("REPRESENTATIVE LOG EXCERPTS - FAILED_AUCTION")
    lines.append("-" * 90)
    for i, ex in enumerate(failed_examples, 1):
        lines.append(f"\n[FAILED_AUCTION Example {i}]")
        lines.append(ex)
    lines.append("")

    lines.append("=" * 90)

    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        print("Usage: python phase_attribution.py <logfile.txt> [--output report.txt]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = None

    if '--output' in sys.argv:
        idx = sys.argv.index('--output')
        if idx + 1 < len(sys.argv):
            output_file = sys.argv[idx + 1]

    print(f"Analyzing: {input_file}")

    bars, _ = analyze_logs(input_file)
    print(f"Parsed {len(bars)} bar states")

    pullback_stats = compute_pullback_predicates(bars)
    failed_stats = compute_failed_auction_analysis(bars)

    pullback_examples = get_pullback_examples(input_file, 5)
    failed_examples = get_failed_examples(input_file, 5)

    report = generate_report(bars, pullback_stats, failed_stats,
                            pullback_examples, failed_examples)

    if output_file:
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write(report)
        print(f"Report written to: {output_file}")
    else:
        print(report)


if __name__ == "__main__":
    main()
