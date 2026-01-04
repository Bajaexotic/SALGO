#!/usr/bin/env python3
"""
Events CSV Merge Pipeline — Execution Design Spec v1.1

Implements the complete merge pipeline with:
- Pre-validation (sort order, timestamps, deduplication)
- Single-pass merge
- Output: context.csv, engagement.csv, error.csv, summary.json

Exit codes:
  0 = Success, no errors
  1 = Success with recoverable errors
  2 = Fatal error, partial output
  3 = Input validation failed
"""

import csv
import json
import sys
import os
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple, Set
from datetime import datetime
from pathlib import Path
from collections import defaultdict

# =============================================================================
# CONSTANTS (from Spec v1.1)
# =============================================================================

EVENT_TYPE_RANK = {
    'PHASE_SNAPSHOT': 1,
    'MODE_LOCK': 2,
    'ENGAGEMENT_FINAL': 3
}

VALID_OUTCOMES = {'ACCEPT', 'REJECT', 'TAG', 'PROBE', 'TEST'}
VALID_ZONE_TYPES = {'VPB_POC', 'VPB_VAH', 'VPB_VAL', 'PRIOR_POC', 'PRIOR_VAH', 'PRIOR_VAL', 'NONE', ''}

STALENESS_THRESHOLD = 50

REQUIRED_COLUMNS = {
    'session_id', 'session_type', 'ts', 'bar', 'event_type'
}

# =============================================================================
# DATA CLASSES
# =============================================================================

@dataclass
class Error:
    error_type: str
    severity: str  # 'FATAL' or 'RECOVERABLE'
    session_id: Optional[str] = None
    bar: Optional[str] = None
    event_type: Optional[str] = None
    column_name: Optional[str] = None
    value_a: Optional[str] = None
    value_b: Optional[str] = None
    raw_row: Optional[str] = None

    def to_dict(self) -> dict:
        return {
            'error_type': self.error_type,
            'severity': self.severity,
            'session_id': self.session_id or '',
            'bar': self.bar or '',
            'event_type': self.event_type or '',
            'column_name': self.column_name or '',
            'value_a': self.value_a or '',
            'value_b': self.value_b or '',
            'raw_row': self.raw_row or '',
            'ts_detected': datetime.now().isoformat()
        }


@dataclass
class ContextRow:
    session_id: str
    bar: str
    session_type: str
    ts: str
    phase: Optional[str] = None
    zone_id_snapshot: Optional[str] = None
    zone_type_snapshot: Optional[str] = None
    aggression: Optional[str] = None
    facilitation: Optional[str] = None
    market_state: Optional[str] = None
    raw_state: Optional[str] = None
    snapshot_message: Optional[str] = None
    context_complete: bool = False
    _source_rows: int = 0

    # Track which event types contributed
    _has_snapshot: bool = False
    _has_lock: bool = False

    def to_dict(self) -> dict:
        return {
            'session_id': self.session_id,
            'bar': self.bar,
            'session_type': self.session_type,
            'ts': self.ts,
            'phase': self.phase or '',
            'zone_id_snapshot': self.zone_id_snapshot or '',
            'zone_type_snapshot': self.zone_type_snapshot or '',
            'aggression': self.aggression or '',
            'facilitation': self.facilitation or '',
            'market_state': self.market_state or '',
            'raw_state': self.raw_state or '',
            'snapshot_message': self.snapshot_message or '',
            'context_complete': 'TRUE' if self.context_complete else 'FALSE',
            '_source_rows': str(self._source_rows)
        }


@dataclass
class EngagementRow:
    session_id: str
    bar: str
    ts: str
    session_type: str
    zone_id: str
    zone_type: str
    entry_price: str
    exit_price: str
    bars: str
    outcome: str
    escape_vel: str
    vol_ratio: str
    context_bar: Optional[str] = None
    context_stale: Optional[bool] = None
    ctx_aggression: Optional[str] = None
    ctx_facilitation: Optional[str] = None
    ctx_market_state: Optional[str] = None
    ctx_phase: Optional[str] = None
    orphan: bool = False
    _flags: List[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        # Handle context_stale: NULL if orphan, else TRUE/FALSE
        if self.orphan:
            stale_str = ''
        else:
            stale_str = 'TRUE' if self.context_stale else 'FALSE'

        return {
            'session_id': self.session_id,
            'bar': self.bar,
            'ts': self.ts,
            'session_type': self.session_type,
            'zone_id': self.zone_id,
            'zone_type': self.zone_type,
            'entry_price': self.entry_price,
            'exit_price': self.exit_price,
            'bars': self.bars,
            'outcome': self.outcome,
            'escape_vel': self.escape_vel,
            'vol_ratio': self.vol_ratio,
            'context_bar': self.context_bar or '',
            'context_stale': stale_str,
            'ctx_aggression': self.ctx_aggression or '',
            'ctx_facilitation': self.ctx_facilitation or '',
            'ctx_market_state': self.ctx_market_state or '',
            'ctx_phase': self.ctx_phase or '',
            'orphan': 'TRUE' if self.orphan else 'FALSE',
            '_flags': ','.join(self._flags)
        }


# =============================================================================
# PIPELINE CLASS
# =============================================================================

class MergePipeline:
    def __init__(self, input_path: str, output_dir: str):
        self.input_path = input_path
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self.errors: List[Error] = []
        self.contexts: List[ContextRow] = []
        self.engagements: List[EngagementRow] = []

        self.has_fatal = False
        self.has_recoverable = False

        # For context lookup during merge
        self.context_by_session_bar: Dict[Tuple[str, str], ContextRow] = {}

        # For zone_type consistency check
        self.zone_type_by_id: Dict[Tuple[str, str], str] = {}  # (session_id, zone_id) -> zone_type

        # For duplicate engagement detection
        self.seen_engagements: Set[Tuple[str, str, str]] = set()  # (session_id, bar, zone_id)

        # Summary stats
        self.summary = {
            'input_rows': 0,
            'duplicate_rows_dropped': 0,
            'context_rows': 0,
            'engagement_rows': 0,
            'error_count': 0,
            'fatal_errors': 0,
            'recoverable_errors': 0
        }

    def add_error(self, error: Error) -> bool:
        """Add error and return True if fatal (should halt)."""
        self.errors.append(error)
        self.summary['error_count'] += 1

        if error.severity == 'FATAL':
            self.has_fatal = True
            self.summary['fatal_errors'] += 1
            return True
        else:
            self.has_recoverable = True
            self.summary['recoverable_errors'] += 1
            return False

    def run(self) -> int:
        """Execute pipeline and return exit code."""
        try:
            # Phase 0: Load and pre-validate
            rows = self._load_input()
            if self.has_fatal:
                self._write_outputs()
                return 2 if self.summary['fatal_errors'] > 0 else 3

            rows = self._deduplicate(rows)

            if not self._validate_sort_order(rows):
                self._write_outputs()
                return 3

            if not self._validate_timestamps(rows):
                self._write_outputs()
                return 2

            if not self._validate_zone_consistency(rows):
                self._write_outputs()
                return 2

            # Phase 1: Single-pass merge
            self._merge(rows)

            # Write outputs
            self._write_outputs()

            # Determine exit code
            if self.has_fatal:
                return 2
            elif self.has_recoverable:
                return 1
            else:
                return 0

        except Exception as e:
            self.add_error(Error(
                error_type='INTERNAL_ERROR',
                severity='FATAL',
                value_a=str(e)
            ))
            self._write_outputs()
            return 2

    # =========================================================================
    # PHASE 0: PRE-VALIDATION
    # =========================================================================

    def _load_input(self) -> List[dict]:
        """Load CSV and validate required columns."""
        rows = []

        with open(self.input_path, 'r', newline='', encoding='utf-8') as f:
            reader = csv.DictReader(f)

            # Check required columns
            if reader.fieldnames:
                missing = REQUIRED_COLUMNS - set(reader.fieldnames)
                if missing:
                    self.add_error(Error(
                        error_type='MISSING_COLUMNS',
                        severity='FATAL',
                        column_name=','.join(missing)
                    ))
                    return []

            for row in reader:
                self.summary['input_rows'] += 1

                # Check for missing identity values
                if not row.get('session_id') or row.get('session_id').strip() == '':
                    self.add_error(Error(
                        error_type='MISSING_IDENTITY',
                        severity='FATAL',
                        column_name='session_id',
                        raw_row=str(row)
                    ))
                    return rows

                if not row.get('bar') or row.get('bar').strip() == '':
                    self.add_error(Error(
                        error_type='MISSING_IDENTITY',
                        severity='FATAL',
                        column_name='bar',
                        raw_row=str(row)
                    ))
                    return rows

                if not row.get('ts') or row.get('ts').strip() == '':
                    self.add_error(Error(
                        error_type='MISSING_IDENTITY',
                        severity='FATAL',
                        column_name='ts',
                        raw_row=str(row)
                    ))
                    return rows

                rows.append(row)

        return rows

    def _deduplicate(self, rows: List[dict]) -> List[dict]:
        """Remove exact duplicate rows, keeping first occurrence."""
        seen: Dict[str, int] = {}  # row_key -> count
        result = []

        for row in rows:
            # Create hashable key from all values
            row_key = tuple(sorted(row.items()))

            if row_key in seen:
                seen[row_key] += 1
            else:
                seen[row_key] = 1
                result.append(row)

        # Log duplicates
        for row_key, count in seen.items():
            if count > 1:
                row_dict = dict(row_key)
                self.add_error(Error(
                    error_type='DUPLICATE_ROW',
                    severity='RECOVERABLE',
                    session_id=row_dict.get('session_id'),
                    bar=row_dict.get('bar'),
                    event_type=row_dict.get('event_type'),
                    value_a=f'count={count}'
                ))
                self.summary['duplicate_rows_dropped'] += (count - 1)

        return result

    def _validate_sort_order(self, rows: List[dict]) -> bool:
        """Verify rows are sorted by (session_id, bar, event_type_rank)."""
        prev_key = None

        for row in rows:
            session_id = row.get('session_id', '')
            bar = int(row.get('bar', 0))
            event_type = row.get('event_type', '')
            rank = EVENT_TYPE_RANK.get(event_type, 99)

            current_key = (session_id, bar, rank)

            if prev_key is not None:
                if current_key < prev_key:
                    self.add_error(Error(
                        error_type='UNSORTED_INPUT',
                        severity='FATAL',
                        session_id=session_id,
                        bar=str(bar),
                        event_type=event_type
                    ))
                    return False

            prev_key = current_key

        return True

    def _validate_timestamps(self, rows: List[dict]) -> bool:
        """Check timestamp monotonicity within sessions."""
        # Group by session and track max ts per bar
        session_bars: Dict[str, List[Tuple[int, str]]] = defaultdict(list)

        for row in rows:
            session_id = row.get('session_id', '')
            bar = int(row.get('bar', 0))
            ts = row.get('ts', '')
            session_bars[session_id].append((bar, ts))

        for session_id, bar_ts_list in session_bars.items():
            # Sort by bar to check monotonicity
            bar_ts_list.sort(key=lambda x: x[0])

            prev_bar = None
            prev_ts = None

            for bar, ts in bar_ts_list:
                if prev_bar is not None and bar > prev_bar:
                    # Different bar - check ts didn't go backwards
                    if ts < prev_ts:
                        self.add_error(Error(
                            error_type='TIMESTAMP_INVERSION',
                            severity='FATAL',
                            session_id=session_id,
                            value_a=f'bar{prev_bar}={prev_ts}',
                            value_b=f'bar{bar}={ts}'
                        ))
                        return False

                # Update prev with max ts at this bar
                if prev_bar == bar:
                    prev_ts = max(prev_ts, ts) if prev_ts else ts
                else:
                    prev_bar = bar
                    prev_ts = ts

        return True

    def _validate_zone_consistency(self, rows: List[dict]) -> bool:
        """Check zone_type consistency per (session_id, zone_id)."""
        zone_types: Dict[Tuple[str, str], str] = {}

        for row in rows:
            if row.get('event_type') != 'ENGAGEMENT_FINAL':
                continue

            session_id = row.get('session_id', '')
            zone_id = row.get('zone_id', '')
            zone_type = row.get('zone_type', '')

            if not zone_id or zone_id == '-1':
                continue

            key = (session_id, zone_id)

            if key in zone_types:
                if zone_types[key] != zone_type:
                    self.add_error(Error(
                        error_type='ZONE_TYPE_INCONSISTENCY',
                        severity='FATAL',
                        session_id=session_id,
                        column_name=f'zone_id={zone_id}',
                        value_a=zone_types[key],
                        value_b=zone_type
                    ))
                    return False
            else:
                zone_types[key] = zone_type

        return True

    # =========================================================================
    # PHASE 1: SINGLE-PASS MERGE
    # =========================================================================

    def _merge(self, rows: List[dict]) -> None:
        """Single-pass merge: build contexts, then engagements."""
        # Track current context being built
        current_context: Optional[ContextRow] = None
        current_session_bar: Optional[Tuple[str, str]] = None

        # Track context events per (session_id, bar) for duplicate detection
        snapshot_seen: Set[Tuple[str, str]] = set()
        lock_seen: Set[Tuple[str, str]] = set()

        for row in rows:
            session_id = row.get('session_id', '')
            bar = row.get('bar', '')
            event_type = row.get('event_type', '')

            key = (session_id, bar)

            # Handle context events
            if event_type == 'PHASE_SNAPSHOT':
                # Check for duplicate
                if key in snapshot_seen:
                    if self.add_error(Error(
                        error_type='DUPLICATE_CONTEXT_SNAPSHOT',
                        severity='FATAL',
                        session_id=session_id,
                        bar=bar,
                        event_type=event_type
                    )):
                        return
                snapshot_seen.add(key)

                # Start or update context
                if current_session_bar != key:
                    self._finalize_context(current_context)
                    current_context = self._create_context_from_snapshot(row)
                    current_session_bar = key
                else:
                    self._merge_snapshot_into_context(current_context, row)

            elif event_type == 'MODE_LOCK':
                # Check for duplicate
                if key in lock_seen:
                    if self.add_error(Error(
                        error_type='DUPLICATE_CONTEXT_LOCK',
                        severity='FATAL',
                        session_id=session_id,
                        bar=bar,
                        event_type=event_type
                    )):
                        return
                lock_seen.add(key)

                # Start or update context
                if current_session_bar != key:
                    self._finalize_context(current_context)
                    current_context = self._create_context_from_lock(row)
                    current_session_bar = key
                else:
                    self._merge_lock_into_context(current_context, row)

            elif event_type == 'ENGAGEMENT_FINAL':
                # Finalize any pending context first (for same-bar lookup)
                if current_session_bar == key:
                    self._finalize_context(current_context)
                    current_context = None
                    current_session_bar = None
                elif current_context is not None:
                    self._finalize_context(current_context)
                    current_context = None
                    current_session_bar = None

                # Process engagement
                self._process_engagement(row)

        # Finalize last context
        self._finalize_context(current_context)

    def _create_context_from_snapshot(self, row: dict) -> ContextRow:
        """Create new context from PHASE_SNAPSHOT."""
        zone_id = row.get('zone_id', '')
        if zone_id == '-1':
            zone_id = None

        zone_type = row.get('zone_type', '')
        if zone_type == 'NONE':
            zone_type = None

        return ContextRow(
            session_id=row.get('session_id', ''),
            bar=row.get('bar', ''),
            session_type=row.get('session_type', ''),
            ts=row.get('ts', ''),
            phase=row.get('phase') or None,
            zone_id_snapshot=zone_id,
            zone_type_snapshot=zone_type,
            snapshot_message=row.get('message') or None,
            _source_rows=1,
            _has_snapshot=True
        )

    def _create_context_from_lock(self, row: dict) -> ContextRow:
        """Create new context from MODE_LOCK."""
        # Extract raw_state from message (format: "raw:VALUE")
        message = row.get('message', '')
        raw_state = None
        if message.startswith('raw:'):
            raw_state = message[4:]

        return ContextRow(
            session_id=row.get('session_id', ''),
            bar=row.get('bar', ''),
            session_type=row.get('session_type', ''),
            ts=row.get('ts', ''),
            aggression=row.get('aggression') or None,
            facilitation=row.get('facilitation') or None,
            market_state=row.get('market_state') or None,
            raw_state=raw_state,
            _source_rows=1,
            _has_lock=True
        )

    def _merge_snapshot_into_context(self, ctx: ContextRow, row: dict) -> None:
        """Merge PHASE_SNAPSHOT into existing context."""
        # Check identity match
        if ctx.session_type != row.get('session_type', ''):
            self.add_error(Error(
                error_type='IDENTITY_CONFLICT',
                severity='FATAL',
                session_id=ctx.session_id,
                bar=ctx.bar,
                column_name='session_type',
                value_a=ctx.session_type,
                value_b=row.get('session_type', '')
            ))
            return

        zone_id = row.get('zone_id', '')
        if zone_id == '-1':
            zone_id = None

        zone_type = row.get('zone_type', '')
        if zone_type == 'NONE':
            zone_type = None

        ctx.phase = row.get('phase') or ctx.phase
        ctx.zone_id_snapshot = zone_id or ctx.zone_id_snapshot
        ctx.zone_type_snapshot = zone_type or ctx.zone_type_snapshot
        ctx.snapshot_message = row.get('message') or ctx.snapshot_message
        ctx._source_rows += 1
        ctx._has_snapshot = True

    def _merge_lock_into_context(self, ctx: ContextRow, row: dict) -> None:
        """Merge MODE_LOCK into existing context."""
        # Check identity match
        if ctx.session_type != row.get('session_type', ''):
            self.add_error(Error(
                error_type='IDENTITY_CONFLICT',
                severity='FATAL',
                session_id=ctx.session_id,
                bar=ctx.bar,
                column_name='session_type',
                value_a=ctx.session_type,
                value_b=row.get('session_type', '')
            ))
            return

        # Extract raw_state from message
        message = row.get('message', '')
        raw_state = None
        if message.startswith('raw:'):
            raw_state = message[4:]

        ctx.aggression = row.get('aggression') or ctx.aggression
        ctx.facilitation = row.get('facilitation') or ctx.facilitation
        ctx.market_state = row.get('market_state') or ctx.market_state
        ctx.raw_state = raw_state or ctx.raw_state
        ctx._source_rows += 1
        ctx._has_lock = True

    def _finalize_context(self, ctx: Optional[ContextRow]) -> None:
        """Finalize and store context row."""
        if ctx is None:
            return

        ctx.context_complete = ctx._has_snapshot and ctx._has_lock
        self.contexts.append(ctx)
        self.context_by_session_bar[(ctx.session_id, ctx.bar)] = ctx
        self.summary['context_rows'] += 1

    def _process_engagement(self, row: dict) -> None:
        """Process ENGAGEMENT_FINAL row."""
        session_id = row.get('session_id', '')
        bar = row.get('bar', '')
        zone_id = row.get('zone_id', '')

        # Check for duplicate engagement
        eng_key = (session_id, bar, zone_id)
        if eng_key in self.seen_engagements:
            self.add_error(Error(
                error_type='DUPLICATE_ENGAGEMENT',
                severity='RECOVERABLE',
                session_id=session_id,
                bar=bar,
                value_a=f'zone_id={zone_id}'
            ))
            return
        self.seen_engagements.add(eng_key)

        # Validate prices
        entry_price = row.get('entry_price', '0')
        exit_price = row.get('exit_price', '0')

        try:
            if float(entry_price) == 0:
                self.add_error(Error(
                    error_type='INVALID_ENGAGEMENT_PRICE',
                    severity='RECOVERABLE',
                    session_id=session_id,
                    bar=bar,
                    column_name='entry_price'
                ))
                return
        except ValueError:
            pass

        try:
            if float(exit_price) == 0:
                self.add_error(Error(
                    error_type='INVALID_ENGAGEMENT_PRICE',
                    severity='RECOVERABLE',
                    session_id=session_id,
                    bar=bar,
                    column_name='exit_price'
                ))
                return
        except ValueError:
            pass

        # Validate outcome
        outcome = row.get('outcome', '')
        if outcome not in VALID_OUTCOMES:
            self.add_error(Error(
                error_type='INVALID_OUTCOME',
                severity='RECOVERABLE',
                session_id=session_id,
                bar=bar,
                column_name='outcome',
                value_a=outcome
            ))
            return

        # Context lookup: MAX(bar) WHERE bar <= engagement.bar AND same session
        context = self._lookup_context(session_id, int(bar))

        # Build engagement row
        eng = EngagementRow(
            session_id=session_id,
            bar=bar,
            ts=row.get('ts', ''),
            session_type=row.get('session_type', ''),
            zone_id=zone_id,
            zone_type=row.get('zone_type', ''),
            entry_price=entry_price,
            exit_price=exit_price,
            bars=row.get('bars', ''),
            outcome=outcome,
            escape_vel=row.get('escape_vel', ''),
            vol_ratio=row.get('vol_ratio', '')
        )

        if context is None:
            eng.orphan = True
            eng._flags.append('ORPHAN')
        else:
            eng.context_bar = context.bar
            eng.ctx_aggression = context.aggression
            eng.ctx_facilitation = context.facilitation
            eng.ctx_market_state = context.market_state
            eng.ctx_phase = context.phase

            # Check staleness
            bar_delta = int(bar) - int(context.bar)
            eng.context_stale = bar_delta > STALENESS_THRESHOLD

            if eng.context_stale:
                eng._flags.append('STALE_CONTEXT')

            # Check partial context
            if not context.context_complete:
                eng._flags.append('PARTIAL_CONTEXT')

        self.engagements.append(eng)
        self.summary['engagement_rows'] += 1

    def _lookup_context(self, session_id: str, bar: int) -> Optional[ContextRow]:
        """Find most recent context for session at or before bar."""
        best_context = None
        best_bar = -1

        for (sid, ctx_bar), ctx in self.context_by_session_bar.items():
            if sid != session_id:
                continue
            ctx_bar_int = int(ctx_bar)
            if ctx_bar_int <= bar and ctx_bar_int > best_bar:
                best_bar = ctx_bar_int
                best_context = ctx

        return best_context

    # =========================================================================
    # OUTPUT
    # =========================================================================

    def _write_outputs(self) -> None:
        """Write all output files."""
        self._write_context_csv()
        self._write_engagement_csv()
        self._write_error_csv()
        self._write_summary_json()

    def _write_context_csv(self) -> None:
        """Write context.csv."""
        path = self.output_dir / 'context.csv'

        fieldnames = [
            'session_id', 'bar', 'session_type', 'ts', 'phase',
            'zone_id_snapshot', 'zone_type_snapshot', 'aggression',
            'facilitation', 'market_state', 'raw_state', 'snapshot_message',
            'context_complete', '_source_rows'
        ]

        with open(path, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for ctx in self.contexts:
                writer.writerow(ctx.to_dict())

    def _write_engagement_csv(self) -> None:
        """Write engagement.csv."""
        path = self.output_dir / 'engagement.csv'

        fieldnames = [
            'session_id', 'bar', 'ts', 'session_type', 'zone_id', 'zone_type',
            'entry_price', 'exit_price', 'bars', 'outcome', 'escape_vel', 'vol_ratio',
            'context_bar', 'context_stale', 'ctx_aggression', 'ctx_facilitation',
            'ctx_market_state', 'ctx_phase', 'orphan', '_flags'
        ]

        with open(path, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for eng in self.engagements:
                writer.writerow(eng.to_dict())

    def _write_error_csv(self) -> None:
        """Write error.csv."""
        path = self.output_dir / 'error.csv'

        fieldnames = [
            'error_type', 'severity', 'session_id', 'bar', 'event_type',
            'column_name', 'value_a', 'value_b', 'raw_row', 'ts_detected'
        ]

        with open(path, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for err in self.errors:
                writer.writerow(err.to_dict())

    def _write_summary_json(self) -> None:
        """Write summary.json."""
        path = self.output_dir / 'summary.json'

        self.summary['exit_code'] = 2 if self.has_fatal else (1 if self.has_recoverable else 0)
        self.summary['ts_completed'] = datetime.now().isoformat()

        with open(path, 'w', encoding='utf-8') as f:
            json.dump(self.summary, f, indent=2)


# =============================================================================
# MAIN
# =============================================================================

def main():
    if len(sys.argv) < 3:
        print("Usage: merge_pipeline.py <input.csv> <output_dir>", file=sys.stderr)
        sys.exit(3)

    input_path = sys.argv[1]
    output_dir = sys.argv[2]

    if not os.path.exists(input_path):
        print(f"Input file not found: {input_path}", file=sys.stderr)
        sys.exit(3)

    pipeline = MergePipeline(input_path, output_dir)
    exit_code = pipeline.run()
    sys.exit(exit_code)


if __name__ == '__main__':
    main()
