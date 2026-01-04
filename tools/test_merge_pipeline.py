#!/usr/bin/env python3
"""
Test Suite for Merge Pipeline — Spec v1.1

Tests T01-T21 as defined in the test suite design.
"""

import csv
import json
import os
import tempfile
import shutil
from pathlib import Path
from typing import List, Dict, Optional
import subprocess
import sys

# Path to the pipeline script
PIPELINE_SCRIPT = Path(__file__).parent / 'merge_pipeline.py'


class TestResult:
    def __init__(self, test_id: str):
        self.test_id = test_id
        self.passed = False
        self.errors: List[str] = []

    def fail(self, msg: str):
        self.passed = False
        self.errors.append(msg)

    def __str__(self):
        status = "PASS" if self.passed else "FAIL"
        result = f"{self.test_id}: {status}"
        if self.errors:
            result += "\n  " + "\n  ".join(self.errors)
        return result


def write_csv(path: Path, headers: List[str], rows: List[Dict]):
    """Write test input CSV."""
    with open(path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=headers)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def read_csv(path: Path) -> List[Dict]:
    """Read output CSV."""
    if not path.exists():
        return []
    with open(path, 'r', newline='', encoding='utf-8') as f:
        return list(csv.DictReader(f))


def run_pipeline(input_path: Path, output_dir: Path) -> int:
    """Run the pipeline and return exit code."""
    result = subprocess.run(
        [sys.executable, str(PIPELINE_SCRIPT), str(input_path), str(output_dir)],
        capture_output=True,
        text=True
    )
    return result.returncode


# =============================================================================
# STANDARD HEADERS
# =============================================================================

FULL_HEADERS = [
    'session_id', 'session_type', 'ts', 'bar', 'event_type',
    'zone_id', 'zone_type', 'entry_price', 'exit_price', 'bars',
    'outcome', 'escape_vel', 'vol_ratio', 'aggression', 'facilitation',
    'market_state', 'phase', 'message'
]


# =============================================================================
# TEST IMPLEMENTATIONS
# =============================================================================

def test_t01_happy_path_complete(tmp_dir: Path) -> TestResult:
    """T01: Happy Path — Complete Context + Engagement"""
    result = TestResult("T01")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'AT_BOUNDARY', 'message': 'BASE|test'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': 'EFFICIENT',
         'market_state': 'BALANCE', 'phase': 'ROTATION', 'message': 'raw:BALANCE'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:05', 'bar': '105',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000.00', 'exit_price': '6001.00', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.50', 'vol_ratio': '0.80', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    # Check exit code
    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")
        return result

    # Check context.csv
    contexts = read_csv(output_dir / 'context.csv')
    if len(contexts) != 1:
        result.fail(f"Expected 1 context row, got {len(contexts)}")
    else:
        ctx = contexts[0]
        if ctx['context_complete'] != 'TRUE':
            result.fail(f"Expected context_complete=TRUE, got {ctx['context_complete']}")
        if ctx['phase'] != 'AT_BOUNDARY':
            result.fail(f"Expected phase=AT_BOUNDARY, got {ctx['phase']}")
        if ctx['aggression'] != 'RESPONSIVE':
            result.fail(f"Expected aggression=RESPONSIVE, got {ctx['aggression']}")

    # Check engagement.csv
    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement row, got {len(engagements)}")
    else:
        eng = engagements[0]
        if eng['context_bar'] != '100':
            result.fail(f"Expected context_bar=100, got {eng['context_bar']}")
        if eng['context_stale'] != 'FALSE':
            result.fail(f"Expected context_stale=FALSE, got {eng['context_stale']}")
        if eng['orphan'] != 'FALSE':
            result.fail(f"Expected orphan=FALSE, got {eng['orphan']}")

    # Check error.csv is empty
    errors = read_csv(output_dir / 'error.csv')
    if len(errors) != 0:
        result.fail(f"Expected 0 errors, got {len(errors)}")

    if not result.errors:
        result.passed = True
    return result


def test_t02_multiple_sessions(tmp_dir: Path) -> TestResult:
    """T02: Happy Path — Multiple Sessions (session isolation)"""
    result = TestResult("T02")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'ROTATION', 'message': 'msg1'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': 'EFFICIENT',
         'market_state': 'BALANCE', 'phase': 'ROTATION', 'message': 'raw:BAL'},
        {'session_id': '2', 'session_type': 'RTH', 'ts': '2025-01-01 09:30', 'bar': '50',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '1', 'zone_type': 'VPB_POC',
         'entry_price': '5000.00', 'exit_price': '5001.00', 'bars': '3', 'outcome': 'ACCEPT',
         'escape_vel': '1.00', 'vol_ratio': '0.50', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")
        return result

    # Check engagement is orphan (session 2 has no context)
    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement row, got {len(engagements)}")
    else:
        eng = engagements[0]
        if eng['orphan'] != 'TRUE':
            result.fail(f"Expected orphan=TRUE, got {eng['orphan']}")
        if 'ORPHAN' not in eng['_flags']:
            result.fail(f"Expected ORPHAN in _flags, got {eng['_flags']}")

    if not result.errors:
        result.passed = True
    return result


def test_t03_fatal_duplicate_snapshot(tmp_dir: Path) -> TestResult:
    """T03: Fatal — DUPLICATE_CONTEXT_SNAPSHOT"""
    result = TestResult("T03")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg1'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg2'},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 2:
        result.fail(f"Expected exit code 2, got {exit_code}")

    errors = read_csv(output_dir / 'error.csv')
    fatal_found = any(e['error_type'] == 'DUPLICATE_CONTEXT_SNAPSHOT' and e['severity'] == 'FATAL' for e in errors)
    if not fatal_found:
        result.fail("Expected DUPLICATE_CONTEXT_SNAPSHOT FATAL error")

    if not result.errors:
        result.passed = True
    return result


def test_t04_fatal_duplicate_lock(tmp_dir: Path) -> TestResult:
    """T04: Fatal — DUPLICATE_CONTEXT_LOCK"""
    result = TestResult("T04")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'raw:A'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'INITIATIVE', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'raw:B'},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 2:
        result.fail(f"Expected exit code 2, got {exit_code}")

    errors = read_csv(output_dir / 'error.csv')
    fatal_found = any(e['error_type'] == 'DUPLICATE_CONTEXT_LOCK' and e['severity'] == 'FATAL' for e in errors)
    if not fatal_found:
        result.fail("Expected DUPLICATE_CONTEXT_LOCK FATAL error")

    if not result.errors:
        result.passed = True
    return result


def test_t05_fatal_identity_conflict(tmp_dir: Path) -> TestResult:
    """T05: Fatal — IDENTITY_CONFLICT"""
    result = TestResult("T05")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg'},
        {'session_id': '1', 'session_type': 'RTH', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'raw:X'},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 2:
        result.fail(f"Expected exit code 2, got {exit_code}")

    errors = read_csv(output_dir / 'error.csv')
    fatal_found = any(e['error_type'] == 'IDENTITY_CONFLICT' and e['severity'] == 'FATAL' for e in errors)
    if not fatal_found:
        result.fail("Expected IDENTITY_CONFLICT FATAL error")

    if not result.errors:
        result.passed = True
    return result


def test_t06_fatal_missing_identity(tmp_dir: Path) -> TestResult:
    """T06: Fatal — MISSING_IDENTITY"""
    result = TestResult("T06")

    input_rows = [
        {'session_id': '', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg'},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 2:
        result.fail(f"Expected exit code 2, got {exit_code}")

    errors = read_csv(output_dir / 'error.csv')
    fatal_found = any(e['error_type'] == 'MISSING_IDENTITY' and e['severity'] == 'FATAL' for e in errors)
    if not fatal_found:
        result.fail("Expected MISSING_IDENTITY FATAL error")

    if not result.errors:
        result.passed = True
    return result


def test_t07_fatal_timestamp_inversion(tmp_dir: Path) -> TestResult:
    """T07: Fatal — TIMESTAMP_INVERSION"""
    result = TestResult("T07")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:30', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg1'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '101',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg2'},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 2:
        result.fail(f"Expected exit code 2, got {exit_code}")

    errors = read_csv(output_dir / 'error.csv')
    fatal_found = any(e['error_type'] == 'TIMESTAMP_INVERSION' and e['severity'] == 'FATAL' for e in errors)
    if not fatal_found:
        result.fail("Expected TIMESTAMP_INVERSION FATAL error")

    if not result.errors:
        result.passed = True
    return result


def test_t08_fatal_zone_type_inconsistency(tmp_dir: Path) -> TestResult:
    """T08: Fatal — ZONE_TYPE_INCONSISTENCY"""
    result = TestResult("T08")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:05', 'bar': '105',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'PRIOR_POC',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 2:
        result.fail(f"Expected exit code 2, got {exit_code}")

    errors = read_csv(output_dir / 'error.csv')
    fatal_found = any(e['error_type'] == 'ZONE_TYPE_INCONSISTENCY' and e['severity'] == 'FATAL' for e in errors)
    if not fatal_found:
        result.fail("Expected ZONE_TYPE_INCONSISTENCY FATAL error")

    if not result.errors:
        result.passed = True
    return result


def test_t09_fatal_unsorted_input(tmp_dir: Path) -> TestResult:
    """T09: Fatal — UNSORTED_INPUT"""
    result = TestResult("T09")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:05', 'bar': '105',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg2'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': 'msg1'},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 3:
        result.fail(f"Expected exit code 3, got {exit_code}")

    errors = read_csv(output_dir / 'error.csv')
    fatal_found = any(e['error_type'] == 'UNSORTED_INPUT' and e['severity'] == 'FATAL' for e in errors)
    if not fatal_found:
        result.fail("Expected UNSORTED_INPUT FATAL error")

    if not result.errors:
        result.passed = True
    return result


def test_t10_recoverable_invalid_price(tmp_dir: Path) -> TestResult:
    """T10: Recoverable — INVALID_ENGAGEMENT_PRICE"""
    result = TestResult("T10")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '0', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 1:
        result.fail(f"Expected exit code 1, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 0:
        result.fail(f"Expected 0 engagements (quarantined), got {len(engagements)}")

    errors = read_csv(output_dir / 'error.csv')
    recov_found = any(e['error_type'] == 'INVALID_ENGAGEMENT_PRICE' and e['severity'] == 'RECOVERABLE' for e in errors)
    if not recov_found:
        result.fail("Expected INVALID_ENGAGEMENT_PRICE RECOVERABLE error")

    if not result.errors:
        result.passed = True
    return result


def test_t11_recoverable_invalid_outcome(tmp_dir: Path) -> TestResult:
    """T11: Recoverable — INVALID_OUTCOME"""
    result = TestResult("T11")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'INVALID_VALUE',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 1:
        result.fail(f"Expected exit code 1, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 0:
        result.fail(f"Expected 0 engagements (quarantined), got {len(engagements)}")

    errors = read_csv(output_dir / 'error.csv')
    recov_found = any(e['error_type'] == 'INVALID_OUTCOME' and e['severity'] == 'RECOVERABLE' for e in errors)
    if not recov_found:
        result.fail("Expected INVALID_OUTCOME RECOVERABLE error")

    if not result.errors:
        result.passed = True
    return result


def test_t12_recoverable_duplicate_engagement(tmp_dir: Path) -> TestResult:
    """T12: Recoverable — DUPLICATE_ENGAGEMENT"""
    result = TestResult("T12")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6002', 'bars': '6', 'outcome': 'ACCEPT',
         'escape_vel': '0.6', 'vol_ratio': '0.9', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 1:
        result.fail(f"Expected exit code 1, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement (first wins), got {len(engagements)}")
    elif engagements[0]['outcome'] != 'TEST':
        result.fail(f"Expected first row (outcome=TEST), got {engagements[0]['outcome']}")

    errors = read_csv(output_dir / 'error.csv')
    recov_found = any(e['error_type'] == 'DUPLICATE_ENGAGEMENT' and e['severity'] == 'RECOVERABLE' for e in errors)
    if not recov_found:
        result.fail("Expected DUPLICATE_ENGAGEMENT RECOVERABLE error")

    if not result.errors:
        result.passed = True
    return result


def test_t13_recoverable_duplicate_row(tmp_dir: Path) -> TestResult:
    """T13: Recoverable — DUPLICATE_ROW (Exact Match)"""
    result = TestResult("T13")

    # Three identical rows
    row = {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
           'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
           'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
           'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
           'market_state': '', 'phase': '', 'message': ''}

    input_rows = [row.copy(), row.copy(), row.copy()]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 1:
        result.fail(f"Expected exit code 1, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement (deduped), got {len(engagements)}")

    errors = read_csv(output_dir / 'error.csv')
    dup_found = any(e['error_type'] == 'DUPLICATE_ROW' and e['severity'] == 'RECOVERABLE' for e in errors)
    if not dup_found:
        result.fail("Expected DUPLICATE_ROW RECOVERABLE error")

    if not result.errors:
        result.passed = True
    return result


def test_t14_orphan_engagement(tmp_dir: Path) -> TestResult:
    """T14: Recoverable — ORPHAN_ENGAGEMENT"""
    result = TestResult("T14")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    # Orphan is a flag, not an error - exit code should be 0
    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement, got {len(engagements)}")
    else:
        eng = engagements[0]
        if eng['orphan'] != 'TRUE':
            result.fail(f"Expected orphan=TRUE, got {eng['orphan']}")
        if 'ORPHAN' not in eng['_flags']:
            result.fail(f"Expected ORPHAN in _flags, got {eng['_flags']}")

    if not result.errors:
        result.passed = True
    return result


def test_t15_stale_context(tmp_dir: Path) -> TestResult:
    """T15: Recoverable — STALE_CONTEXT"""
    result = TestResult("T15")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'ROTATION', 'message': 'msg'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': 'EFFICIENT',
         'market_state': 'BALANCE', 'phase': 'ROTATION', 'message': 'raw:X'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 10:00', 'bar': '151',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement, got {len(engagements)}")
    else:
        eng = engagements[0]
        if eng['context_stale'] != 'TRUE':
            result.fail(f"Expected context_stale=TRUE, got {eng['context_stale']}")
        if 'STALE_CONTEXT' not in eng['_flags']:
            result.fail(f"Expected STALE_CONTEXT in _flags, got {eng['_flags']}")

    if not result.errors:
        result.passed = True
    return result


def test_t16_partial_context(tmp_dir: Path) -> TestResult:
    """T16: Recoverable — PARTIAL_CONTEXT_INHERITANCE"""
    result = TestResult("T16")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'AT_BOUNDARY', 'message': 'msg'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:05', 'bar': '105',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")

    contexts = read_csv(output_dir / 'context.csv')
    if len(contexts) != 1:
        result.fail(f"Expected 1 context, got {len(contexts)}")
    else:
        ctx = contexts[0]
        if ctx['context_complete'] != 'FALSE':
            result.fail(f"Expected context_complete=FALSE, got {ctx['context_complete']}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement, got {len(engagements)}")
    else:
        eng = engagements[0]
        if eng['ctx_phase'] != 'AT_BOUNDARY':
            result.fail(f"Expected ctx_phase=AT_BOUNDARY, got {eng['ctx_phase']}")
        if eng['ctx_aggression'] != '':
            result.fail(f"Expected ctx_aggression=NULL/'', got {eng['ctx_aggression']}")
        if 'PARTIAL_CONTEXT' not in eng['_flags']:
            result.fail(f"Expected PARTIAL_CONTEXT in _flags, got {eng['_flags']}")

    if not result.errors:
        result.passed = True
    return result


def test_t17_staleness_at_threshold(tmp_dir: Path) -> TestResult:
    """T17: Boundary — Staleness Exactly at Threshold (50 bars = NOT stale)"""
    result = TestResult("T17")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'ROTATION', 'message': 'msg'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': 'EFFICIENT',
         'market_state': 'BALANCE', 'phase': 'ROTATION', 'message': 'raw:X'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:50', 'bar': '150',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement, got {len(engagements)}")
    else:
        eng = engagements[0]
        # Delta = 150 - 100 = 50, NOT > 50, so NOT stale
        if eng['context_stale'] != 'FALSE':
            result.fail(f"Expected context_stale=FALSE (delta=50, not >50), got {eng['context_stale']}")
        if eng['_flags'] != '':
            result.fail(f"Expected no flags, got {eng['_flags']}")

    if not result.errors:
        result.passed = True
    return result


def test_t18_staleness_past_threshold(tmp_dir: Path) -> TestResult:
    """T18: Boundary — Staleness One Past Threshold (51 bars = stale)"""
    result = TestResult("T18")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'ROTATION', 'message': 'msg'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': 'EFFICIENT',
         'market_state': 'BALANCE', 'phase': 'ROTATION', 'message': 'raw:X'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:51', 'bar': '151',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement, got {len(engagements)}")
    else:
        eng = engagements[0]
        # Delta = 151 - 100 = 51, > 50, so IS stale
        if eng['context_stale'] != 'TRUE':
            result.fail(f"Expected context_stale=TRUE (delta=51, >50), got {eng['context_stale']}")
        if 'STALE_CONTEXT' not in eng['_flags']:
            result.fail(f"Expected STALE_CONTEXT in _flags, got {eng['_flags']}")

    if not result.errors:
        result.passed = True
    return result


def test_t19_same_bar_context_engagement(tmp_dir: Path) -> TestResult:
    """T19: Boundary — Same-Bar Context and Engagement"""
    result = TestResult("T19")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'ROTATION', 'message': 'msg'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': 'EFFICIENT',
         'market_state': 'BALANCE', 'phase': 'ROTATION', 'message': 'raw:X'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path = tmp_dir / 'input.csv'
    output_dir = tmp_dir / 'output'
    write_csv(input_path, FULL_HEADERS, input_rows)

    exit_code = run_pipeline(input_path, output_dir)

    if exit_code != 0:
        result.fail(f"Expected exit code 0, got {exit_code}")

    contexts = read_csv(output_dir / 'context.csv')
    if len(contexts) != 1:
        result.fail(f"Expected 1 context, got {len(contexts)}")

    engagements = read_csv(output_dir / 'engagement.csv')
    if len(engagements) != 1:
        result.fail(f"Expected 1 engagement, got {len(engagements)}")
    else:
        eng = engagements[0]
        if eng['context_bar'] != '100':
            result.fail(f"Expected context_bar=100, got {eng['context_bar']}")
        if eng['orphan'] != 'FALSE':
            result.fail(f"Expected orphan=FALSE, got {eng['orphan']}")
        if eng['context_stale'] != 'FALSE':
            result.fail(f"Expected context_stale=FALSE, got {eng['context_stale']}")

    if not result.errors:
        result.passed = True
    return result


def test_t20_determinism_duplicate_order(tmp_dir: Path) -> TestResult:
    """T20: Determinism — Duplicate Resolution Order (first wins)"""
    result = TestResult("T20")

    # Run A: TEST first
    input_rows_a = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6002', 'bars': '6', 'outcome': 'ACCEPT',
         'escape_vel': '0.6', 'vol_ratio': '0.9', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path_a = tmp_dir / 'input_a.csv'
    output_dir_a = tmp_dir / 'output_a'
    write_csv(input_path_a, FULL_HEADERS, input_rows_a)
    exit_code_a = run_pipeline(input_path_a, output_dir_a)

    # Run B: ACCEPT first (reversed)
    input_rows_b = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6002', 'bars': '6', 'outcome': 'ACCEPT',
         'escape_vel': '0.6', 'vol_ratio': '0.9', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    input_path_b = tmp_dir / 'input_b.csv'
    output_dir_b = tmp_dir / 'output_b'
    write_csv(input_path_b, FULL_HEADERS, input_rows_b)
    exit_code_b = run_pipeline(input_path_b, output_dir_b)

    if exit_code_a != 1 or exit_code_b != 1:
        result.fail(f"Expected exit codes 1, got A={exit_code_a}, B={exit_code_b}")

    eng_a = read_csv(output_dir_a / 'engagement.csv')
    eng_b = read_csv(output_dir_b / 'engagement.csv')

    if len(eng_a) != 1 or len(eng_b) != 1:
        result.fail(f"Expected 1 engagement each, got A={len(eng_a)}, B={len(eng_b)}")
    else:
        # A should have TEST (first in A's input)
        if eng_a[0]['outcome'] != 'TEST':
            result.fail(f"Run A: Expected outcome=TEST (first), got {eng_a[0]['outcome']}")
        # B should have ACCEPT (first in B's input)
        if eng_b[0]['outcome'] != 'ACCEPT':
            result.fail(f"Run B: Expected outcome=ACCEPT (first), got {eng_b[0]['outcome']}")

    if not result.errors:
        result.passed = True
    return result


def test_t21_idempotency(tmp_dir: Path) -> TestResult:
    """T21: Determinism — Idempotency Verification"""
    result = TestResult("T21")

    input_rows = [
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'PHASE_SNAPSHOT', 'zone_id': '-1', 'zone_type': 'NONE',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': 'ROTATION', 'message': 'msg'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:00', 'bar': '100',
         'event_type': 'MODE_LOCK', 'zone_id': '0', 'zone_type': '',
         'entry_price': '0', 'exit_price': '0', 'bars': '0', 'outcome': '',
         'escape_vel': '0', 'vol_ratio': '0', 'aggression': 'RESPONSIVE', 'facilitation': 'EFFICIENT',
         'market_state': 'BALANCE', 'phase': 'ROTATION', 'message': 'raw:X'},
        {'session_id': '1', 'session_type': 'GLOBEX', 'ts': '2025-01-01 09:05', 'bar': '105',
         'event_type': 'ENGAGEMENT_FINAL', 'zone_id': '3', 'zone_type': 'VPB_VAL',
         'entry_price': '6000', 'exit_price': '6001', 'bars': '5', 'outcome': 'TEST',
         'escape_vel': '0.5', 'vol_ratio': '0.8', 'aggression': '', 'facilitation': '',
         'market_state': '', 'phase': '', 'message': ''},
    ]

    # Run 1
    input_path = tmp_dir / 'input.csv'
    output_dir_1 = tmp_dir / 'output_1'
    write_csv(input_path, FULL_HEADERS, input_rows)
    exit_code_1 = run_pipeline(input_path, output_dir_1)

    # Run 2 (same input)
    output_dir_2 = tmp_dir / 'output_2'
    exit_code_2 = run_pipeline(input_path, output_dir_2)

    if exit_code_1 != exit_code_2:
        result.fail(f"Exit codes differ: run1={exit_code_1}, run2={exit_code_2}")

    # Compare context.csv
    ctx_1 = read_csv(output_dir_1 / 'context.csv')
    ctx_2 = read_csv(output_dir_2 / 'context.csv')
    if ctx_1 != ctx_2:
        result.fail("context.csv differs between runs")

    # Compare engagement.csv
    eng_1 = read_csv(output_dir_1 / 'engagement.csv')
    eng_2 = read_csv(output_dir_2 / 'engagement.csv')
    if eng_1 != eng_2:
        result.fail("engagement.csv differs between runs")

    if not result.errors:
        result.passed = True
    return result


# =============================================================================
# TEST RUNNER
# =============================================================================

ALL_TESTS = [
    test_t01_happy_path_complete,
    test_t02_multiple_sessions,
    test_t03_fatal_duplicate_snapshot,
    test_t04_fatal_duplicate_lock,
    test_t05_fatal_identity_conflict,
    test_t06_fatal_missing_identity,
    test_t07_fatal_timestamp_inversion,
    test_t08_fatal_zone_type_inconsistency,
    test_t09_fatal_unsorted_input,
    test_t10_recoverable_invalid_price,
    test_t11_recoverable_invalid_outcome,
    test_t12_recoverable_duplicate_engagement,
    test_t13_recoverable_duplicate_row,
    test_t14_orphan_engagement,
    test_t15_stale_context,
    test_t16_partial_context,
    test_t17_staleness_at_threshold,
    test_t18_staleness_past_threshold,
    test_t19_same_bar_context_engagement,
    test_t20_determinism_duplicate_order,
    test_t21_idempotency,
]


def main():
    print("=" * 60)
    print("Merge Pipeline Test Suite — Spec v1.1")
    print("=" * 60)
    print()

    passed = 0
    failed = 0
    results = []

    for test_fn in ALL_TESTS:
        # Create temp dir for each test
        tmp_dir = Path(tempfile.mkdtemp())
        try:
            result = test_fn(tmp_dir)
            results.append(result)
            if result.passed:
                passed += 1
                print(f"  [PASS] {result.test_id}")
            else:
                failed += 1
                print(f"  [FAIL] {result.test_id}")
                for err in result.errors:
                    print(f"         - {err}")
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    print()
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed, {len(ALL_TESTS)} total")
    print("=" * 60)

    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
