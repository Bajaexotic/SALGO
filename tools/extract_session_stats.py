#!/usr/bin/env python3
"""
AMT Framework - Session Statistics Extractor v2.0

Parses Sierra Chart AMT study logs and extracts per-session calibration metrics
for all engines: Liquidity, Volatility, Delta, Volume Acceptance, Imbalance, etc.

Usage:
    python extract_session_stats.py <logfile.txt> [--output calibration_report.txt]
    python extract_session_stats.py <logfile.txt> --csv metrics.csv
    python extract_session_stats.py <logfile.txt> --json metrics.json

Input: Sierra Chart message log export (copy/paste from Log window)
Output: Structured calibration report with metrics per session

Requirements: Python 3.6+
"""

import re
import sys
import json
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple
from datetime import datetime

# =============================================================================
# DATA CLASSES
# =============================================================================

@dataclass
class LiquiditySample:
    """Single liquidity observation from State: line."""
    bar: int = 0
    # Context fields
    zone: str = ""               # ZONE=NONE, VPB_POC, etc.
    session: str = ""            # SESS=IB, MID_SESS, etc.
    facil: str = ""              # FACIL=LABORED, EFFICIENT, etc.
    delta_frac: float = 0.0      # DELTA_FRAC=0.56
    # Liquidity fields
    liq_score: float = 0.0       # LIQ=0.42
    liq_state: str = ""          # THIN, NORMAL, THICK, VOID
    depth_rank: int = 0          # D=11
    stress_rank: int = 0         # S=100
    resilience_rank: int = 0     # R=0
    tightness_rank: int = 0      # T=0
    obi: float = 0.0             # OBI=+0.20


@dataclass
class VolatilitySample:
    """Single volatility observation from VOL: line."""
    bar: int = 0
    regime: str = ""             # CMP, NRM, EXP, EVT
    percentile: float = 0.0      # p=100
    stable_bars: int = 0         # s=3
    effort_ratio: float = 0.0    # ER=0.70
    effort_pctile: int = 0       # (p62)
    chop: float = 0.0            # chop=0.30
    expansion_mode: str = ""     # EXPAND, COMPRESS, etc.
    coef_variation: float = 0.0  # cv=0.29
    is_shock: bool = False       # SHOCK=Y
    is_extreme: bool = False     # EXT flag
    stop_distance: float = 0.0   # stop>=67.5t
    is_transitioning: bool = False  # TRANS flag
    after_bars: int = 0          # after=2 (bars after shock/transition)


@dataclass
class FacilitationSample:
    """Single facilitation observation from FACIL: line."""
    bar: int = 0
    mode: str = ""               # MODE=ROTATION, TRENDING, etc.
    zone: str = ""               # ZONE=LOWER_VALUE
    effort_result: str = ""      # ER=HIGH_LOW
    interpretation: str = ""     # INTERP=ABSORPTION_IN_VALUE
    vol_pct: int = 0             # vol%=99
    rng_pct: int = 0             # rng%=0


@dataclass
class DeltaSample:
    """Single DELTA observation."""
    timestamp: str = ""
    bar: int = 0
    character: str = ""          # UNKNOWN, NEUTRAL, EPISODIC, SUSTAINED, BUILDING, FADING, REVERSAL
    alignment: str = ""          # UNKNOWN, NEUTRAL, CONVERGENT, DIVERGENT, ABSORPTION_BID, ABSORPTION_ASK
    confidence: str = ""         # UNKNOWN, BLOCKED, LOW, DEGRADED, FULL
    bar_pct: int = 0             # Bar delta percentile
    sess_pct: int = 0            # Session delta percentile
    vol_pct: int = 0             # Volume percentile
    flags: str = ""              # !REV, INST, (range-adj), etc.
    error_reason: str = ""       # BLOCKED_VOL_EVENT, etc.
    # Trading constraints
    cont_ok: bool = False        # Continuation allowed
    bkout_ok: bool = False       # Breakout allowed
    pos_mult: float = 1.0        # Position size multiplier


@dataclass
class VolAccSample:
    """Single VOLACC observation (both old and new formats)."""
    timestamp: str = ""
    bar: int = 0
    state: str = ""              # UNK, TEST, ACC, REJ, TESTING, ACCEPTED, REJECTED
    intensity: str = ""          # VL, LO, NM, HI, VH, EX, SH
    migration: str = ""          # UNCHANGED, HIGHER, LOWER, etc.
    percentile: int = 0
    acc_score: float = 0.0
    rej_score: float = 0.0
    multiplier: float = 1.0
    flags: str = ""              # LV, WK, DV, FR
    hold_bars: int = 0           # HOLD=14403bars
    reentry: str = ""            # reentry=NO/YES


@dataclass
class StructureSample:
    """Single structure observation from Struct: line."""
    bar: int = 0
    sess_hi: float = 0.0
    sess_lo: float = 0.0
    dist_hi_ticks: int = 0
    dist_lo_ticks: int = 0
    ib_hi: float = 0.0
    ib_lo: float = 0.0
    dist_ib_hi_ticks: int = 0
    dist_ib_lo_ticks: int = 0
    ib_status: str = ""          # OPEN, FROZEN
    range_ticks: int = 0


@dataclass
class DaltonSample:
    """Single DALTON observation."""
    bar: int = 0
    session_type: str = ""       # GLOBEX, RTH_AM, etc.
    mini_ib_hi: float = 0.0
    mini_ib_lo: float = 0.0
    mini_ib_status: str = ""     # OPEN, FROZEN
    timeframe: str = ""          # 1TF_DOWN, 2TF, etc.
    rotation_count: int = 0
    opening_type: str = ""       # OD_UP, OD_DN, OTD_UP, OTD_DN, ORR_UP, ORR_DN, OA
    opening_status: str = ""     # CLASSIFIED, PENDING
    bridge_status: str = ""      # pending, complete


@dataclass
class AMTStateSample:
    """Single AMT state observation from AMT: line."""
    bar: int = 0
    market_state: str = ""       # BALANCE, IMBALANCE
    location: str = ""           # UPPER_VALUE, LOWER_VALUE, AT_POC, AT_VAH, AT_VAL, etc.
    activity: str = ""           # INITIATIVE, RESPONSIVE, NEUTRAL
    phase: str = ""              # DRIVING_UP, DRIVING_DN, ROTATION, TESTING_BOUNDARY, etc.
    excess: str = ""             # POOR_HIGH, POOR_LOW, EXCESS_HIGH, EXCESS_LOW, NONE
    signal_priority: int = 0     # SP=4


@dataclass
class SessionDeltaSample:
    """Single SessionDelta observation."""
    bar: int = 0
    sess_cum: float = 0.0
    sess_ratio: float = 0.0
    phase_cum: float = 0.0
    phase_ratio: float = 0.0
    pctile: float = 0.0
    volume: int = 0
    phase: str = ""
    phase_count: int = 0


@dataclass
class DeltaFlagsSample:
    """Single DeltaFlags observation."""
    bar: int = 0
    ext_bar: bool = False
    ext_sess: bool = False
    extreme: bool = False
    coherent: bool = False
    valid: bool = False
    bar_abs_pctl: int = 0


@dataclass
class EnvSample:
    """Single Env: observation."""
    bar: int = 0
    vol_state: str = ""          # COMPRESSION, NORMAL, EXPANSION, EXTREME
    liq_state: str = ""          # VOID, THIN, NORMAL, THICK
    friction: str = ""           # TIGHT, NORMAL, WIDE
    outcome: str = ""            # PENDING, ACCEPTED, REJECTED
    transition: str = ""         # NONE, etc.
    intent: str = ""             # NEUTRAL, etc.


@dataclass
class RegimeSample:
    """Single Regime: observation."""
    bar: int = 0
    bar_regime_prev: str = ""    # BALANCE, IMBALANCE
    phase: str = ""              # ACCEPTING, ROTATION, DRIVING, etc.
    aggression_prev: str = ""    # INITIATIVE, RESPONSIVE, NEUTRAL
    side_prev: str = ""          # NEUTRAL, BULLISH, BEARISH


@dataclass
class OBISample:
    """Single OBI-TRACE observation (Order Book Imbalance)."""
    bar: int = 0
    input_bid: int = 0           # Raw bid levels
    input_ask: int = 0           # Raw ask levels
    filtered_bid: int = 0        # Filtered bid levels
    filtered_ask: int = 0        # Filtered ask levels
    skipped: int = 0             # Skipped levels
    dir_valid: bool = False      # Directional validity
    obi: float = 0.0             # Order Book Imbalance [-1, +1]
    snap_obi: float = 0.0        # Snapshot OBI
    mkt_state: int = 0           # Market state code


@dataclass
class BarSummarySample:
    """Bar summary line: Bar N | Phase: X | Zones: N active."""
    bar: int = 0
    phase: str = ""              # FAILED_AUC, ROTATION, DRIVING, etc.
    active_zones: int = 0        # Number of active zones


@dataclass
class DeltaExtSample:
    """Delta extended metrics: trades/range/avg percentiles, thresholds, hysteresis."""
    bar: int = 0
    trades_pctile: float = 0.0   # Trades percentile
    range_pctile: float = 0.0    # Range percentile
    avg_pctile: float = 0.0      # Avg trade size percentile
    noise_floor: float = 0.0     # Noise floor threshold
    strong_signal: float = 0.0   # Strong signal threshold
    char_req: int = 0            # Character hysteresis requirement
    align_req: int = 0           # Alignment hysteresis requirement


@dataclass
class MarketStateSample:
    """Market state from STATE line: STATE: IMBALANCE | PHASE: FAILED_AUC | streak=0/3."""
    bar: int = 0
    market_state: str = ""       # BALANCE, IMBALANCE, UNKNOWN
    phase: str = ""              # FAILED_AUC, ROTATION, DRIVING_UP, etc.
    streak_current: int = 0      # Current streak count
    streak_required: int = 0     # Required streak for confirmation


@dataclass
class DayTypeSample:
    """Day type and shape from DayType line."""
    bar: int = 0
    structure: str = ""          # BALANCED, IMBALANCED
    raw_shape: str = ""          # P_SHAPED, B_SHAPED, D_SHAPED, etc.
    resolved_shape: str = ""     # UNDEFINED, P_SHAPED, etc.
    is_conflict: bool = False    # [CONFLICT] flag present


@dataclass
class VolumeSample:
    """Per-bar volume from Volume: line."""
    bar: int = 0
    volume: int = 0              # Vol=952
    delta: int = 0               # Delta=-154
    delta_pct: float = 0.0       # (-16.2%)


@dataclass
class WarningSample:
    """Warning counts from Warnings: line."""
    bar: int = 0
    width_mismatch: int = 0      # WidthMismatch=0
    val_divergence: int = 0      # ValDivergence=36
    config_err: int = 0          # ConfigErr=0
    vbp_warn: int = 0            # VbPWarn=0
    has_active: bool = False     # * at end indicates active warnings


@dataclass
class TpoSample:
    """TPO observation from TPO: line."""
    bar: int = 0
    period: str = ""             # PERIOD=N (A, B, C, ..., N, etc.)
    timeframe: str = ""          # TF=1TF_DOWN, 2TF, etc.
    ext_hi: bool = False         # EXT_HI=Y/N
    ext_hi_ticks: int = 0        # (-40t) distance
    ext_lo: bool = False         # EXT_LO=Y/N
    ext_lo_ticks: int = 0        # (13t) distance
    threshold: int = 0           # THR=4


@dataclass
class EffortSample:
    """Effort observation from Effort: line (rate format)."""
    bar: int = 0
    source: str = ""             # SRC=NB
    bid_rate: float = 0.0        # BidRate=33.00 (vol/sec)
    ask_rate: float = 0.0        # AskRate=17.00 (vol/sec)
    bar_sec: int = 0             # BarSec=60
    est_vol_bid: int = 0         # EstVol=1980/1020 (bid part)
    est_vol_ask: int = 0         # EstVol=1980/1020 (ask part)
    total_vol: int = 0           # TotVol=50


@dataclass
class EffortDeltaSample:
    """Effort observation from Effort: line (delta format)."""
    bar: int = 0
    nb_cum_delta: int = 0        # NB_CumDelta=-5443
    max_delta: int = 0           # MaxDelta=14


@dataclass
class MarketStateBarsSample:
    """Market state observation from 'Market State:' line."""
    bar: int = 0
    state: str = ""              # BALANCE, IMBALANCE, UNKNOWN
    bars_in_state: int = 0       # bars=19


@dataclass
class StructSample:
    """Structure observation from Struct: line."""
    bar: int = 0
    sess_hi: float = 0.0         # SESS_HI=7016.75
    sess_lo: float = 0.0         # SESS_LO=6959.00
    dist_hi_t: int = 0           # DIST_HI_T=50
    dist_lo_t: int = 0           # DIST_LO_T=181
    ib_hi: float = 0.0           # IB_HI=6997.25
    ib_lo: float = 0.0           # IB_LO=6959.00
    dist_ib_hi_t: int = 0        # DIST_IB_HI_T=-28
    dist_ib_lo_t: int = 0        # DIST_IB_LO_T=181
    ib_status: str = ""          # IB=FROZEN
    range_t: int = 0             # RANGE_T=231


@dataclass
class VASample:
    """Value Area observation from VA: line."""
    bar: int = 0
    poc: float = 0.0             # POC=7004.25
    vah: float = 0.0             # VAH=7016.75
    val: float = 0.0             # VAL=6984.50
    range_ticks: int = 0         # Range=129 ticks


@dataclass
class SessionDeltaSample:
    """Session delta observation from SessionDelta: line."""
    bar: int = 0
    sess_cum: int = 0            # SessCum=-5478
    sess_ratio: float = 0.0      # SessRatio=-0.0050
    phase_cum: int = 0           # PhaseCum=-1838035
    phase_ratio: float = 0.0     # PhaseRatio=-0.0737
    pctile: float = 0.0          # Pctile=100.0
    volume: int = 0              # Vol=1089918
    phase: str = ""              # Phase=CLOSING
    phase_n: int = 0             # (n=5)


@dataclass
class DeltaFlagsSample:
    """Delta flags observation from DeltaFlags: line."""
    bar: int = 0
    ext_bar: bool = False        # ExtBar=N
    ext_sess: bool = False       # ExtSess=N
    extreme: bool = False        # Extreme=N
    coherent: bool = False       # Coherent=N
    valid: bool = False          # Valid=Y
    bar_abs_pctl: int = 0        # BarAbsPctl=79


@dataclass
class TouchesSample:
    """Zone touch counts from Touches: line."""
    bar: int = 0
    vah_touches: int = 0         # VAH=21
    poc_touches: int = 0         # POC=11
    val_touches: int = 0         # VAL=2
    total_touches: int = 0       # Total=34


@dataclass
class HvnLvnSummarySample:
    """HVN/LVN summary from HVN: line."""
    bar: int = 0
    hvn_count: int = 0           # HVN: 10
    hvn_added: int = 0           # (+73803/-0) added part
    hvn_removed: int = 0         # (+73803/-0) removed part
    lvn_count: int = 0           # LVN: 6
    lvn_added: int = 0           # (+41698/-0) added part
    lvn_removed: int = 0         # (+41698/-0) removed part
    widest_gap: int = 0          # WidestGap: 88 ticks


@dataclass
class HvnPricesSample:
    """HVN prices from HVN prices: line."""
    bar: int = 0
    prices: List[Tuple[float, int]] = None  # [(6960.00, 3114), ...]

    def __post_init__(self):
        if self.prices is None:
            self.prices = []


@dataclass
class LvnPricesSample:
    """LVN prices from LVN prices: line."""
    bar: int = 0
    prices: List[Tuple[float, int]] = None  # [(6961.50, 409), ...]

    def __post_init__(self):
        if self.prices is None:
            self.prices = []


@dataclass
class TransitionsSample:
    """Transition counts from Transitions: line."""
    bar: int = 0
    session: int = 0             # Session=1
    phase: int = 0               # Phase=40
    state: int = 0               # State=4331


@dataclass
class EffortRatesSample:
    """Effort rates from Effort: DeltaSec=... line."""
    bar: int = 0
    delta_sec: float = 0.0       # DeltaSec=-0.02
    trades_sec: float = 0.0      # TradesSec=0.02


@dataclass
class DomSample:
    """DOM snapshot from DOM: line."""
    bar: int = 0
    bid_sz: int = 0              # BidSz=149
    ask_sz: int = 0              # AskSz=79
    bid_stk: int = 0             # BidStk=345
    ask_stk: int = 0             # AskStk=576
    bid_price: float = 0.0       # Bid=7004.25
    ask_price: float = 0.0       # Ask=7004.50


@dataclass
class EffortBaselinesSample:
    """Effort baselines status from EffortBaselines: line."""
    bar: int = 0
    cur_phase: str = ""          # CurPhase=CLOSING
    sessions_ready: int = 0      # sessions=5/4 (ready part)
    sessions_total: int = 0      # sessions=5/4 (total part)
    bars: int = 0                # bars=225


@dataclass
class SessionDeltaBaselineSample:
    """Per-phase session delta baseline from SessionDelta[PHASE]: line."""
    bar: int = 0
    phase: str = ""              # [CLOSING]
    status: str = ""             # READY
    sessions: int = 0            # sessions=5
    size: int = 0                # size=5
    mean: float = 0.0            # mean=0.0187
    median: float = 0.0          # median=0.0200
    mad: float = 0.0             # mad=0.0057


@dataclass
class DomBaselineSample:
    """Per-phase DOM baseline from DOMBaseline[PHASE]: line."""
    bar: int = 0
    phase: str = ""              # [CLOSING]
    core_status: str = ""        # core=READY
    core_n: int = 0              # n=5
    core_depth: int = 0          # depth=5107
    halo_status: str = ""        # halo=READY
    halo_mass: int = 0           # mass=4758
    halo_imbal: int = 0          # imbal=4758
    spread_status: str = ""      # spread=READY
    spread_n: int = 0            # n=6000


@dataclass
class DomHaloSample:
    """Per-phase DOM halo from DOMHalo[PHASE]: line."""
    bar: int = 0
    phase: str = ""              # [CLOSING]
    mass_median: int = 0         # massMedian=1012
    imbal_median: float = 0.0    # imbalMedian=-0.037


@dataclass
class ProbeSample:
    """Probe statistics from Probes: line."""
    bar: int = 0
    fired: int = 0
    resolved: int = 0
    hit: int = 0
    miss: int = 0
    expired: int = 0
    hit_rate: float = 0.0
    avg_score: float = 0.0


@dataclass
class EngagementSample:
    """Engagement statistics from Engagements: line."""
    bar: int = 0
    engagements: int = 0
    escapes: int = 0
    avg_bars: float = 0.0
    avg_vel: float = 0.0


@dataclass
class ZoneOutcomeSample:
    """Terminal zone engagement outcome from ZONE_OUTCOME: line.

    Format: ZONE_OUTCOME: Zone=%s Outcome=%s Bars=%d VolRatio=%.2f Touch=%s EscVel=%.2f
    This captures the ACTUAL outcome (ACCEPTED/REJECTED/PENDING) at zone exit,
    which is not visible in Env line (shows UNK after zone exit).
    """
    bar: int = 0
    zone_type: str = ""          # VPB_POC, VPB_VAH, VPB_VAL, PRIOR_POC, etc.
    outcome: str = ""            # ACCEPTED, REJECTED, PENDING
    bars_engaged: int = 0        # Number of bars engaged
    vol_ratio: float = 0.0       # Volume ratio vs session average
    touch_type: str = ""         # TAG, PROBE, TEST, ACCEPTANCE, UNRESOLVED
    escape_velocity: float = 0.0 # Escape velocity in ticks/bar


@dataclass
class CorrelatedSample:
    """VOLACC + DELTA + LIQ paired on the same bar."""
    bar: int = 0
    # VOLACC fields
    volacc_state: str = ""
    volacc_intensity: str = ""
    volacc_pct: int = 0
    volacc_acc: float = 0.0
    volacc_rej: float = 0.0
    # DELTA fields
    delta_char: str = ""
    delta_align: str = ""
    delta_conf: str = ""
    delta_bar_pct: int = 0
    delta_sess_pct: int = 0
    delta_vol_pct: int = 0
    delta_flags: str = ""
    # LIQ fields
    liq_score: float = 0.0
    liq_state: str = ""
    depth_rank: int = 0
    stress_rank: int = 0
    # VOL fields
    vol_regime: str = ""
    vol_pctile: float = 0.0
    is_shock: bool = False


@dataclass
class SessionStats:
    """Holds calibration metrics for one session."""
    session_date: str = ""
    session_type: str = ""  # RTH_AM, MID_SESS, RTH_PM, GLOBEX, etc.
    first_bar: int = 0
    last_bar: int = 0
    total_bars: int = 0

    # Phase distribution (final values)
    pct_rotation: float = 0.0
    pct_testing: float = 0.0
    pct_driving: float = 0.0
    pct_extension: float = 0.0
    pct_pullback: float = 0.0
    pct_failed: float = 0.0
    pct_accepting: float = 0.0
    pct_outside_balance: float = 0.0

    # Transition counts
    session_transitions: int = 0
    phase_transitions: int = 0
    state_transitions: int = 0

    # Market State observations (AMTMarketState - SSOT)
    market_state_balance_count: int = 0
    market_state_imbalance_count: int = 0

    # Key events
    pullback_events: int = 0
    acceptance_events: int = 0
    range_ext_events: int = 0

    # Structure
    session_range_ticks: int = 0
    va_range_ticks: int = 0

    # Market state
    market_state: str = ""
    balance_pct: float = 0.0

    # Shape freeze
    shape_freeze_bar: int = 0
    shape_day_structure: str = ""
    shape_raw_frozen: str = ""
    shape_final_frozen: str = ""
    shape_conflict: bool = False
    shape_frozen: bool = False

    # ===== NEW METRICS =====

    # Liquidity Engine samples
    liq_samples: List[LiquiditySample] = field(default_factory=list)
    liq_state_counts: Dict[str, int] = field(default_factory=dict)  # VOID, THIN, NORMAL, THICK
    liq_score_values: List[float] = field(default_factory=list)
    liq_depth_values: List[int] = field(default_factory=list)
    liq_stress_values: List[int] = field(default_factory=list)
    liq_resilience_values: List[int] = field(default_factory=list)
    liq_obi_values: List[float] = field(default_factory=list)

    # Volatility Engine samples
    vol_samples: List[VolatilitySample] = field(default_factory=list)
    vol_regime_counts: Dict[str, int] = field(default_factory=dict)
    vol_pctile_values: List[float] = field(default_factory=list)
    vol_stable_values: List[int] = field(default_factory=list)
    vol_er_values: List[float] = field(default_factory=list)
    vol_chop_values: List[float] = field(default_factory=list)
    vol_shock_count: int = 0
    vol_extreme_count: int = 0

    # Facilitation samples
    facil_samples: List[FacilitationSample] = field(default_factory=list)
    facil_mode_counts: Dict[str, int] = field(default_factory=dict)
    facil_er_counts: Dict[str, int] = field(default_factory=dict)
    facil_interp_counts: Dict[str, int] = field(default_factory=dict)

    # Volume Acceptance samples
    volacc_samples: List[VolAccSample] = field(default_factory=list)
    volacc_warmup_count: int = 0
    volacc_ready_count: int = 0
    volacc_state_counts: Dict[str, int] = field(default_factory=dict)
    volacc_intensity_counts: Dict[str, int] = field(default_factory=dict)
    volacc_migration_counts: Dict[str, int] = field(default_factory=dict)
    volacc_flag_counts: Dict[str, int] = field(default_factory=dict)
    volacc_avg_pct: float = 0.0
    volacc_avg_acc: float = 0.0
    volacc_avg_rej: float = 0.0
    volacc_avg_mult: float = 0.0

    # Delta Engine samples
    delta_samples: List[DeltaSample] = field(default_factory=list)
    delta_char_counts: Dict[str, int] = field(default_factory=dict)
    delta_align_counts: Dict[str, int] = field(default_factory=dict)
    delta_conf_counts: Dict[str, int] = field(default_factory=dict)
    delta_error_counts: Dict[str, int] = field(default_factory=dict)
    delta_bar_pct_values: List[int] = field(default_factory=list)
    delta_sess_pct_values: List[int] = field(default_factory=list)
    delta_vol_pct_values: List[int] = field(default_factory=list)
    delta_flag_counts: Dict[str, int] = field(default_factory=dict)

    # Session Delta metrics
    session_delta_samples: List[SessionDeltaSample] = field(default_factory=list)
    sess_delta_pctile_values: List[float] = field(default_factory=list)
    sess_delta_ratio_values: List[float] = field(default_factory=list)

    # Delta Flags metrics
    delta_flags_samples: List[DeltaFlagsSample] = field(default_factory=list)
    delta_extreme_bar_count: int = 0
    delta_extreme_sess_count: int = 0
    delta_extreme_combined_count: int = 0
    delta_coherent_count: int = 0

    # Structure samples
    struct_samples: List[StructureSample] = field(default_factory=list)
    ib_status_counts: Dict[str, int] = field(default_factory=dict)

    # Dalton samples
    dalton_samples: List[DaltonSample] = field(default_factory=list)
    dalton_session_counts: Dict[str, int] = field(default_factory=dict)
    dalton_tf_counts: Dict[str, int] = field(default_factory=dict)
    dalton_opening_counts: Dict[str, int] = field(default_factory=dict)

    # AMT State samples
    amt_state_samples: List[AMTStateSample] = field(default_factory=list)
    amt_location_counts: Dict[str, int] = field(default_factory=dict)
    amt_activity_counts: Dict[str, int] = field(default_factory=dict)
    amt_phase_counts: Dict[str, int] = field(default_factory=dict)
    amt_excess_counts: Dict[str, int] = field(default_factory=dict)
    amt_signal_priority_values: List[int] = field(default_factory=list)

    # Environment samples
    env_samples: List[EnvSample] = field(default_factory=list)
    env_vol_counts: Dict[str, int] = field(default_factory=dict)
    env_liq_counts: Dict[str, int] = field(default_factory=dict)
    env_fric_counts: Dict[str, int] = field(default_factory=dict)
    env_outcome_counts: Dict[str, int] = field(default_factory=dict)
    env_intent_counts: Dict[str, int] = field(default_factory=dict)
    env_transition_counts: Dict[str, int] = field(default_factory=dict)

    # Regime samples
    regime_samples: List[RegimeSample] = field(default_factory=list)
    regime_bar_regime_counts: Dict[str, int] = field(default_factory=dict)
    regime_phase_counts: Dict[str, int] = field(default_factory=dict)
    regime_aggression_counts: Dict[str, int] = field(default_factory=dict)
    regime_side_counts: Dict[str, int] = field(default_factory=dict)

    # OBI (Order Book Imbalance) metrics
    obi_samples: List[OBISample] = field(default_factory=list)
    obi_dir_valid_count: int = 0         # Count where dir.valid=1
    obi_mkt_state_counts: Dict[int, int] = field(default_factory=dict)

    # Bar summary metrics
    bar_summary_samples: List[BarSummarySample] = field(default_factory=list)
    bar_summary_phase_counts: Dict[str, int] = field(default_factory=dict)

    # Delta extended metrics
    delta_ext_samples: List[DeltaExtSample] = field(default_factory=list)

    # Market state metrics (from STATE: line)
    market_state_samples: List[MarketStateSample] = field(default_factory=list)
    market_state_state_counts: Dict[str, int] = field(default_factory=dict)  # BALANCE, IMBALANCE
    market_state_phase_counts: Dict[str, int] = field(default_factory=dict)  # FAILED_AUC, ROTATION, etc.

    # Day type / shape metrics (from DayType: line)
    daytype_samples: List[DayTypeSample] = field(default_factory=list)
    daytype_structure_counts: Dict[str, int] = field(default_factory=dict)  # BALANCED, IMBALANCED
    daytype_raw_shape_counts: Dict[str, int] = field(default_factory=dict)  # P_SHAPED, B_SHAPED, etc.
    daytype_resolved_shape_counts: Dict[str, int] = field(default_factory=dict)
    daytype_conflict_count: int = 0  # Number of [CONFLICT] observations

    # Per-bar volume metrics (from Volume: line)
    volume_samples: List[VolumeSample] = field(default_factory=list)
    volume_total: int = 0            # Sum of all bar volumes
    volume_values: List[int] = field(default_factory=list)
    delta_values: List[int] = field(default_factory=list)
    delta_pct_values: List[float] = field(default_factory=list)

    # Warning metrics (from Warnings: line)
    warning_samples: List[WarningSample] = field(default_factory=list)
    warning_width_mismatch_total: int = 0
    warning_val_divergence_total: int = 0
    warning_config_err_total: int = 0
    warning_vbp_warn_total: int = 0
    warning_active_count: int = 0    # Count of samples with * (active warnings)

    # TPO metrics (from TPO: line)
    tpo_samples: List[TpoSample] = field(default_factory=list)
    tpo_period_counts: Dict[str, int] = field(default_factory=dict)
    tpo_timeframe_counts: Dict[str, int] = field(default_factory=dict)
    tpo_ext_hi_count: int = 0        # Count of EXT_HI=Y
    tpo_ext_lo_count: int = 0        # Count of EXT_LO=Y

    # Effort metrics (from Effort: line - rate format)
    effort_samples: List[EffortSample] = field(default_factory=list)
    effort_bid_rate_values: List[float] = field(default_factory=list)
    effort_ask_rate_values: List[float] = field(default_factory=list)
    effort_total_vol_values: List[int] = field(default_factory=list)

    # Effort delta metrics (from Effort: line - delta format)
    effort_delta_samples: List[EffortDeltaSample] = field(default_factory=list)
    effort_cum_delta_values: List[int] = field(default_factory=list)
    effort_max_delta_values: List[int] = field(default_factory=list)

    # Market state bars metrics (from 'Market State:' line)
    market_state_bars_samples: List[MarketStateBarsSample] = field(default_factory=list)
    market_state_bars_state_counts: Dict[str, int] = field(default_factory=dict)
    market_state_bars_values: List[int] = field(default_factory=list)

    # Probe metrics
    probe_samples: List[ProbeSample] = field(default_factory=list)
    total_probes_fired: int = 0
    total_probes_hit: int = 0
    total_probes_miss: int = 0
    probe_hit_rate: float = 0.0

    # Engagement metrics
    engagement_samples: List[EngagementSample] = field(default_factory=list)
    total_engagements: int = 0
    total_escapes: int = 0
    avg_engagement_bars: float = 0.0

    # Touches
    vah_touches: int = 0
    poc_touches: int = 0
    val_touches: int = 0
    total_touches: int = 0

    # HVN/LVN
    hvn_count: int = 0
    lvn_count: int = 0
    widest_gap_ticks: int = 0

    # Correlation samples
    correlated_samples: List[CorrelatedSample] = field(default_factory=list)
    correlation_volacc_delta: Dict[str, Dict[str, int]] = field(default_factory=dict)

    # Zone Outcome samples (terminal engagement outcomes from ZONE_OUTCOME: line)
    zone_outcome_samples: List[ZoneOutcomeSample] = field(default_factory=list)
    zone_outcome_counts: Dict[str, int] = field(default_factory=dict)  # ACCEPTED, REJECTED, PENDING
    zone_outcome_by_type: Dict[str, Dict[str, int]] = field(default_factory=dict)  # Zone -> Outcome -> count
    zone_outcome_vol_ratios: List[float] = field(default_factory=list)  # For distribution analysis
    zone_outcome_bars_engaged: List[int] = field(default_factory=list)  # For duration analysis


# =============================================================================
# PARSER FUNCTIONS
# =============================================================================

def parse_bar_number(line: str) -> Optional[int]:
    """Extract bar number from: Bar 6333 |"""
    match = re.search(r'Bar\s+(\d+)', line)
    if match:
        return int(match.group(1))
    return None


def parse_date(line: str) -> Optional[str]:
    """Extract date from log line: 2025-12-29  10:35:59.598"""
    match = re.match(r'(\d{4}-\d{2}-\d{2})', line)
    if match:
        return match.group(1)
    return None


def parse_timestamp(line: str) -> Optional[Tuple[str, int]]:
    """Extract date and time-of-day in seconds from log line.

    Format: 2026-01-09  09:31:59.566
    Returns: (date_str, seconds_from_midnight)
    """
    match = re.match(r'(\d{4}-\d{2}-\d{2})\s+(\d{2}):(\d{2}):(\d{2})', line)
    if match:
        date_str = match.group(1)
        hours = int(match.group(2))
        minutes = int(match.group(3))
        seconds = int(match.group(4))
        time_sec = hours * 3600 + minutes * 60 + seconds
        return (date_str, time_sec)
    return None


# Session phase time boundaries (matching AMT_Helpers.h DetermineExactPhase)
# All times in seconds from midnight
SESSION_BOUNDARIES = {
    'LONDON_OPEN_SEC': 10800,      # 03:00:00
    'PRE_MARKET_START_SEC': 30600, # 08:30:00
    'RTH_START_SEC': 34200,        # 09:30:00
    'IB_END_SEC': 37800,           # 10:30:00 (RTH_START + 60 min)
    'CLOSING_START_SEC': 55800,    # 15:30:00
    'RTH_END_SEC': 58500,          # 16:15:00
    'POST_CLOSE_END_SEC': 61200,   # 17:00:00
    'MAINTENANCE_END_SEC': 64800,  # 18:00:00
}


def determine_session_phase_from_time(time_sec: int) -> str:
    """Determine session phase from time of day (matching C++ DetermineExactPhase).

    This is the SSOT logic - matches AMT_Helpers.h exactly.
    """
    b = SESSION_BOUNDARIES

    # RTH phases: [09:30, 16:15)
    if time_sec >= b['RTH_START_SEC'] and time_sec < b['RTH_END_SEC']:
        # INITIAL_BALANCE = [09:30, 10:30)
        if time_sec < b['IB_END_SEC']:
            return 'IB'
        # CLOSING_SESSION = [15:30, 16:15)
        if time_sec >= b['CLOSING_START_SEC']:
            return 'CLOSING'
        # MID_SESSION = [10:30, 15:30)
        return 'MID_SESS'

    # POST_CLOSE = [16:15, 17:00)
    if time_sec >= b['RTH_END_SEC'] and time_sec < b['POST_CLOSE_END_SEC']:
        return 'POST_CLOSE'

    # MAINTENANCE = [17:00, 18:00)
    if time_sec >= b['POST_CLOSE_END_SEC'] and time_sec < b['MAINTENANCE_END_SEC']:
        return 'MAINTENANCE'

    # GLOBEX = [18:00, 03:00) - wraps midnight
    if time_sec >= b['MAINTENANCE_END_SEC'] or time_sec < b['LONDON_OPEN_SEC']:
        return 'GLOBEX'

    # LONDON_OPEN = [03:00, 08:30)
    if time_sec >= b['LONDON_OPEN_SEC'] and time_sec < b['PRE_MARKET_START_SEC']:
        return 'LONDON'

    # PRE_MARKET = [08:30, 09:30)
    if time_sec >= b['PRE_MARKET_START_SEC'] and time_sec < b['RTH_START_SEC']:
        return 'PRE_MKT'

    return 'UNKNOWN'


def parse_session_phase(line: str) -> Optional[str]:
    """Extract session phase from: SESS=MID_SESS"""
    match = re.search(r'SESS=(\w+)', line)
    if match:
        return match.group(1)
    return None


def parse_current_phase(line: str) -> Optional[str]:
    """Extract current auction phase from STATE line.

    Format: [AMT] STATE: BALANCE | PHASE: ACCEPTING | streak=0/3
    Returns: ROTATION, TESTING, DRIVING, EXTENSION, PULLBACK, FAILED, ACCEPTING
    """
    if 'STATE:' not in line or 'PHASE:' not in line:
        return None

    match = re.search(r'PHASE:\s*(\w+)', line)
    if match:
        phase = match.group(1).upper()
        # Normalize driving phases
        if phase in ('DRIVING_UP', 'DRIVING_DN', 'DRIVING'):
            return 'DRIVING'
        # Normalize testing boundary (log uses TEST_BND)
        if phase in ('TESTING_BOUNDARY', 'TEST_BND'):
            return 'TESTING'
        if phase == 'RANGE_EXTENSION':
            return 'EXTENSION'
        # Normalize failed auction (log uses FAILED_AUC)
        if phase in ('FAILED_AUCTION', 'FAILED_AUC'):
            return 'FAILED'
        if phase == 'ACCEPTING_VALUE':
            return 'ACCEPTING'
        if phase == 'UNKNOWN':
            return None  # Skip unknown phases
        return phase
    return None


def parse_market_state_line(line: str, bar: int) -> Optional[MarketStateSample]:
    """Parse STATE: IMBALANCE | PHASE: FAILED_AUC | streak=0/3

    Captures full market state information including:
    - market_state: BALANCE, IMBALANCE, UNKNOWN
    - phase: FAILED_AUC, ROTATION, DRIVING_UP, etc. (raw, not normalized)
    - streak_current/streak_required: confirmation streak progress
    """
    if 'STATE:' not in line or 'PHASE:' not in line:
        return None

    sample = MarketStateSample(bar=bar)

    # Extract market state: STATE: IMBALANCE |
    m = re.search(r'STATE:\s*(\w+)', line)
    if m:
        sample.market_state = m.group(1)

    # Extract phase: PHASE: FAILED_AUC |
    m = re.search(r'PHASE:\s*(\w+)', line)
    if m:
        sample.phase = m.group(1)

    # Extract streak: streak=0/3
    m = re.search(r'streak=(\d+)/(\d+)', line)
    if m:
        sample.streak_current = int(m.group(1))
        sample.streak_required = int(m.group(2))

    return sample


def parse_daytype_line(line: str, bar: int) -> Optional[DayTypeSample]:
    """Parse DayType: STRUCT=BALANCED | Shape: RAW_NOW=P_SHAPED RESOLVED_NOW=UNDEFINED [CONFLICT]

    Captures day structure and profile shape information:
    - structure: BALANCED, IMBALANCED
    - raw_shape: P_SHAPED, B_SHAPED, D_SHAPED, DOUBLE_DIST, etc.
    - resolved_shape: UNDEFINED or resolved shape
    - is_conflict: True if [CONFLICT] flag present
    """
    if 'DayType:' not in line:
        return None

    sample = DayTypeSample(bar=bar)

    # Extract structure: STRUCT=BALANCED
    m = re.search(r'STRUCT=(\w+)', line)
    if m:
        sample.structure = m.group(1)

    # Extract raw shape: RAW_NOW=P_SHAPED
    m = re.search(r'RAW_NOW=(\w+)', line)
    if m:
        sample.raw_shape = m.group(1)

    # Extract resolved shape: RESOLVED_NOW=UNDEFINED
    m = re.search(r'RESOLVED_NOW=(\w+)', line)
    if m:
        sample.resolved_shape = m.group(1)

    # Check for conflict flag
    sample.is_conflict = '[CONFLICT]' in line

    return sample


def parse_volume_line(line: str) -> Optional[VolumeSample]:
    """Parse Volume: ClosedBar[10994] Vol=952 Delta=-154 (-16.2%)

    Captures per-bar volume and delta information.
    """
    if 'Volume:' not in line or 'ClosedBar' not in line:
        return None

    sample = VolumeSample()

    # Extract bar number: ClosedBar[10994]
    m = re.search(r'ClosedBar\[(\d+)\]', line)
    if m:
        sample.bar = int(m.group(1))

    # Extract volume: Vol=952
    m = re.search(r'Vol=(\d+)', line)
    if m:
        sample.volume = int(m.group(1))

    # Extract delta: Delta=-154 or Delta=154
    m = re.search(r'Delta=(-?\d+)', line)
    if m:
        sample.delta = int(m.group(1))

    # Extract delta percentage: (-16.2%) or (16.2%)
    m = re.search(r'\((-?[\d.]+)%\)', line)
    if m:
        sample.delta_pct = float(m.group(1))

    return sample


def parse_warning_line(line: str, bar: int) -> Optional[WarningSample]:
    """Parse Warnings: WidthMismatch=0 ValDivergence=36 ConfigErr=0 VbPWarn=0 *

    Captures warning counts. The * at the end indicates active warnings.
    """
    if 'Warnings:' not in line:
        return None

    sample = WarningSample(bar=bar)

    # Extract WidthMismatch=N
    m = re.search(r'WidthMismatch=(\d+)', line)
    if m:
        sample.width_mismatch = int(m.group(1))

    # Extract ValDivergence=N
    m = re.search(r'ValDivergence=(\d+)', line)
    if m:
        sample.val_divergence = int(m.group(1))

    # Extract ConfigErr=N
    m = re.search(r'ConfigErr=(\d+)', line)
    if m:
        sample.config_err = int(m.group(1))

    # Extract VbPWarn=N
    m = re.search(r'VbPWarn=(\d+)', line)
    if m:
        sample.vbp_warn = int(m.group(1))

    # Check for active warning indicator (*)
    sample.has_active = line.rstrip().endswith('*')

    return sample


def parse_tpo_line(line: str, bar: int) -> Optional[TpoSample]:
    """Parse TPO: PERIOD=N | TF=1TF_DOWN | EXT_HI=N(-40t) EXT_LO=Y(13t) THR=4"""
    if 'TPO:' not in line or 'PERIOD=' not in line:
        return None

    sample = TpoSample(bar=bar)

    # Extract PERIOD=X
    m = re.search(r'PERIOD=(\w+)', line)
    if m:
        sample.period = m.group(1)

    # Extract TF=XXX
    m = re.search(r'TF=(\w+)', line)
    if m:
        sample.timeframe = m.group(1)

    # Extract EXT_HI=Y/N with optional tick distance
    m = re.search(r'EXT_HI=([YN])\((-?\d+)t\)', line)
    if m:
        sample.ext_hi = (m.group(1) == 'Y')
        sample.ext_hi_ticks = int(m.group(2))

    # Extract EXT_LO=Y/N with optional tick distance
    m = re.search(r'EXT_LO=([YN])\((-?\d+)t\)', line)
    if m:
        sample.ext_lo = (m.group(1) == 'Y')
        sample.ext_lo_ticks = int(m.group(2))

    # Extract THR=N
    m = re.search(r'THR=(\d+)', line)
    if m:
        sample.threshold = int(m.group(1))

    return sample


def parse_effort_line(line: str, bar: int) -> Optional[EffortSample]:
    """Parse Effort: SRC=NB | BidRate=33.00 AskRate=17.00 (vol/sec) | BarSec=60 | EstVol=1980/1020 | TotVol=50"""
    if 'Effort:' not in line or 'SRC=' not in line:
        return None

    sample = EffortSample(bar=bar)

    # Extract SRC=XX
    m = re.search(r'SRC=(\w+)', line)
    if m:
        sample.source = m.group(1)

    # Extract BidRate=XX.XX
    m = re.search(r'BidRate=([\d.]+)', line)
    if m:
        sample.bid_rate = float(m.group(1))

    # Extract AskRate=XX.XX
    m = re.search(r'AskRate=([\d.]+)', line)
    if m:
        sample.ask_rate = float(m.group(1))

    # Extract BarSec=NN
    m = re.search(r'BarSec=(\d+)', line)
    if m:
        sample.bar_sec = int(m.group(1))

    # Extract EstVol=NNNN/NNNN
    m = re.search(r'EstVol=(\d+)/(\d+)', line)
    if m:
        sample.est_vol_bid = int(m.group(1))
        sample.est_vol_ask = int(m.group(2))

    # Extract TotVol=NN
    m = re.search(r'TotVol=(\d+)', line)
    if m:
        sample.total_vol = int(m.group(1))

    return sample


def parse_effort_delta_line(line: str, bar: int) -> Optional[EffortDeltaSample]:
    """Parse Effort: NB_CumDelta=-5443(tick) MaxDelta=14"""
    if 'Effort:' not in line or 'NB_CumDelta=' not in line:
        return None

    sample = EffortDeltaSample(bar=bar)

    # Extract NB_CumDelta=NNNNN (can be negative)
    m = re.search(r'NB_CumDelta=(-?\d+)', line)
    if m:
        sample.nb_cum_delta = int(m.group(1))

    # Extract MaxDelta=NN
    m = re.search(r'MaxDelta=(\d+)', line)
    if m:
        sample.max_delta = int(m.group(1))

    return sample


def parse_market_state_bars_line(line: str, bar: int) -> Optional[MarketStateBarsSample]:
    """Parse Market State: IMBALANCE bars=19"""
    if 'Market State:' not in line or 'bars=' not in line:
        return None

    sample = MarketStateBarsSample(bar=bar)

    # Extract state (BALANCE, IMBALANCE, UNKNOWN)
    m = re.search(r'Market State:\s*(\w+)', line)
    if m:
        sample.state = m.group(1)

    # Extract bars=NN
    m = re.search(r'bars=(\d+)', line)
    if m:
        sample.bars_in_state = int(m.group(1))

    return sample


def parse_struct_line(line: str, bar: int) -> Optional[StructSample]:
    """Parse Struct: SESS_HI=7016.75 SESS_LO=6959.00 DIST_HI_T=50 DIST_LO_T=181 | IB_HI=6997.25 IB_LO=6959.00 DIST_IB_HI_T=-28 DIST_IB_LO_T=181 IB=FROZEN | RANGE_T=231"""
    if 'Struct:' not in line or 'SESS_HI=' not in line:
        return None

    sample = StructSample(bar=bar)

    m = re.search(r'SESS_HI=([0-9.]+)', line)
    if m:
        sample.sess_hi = float(m.group(1))

    m = re.search(r'SESS_LO=([0-9.]+)', line)
    if m:
        sample.sess_lo = float(m.group(1))

    m = re.search(r'DIST_HI_T=(-?\d+)', line)
    if m:
        sample.dist_hi_t = int(m.group(1))

    m = re.search(r'DIST_LO_T=(-?\d+)', line)
    if m:
        sample.dist_lo_t = int(m.group(1))

    m = re.search(r'IB_HI=([0-9.]+)', line)
    if m:
        sample.ib_hi = float(m.group(1))

    m = re.search(r'IB_LO=([0-9.]+)', line)
    if m:
        sample.ib_lo = float(m.group(1))

    m = re.search(r'DIST_IB_HI_T=(-?\d+)', line)
    if m:
        sample.dist_ib_hi_t = int(m.group(1))

    m = re.search(r'DIST_IB_LO_T=(-?\d+)', line)
    if m:
        sample.dist_ib_lo_t = int(m.group(1))

    m = re.search(r'IB=(\w+)', line)
    if m:
        sample.ib_status = m.group(1)

    m = re.search(r'RANGE_T=(\d+)', line)
    if m:
        sample.range_t = int(m.group(1))

    return sample


def parse_va_line(line: str, bar: int) -> Optional[VASample]:
    """Parse VA: POC=7004.25 VAH=7016.75 VAL=6984.50 | Range=129 ticks"""
    if 'VA:' not in line or 'POC=' not in line:
        return None

    sample = VASample(bar=bar)

    m = re.search(r'POC=([0-9.]+)', line)
    if m:
        sample.poc = float(m.group(1))

    m = re.search(r'VAH=([0-9.]+)', line)
    if m:
        sample.vah = float(m.group(1))

    m = re.search(r'VAL=([0-9.]+)', line)
    if m:
        sample.val = float(m.group(1))

    m = re.search(r'Range=(\d+)', line)
    if m:
        sample.range_ticks = int(m.group(1))

    return sample


def parse_session_delta_line(line: str, bar: int) -> Optional[SessionDeltaSample]:
    """Parse SessionDelta: SessCum=-5478 SessRatio=-0.0050 | PhaseCum=-1838035 PhaseRatio=-0.0737 Pctile=100.0 | Vol=1089918 | Phase=CLOSING (n=5)"""
    if 'SessionDelta:' not in line or 'SessCum=' not in line:
        return None

    sample = SessionDeltaSample(bar=bar)

    m = re.search(r'SessCum=(-?\d+)', line)
    if m:
        sample.sess_cum = int(m.group(1))

    m = re.search(r'SessRatio=(-?[0-9.]+)', line)
    if m:
        sample.sess_ratio = float(m.group(1))

    m = re.search(r'PhaseCum=(-?\d+)', line)
    if m:
        sample.phase_cum = int(m.group(1))

    m = re.search(r'PhaseRatio=(-?[0-9.]+)', line)
    if m:
        sample.phase_ratio = float(m.group(1))

    m = re.search(r'Pctile=([0-9.]+)', line)
    if m:
        sample.pctile = float(m.group(1))

    m = re.search(r'Vol=(\d+)', line)
    if m:
        sample.volume = int(m.group(1))

    m = re.search(r'Phase=(\w+)', line)
    if m:
        sample.phase = m.group(1)

    m = re.search(r'\(n=(\d+)\)', line)
    if m:
        sample.phase_n = int(m.group(1))

    return sample


def parse_delta_flags_line(line: str, bar: int) -> Optional[DeltaFlagsSample]:
    """Parse DeltaFlags: ExtBar=N ExtSess=N Extreme=N Coherent=N | Valid=Y | BarAbsPctl=79"""
    if 'DeltaFlags:' not in line:
        return None

    sample = DeltaFlagsSample(bar=bar)

    m = re.search(r'ExtBar=([YN])', line)
    if m:
        sample.ext_bar = (m.group(1) == 'Y')

    m = re.search(r'ExtSess=([YN])', line)
    if m:
        sample.ext_sess = (m.group(1) == 'Y')

    m = re.search(r'Extreme=([YN])', line)
    if m:
        sample.extreme = (m.group(1) == 'Y')

    m = re.search(r'Coherent=([YN])', line)
    if m:
        sample.coherent = (m.group(1) == 'Y')

    m = re.search(r'Valid=([YN])', line)
    if m:
        sample.valid = (m.group(1) == 'Y')

    m = re.search(r'BarAbsPctl=(\d+)', line)
    if m:
        sample.bar_abs_pctl = int(m.group(1))

    return sample


def parse_touches_line(line: str, bar: int) -> Optional[TouchesSample]:
    """Parse Touches: VAH=21 POC=11 VAL=2 | Total=34"""
    if 'Touches:' not in line:
        return None

    sample = TouchesSample(bar=bar)

    m = re.search(r'VAH=(\d+)', line)
    if m:
        sample.vah_touches = int(m.group(1))

    m = re.search(r'POC=(\d+)', line)
    if m:
        sample.poc_touches = int(m.group(1))

    m = re.search(r'VAL=(\d+)', line)
    if m:
        sample.val_touches = int(m.group(1))

    m = re.search(r'Total=(\d+)', line)
    if m:
        sample.total_touches = int(m.group(1))

    return sample


def parse_hvn_lvn_summary_line(line: str, bar: int) -> Optional[HvnLvnSummarySample]:
    """Parse HVN: 10 (+73803/-0) | LVN: 6 (+41698/-0) | WidestGap: 88 ticks"""
    if 'HVN:' not in line or 'LVN:' not in line or 'WidestGap:' not in line:
        return None

    sample = HvnLvnSummarySample(bar=bar)

    # HVN: 10 (+73803/-0)
    m = re.search(r'HVN:\s*(\d+)\s*\(\+(\d+)/-(\d+)\)', line)
    if m:
        sample.hvn_count = int(m.group(1))
        sample.hvn_added = int(m.group(2))
        sample.hvn_removed = int(m.group(3))

    # LVN: 6 (+41698/-0)
    m = re.search(r'LVN:\s*(\d+)\s*\(\+(\d+)/-(\d+)\)', line)
    if m:
        sample.lvn_count = int(m.group(1))
        sample.lvn_added = int(m.group(2))
        sample.lvn_removed = int(m.group(3))

    # WidestGap: 88
    m = re.search(r'WidestGap:\s*(\d+)', line)
    if m:
        sample.widest_gap = int(m.group(1))

    return sample


def parse_hvn_prices_line(line: str, bar: int) -> Optional[HvnPricesSample]:
    """Parse HVN prices: 6960.00(3114) 6964.00(2606) ..."""
    if 'HVN prices:' not in line:
        return None

    sample = HvnPricesSample(bar=bar)

    # Find all price(volume) pairs
    for m in re.finditer(r'([0-9.]+)\((\d+)\)', line):
        price = float(m.group(1))
        volume = int(m.group(2))
        sample.prices.append((price, volume))

    return sample


def parse_lvn_prices_line(line: str, bar: int) -> Optional[LvnPricesSample]:
    """Parse LVN prices: 6961.50(409) 6966.50(137) ..."""
    if 'LVN prices:' not in line:
        return None

    sample = LvnPricesSample(bar=bar)

    # Find all price(volume) pairs
    for m in re.finditer(r'([0-9.]+)\((\d+)\)', line):
        price = float(m.group(1))
        volume = int(m.group(2))
        sample.prices.append((price, volume))

    return sample


def parse_engagements_line(line: str, bar: int) -> Optional[EngagementSample]:
    """Parse Engagements: 73 | Escapes: 69 | AvgBars: 3.3 | AvgVel: 3.83"""
    if 'Engagements:' not in line or 'Escapes:' not in line:
        return None

    sample = EngagementSample(bar=bar)

    m = re.search(r'Engagements:\s*(\d+)', line)
    if m:
        sample.engagements = int(m.group(1))

    m = re.search(r'Escapes:\s*(\d+)', line)
    if m:
        sample.escapes = int(m.group(1))

    m = re.search(r'AvgBars:\s*([0-9.]+)', line)
    if m:
        sample.avg_bars = float(m.group(1))

    m = re.search(r'AvgVel:\s*([0-9.]+)', line)
    if m:
        sample.avg_vel = float(m.group(1))

    return sample


def parse_probes_line(line: str, bar: int) -> Optional[ProbeSample]:
    """Parse Probes: Fired=38 Resolved=38 | Hit=4 Miss=30 Exp=4 | HitRate=10.5% AvgScore=9.8"""
    if 'Probes:' not in line or 'Fired=' not in line:
        return None

    sample = ProbeSample(bar=bar)

    m = re.search(r'Fired=(\d+)', line)
    if m:
        sample.fired = int(m.group(1))

    m = re.search(r'Resolved=(\d+)', line)
    if m:
        sample.resolved = int(m.group(1))

    m = re.search(r'Hit=(\d+)', line)
    if m:
        sample.hit = int(m.group(1))

    m = re.search(r'Miss=(\d+)', line)
    if m:
        sample.miss = int(m.group(1))

    m = re.search(r'Exp=(\d+)', line)
    if m:
        sample.expired = int(m.group(1))

    m = re.search(r'HitRate=([0-9.]+)', line)
    if m:
        sample.hit_rate = float(m.group(1))

    m = re.search(r'AvgScore=([0-9.]+)', line)
    if m:
        sample.avg_score = float(m.group(1))

    return sample


def parse_transitions_line(line: str, bar: int) -> Optional[TransitionsSample]:
    """Parse Transitions: Session=1 Phase=40 State=4331"""
    if 'Transitions:' not in line or 'Session=' not in line:
        return None

    sample = TransitionsSample(bar=bar)

    m = re.search(r'Session=(\d+)', line)
    if m:
        sample.session = int(m.group(1))

    m = re.search(r'Phase=(\d+)', line)
    if m:
        sample.phase = int(m.group(1))

    m = re.search(r'State=(\d+)', line)
    if m:
        sample.state = int(m.group(1))

    return sample


def parse_effort_rates_line(line: str, bar: int) -> Optional[EffortRatesSample]:
    """Parse Effort: DeltaSec=-0.02 TradesSec=0.02"""
    if 'Effort:' not in line or 'DeltaSec=' not in line:
        return None

    sample = EffortRatesSample(bar=bar)

    m = re.search(r'DeltaSec=(-?[0-9.]+)', line)
    if m:
        sample.delta_sec = float(m.group(1))

    m = re.search(r'TradesSec=([0-9.]+)', line)
    if m:
        sample.trades_sec = float(m.group(1))

    return sample


def parse_dom_line(line: str, bar: int) -> Optional[DomSample]:
    """Parse DOM: BidSz=149 AskSz=79 BidStk=345 AskStk=576 Bid=7004.25 Ask=7004.50"""
    if 'DOM:' not in line or 'BidSz=' not in line:
        return None

    sample = DomSample(bar=bar)

    m = re.search(r'BidSz=(\d+)', line)
    if m:
        sample.bid_sz = int(m.group(1))

    m = re.search(r'AskSz=(\d+)', line)
    if m:
        sample.ask_sz = int(m.group(1))

    m = re.search(r'BidStk=(\d+)', line)
    if m:
        sample.bid_stk = int(m.group(1))

    m = re.search(r'AskStk=(\d+)', line)
    if m:
        sample.ask_stk = int(m.group(1))

    m = re.search(r'Bid=([0-9.]+)', line)
    if m:
        sample.bid_price = float(m.group(1))

    m = re.search(r'Ask=([0-9.]+)', line)
    if m:
        sample.ask_price = float(m.group(1))

    return sample


def parse_effort_baselines_line(line: str, bar: int) -> Optional[EffortBaselinesSample]:
    """Parse EffortBaselines: CurPhase=CLOSING sessions=5/4 bars=225"""
    if 'EffortBaselines:' not in line:
        return None

    sample = EffortBaselinesSample(bar=bar)

    m = re.search(r'CurPhase=(\w+)', line)
    if m:
        sample.cur_phase = m.group(1)

    m = re.search(r'sessions=(\d+)/(\d+)', line)
    if m:
        sample.sessions_ready = int(m.group(1))
        sample.sessions_total = int(m.group(2))

    m = re.search(r'bars=(\d+)', line)
    if m:
        sample.bars = int(m.group(1))

    return sample


def parse_session_delta_baseline_line(line: str, bar: int) -> Optional[SessionDeltaBaselineSample]:
    """Parse SessionDelta[CLOSING]: READY sessions=5 size=5 mean=0.0187 median=0.0200 mad=0.0057"""
    if 'SessionDelta[' not in line:
        return None

    sample = SessionDeltaBaselineSample(bar=bar)

    m = re.search(r'SessionDelta\[(\w+)\]:\s*(\w+)', line)
    if m:
        sample.phase = m.group(1)
        sample.status = m.group(2)

    m = re.search(r'sessions=(\d+)', line)
    if m:
        sample.sessions = int(m.group(1))

    m = re.search(r'size=(\d+)', line)
    if m:
        sample.size = int(m.group(1))

    m = re.search(r'mean=(-?[0-9.]+)', line)
    if m:
        sample.mean = float(m.group(1))

    m = re.search(r'median=(-?[0-9.]+)', line)
    if m:
        sample.median = float(m.group(1))

    m = re.search(r'mad=([0-9.]+)', line)
    if m:
        sample.mad = float(m.group(1))

    return sample


def parse_dom_baseline_line(line: str, bar: int) -> Optional[DomBaselineSample]:
    """Parse DOMBaseline[CLOSING]: core=READY n=5 depth=5107 | halo=READY mass=4758 imbal=4758 | spread=READY n=6000"""
    if 'DOMBaseline[' not in line:
        return None

    sample = DomBaselineSample(bar=bar)

    m = re.search(r'DOMBaseline\[(\w+)\]:', line)
    if m:
        sample.phase = m.group(1)

    m = re.search(r'core=(\w+)', line)
    if m:
        sample.core_status = m.group(1)

    # First n= is core_n, there's also a second one for spread_n
    # core=READY n=5 depth=5107
    m = re.search(r'core=\w+\s+n=(\d+)', line)
    if m:
        sample.core_n = int(m.group(1))

    m = re.search(r'depth=(\d+)', line)
    if m:
        sample.core_depth = int(m.group(1))

    m = re.search(r'halo=(\w+)', line)
    if m:
        sample.halo_status = m.group(1)

    m = re.search(r'mass=(\d+)', line)
    if m:
        sample.halo_mass = int(m.group(1))

    m = re.search(r'imbal=(\d+)', line)
    if m:
        sample.halo_imbal = int(m.group(1))

    m = re.search(r'spread=(\w+)', line)
    if m:
        sample.spread_status = m.group(1)

    # spread=READY n=6000
    m = re.search(r'spread=\w+\s+n=(\d+)', line)
    if m:
        sample.spread_n = int(m.group(1))

    return sample


def parse_dom_halo_line(line: str, bar: int) -> Optional[DomHaloSample]:
    """Parse DOMHalo[CLOSING]: massMedian=1012 imbalMedian=-0.037"""
    if 'DOMHalo[' not in line:
        return None

    sample = DomHaloSample(bar=bar)

    m = re.search(r'DOMHalo\[(\w+)\]:', line)
    if m:
        sample.phase = m.group(1)

    m = re.search(r'massMedian=(\d+)', line)
    if m:
        sample.mass_median = int(m.group(1))

    m = re.search(r'imbalMedian=(-?[0-9.]+)', line)
    if m:
        sample.imbal_median = float(m.group(1))

    return sample


def parse_state_line(line: str, bar: int) -> Optional[LiquiditySample]:
    """Parse State: ZONE=... | SESS=... | FACIL=... | DELTA_FRAC=... | LIQ=0.42 THIN [D=11 S=100 R=0 T=0] OBI=+0.20"""
    if 'State:' not in line or 'LIQ=' not in line:
        return None

    sample = LiquiditySample(bar=bar)

    # Extract context fields
    m = re.search(r'ZONE=(\w+)', line)
    if m:
        sample.zone = m.group(1)

    m = re.search(r'SESS=(\w+)', line)
    if m:
        sample.session = m.group(1)

    m = re.search(r'FACIL=(\w+)', line)
    if m:
        sample.facil = m.group(1)

    m = re.search(r'DELTA_FRAC=([\d.]+)', line)
    if m:
        sample.delta_frac = float(m.group(1))

    # Extract LIQ score and state: LIQ=0.42 THIN
    m = re.search(r'LIQ=([\d.]+)\s+(\w+)', line)
    if m:
        sample.liq_score = float(m.group(1))
        sample.liq_state = m.group(2)

    # Extract DSRT: [D=11 S=100 R=0 T=0]
    m = re.search(r'\[D=(\d+)\s+S=(\d+)\s+R=(\d+)\s+T=(\d+)\]', line)
    if m:
        sample.depth_rank = int(m.group(1))
        sample.stress_rank = int(m.group(2))
        sample.resilience_rank = int(m.group(3))
        sample.tightness_rank = int(m.group(4))

    # Extract OBI: OBI=+0.20 or OBI=-0.15
    m = re.search(r'OBI=([+-]?[\d.]+)', line)
    if m:
        sample.obi = float(m.group(1))

    return sample


def parse_facil_line(line: str, bar: int) -> Optional[FacilitationSample]:
    """Parse FACIL: MODE=ROTATION ZONE=LOWER_VALUE | ER=HIGH_LOW | INTERP=ABSORPTION_IN_VALUE | vol%=99 rng%=0"""
    if 'FACIL:' not in line or 'MODE=' not in line:
        return None

    sample = FacilitationSample(bar=bar)

    m = re.search(r'MODE=(\w+)', line)
    if m:
        sample.mode = m.group(1)

    m = re.search(r'ZONE=(\w+)', line)
    if m:
        sample.zone = m.group(1)

    m = re.search(r'ER=(\w+)', line)
    if m:
        sample.effort_result = m.group(1)

    m = re.search(r'INTERP=(\w+)', line)
    if m:
        sample.interpretation = m.group(1)

    m = re.search(r'vol%=(\d+)', line)
    if m:
        sample.vol_pct = int(m.group(1))

    m = re.search(r'rng%=(\d+)', line)
    if m:
        sample.rng_pct = int(m.group(1))

    return sample


def parse_vol_line(line: str, bar: int) -> Optional[VolatilitySample]:
    """Parse VOL: EVT p=100 s=3 | ER=0.70(p62) chop=0.30 | EXT | SHOCK=Y | stop>=67.5t
       Also handles: [VOL] Bar 9902 | REGIME=COMPRESSION (was NORMAL) | pctile=0.0 stable=0
    """
    if 'VOL:' not in line and '[VOL]' not in line:
        return None
    if 'REGIME=' not in line and 'p=' not in line:
        return None

    sample = VolatilitySample(bar=bar)

    # New compact format: VOL: EVT p=100 s=3
    m = re.search(r'VOL:\s*(\w+)\s+p=([\d.]+)\s+s=(\d+)', line)
    if m:
        regime_map = {'CMP': 'COMPRESSION', 'NRM': 'NORMAL', 'EXP': 'EXPANSION', 'EVT': 'EVENT'}
        sample.regime = regime_map.get(m.group(1), m.group(1))
        sample.percentile = float(m.group(2))
        sample.stable_bars = int(m.group(3))

    # Old format: [VOL] REGIME=COMPRESSION pctile=0.0 stable=0
    m = re.search(r'REGIME=(\w+)', line)
    if m and not sample.regime:
        sample.regime = m.group(1)
    m = re.search(r'pctile=([\d.]+)', line)
    if m and sample.percentile == 0:
        sample.percentile = float(m.group(1))
    m = re.search(r'stable=(\d+)', line)
    if m and sample.stable_bars == 0:
        sample.stable_bars = int(m.group(1))

    # ER and chop
    m = re.search(r'ER=([\d.]+)\(p(\d+)\)', line)
    if m:
        sample.effort_ratio = float(m.group(1))
        sample.effort_pctile = int(m.group(2))

    m = re.search(r'chop=([\d.]+)', line)
    if m:
        sample.chop = float(m.group(1))

    # Expansion mode and coefficient of variation: EXPAND cv=0.29 or COMPRESS cv=0.15
    m = re.search(r'\b(EXPAND|COMPRESS|CONTRACT|STABLE)\s+cv=([\d.]+)', line)
    if m:
        sample.expansion_mode = m.group(1)
        sample.coef_variation = float(m.group(2))

    # Flags
    sample.is_shock = 'SHOCK=Y' in line
    sample.is_extreme = 'EXT' in line and 'EXT_' not in line  # Avoid matching EXT_HI
    sample.is_transitioning = ' TRANS' in line or '\tTRANS' in line

    # After bars (bars since shock or transition)
    m = re.search(r'after=(\d+)', line)
    if m:
        sample.after_bars = int(m.group(1))

    # Stop distance
    m = re.search(r'stop>=([\d.]+)t', line)
    if m:
        sample.stop_distance = float(m.group(1))

    return sample


def parse_delta_line(line: str, bar: int) -> Optional[DeltaSample]:
    """Parse DELTA: CHAR=X ALIGN=Y CONF=Z | bar=X sess=Y vol=Z
       Also handles: DELTA: ERROR (reason=BLOCKED_VOL_EVENT)
       And [DELTA] Bar format
    """
    if 'DELTA:' not in line and '[DELTA]' not in line:
        return None

    sample = DeltaSample(bar=bar)

    # Extract timestamp
    ts_match = re.match(r'(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+)', line)
    if ts_match:
        sample.timestamp = ts_match.group(1)

    # Extract bar number from [DELTA] Bar X format
    bar_match = re.search(r'Bar\s+(\d+)', line)
    if bar_match:
        sample.bar = int(bar_match.group(1))

    # Check for error
    if 'ERROR' in line:
        m = re.search(r'reason=(\w+)', line)
        if m:
            sample.error_reason = m.group(1)
        return sample

    # Extract character
    m = re.search(r'CHAR=(\w+)', line)
    if m:
        sample.character = m.group(1)

    # Extract alignment
    m = re.search(r'ALIGN=(\w+)', line)
    if m:
        sample.alignment = m.group(1)

    # Extract confidence (both CONF= and conf= formats)
    m = re.search(r'CONF=(\w+)', line)
    if m:
        sample.confidence = m.group(1)
    else:
        m = re.search(r'conf=(\w+)', line)
        if m:
            sample.confidence = m.group(1)

    # Extract abbreviated character/alignment format: DELTA: B/N or S/C etc.
    # Format is DELTA: X/Y where X=character abbrev, Y=alignment abbrev
    m = re.search(r'DELTA:\s+([A-Z]+)/([A-Z]+)', line)
    if m and not sample.character:
        abbrev_char = m.group(1)
        abbrev_align = m.group(2)
        # Map abbreviations to full names
        char_map = {'U': 'UNKNOWN', 'N': 'NEUTRAL', 'E': 'EPISODIC', 'S': 'SUSTAINED',
                    'B': 'BUILDING', 'F': 'FADING', 'R': 'REVERSAL'}
        align_map = {'U': 'UNKNOWN', 'N': 'NEUTRAL', 'C': 'CONVERGENT', 'D': 'DIVERGENT',
                     'AB': 'ABSORPTION_BID', 'AA': 'ABSORPTION_ASK'}
        sample.character = char_map.get(abbrev_char, abbrev_char)
        sample.alignment = align_map.get(abbrev_align, abbrev_align)

    # Extract percentiles
    m = re.search(r'\|\s*bar=(\d+)', line)
    if m:
        sample.bar_pct = int(m.group(1))
    m = re.search(r'sess=(\d+)', line)
    if m:
        sample.sess_pct = int(m.group(1))
    m = re.search(r'vol=(\d+)', line)
    if m:
        sample.vol_pct = int(m.group(1))

    # Extract trading constraints
    m = re.search(r'cont=(\w+)', line)
    if m:
        sample.cont_ok = (m.group(1) == 'OK')
    m = re.search(r'bkout=(\w+)', line)
    if m:
        sample.bkout_ok = (m.group(1) == 'OK')
    m = re.search(r'pos=([\d.]+)x', line)
    if m:
        sample.pos_mult = float(m.group(1))

    # Extract flags
    flags = []
    if '!REV' in line:
        flags.append('!REV')
    if 'INST' in line:
        flags.append('INST')
    if '(range-adj)' in line:
        flags.append('range-adj')
    if 'SHOCK' in line:
        flags.append('SHOCK')
    sample.flags = ' '.join(flags)

    return sample


def parse_volacc_line(line: str, bar: int) -> Optional[VolAccSample]:
    """Parse both old and new VOLACC formats:
       Old: VOLACC: ACC/EX migr=HIGHER | pct=99 acc=0.62 rej=0.25 | mult=1.00 [WK]
       New: [VOLACC] Bar 10629 | STATE=TESTING INT=HI | reentry=NO | HOLD=14403bars VOL=P88 | OBS_MIGR=HIGHER
    """
    if 'VOLACC:' not in line and '[VOLACC]' not in line:
        return None

    sample = VolAccSample(bar=bar)

    # Extract timestamp
    ts_match = re.match(r'(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+)', line)
    if ts_match:
        sample.timestamp = ts_match.group(1)

    # Extract bar from [VOLACC] Bar X format
    bar_match = re.search(r'Bar\s+(\d+)', line)
    if bar_match:
        sample.bar = int(bar_match.group(1))

    # Check for WARMUP
    if 'WARMUP' in line:
        sample.state = 'WARMUP'
        m = re.search(r'reason=(\w+)', line)
        if m:
            sample.flags = m.group(1)
        return sample

    # New format: STATE=TESTING INT=HI
    m = re.search(r'STATE=(\w+)\s+INT=(\w+)', line)
    if m:
        state_map = {'TESTING': 'TEST', 'ACCEPTED': 'ACC', 'REJECTED': 'REJ', 'UNKNOWN': 'UNK'}
        sample.state = state_map.get(m.group(1), m.group(1))
        sample.intensity = m.group(2)

    # Old format: ACC/EX
    if not sample.state:
        m = re.search(r'VOLACC:\s*(\w+)/(\w+)', line)
        if m:
            sample.state = m.group(1)
            sample.intensity = m.group(2)

    # Migration (both formats)
    m = re.search(r'(?:migr|OBS_MIGR)=(\w+)', line)
    if m:
        sample.migration = m.group(1)

    # Percentile
    m = re.search(r'pct=(\d+)', line)
    if m:
        sample.percentile = int(m.group(1))
    m = re.search(r'VOL=P(\d+)', line)
    if m:
        sample.percentile = int(m.group(1))

    # Scores
    m = re.search(r'acc=([\d.]+)', line)
    if m:
        sample.acc_score = float(m.group(1))
    m = re.search(r'rej=([\d.]+)', line)
    if m:
        sample.rej_score = float(m.group(1))

    # Multiplier
    m = re.search(r'mult=([\d.]+)', line)
    if m:
        sample.multiplier = float(m.group(1))

    # Hold bars (new format)
    m = re.search(r'HOLD=(\d+)bars', line)
    if m:
        sample.hold_bars = int(m.group(1))

    # Reentry (new format)
    m = re.search(r'reentry=(\w+)', line)
    if m:
        sample.reentry = m.group(1)

    # Flags
    m = re.search(r'\[(\w+)\]\s*$', line)
    if m:
        sample.flags = m.group(1)

    # Parse individual flags from combined string
    if sample.flags:
        for flag in ['LV', 'WK', 'DV', 'FR']:
            if flag in sample.flags:
                pass  # Flag counts handled in extraction

    return sample


def parse_zone_outcome_line(line: str, bar: int) -> Optional[ZoneOutcomeSample]:
    """Parse ZONE_OUTCOME: Zone=%s Outcome=%s Bars=%d VolRatio=%.2f Touch=%s EscVel=%.2f

    This is the terminal outcome logged when a zone engagement ends.
    Captures the ACTUAL outcome (ACCEPTED/REJECTED/PENDING) that is not visible
    in the Env line after zone exit.
    """
    if 'ZONE_OUTCOME:' not in line:
        return None

    sample = ZoneOutcomeSample(bar=bar)

    # Zone type: VPB_POC, VPB_VAH, VPB_VAL, PRIOR_POC, etc.
    m = re.search(r'Zone=(\w+)', line)
    if m:
        sample.zone_type = m.group(1)

    # Outcome: ACCEPTED, REJECTED, PENDING
    m = re.search(r'Outcome=(\w+)', line)
    if m:
        sample.outcome = m.group(1)

    # Bars engaged
    m = re.search(r'Bars=(\d+)', line)
    if m:
        sample.bars_engaged = int(m.group(1))

    # Volume ratio
    m = re.search(r'VolRatio=([\d.]+)', line)
    if m:
        sample.vol_ratio = float(m.group(1))

    # Touch type: TAG, PROBE, TEST, ACCEPTANCE, UNRESOLVED
    m = re.search(r'Touch=(\w+)', line)
    if m:
        sample.touch_type = m.group(1)

    # Escape velocity
    m = re.search(r'EscVel=([\d.]+)', line)
    if m:
        sample.escape_velocity = float(m.group(1))

    return sample


def parse_struct_line_legacy(line: str, bar: int) -> Optional[StructureSample]:
    """Parse Struct: SESS_HI=6988.25 SESS_LO=6954.25 (legacy version returning StructureSample)"""
    if 'Struct:' not in line:
        return None

    sample = StructureSample(bar=bar)

    m = re.search(r'SESS_HI=([\d.]+)', line)
    if m:
        sample.sess_hi = float(m.group(1))
    m = re.search(r'SESS_LO=([\d.]+)', line)
    if m:
        sample.sess_lo = float(m.group(1))
    m = re.search(r'DIST_HI_T=(-?\d+)', line)
    if m:
        sample.dist_hi_ticks = int(m.group(1))
    m = re.search(r'DIST_LO_T=(-?\d+)', line)
    if m:
        sample.dist_lo_ticks = int(m.group(1))
    m = re.search(r'IB_HI=([\d.]+)', line)
    if m:
        sample.ib_hi = float(m.group(1))
    m = re.search(r'IB_LO=([\d.]+)', line)
    if m:
        sample.ib_lo = float(m.group(1))
    m = re.search(r'DIST_IB_HI_T=(-?\d+)', line)
    if m:
        sample.dist_ib_hi_ticks = int(m.group(1))
    m = re.search(r'DIST_IB_LO_T=(-?\d+)', line)
    if m:
        sample.dist_ib_lo_ticks = int(m.group(1))
    m = re.search(r'IB=(\w+)', line)
    if m:
        sample.ib_status = m.group(1)
    m = re.search(r'RANGE_T=(\d+)', line)
    if m:
        sample.range_ticks = int(m.group(1))

    return sample


def parse_dalton_line(line: str, bar: int) -> Optional[DaltonSample]:
    """Parse DALTON lines in multiple formats:
       Format 1: DALTON: GLOBEX | mini-IB: 0.00-0.00 (OPEN) | TF=1TF_DOWN rot=22
       Format 2: DALTON: OPEN=OD_UP (CLASSIFIED) | Bridge: pending
    """
    if 'DALTON:' not in line:
        return None

    sample = DaltonSample(bar=bar)

    # Opening type format: OPEN=OD_UP (CLASSIFIED) | Bridge: pending
    m = re.search(r'OPEN=(\w+)\s*\((\w+)\)', line)
    if m:
        sample.opening_type = m.group(1)
        sample.opening_status = m.group(2)

    # Bridge status
    m = re.search(r'Bridge:\s*(\w+)', line)
    if m:
        sample.bridge_status = m.group(1)

    # Session type (only if not an OPEN= line)
    if not sample.opening_type:
        m = re.search(r'DALTON:\s*(\w+)', line)
        if m:
            sample.session_type = m.group(1)

    # Mini-IB
    m = re.search(r'mini-IB:\s*([\d.]+)-([\d.]+)\s*\((\w+)\)', line)
    if m:
        sample.mini_ib_hi = float(m.group(1))
        sample.mini_ib_lo = float(m.group(2))
        sample.mini_ib_status = m.group(3)

    # Timeframe
    m = re.search(r'TF=(\w+)', line)
    if m:
        sample.timeframe = m.group(1)

    # Rotation count
    m = re.search(r'rot=(\d+)', line)
    if m:
        sample.rotation_count = int(m.group(1))

    return sample


def parse_amt_state_line(line: str, bar: int) -> Optional[AMTStateSample]:
    """Parse AMT: IMBALANCE | loc=UPPER_VALUE act=RESPONSIVE | phase=DRIVING_DN | ex=POOR_HIGH | SP=4"""
    # Must have "AMT:" followed by BALANCE or IMBALANCE
    if '[AMT] AMT:' not in line:
        return None
    if 'BALANCE' not in line and 'IMBALANCE' not in line:
        return None

    sample = AMTStateSample(bar=bar)

    # Market state: BALANCE or IMBALANCE
    if 'IMBALANCE' in line:
        sample.market_state = 'IMBALANCE'
    elif 'BALANCE' in line:
        sample.market_state = 'BALANCE'

    # Location: loc=UPPER_VALUE
    m = re.search(r'loc=(\w+)', line)
    if m:
        sample.location = m.group(1)

    # Activity: act=RESPONSIVE
    m = re.search(r'act=(\w+)', line)
    if m:
        sample.activity = m.group(1)

    # Phase: phase=DRIVING_DN
    m = re.search(r'phase=(\w+)', line)
    if m:
        sample.phase = m.group(1)

    # Excess: ex=POOR_HIGH
    m = re.search(r'ex=(\w+)', line)
    if m:
        sample.excess = m.group(1)

    # Signal priority: SP=4
    m = re.search(r'SP=(\d+)', line)
    if m:
        sample.signal_priority = int(m.group(1))

    return sample


def parse_session_delta_line_legacy(line: str, bar: int) -> Optional[SessionDeltaSample]:
    """Parse SessionDelta: (legacy version using phase_count field)"""
    if 'SessionDelta:' not in line:
        return None

    sample = SessionDeltaSample(bar=bar)

    m = re.search(r'SessCum=(-?[\d.]+)', line)
    if m:
        sample.sess_cum = float(m.group(1))
    m = re.search(r'SessRatio=(-?[\d.]+)', line)
    if m:
        sample.sess_ratio = float(m.group(1))
    m = re.search(r'PhaseCum=(-?[\d.]+)', line)
    if m:
        sample.phase_cum = float(m.group(1))
    m = re.search(r'PhaseRatio=(-?[\d.]+)', line)
    if m:
        sample.phase_ratio = float(m.group(1))
    m = re.search(r'Pctile=([\d.]+)', line)
    if m:
        sample.pctile = float(m.group(1))
    m = re.search(r'Vol=(\d+)', line)
    if m:
        sample.volume = int(m.group(1))
    m = re.search(r'Phase=(\w+)', line)
    if m:
        sample.phase = m.group(1)
    m = re.search(r'\(n=(\d+)\)', line)
    if m:
        sample.phase_count = int(m.group(1))

    return sample


def parse_delta_flags_line(line: str, bar: int) -> Optional[DeltaFlagsSample]:
    """Parse DeltaFlags: ExtBar=N ExtSess=N Extreme=N Coherent=N | Valid=Y | BarAbsPctl=58"""
    if 'DeltaFlags:' not in line:
        return None

    sample = DeltaFlagsSample(bar=bar)

    sample.ext_bar = 'ExtBar=Y' in line
    sample.ext_sess = 'ExtSess=Y' in line
    sample.extreme = 'Extreme=Y' in line
    sample.coherent = 'Coherent=Y' in line
    sample.valid = 'Valid=Y' in line

    m = re.search(r'BarAbsPctl=(\d+)', line)
    if m:
        sample.bar_abs_pctl = int(m.group(1))

    return sample


def parse_env_line(line: str, bar: int) -> Optional[EnvSample]:
    """Parse Env: VOL=EXTREME | LIQSTATE=THIN | FRIC=TIGHT | OUTCOME=PENDING | TRANS=NONE | INTENT=NEUTRAL"""
    if 'Env:' not in line:
        return None

    sample = EnvSample(bar=bar)

    m = re.search(r'VOL=(\w+)', line)
    if m:
        sample.vol_state = m.group(1)
    m = re.search(r'LIQSTATE=(\w+)', line)
    if m:
        sample.liq_state = m.group(1)
    m = re.search(r'FRIC=(\w+)', line)
    if m:
        sample.friction = m.group(1)
    m = re.search(r'OUTCOME=(\w+)', line)
    if m:
        sample.outcome = m.group(1)
    m = re.search(r'TRANS=(\w+)', line)
    if m:
        sample.transition = m.group(1)
    m = re.search(r'INTENT=(\w+)', line)
    if m:
        sample.intent = m.group(1)

    return sample


def parse_regime_line(line: str, bar: int) -> Optional[RegimeSample]:
    """Parse Regime: BAR_REGIME(prev)=BALANCE | PHASE=ACCEPTING | AGGR(prev)=INITIATIVE | SIDE(prev)=NEUTRAL"""
    if 'Regime:' not in line:
        return None

    sample = RegimeSample(bar=bar)

    # BAR_REGIME(prev)=BALANCE
    m = re.search(r'BAR_REGIME\(prev\)=(\w+)', line)
    if m:
        sample.bar_regime_prev = m.group(1)

    # PHASE=ACCEPTING
    m = re.search(r'PHASE=(\w+)', line)
    if m:
        sample.phase = m.group(1)

    # AGGR(prev)=INITIATIVE
    m = re.search(r'AGGR\(prev\)=(\w+)', line)
    if m:
        sample.aggression_prev = m.group(1)

    # SIDE(prev)=NEUTRAL
    m = re.search(r'SIDE\(prev\)=(\w+)', line)
    if m:
        sample.side_prev = m.group(1)

    return sample


def parse_obi_trace_line(line: str, bar: int) -> Optional[OBISample]:
    """Parse OBI-TRACE: Bar 10909 | inputBid=53 inputAsk=52 | filteredBid=10 filteredAsk=11 | skipped=0 dir.valid=1 OBI=-0.093 | snap.OBI=-0.093 | mktState=1"""
    if '[OBI-TRACE]' not in line:
        return None

    sample = OBISample(bar=bar)

    # inputBid=53 inputAsk=52
    m = re.search(r'inputBid=(\d+)', line)
    if m:
        sample.input_bid = int(m.group(1))
    m = re.search(r'inputAsk=(\d+)', line)
    if m:
        sample.input_ask = int(m.group(1))

    # filteredBid=10 filteredAsk=11
    m = re.search(r'filteredBid=(\d+)', line)
    if m:
        sample.filtered_bid = int(m.group(1))
    m = re.search(r'filteredAsk=(\d+)', line)
    if m:
        sample.filtered_ask = int(m.group(1))

    # skipped=0
    m = re.search(r'skipped=(\d+)', line)
    if m:
        sample.skipped = int(m.group(1))

    # dir.valid=1
    m = re.search(r'dir\.valid=(\d+)', line)
    if m:
        sample.dir_valid = (m.group(1) == '1')

    # OBI=-0.093 (not snap.OBI)
    m = re.search(r'(?<!snap\.)OBI=(-?[\d.]+)', line)
    if m:
        sample.obi = float(m.group(1))

    # snap.OBI=-0.093
    m = re.search(r'snap\.OBI=(-?[\d.]+)', line)
    if m:
        sample.snap_obi = float(m.group(1))

    # mktState=1
    m = re.search(r'mktState=(\d+)', line)
    if m:
        sample.mkt_state = int(m.group(1))

    return sample


def parse_bar_summary_line(line: str) -> Optional[BarSummarySample]:
    """Parse Bar summary: [AMT] Bar 10694 | Phase: FAILED_AUC | Zones: 10 active"""
    # Must have Bar N | Phase: and Zones:
    if '| Phase:' not in line or '| Zones:' not in line:
        return None

    sample = BarSummarySample()

    # Bar 10694
    m = re.search(r'Bar\s+(\d+)', line)
    if m:
        sample.bar = int(m.group(1))

    # Phase: FAILED_AUC
    m = re.search(r'Phase:\s*(\w+)', line)
    if m:
        sample.phase = m.group(1)

    # Zones: 10 active
    m = re.search(r'Zones:\s*(\d+)\s*active', line)
    if m:
        sample.active_zones = int(m.group(1))

    return sample


def parse_delta_ext_line(line: str, bar: int) -> Optional[DeltaExtSample]:
    """Parse DELTA-EXT: trades_P=70 range_P=56 avg_P=95 | noise=25.0 strong=75.0 | hyst: char_req=1 align_req=2"""
    if '[DELTA-EXT]' not in line:
        return None

    sample = DeltaExtSample(bar=bar)

    # trades_P=70
    m = re.search(r'trades_P=(\d+)', line)
    if m:
        sample.trades_pctile = float(m.group(1))

    # range_P=56
    m = re.search(r'range_P=(\d+)', line)
    if m:
        sample.range_pctile = float(m.group(1))

    # avg_P=95
    m = re.search(r'avg_P=(\d+)', line)
    if m:
        sample.avg_pctile = float(m.group(1))

    # noise=25.0
    m = re.search(r'noise=([\d.]+)', line)
    if m:
        sample.noise_floor = float(m.group(1))

    # strong=75.0
    m = re.search(r'strong=([\d.]+)', line)
    if m:
        sample.strong_signal = float(m.group(1))

    # char_req=1
    m = re.search(r'char_req=(\d+)', line)
    if m:
        sample.char_req = int(m.group(1))

    # align_req=2
    m = re.search(r'align_req=(\d+)', line)
    if m:
        sample.align_req = int(m.group(1))

    return sample


def parse_probes_line(line: str, bar: int) -> Optional[ProbeSample]:
    """Parse Probes: Fired=18 Resolved=18 | Hit=1 Miss=13 Exp=4 | HitRate=5.6% AvgScore=10.3"""
    if 'Probes:' not in line:
        return None

    sample = ProbeSample(bar=bar)

    m = re.search(r'Fired=(\d+)', line)
    if m:
        sample.fired = int(m.group(1))
    m = re.search(r'Resolved=(\d+)', line)
    if m:
        sample.resolved = int(m.group(1))
    m = re.search(r'Hit=(\d+)', line)
    if m:
        sample.hit = int(m.group(1))
    m = re.search(r'Miss=(\d+)', line)
    if m:
        sample.miss = int(m.group(1))
    m = re.search(r'Exp=(\d+)', line)
    if m:
        sample.expired = int(m.group(1))
    m = re.search(r'HitRate=([\d.]+)%', line)
    if m:
        sample.hit_rate = float(m.group(1))
    m = re.search(r'AvgScore=([\d.]+)', line)
    if m:
        sample.avg_score = float(m.group(1))

    return sample


def parse_engagements_line(line: str, bar: int) -> Optional[EngagementSample]:
    """Parse Engagements: 79 | Escapes: 79 | AvgBars: 7.3 | AvgVel: 1.16"""
    if 'Engagements:' not in line:
        return None

    sample = EngagementSample(bar=bar)

    m = re.search(r'Engagements:\s*(\d+)', line)
    if m:
        sample.engagements = int(m.group(1))
    m = re.search(r'Escapes:\s*(\d+)', line)
    if m:
        sample.escapes = int(m.group(1))
    m = re.search(r'AvgBars:\s*([\d.]+)', line)
    if m:
        sample.avg_bars = float(m.group(1))
    m = re.search(r'AvgVel:\s*([\d.]+)', line)
    if m:
        sample.avg_vel = float(m.group(1))

    return sample


def parse_touches_line_legacy(line: str) -> Dict[str, int]:
    """Parse Touches: VAH=16 POC=33 VAL=11 | Total=60 (legacy dict version)"""
    result = {}
    m = re.search(r'VAH=(\d+)', line)
    if m:
        result['vah'] = int(m.group(1))
    m = re.search(r'POC=(\d+)', line)
    if m:
        result['poc'] = int(m.group(1))
    m = re.search(r'VAL=(\d+)', line)
    if m:
        result['val'] = int(m.group(1))
    m = re.search(r'Total=(\d+)', line)
    if m:
        result['total'] = int(m.group(1))
    return result


def parse_hvn_lvn_line(line: str) -> Dict[str, any]:
    """Parse HVN: 6 (+21502/-0) | LVN: 4 (+16437/-0) | WidestGap: 33 ticks"""
    result = {}
    m = re.search(r'HVN:\s*(\d+)', line)
    if m:
        result['hvn_count'] = int(m.group(1))
    m = re.search(r'LVN:\s*(\d+)', line)
    if m:
        result['lvn_count'] = int(m.group(1))
    m = re.search(r'WidestGap:\s*(\d+)', line)
    if m:
        result['widest_gap'] = int(m.group(1))
    return result


def parse_phase_distribution(line: str) -> Optional[Dict[str, float]]:
    """Parse: Phase Distribution: ROT=9.7% TEST=25.8% DRIVE=41.9% EXT=0.0% PULL=0.0% FAIL=0.0% ACPT=3.2%"""
    if 'Phase Distribution:' not in line:
        return None
    result = {}
    patterns = [
        (r'ROT=(\d+\.?\d*)%', 'rotation'),
        (r'TEST=(\d+\.?\d*)%', 'testing'),
        (r'DRIVE=(\d+\.?\d*)%', 'driving'),
        (r'TREND=(\d+\.?\d*)%', 'driving'),  # Legacy
        (r'EXT=(\d+\.?\d*)%', 'extension'),
        (r'PULL=(\d+\.?\d*)%', 'pullback'),
        (r'FAIL=(\d+\.?\d*)%', 'failed'),
        (r'ACPT=(\d+\.?\d*)%', 'accepting'),
    ]
    for pattern, key in patterns:
        match = re.search(pattern, line)
        if match:
            result[key] = float(match.group(1))
    return result if result else None


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


def parse_market_state(line: str) -> Dict[str, any]:
    """Parse: Market State: IMBALANCE bars=6"""
    result = {}
    m = re.search(r'Market State:\s+(\w+)\s+bars=(\d+)', line)
    if m:
        result['state'] = m.group(1)
        result['consecutive_bars'] = int(m.group(2))
        result['balance_pct'] = 100.0 if m.group(1) == 'BALANCE' else 0.0
    return result


def parse_va_line_legacy(line: str) -> Dict[str, any]:
    """Parse VA: POC=6973.00 VAH=6976.25 VAL=6958.00 | Range=73 ticks (legacy dict version)"""
    result = {}
    m = re.search(r'POC=([\d.]+)', line)
    if m:
        result['poc'] = float(m.group(1))
    m = re.search(r'VAH=([\d.]+)', line)
    if m:
        result['vah'] = float(m.group(1))
    m = re.search(r'VAL=([\d.]+)', line)
    if m:
        result['val'] = float(m.group(1))
    m = re.search(r'Range=(\d+)', line)
    if m:
        result['va_range'] = int(m.group(1))
    return result


def parse_shape_freeze(line: str) -> Dict[str, any]:
    """Parse SHAPE_FREEZE: t_freeze=123 | STRUCT=BALANCED RAW_FROZEN=D_SHAPED FINAL_FROZEN=D_SHAPED | conflict=0"""
    result = {}
    m = re.search(r't_freeze=(\d+)', line)
    if m:
        result['freeze_bar'] = int(m.group(1))
    m = re.search(r'STRUCT=(\w+)', line)
    if m:
        result['structure'] = m.group(1)
    m = re.search(r'RAW_FROZEN=(\w+)', line)
    if m:
        result['raw_frozen'] = m.group(1)
    m = re.search(r'FINAL_FROZEN=(\w+)', line)
    if m:
        result['final_frozen'] = m.group(1)
    m = re.search(r'conflict=(\d)', line)
    if m:
        result['conflict'] = m.group(1) == '1'
    return result


# =============================================================================
# SESSION DETECTION AND EXTRACTION
# =============================================================================

def detect_session_boundaries(lines: List[str]) -> List[Tuple[int, int, str]]:
    """Detect session boundaries based on TIMESTAMPS (not SESS= parsing).

    Uses time-based session phase determination matching AMT_Helpers.h DetermineExactPhase.
    This is the authoritative method - timestamps don't lie.

    Returns list of (start_idx, end_idx, session_phase) tuples.
    Session phases: GLOBEX, LONDON, PRE_MKT, IB, MID_SESS, CLOSING, POST_CLOSE, MAINTENANCE
    """
    sessions = []
    current_session_start = 0
    current_date = None
    current_phase = None

    for i, line in enumerate(lines):
        # Parse timestamp to get date and time
        ts = parse_timestamp(line)
        if not ts:
            continue

        date_str, time_sec = ts

        # Determine phase from TIME (authoritative)
        phase = determine_session_phase_from_time(time_sec)

        is_boundary = False

        # Date change combined with phase change (handles midnight wrap for GLOBEX)
        # GLOBEX spans midnight, so we only trigger on date change if phase also changes
        if date_str != current_date:
            if current_date is not None and phase != current_phase:
                is_boundary = True
            current_date = date_str

        # Session phase change is the primary boundary trigger
        if phase != current_phase:
            if current_phase is not None:
                is_boundary = True
            # Track the new phase
            prev_phase = current_phase
            current_phase = phase

            # Only create boundary if we have enough lines in current session
            if is_boundary and i > current_session_start + 5:
                sessions.append((current_session_start, i - 1, prev_phase or "UNKNOWN"))
                current_session_start = i

    # Don't forget the last session
    if current_session_start < len(lines) - 1:
        sessions.append((current_session_start, len(lines) - 1, current_phase or "UNKNOWN"))

    return sessions


def extract_session_stats(lines: List[str], start_idx: int, end_idx: int) -> SessionStats:
    """Extract stats from a session's log lines."""
    stats = SessionStats()

    market_state_counts = {'BALANCE': 0, 'IMBALANCE': 0}
    # Track per-bar auction phases (not cumulative Phase Distribution)
    perbar_phase_counts = {
        'ROTATION': 0, 'TESTING': 0, 'DRIVING': 0, 'EXTENSION': 0,
        'PULLBACK': 0, 'FAILED': 0, 'ACCEPTING': 0
    }
    prev_market_state = None
    current_bar = 0
    last_counted_bar = -1  # Avoid double-counting same bar

    for i in range(start_idx, end_idx + 1):
        line = lines[i]

        # Get date
        if not stats.session_date:
            date = parse_date(line)
            if date:
                stats.session_date = date

        # Get bar number
        bar = parse_bar_number(line)
        if bar is not None:
            current_bar = bar
            if stats.first_bar == 0:
                stats.first_bar = bar
            stats.last_bar = bar

        # Get session type
        sess_phase = parse_session_phase(line)
        if sess_phase:
            stats.session_type = sess_phase

        # ===== LIQUIDITY =====
        liq_sample = parse_state_line(line, current_bar)
        if liq_sample:
            stats.liq_samples.append(liq_sample)
            if liq_sample.liq_state:
                stats.liq_state_counts[liq_sample.liq_state] = \
                    stats.liq_state_counts.get(liq_sample.liq_state, 0) + 1
            stats.liq_score_values.append(liq_sample.liq_score)
            stats.liq_depth_values.append(liq_sample.depth_rank)
            stats.liq_stress_values.append(liq_sample.stress_rank)
            stats.liq_resilience_values.append(liq_sample.resilience_rank)
            stats.liq_obi_values.append(liq_sample.obi)

        # ===== PER-BAR AUCTION PHASE (from STATE line) =====
        # This gives us session-specific phase distribution instead of cumulative RTH
        cur_phase = parse_current_phase(line)
        if cur_phase and current_bar != last_counted_bar:
            if cur_phase in perbar_phase_counts:
                perbar_phase_counts[cur_phase] += 1
            last_counted_bar = current_bar

        # ===== FACILITATION =====
        facil_sample = parse_facil_line(line, current_bar)
        if facil_sample:
            stats.facil_samples.append(facil_sample)
            if facil_sample.mode:
                stats.facil_mode_counts[facil_sample.mode] = \
                    stats.facil_mode_counts.get(facil_sample.mode, 0) + 1
            if facil_sample.effort_result:
                stats.facil_er_counts[facil_sample.effort_result] = \
                    stats.facil_er_counts.get(facil_sample.effort_result, 0) + 1
            if facil_sample.interpretation:
                stats.facil_interp_counts[facil_sample.interpretation] = \
                    stats.facil_interp_counts.get(facil_sample.interpretation, 0) + 1

        # ===== VOLATILITY =====
        vol_sample = parse_vol_line(line, current_bar)
        if vol_sample:
            stats.vol_samples.append(vol_sample)
            if vol_sample.regime:
                stats.vol_regime_counts[vol_sample.regime] = \
                    stats.vol_regime_counts.get(vol_sample.regime, 0) + 1
            stats.vol_pctile_values.append(vol_sample.percentile)
            stats.vol_stable_values.append(vol_sample.stable_bars)
            if vol_sample.effort_ratio > 0:
                stats.vol_er_values.append(vol_sample.effort_ratio)
            stats.vol_chop_values.append(vol_sample.chop)
            if vol_sample.is_shock:
                stats.vol_shock_count += 1
            if vol_sample.is_extreme:
                stats.vol_extreme_count += 1

        # ===== DELTA =====
        delta_sample = parse_delta_line(line, current_bar)
        if delta_sample:
            stats.delta_samples.append(delta_sample)
            if delta_sample.error_reason:
                stats.delta_error_counts[delta_sample.error_reason] = \
                    stats.delta_error_counts.get(delta_sample.error_reason, 0) + 1
            else:
                if delta_sample.character:
                    stats.delta_char_counts[delta_sample.character] = \
                        stats.delta_char_counts.get(delta_sample.character, 0) + 1
                if delta_sample.alignment:
                    stats.delta_align_counts[delta_sample.alignment] = \
                        stats.delta_align_counts.get(delta_sample.alignment, 0) + 1
                if delta_sample.confidence:
                    stats.delta_conf_counts[delta_sample.confidence] = \
                        stats.delta_conf_counts.get(delta_sample.confidence, 0) + 1
                stats.delta_bar_pct_values.append(delta_sample.bar_pct)
                stats.delta_sess_pct_values.append(delta_sample.sess_pct)
                stats.delta_vol_pct_values.append(delta_sample.vol_pct)
                if delta_sample.flags:
                    for flag in delta_sample.flags.split():
                        stats.delta_flag_counts[flag] = \
                            stats.delta_flag_counts.get(flag, 0) + 1

        # ===== VOLACC =====
        volacc_sample = parse_volacc_line(line, current_bar)
        if volacc_sample:
            stats.volacc_samples.append(volacc_sample)
            if volacc_sample.state == 'WARMUP':
                stats.volacc_warmup_count += 1
            else:
                stats.volacc_ready_count += 1
                if volacc_sample.state:
                    stats.volacc_state_counts[volacc_sample.state] = \
                        stats.volacc_state_counts.get(volacc_sample.state, 0) + 1
                if volacc_sample.intensity:
                    stats.volacc_intensity_counts[volacc_sample.intensity] = \
                        stats.volacc_intensity_counts.get(volacc_sample.intensity, 0) + 1
                if volacc_sample.migration:
                    stats.volacc_migration_counts[volacc_sample.migration] = \
                        stats.volacc_migration_counts.get(volacc_sample.migration, 0) + 1
                if volacc_sample.flags:
                    for flag in ['LV', 'WK', 'DV', 'FR']:
                        if flag in volacc_sample.flags:
                            stats.volacc_flag_counts[flag] = \
                                stats.volacc_flag_counts.get(flag, 0) + 1

        # ===== STRUCTURE =====
        struct_sample = parse_struct_line_legacy(line, current_bar)
        if struct_sample:
            stats.struct_samples.append(struct_sample)
            stats.session_range_ticks = struct_sample.range_ticks
            if struct_sample.ib_status:
                stats.ib_status_counts[struct_sample.ib_status] = \
                    stats.ib_status_counts.get(struct_sample.ib_status, 0) + 1

        # ===== DALTON =====
        dalton_sample = parse_dalton_line(line, current_bar)
        if dalton_sample:
            stats.dalton_samples.append(dalton_sample)
            if dalton_sample.session_type:
                stats.dalton_session_counts[dalton_sample.session_type] = \
                    stats.dalton_session_counts.get(dalton_sample.session_type, 0) + 1
            if dalton_sample.timeframe:
                stats.dalton_tf_counts[dalton_sample.timeframe] = \
                    stats.dalton_tf_counts.get(dalton_sample.timeframe, 0) + 1
            if dalton_sample.opening_type:
                stats.dalton_opening_counts[dalton_sample.opening_type] = \
                    stats.dalton_opening_counts.get(dalton_sample.opening_type, 0) + 1

        # ===== AMT STATE =====
        amt_state_sample = parse_amt_state_line(line, current_bar)
        if amt_state_sample:
            stats.amt_state_samples.append(amt_state_sample)
            if amt_state_sample.location:
                stats.amt_location_counts[amt_state_sample.location] = \
                    stats.amt_location_counts.get(amt_state_sample.location, 0) + 1
            if amt_state_sample.activity:
                stats.amt_activity_counts[amt_state_sample.activity] = \
                    stats.amt_activity_counts.get(amt_state_sample.activity, 0) + 1
            if amt_state_sample.phase:
                stats.amt_phase_counts[amt_state_sample.phase] = \
                    stats.amt_phase_counts.get(amt_state_sample.phase, 0) + 1
            if amt_state_sample.excess:
                stats.amt_excess_counts[amt_state_sample.excess] = \
                    stats.amt_excess_counts.get(amt_state_sample.excess, 0) + 1
            if amt_state_sample.signal_priority > 0:
                stats.amt_signal_priority_values.append(amt_state_sample.signal_priority)

        # ===== SESSION DELTA =====
        sess_delta_sample = parse_session_delta_line(line, current_bar)
        if sess_delta_sample:
            stats.session_delta_samples.append(sess_delta_sample)
            stats.sess_delta_pctile_values.append(sess_delta_sample.pctile)
            stats.sess_delta_ratio_values.append(sess_delta_sample.sess_ratio)

        # ===== DELTA FLAGS =====
        delta_flags_sample = parse_delta_flags_line(line, current_bar)
        if delta_flags_sample:
            stats.delta_flags_samples.append(delta_flags_sample)
            if delta_flags_sample.ext_bar:
                stats.delta_extreme_bar_count += 1
            if delta_flags_sample.ext_sess:
                stats.delta_extreme_sess_count += 1
            if delta_flags_sample.extreme:
                stats.delta_extreme_combined_count += 1
            if delta_flags_sample.coherent:
                stats.delta_coherent_count += 1

        # ===== ENVIRONMENT =====
        env_sample = parse_env_line(line, current_bar)
        if env_sample:
            stats.env_samples.append(env_sample)
            if env_sample.vol_state:
                stats.env_vol_counts[env_sample.vol_state] = \
                    stats.env_vol_counts.get(env_sample.vol_state, 0) + 1
            if env_sample.liq_state:
                stats.env_liq_counts[env_sample.liq_state] = \
                    stats.env_liq_counts.get(env_sample.liq_state, 0) + 1
            if env_sample.friction:
                stats.env_fric_counts[env_sample.friction] = \
                    stats.env_fric_counts.get(env_sample.friction, 0) + 1
            if env_sample.outcome:
                stats.env_outcome_counts[env_sample.outcome] = \
                    stats.env_outcome_counts.get(env_sample.outcome, 0) + 1
            if env_sample.intent:
                stats.env_intent_counts[env_sample.intent] = \
                    stats.env_intent_counts.get(env_sample.intent, 0) + 1
            if env_sample.transition:
                stats.env_transition_counts[env_sample.transition] = \
                    stats.env_transition_counts.get(env_sample.transition, 0) + 1

        # ===== REGIME =====
        regime_sample = parse_regime_line(line, current_bar)
        if regime_sample:
            stats.regime_samples.append(regime_sample)
            if regime_sample.bar_regime_prev:
                stats.regime_bar_regime_counts[regime_sample.bar_regime_prev] = \
                    stats.regime_bar_regime_counts.get(regime_sample.bar_regime_prev, 0) + 1
            if regime_sample.phase:
                stats.regime_phase_counts[regime_sample.phase] = \
                    stats.regime_phase_counts.get(regime_sample.phase, 0) + 1
            if regime_sample.aggression_prev:
                stats.regime_aggression_counts[regime_sample.aggression_prev] = \
                    stats.regime_aggression_counts.get(regime_sample.aggression_prev, 0) + 1
            if regime_sample.side_prev:
                stats.regime_side_counts[regime_sample.side_prev] = \
                    stats.regime_side_counts.get(regime_sample.side_prev, 0) + 1

        # ===== OBI-TRACE =====
        obi_sample = parse_obi_trace_line(line, current_bar)
        if obi_sample:
            stats.obi_samples.append(obi_sample)
            if obi_sample.dir_valid:
                stats.obi_dir_valid_count += 1
            stats.obi_mkt_state_counts[obi_sample.mkt_state] = \
                stats.obi_mkt_state_counts.get(obi_sample.mkt_state, 0) + 1

        # ===== BAR SUMMARY =====
        bar_summary = parse_bar_summary_line(line)
        if bar_summary:
            stats.bar_summary_samples.append(bar_summary)
            if bar_summary.phase:
                stats.bar_summary_phase_counts[bar_summary.phase] = \
                    stats.bar_summary_phase_counts.get(bar_summary.phase, 0) + 1

        # ===== DELTA-EXT =====
        delta_ext = parse_delta_ext_line(line, current_bar)
        if delta_ext:
            stats.delta_ext_samples.append(delta_ext)

        # ===== MARKET STATE (from STATE: line) =====
        mkt_state_sample = parse_market_state_line(line, current_bar)
        if mkt_state_sample:
            stats.market_state_samples.append(mkt_state_sample)
            if mkt_state_sample.market_state:
                stats.market_state_state_counts[mkt_state_sample.market_state] = \
                    stats.market_state_state_counts.get(mkt_state_sample.market_state, 0) + 1
            if mkt_state_sample.phase:
                stats.market_state_phase_counts[mkt_state_sample.phase] = \
                    stats.market_state_phase_counts.get(mkt_state_sample.phase, 0) + 1

        # ===== DAY TYPE / SHAPE =====
        daytype_sample = parse_daytype_line(line, current_bar)
        if daytype_sample:
            stats.daytype_samples.append(daytype_sample)
            if daytype_sample.structure:
                stats.daytype_structure_counts[daytype_sample.structure] = \
                    stats.daytype_structure_counts.get(daytype_sample.structure, 0) + 1
            if daytype_sample.raw_shape:
                stats.daytype_raw_shape_counts[daytype_sample.raw_shape] = \
                    stats.daytype_raw_shape_counts.get(daytype_sample.raw_shape, 0) + 1
            if daytype_sample.resolved_shape:
                stats.daytype_resolved_shape_counts[daytype_sample.resolved_shape] = \
                    stats.daytype_resolved_shape_counts.get(daytype_sample.resolved_shape, 0) + 1
            if daytype_sample.is_conflict:
                stats.daytype_conflict_count += 1

        # ===== PER-BAR VOLUME =====
        volume_sample = parse_volume_line(line)
        if volume_sample:
            stats.volume_samples.append(volume_sample)
            stats.volume_total += volume_sample.volume
            stats.volume_values.append(volume_sample.volume)
            stats.delta_values.append(volume_sample.delta)
            stats.delta_pct_values.append(volume_sample.delta_pct)

        # ===== WARNINGS =====
        warning_sample = parse_warning_line(line, current_bar)
        if warning_sample:
            stats.warning_samples.append(warning_sample)
            stats.warning_width_mismatch_total += warning_sample.width_mismatch
            stats.warning_val_divergence_total += warning_sample.val_divergence
            stats.warning_config_err_total += warning_sample.config_err
            stats.warning_vbp_warn_total += warning_sample.vbp_warn
            if warning_sample.has_active:
                stats.warning_active_count += 1

        # ===== TPO =====
        tpo_sample = parse_tpo_line(line, current_bar)
        if tpo_sample:
            stats.tpo_samples.append(tpo_sample)
            if tpo_sample.period:
                stats.tpo_period_counts[tpo_sample.period] = \
                    stats.tpo_period_counts.get(tpo_sample.period, 0) + 1
            if tpo_sample.timeframe:
                stats.tpo_timeframe_counts[tpo_sample.timeframe] = \
                    stats.tpo_timeframe_counts.get(tpo_sample.timeframe, 0) + 1
            if tpo_sample.ext_hi:
                stats.tpo_ext_hi_count += 1
            if tpo_sample.ext_lo:
                stats.tpo_ext_lo_count += 1

        # ===== EFFORT (rate format) =====
        effort_sample = parse_effort_line(line, current_bar)
        if effort_sample:
            stats.effort_samples.append(effort_sample)
            stats.effort_bid_rate_values.append(effort_sample.bid_rate)
            stats.effort_ask_rate_values.append(effort_sample.ask_rate)
            stats.effort_total_vol_values.append(effort_sample.total_vol)

        # ===== EFFORT DELTA =====
        effort_delta_sample = parse_effort_delta_line(line, current_bar)
        if effort_delta_sample:
            stats.effort_delta_samples.append(effort_delta_sample)
            stats.effort_cum_delta_values.append(effort_delta_sample.nb_cum_delta)
            stats.effort_max_delta_values.append(effort_delta_sample.max_delta)

        # ===== MARKET STATE BARS =====
        mkt_state_bars = parse_market_state_bars_line(line, current_bar)
        if mkt_state_bars:
            stats.market_state_bars_samples.append(mkt_state_bars)
            if mkt_state_bars.state:
                stats.market_state_bars_state_counts[mkt_state_bars.state] = \
                    stats.market_state_bars_state_counts.get(mkt_state_bars.state, 0) + 1
            stats.market_state_bars_values.append(mkt_state_bars.bars_in_state)

        # ===== PROBES =====
        probe_sample = parse_probes_line(line, current_bar)
        if probe_sample:
            stats.probe_samples.append(probe_sample)
            stats.total_probes_fired = probe_sample.fired
            stats.total_probes_hit = probe_sample.hit
            stats.total_probes_miss = probe_sample.miss
            stats.probe_hit_rate = probe_sample.hit_rate

        # ===== ENGAGEMENTS =====
        engagement_sample = parse_engagements_line(line, current_bar)
        if engagement_sample:
            stats.engagement_samples.append(engagement_sample)
            stats.total_engagements = engagement_sample.engagements
            stats.total_escapes = engagement_sample.escapes
            stats.avg_engagement_bars = engagement_sample.avg_bars

        # ===== ZONE OUTCOME (Terminal engagement outcomes) =====
        zone_outcome_sample = parse_zone_outcome_line(line, current_bar)
        if zone_outcome_sample:
            stats.zone_outcome_samples.append(zone_outcome_sample)
            # Aggregate by outcome
            outcome = zone_outcome_sample.outcome
            stats.zone_outcome_counts[outcome] = stats.zone_outcome_counts.get(outcome, 0) + 1
            # Aggregate by zone type -> outcome
            zone_type = zone_outcome_sample.zone_type
            if zone_type not in stats.zone_outcome_by_type:
                stats.zone_outcome_by_type[zone_type] = {}
            stats.zone_outcome_by_type[zone_type][outcome] = \
                stats.zone_outcome_by_type[zone_type].get(outcome, 0) + 1
            # Track distributions
            stats.zone_outcome_vol_ratios.append(zone_outcome_sample.vol_ratio)
            stats.zone_outcome_bars_engaged.append(zone_outcome_sample.bars_engaged)

        # ===== TOUCHES =====
        if 'Touches:' in line:
            touches = parse_touches_line_legacy(line)
            stats.vah_touches = touches.get('vah', 0)
            stats.poc_touches = touches.get('poc', 0)
            stats.val_touches = touches.get('val', 0)
            stats.total_touches = touches.get('total', 0)

        # ===== HVN/LVN =====
        if 'HVN:' in line and 'LVN:' in line:
            hvn_lvn = parse_hvn_lvn_line(line)
            stats.hvn_count = hvn_lvn.get('hvn_count', 0)
            stats.lvn_count = hvn_lvn.get('lvn_count', 0)
            stats.widest_gap_ticks = hvn_lvn.get('widest_gap', 0)

        # ===== PHASE DISTRIBUTION (CUMULATIVE - for reference only) =====
        # NOTE: We now use per-bar phase counts from STATE lines for session-specific stats
        # The cumulative "Phase Distribution:" line shows full-RTH stats, not session-specific

        # ===== TRANSITIONS =====
        if 'Transitions:' in line:
            trans = parse_transitions(line)
            stats.session_transitions = trans.get('session', 0)
            stats.phase_transitions = trans.get('phase', 0)
            stats.state_transitions = trans.get('state', 0)

        # ===== MARKET STATE =====
        if 'Market State:' in line:
            ms = parse_market_state(line)
            state = ms.get('state', '')
            stats.market_state = state
            stats.balance_pct = ms.get('balance_pct', 0)
            if state in market_state_counts:
                market_state_counts[state] += 1
            if state == 'IMBALANCE' and prev_market_state != 'IMBALANCE':
                stats.acceptance_events += 1
            prev_market_state = state

        # ===== VA =====
        if 'VA:' in line and 'POC=' in line:
            va = parse_va_line_legacy(line)
            stats.va_range_ticks = va.get('va_range', 0)

        # ===== SHAPE FREEZE =====
        if 'SHAPE_FREEZE:' in line and not stats.shape_frozen:
            sf = parse_shape_freeze(line)
            stats.shape_freeze_bar = sf.get('freeze_bar', 0)
            stats.shape_day_structure = sf.get('structure', '')
            stats.shape_raw_frozen = sf.get('raw_frozen', '')
            stats.shape_final_frozen = sf.get('final_frozen', '')
            stats.shape_conflict = sf.get('conflict', False)
            stats.shape_frozen = True

    # Compute aggregates
    stats.market_state_balance_count = market_state_counts['BALANCE']
    stats.market_state_imbalance_count = market_state_counts['IMBALANCE']

    # VOLACC averages
    ready_samples = [s for s in stats.volacc_samples if s.state != 'WARMUP']
    if ready_samples:
        stats.volacc_avg_pct = sum(s.percentile for s in ready_samples) / len(ready_samples)
        stats.volacc_avg_acc = sum(s.acc_score for s in ready_samples) / len(ready_samples)
        stats.volacc_avg_rej = sum(s.rej_score for s in ready_samples) / len(ready_samples)
        stats.volacc_avg_mult = sum(s.multiplier for s in ready_samples) / len(ready_samples)

    # Correlate samples by bar number
    volacc_by_bar = {s.bar: s for s in stats.volacc_samples if s.bar > 0 and s.state != 'WARMUP'}
    delta_by_bar = {s.bar: s for s in stats.delta_samples if s.bar > 0 and not s.error_reason}
    liq_by_bar = {s.bar: s for s in stats.liq_samples if s.bar > 0}
    vol_by_bar = {s.bar: s for s in stats.vol_samples if s.bar > 0}

    all_bars = set(volacc_by_bar.keys()) & set(delta_by_bar.keys())
    for bar_num in all_bars:
        va = volacc_by_bar[bar_num]
        de = delta_by_bar[bar_num]
        lq = liq_by_bar.get(bar_num)
        vl = vol_by_bar.get(bar_num)

        corr = CorrelatedSample(
            bar=bar_num,
            volacc_state=va.state,
            volacc_intensity=va.intensity,
            volacc_pct=va.percentile,
            volacc_acc=va.acc_score,
            volacc_rej=va.rej_score,
            delta_char=de.character,
            delta_align=de.alignment,
            delta_conf=de.confidence,
            delta_bar_pct=de.bar_pct,
            delta_sess_pct=de.sess_pct,
            delta_vol_pct=de.vol_pct,
            delta_flags=de.flags,
            liq_score=lq.liq_score if lq else 0.0,
            liq_state=lq.liq_state if lq else '',
            depth_rank=lq.depth_rank if lq else 0,
            stress_rank=lq.stress_rank if lq else 0,
            vol_regime=vl.regime if vl else '',
            vol_pctile=vl.percentile if vl else 0.0,
            is_shock=vl.is_shock if vl else False,
        )
        stats.correlated_samples.append(corr)

        # Build correlation matrix
        if va.state not in stats.correlation_volacc_delta:
            stats.correlation_volacc_delta[va.state] = {}
        if de.character not in stats.correlation_volacc_delta[va.state]:
            stats.correlation_volacc_delta[va.state][de.character] = 0
        stats.correlation_volacc_delta[va.state][de.character] += 1

    # Total bars
    if stats.last_bar >= stats.first_bar and stats.first_bar > 0:
        stats.total_bars = stats.last_bar - stats.first_bar + 1

    # Compute phase distribution from per-bar counts (session-specific, not cumulative RTH)
    total_phase_bars = sum(perbar_phase_counts.values())
    if total_phase_bars > 0:
        stats.pct_rotation = 100.0 * perbar_phase_counts['ROTATION'] / total_phase_bars
        stats.pct_testing = 100.0 * perbar_phase_counts['TESTING'] / total_phase_bars
        stats.pct_driving = 100.0 * perbar_phase_counts['DRIVING'] / total_phase_bars
        stats.pct_extension = 100.0 * perbar_phase_counts['EXTENSION'] / total_phase_bars
        stats.pct_pullback = 100.0 * perbar_phase_counts['PULLBACK'] / total_phase_bars
        stats.pct_failed = 100.0 * perbar_phase_counts['FAILED'] / total_phase_bars
        stats.pct_accepting = 100.0 * perbar_phase_counts['ACCEPTING'] / total_phase_bars

    # Outside balance
    accounted = (stats.pct_rotation + stats.pct_testing + stats.pct_driving +
                 stats.pct_extension + stats.pct_pullback + stats.pct_failed +
                 stats.pct_accepting)
    stats.pct_outside_balance = max(0, 100.0 - accounted)

    return stats


# =============================================================================
# REPORT GENERATION
# =============================================================================

def percentile_dist(values: List, n: int) -> str:
    """Format percentile distribution string."""
    if not values:
        return "(no data)"
    at_0 = sum(1 for v in values if v == 0)
    low = sum(1 for v in values if 0 < v <= 25)
    mid = sum(1 for v in values if 25 < v <= 75)
    high = sum(1 for v in values if 75 < v < 100)
    at_100 = sum(1 for v in values if v >= 100)
    return f"0={at_0} ({100*at_0/n:.0f}%) | 1-25={low} ({100*low/n:.0f}%) | 26-75={mid} ({100*mid/n:.0f}%) | 76-99={high} ({100*high/n:.0f}%) | 100={at_100} ({100*at_100/n:.0f}%)"


def format_dict_counts(counts: Dict[str, int], total: int = None) -> str:
    """Format dictionary counts with percentages."""
    if not counts:
        return "(none)"
    if total is None:
        total = sum(counts.values())
    if total == 0:
        return "(none)"
    return " ".join(f"{k}={v} ({100*v/total:.0f}%)" for k, v in sorted(counts.items(), key=lambda x: -x[1]))


def classify_session(stats: SessionStats) -> str:
    """Classify session as Balance, Trend, or Mixed based on metrics."""
    if stats.pct_rotation >= 60 and stats.pct_driving < 10:
        return "BALANCE"
    elif stats.pct_rotation < 50 and (stats.pct_driving >= 15 or stats.pct_extension >= 8):
        return "TREND"
    elif stats.market_state_imbalance_count > stats.market_state_balance_count:
        return "TREND"
    else:
        return "MIXED"


def generate_report(sessions: List[SessionStats]) -> str:
    """Generate comprehensive calibration report."""
    lines = []
    lines.append("=" * 100)
    lines.append("AMT FRAMEWORK - CALIBRATION REPORT v2.0")
    lines.append("=" * 100)
    lines.append("")
    lines.append(f"Sessions analyzed: {len(sessions)}")
    lines.append("")

    # Per-session summary
    lines.append("-" * 100)
    lines.append("PER-SESSION METRICS")
    lines.append("-" * 100)

    for i, s in enumerate(sessions, 1):
        classification = classify_session(s)
        lines.append("")
        lines.append(f"{'='*60}")
        lines.append(f"SESSION {i}: {s.session_date} [{s.session_type}] ({classification})")
        lines.append(f"{'='*60}")
        lines.append(f"  Bars: {s.first_bar}-{s.last_bar} ({s.total_bars} bars)")
        lines.append(f"  Range: {s.session_range_ticks}t | VA: {s.va_range_ticks}t")
        lines.append("")

        # Phase Distribution
        lines.append("  PHASE DISTRIBUTION:")
        lines.append(f"    ROT={s.pct_rotation:5.1f}%  TEST={s.pct_testing:5.1f}%  DRIVE={s.pct_driving:5.1f}%")
        lines.append(f"    EXT={s.pct_extension:5.1f}%  PULL={s.pct_pullback:5.1f}%  FAIL={s.pct_failed:5.1f}%  ACPT={s.pct_accepting:5.1f}%")
        lines.append("")

        # Market State
        lines.append("  MARKET STATE:")
        total_ms = s.market_state_balance_count + s.market_state_imbalance_count
        if total_ms > 0:
            lines.append(f"    BALANCE={s.market_state_balance_count} ({100*s.market_state_balance_count/total_ms:.1f}%)")
            lines.append(f"    IMBALANCE={s.market_state_imbalance_count} ({100*s.market_state_imbalance_count/total_ms:.1f}%)")
        lines.append("")

        # ===== LIQUIDITY ENGINE =====
        lines.append("  LIQUIDITY ENGINE:")
        if s.liq_samples:
            n = len(s.liq_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    States: {format_dict_counts(s.liq_state_counts, n)}")
            if s.liq_score_values:
                avg_score = sum(s.liq_score_values) / n
                lines.append(f"    Avg Score: {avg_score:.2f}")
            if s.liq_depth_values:
                avg_d = sum(s.liq_depth_values) / n
                avg_s = sum(s.liq_stress_values) / n
                avg_r = sum(s.liq_resilience_values) / n
                lines.append(f"    Kyle Components (avg): D={avg_d:.0f} S={avg_s:.0f} R={avg_r:.0f}")
            if s.liq_obi_values:
                avg_obi = sum(s.liq_obi_values) / n
                pos_obi = sum(1 for o in s.liq_obi_values if o > 0.05)
                neg_obi = sum(1 for o in s.liq_obi_values if o < -0.05)
                lines.append(f"    OBI: avg={avg_obi:+.3f} | bullish={pos_obi} neutral={n-pos_obi-neg_obi} bearish={neg_obi}")
        else:
            lines.append("    (no liquidity data)")
        lines.append("")

        # ===== VOLATILITY ENGINE =====
        lines.append("  VOLATILITY ENGINE:")
        if s.vol_samples:
            n = len(s.vol_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Regimes: {format_dict_counts(s.vol_regime_counts, n)}")
            if s.vol_pctile_values:
                avg_p = sum(s.vol_pctile_values) / n
                lines.append(f"    Avg Percentile: {avg_p:.1f}")
                lines.append(f"    Pctile Dist: {percentile_dist(s.vol_pctile_values, n)}")
            if s.vol_er_values:
                avg_er = sum(s.vol_er_values) / len(s.vol_er_values)
                lines.append(f"    Avg ER: {avg_er:.2f}")
            if s.vol_chop_values:
                avg_chop = sum(s.vol_chop_values) / len(s.vol_chop_values)
                lines.append(f"    Avg Chop: {avg_chop:.2f}")
            lines.append(f"    Shock Events: {s.vol_shock_count} ({100*s.vol_shock_count/n:.1f}%)")
            lines.append(f"    Extreme Events: {s.vol_extreme_count} ({100*s.vol_extreme_count/n:.1f}%)")
        else:
            lines.append("    (no volatility data)")
        lines.append("")

        # ===== FACILITATION =====
        lines.append("  FACILITATION:")
        if s.facil_samples:
            n = len(s.facil_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Modes: {format_dict_counts(s.facil_mode_counts, n)}")
            lines.append(f"    Effort/Result: {format_dict_counts(s.facil_er_counts, n)}")
            lines.append(f"    Interpretation: {format_dict_counts(s.facil_interp_counts, n)}")
        else:
            lines.append("    (no facilitation data)")
        lines.append("")

        # ===== DELTA ENGINE =====
        lines.append("  DELTA ENGINE:")
        if s.delta_samples:
            n = len(s.delta_samples)
            valid = n - sum(s.delta_error_counts.values())
            lines.append(f"    Samples: {n} ({valid} valid, {n - valid} errors)")
            if s.delta_error_counts:
                lines.append(f"    Errors: {format_dict_counts(s.delta_error_counts)}")
            if s.delta_char_counts:
                lines.append(f"    Character: {format_dict_counts(s.delta_char_counts, valid)}")
            if s.delta_align_counts:
                lines.append(f"    Alignment: {format_dict_counts(s.delta_align_counts, valid)}")
            if s.delta_conf_counts:
                lines.append(f"    Confidence: {format_dict_counts(s.delta_conf_counts, valid)}")
            if s.delta_bar_pct_values:
                lines.append(f"    Bar Pct Dist: {percentile_dist(s.delta_bar_pct_values, valid)}")
        else:
            lines.append("    (no delta data)")
        lines.append("")

        # ===== DELTA FLAGS =====
        if s.delta_flags_samples:
            n = len(s.delta_flags_samples)
            lines.append("  DELTA FLAGS:")
            lines.append(f"    Samples: {n}")
            lines.append(f"    ExtBar: {s.delta_extreme_bar_count} ({100*s.delta_extreme_bar_count/n:.1f}%)")
            lines.append(f"    ExtSess: {s.delta_extreme_sess_count} ({100*s.delta_extreme_sess_count/n:.1f}%)")
            lines.append(f"    Combined Extreme: {s.delta_extreme_combined_count} ({100*s.delta_extreme_combined_count/n:.1f}%)")
            lines.append(f"    Coherent: {s.delta_coherent_count} ({100*s.delta_coherent_count/n:.1f}%)")
            lines.append("")

        # ===== VOLACC =====
        lines.append("  VOLUME ACCEPTANCE:")
        if s.volacc_ready_count > 0:
            lines.append(f"    Samples: {s.volacc_ready_count} ready, {s.volacc_warmup_count} warmup")
            lines.append(f"    States: {format_dict_counts(s.volacc_state_counts, s.volacc_ready_count)}")
            lines.append(f"    Intensity: {format_dict_counts(s.volacc_intensity_counts, s.volacc_ready_count)}")
            lines.append(f"    Migration: {format_dict_counts(s.volacc_migration_counts, s.volacc_ready_count)}")
            if s.volacc_flag_counts:
                lines.append(f"    Flags: {format_dict_counts(s.volacc_flag_counts)}")
            lines.append(f"    Averages: pct={s.volacc_avg_pct:.0f} acc={s.volacc_avg_acc:.2f} rej={s.volacc_avg_rej:.2f}")
        else:
            lines.append("    (no VOLACC data)")
        lines.append("")

        # ===== ENVIRONMENT =====
        lines.append("  ENVIRONMENT CONTEXT:")
        if s.env_samples:
            n = len(s.env_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Volatility: {format_dict_counts(s.env_vol_counts, n)}")
            lines.append(f"    Liquidity: {format_dict_counts(s.env_liq_counts, n)}")
            lines.append(f"    Friction: {format_dict_counts(s.env_fric_counts, n)}")
            lines.append(f"    Outcome: {format_dict_counts(s.env_outcome_counts, n)}")
            lines.append(f"    Intent: {format_dict_counts(s.env_intent_counts, n)}")
            lines.append(f"    Transition: {format_dict_counts(s.env_transition_counts, n)}")
        else:
            lines.append("    (no environment data)")
        lines.append("")

        # ===== REGIME =====
        lines.append("  REGIME:")
        if s.regime_samples:
            n = len(s.regime_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Bar Regime: {format_dict_counts(s.regime_bar_regime_counts, n)}")
            lines.append(f"    Phase: {format_dict_counts(s.regime_phase_counts, n)}")
            lines.append(f"    Aggression: {format_dict_counts(s.regime_aggression_counts, n)}")
            lines.append(f"    Side: {format_dict_counts(s.regime_side_counts, n)}")
        else:
            lines.append("    (no regime data)")
        lines.append("")

        # ===== OBI (Order Book Imbalance) =====
        lines.append("  OBI:")
        if s.obi_samples:
            n = len(s.obi_samples)
            obi_values = [x.obi for x in s.obi_samples]
            avg_obi = sum(obi_values) / n if n > 0 else 0.0
            dir_valid_pct = 100.0 * s.obi_dir_valid_count / n if n > 0 else 0.0
            lines.append(f"    Samples: {n}")
            lines.append(f"    Avg OBI: {avg_obi:.3f}")
            lines.append(f"    Dir Valid: {dir_valid_pct:.1f}%")
            lines.append(f"    Market States: {dict(sorted(s.obi_mkt_state_counts.items()))}")
        else:
            lines.append("    (no OBI data)")
        lines.append("")

        # ===== BAR SUMMARY =====
        lines.append("  BAR SUMMARY:")
        if s.bar_summary_samples:
            n = len(s.bar_summary_samples)
            avg_zones = sum(x.active_zones for x in s.bar_summary_samples) / n if n > 0 else 0.0
            lines.append(f"    Samples: {n}")
            lines.append(f"    Avg Active Zones: {avg_zones:.1f}")
            lines.append(f"    Phase: {format_dict_counts(s.bar_summary_phase_counts, n)}")
        else:
            lines.append("    (no bar summary data)")
        lines.append("")

        # ===== DELTA-EXT =====
        lines.append("  DELTA-EXT:")
        if s.delta_ext_samples:
            n = len(s.delta_ext_samples)
            avg_trades = sum(x.trades_pctile for x in s.delta_ext_samples) / n if n > 0 else 0.0
            avg_range = sum(x.range_pctile for x in s.delta_ext_samples) / n if n > 0 else 0.0
            avg_avg = sum(x.avg_pctile for x in s.delta_ext_samples) / n if n > 0 else 0.0
            lines.append(f"    Samples: {n}")
            lines.append(f"    Avg Trades P: {avg_trades:.1f}")
            lines.append(f"    Avg Range P: {avg_range:.1f}")
            lines.append(f"    Avg AvgTrade P: {avg_avg:.1f}")
        else:
            lines.append("    (no delta-ext data)")
        lines.append("")

        # ===== MARKET STATE (from STATE: line) =====
        lines.append("  MARKET STATE (STATE: line):")
        if s.market_state_samples:
            n = len(s.market_state_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    State: {format_dict_counts(s.market_state_state_counts, n)}")
            lines.append(f"    Phase: {format_dict_counts(s.market_state_phase_counts, n)}")
            # Streak stats
            confirmed = sum(1 for x in s.market_state_samples if x.streak_current >= x.streak_required)
            lines.append(f"    Streak Confirmed: {confirmed}/{n} ({100*confirmed/n:.1f}%)")
        else:
            lines.append("    (no market state data)")
        lines.append("")

        # ===== DAY TYPE / SHAPE =====
        lines.append("  DAY TYPE / SHAPE:")
        if s.daytype_samples:
            n = len(s.daytype_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Structure: {format_dict_counts(s.daytype_structure_counts, n)}")
            lines.append(f"    Raw Shape: {format_dict_counts(s.daytype_raw_shape_counts, n)}")
            lines.append(f"    Resolved Shape: {format_dict_counts(s.daytype_resolved_shape_counts, n)}")
            conflict_pct = 100 * s.daytype_conflict_count / n if n > 0 else 0.0
            lines.append(f"    Conflicts: {s.daytype_conflict_count}/{n} ({conflict_pct:.1f}%)")
        else:
            lines.append("    (no day type data)")
        lines.append("")

        # ===== PER-BAR VOLUME =====
        lines.append("  PER-BAR VOLUME:")
        if s.volume_samples:
            n = len(s.volume_samples)
            avg_vol = s.volume_total / n if n > 0 else 0
            avg_delta = sum(s.delta_values) / n if n > 0 else 0
            avg_delta_pct = sum(s.delta_pct_values) / n if n > 0 else 0
            lines.append(f"    Samples: {n}")
            lines.append(f"    Total Volume: {s.volume_total:,}")
            lines.append(f"    Avg Volume/Bar: {avg_vol:,.1f}")
            lines.append(f"    Avg Delta: {avg_delta:+.1f}")
            lines.append(f"    Avg Delta %: {avg_delta_pct:+.1f}%")
            # Positive vs negative delta bars
            pos_delta = sum(1 for d in s.delta_values if d > 0)
            neg_delta = sum(1 for d in s.delta_values if d < 0)
            lines.append(f"    Delta Bias: +{pos_delta} / -{neg_delta} bars")
        else:
            lines.append("    (no volume data)")
        lines.append("")

        # ===== WARNINGS =====
        lines.append("  WARNINGS:")
        if s.warning_samples:
            n = len(s.warning_samples)
            total_warnings = (s.warning_width_mismatch_total + s.warning_val_divergence_total +
                              s.warning_config_err_total + s.warning_vbp_warn_total)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Total Warnings: {total_warnings}")
            lines.append(f"      WidthMismatch: {s.warning_width_mismatch_total}")
            lines.append(f"      ValDivergence: {s.warning_val_divergence_total}")
            lines.append(f"      ConfigErr: {s.warning_config_err_total}")
            lines.append(f"      VbPWarn: {s.warning_vbp_warn_total}")
            active_pct = 100 * s.warning_active_count / n if n > 0 else 0.0
            lines.append(f"    Active (*): {s.warning_active_count}/{n} ({active_pct:.1f}%)")
        else:
            lines.append("    (no warning data)")
        lines.append("")

        # ===== TPO =====
        lines.append("  TPO:")
        if s.tpo_samples:
            n = len(s.tpo_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Period: {format_dict_counts(s.tpo_period_counts, n)}")
            lines.append(f"    Timeframe: {format_dict_counts(s.tpo_timeframe_counts, n)}")
            ext_hi_pct = 100 * s.tpo_ext_hi_count / n if n > 0 else 0.0
            ext_lo_pct = 100 * s.tpo_ext_lo_count / n if n > 0 else 0.0
            lines.append(f"    Extension Hi: {s.tpo_ext_hi_count}/{n} ({ext_hi_pct:.1f}%)")
            lines.append(f"    Extension Lo: {s.tpo_ext_lo_count}/{n} ({ext_lo_pct:.1f}%)")
        else:
            lines.append("    (no TPO data)")
        lines.append("")

        # ===== EFFORT =====
        lines.append("  EFFORT:")
        if s.effort_samples:
            n = len(s.effort_samples)
            avg_bid_rate = sum(s.effort_bid_rate_values) / n if n > 0 else 0
            avg_ask_rate = sum(s.effort_ask_rate_values) / n if n > 0 else 0
            avg_total_vol = sum(s.effort_total_vol_values) / n if n > 0 else 0
            total_rate = avg_bid_rate + avg_ask_rate
            bid_pct = 100 * avg_bid_rate / total_rate if total_rate > 0 else 50.0
            lines.append(f"    Samples: {n}")
            lines.append(f"    Avg BidRate: {avg_bid_rate:.2f} vol/sec")
            lines.append(f"    Avg AskRate: {avg_ask_rate:.2f} vol/sec")
            lines.append(f"    Bid/Ask Bias: {bid_pct:.1f}% / {100-bid_pct:.1f}%")
            lines.append(f"    Avg TotVol: {avg_total_vol:.1f}")
        else:
            lines.append("    (no Effort data)")
        lines.append("")

        # ===== EFFORT DELTA =====
        lines.append("  EFFORT DELTA:")
        if s.effort_delta_samples:
            n = len(s.effort_delta_samples)
            last_cum_delta = s.effort_cum_delta_values[-1] if s.effort_cum_delta_values else 0
            avg_max_delta = sum(s.effort_max_delta_values) / n if n > 0 else 0
            max_max_delta = max(s.effort_max_delta_values) if s.effort_max_delta_values else 0
            lines.append(f"    Samples: {n}")
            lines.append(f"    Final CumDelta: {last_cum_delta:+,}")
            lines.append(f"    Avg MaxDelta: {avg_max_delta:.1f}")
            lines.append(f"    Peak MaxDelta: {max_max_delta}")
        else:
            lines.append("    (no Effort Delta data)")
        lines.append("")

        # ===== MARKET STATE BARS =====
        lines.append("  MARKET STATE BARS:")
        if s.market_state_bars_samples:
            n = len(s.market_state_bars_samples)
            avg_bars = sum(s.market_state_bars_values) / n if n > 0 else 0
            max_bars = max(s.market_state_bars_values) if s.market_state_bars_values else 0
            lines.append(f"    Samples: {n}")
            lines.append(f"    State: {format_dict_counts(s.market_state_bars_state_counts, n)}")
            lines.append(f"    Avg Bars in State: {avg_bars:.1f}")
            lines.append(f"    Max Bars in State: {max_bars}")
        else:
            lines.append("    (no Market State Bars data)")
        lines.append("")

        # ===== DALTON =====
        lines.append("  DALTON:")
        if s.dalton_samples:
            n = len(s.dalton_samples)
            lines.append(f"    Sessions: {format_dict_counts(s.dalton_session_counts, n)}")
            lines.append(f"    Timeframe: {format_dict_counts(s.dalton_tf_counts, n)}")
            if s.dalton_opening_counts:
                lines.append(f"    Opening Type: {format_dict_counts(s.dalton_opening_counts, n)}")
        else:
            lines.append("    (no Dalton data)")
        lines.append("")

        # ===== AMT STATE =====
        lines.append("  AMT STATE:")
        if s.amt_state_samples:
            n = len(s.amt_state_samples)
            lines.append(f"    Samples: {n}")
            lines.append(f"    Location: {format_dict_counts(s.amt_location_counts, n)}")
            lines.append(f"    Activity: {format_dict_counts(s.amt_activity_counts, n)}")
            lines.append(f"    Phase: {format_dict_counts(s.amt_phase_counts, n)}")
            lines.append(f"    Excess: {format_dict_counts(s.amt_excess_counts, n)}")
            if s.amt_signal_priority_values:
                avg_sp = sum(s.amt_signal_priority_values) / len(s.amt_signal_priority_values)
                max_sp = max(s.amt_signal_priority_values)
                lines.append(f"    Signal Priority: avg={avg_sp:.1f} max={max_sp}")
        else:
            lines.append("    (no AMT state data)")
        lines.append("")

        # ===== STRUCTURE =====
        lines.append("  STRUCTURE:")
        lines.append(f"    IB Status: {format_dict_counts(s.ib_status_counts)}")
        lines.append(f"    HVN/LVN: HVN={s.hvn_count} LVN={s.lvn_count} WidestGap={s.widest_gap_ticks}t")
        lines.append(f"    Touches: VAH={s.vah_touches} POC={s.poc_touches} VAL={s.val_touches} Total={s.total_touches}")
        lines.append("")

        # ===== PROBES & ENGAGEMENTS =====
        lines.append("  PROBES & ENGAGEMENTS:")
        lines.append(f"    Probes: Fired={s.total_probes_fired} Hit={s.total_probes_hit} Miss={s.total_probes_miss} Rate={s.probe_hit_rate:.1f}%")
        lines.append(f"    Engagements: {s.total_engagements} | Escapes: {s.total_escapes} | AvgBars: {s.avg_engagement_bars:.1f}")
        lines.append("")

        # ===== TRANSITIONS =====
        lines.append("  TRANSITIONS:")
        lines.append(f"    Session={s.session_transitions} Phase={s.phase_transitions} State={s.state_transitions}")
        lines.append("")

    # ==========================================================================
    # AGGREGATE METRICS
    # ==========================================================================
    lines.append("")
    lines.append("=" * 100)
    lines.append("AGGREGATE METRICS (ALL SESSIONS)")
    lines.append("=" * 100)
    lines.append("")

    n = len(sessions)
    if n == 0:
        lines.append("No sessions to aggregate.")
        return "\n".join(lines)

    # Aggregate Phase Distribution
    avg_rot = sum(s.pct_rotation for s in sessions) / n
    avg_test = sum(s.pct_testing for s in sessions) / n
    avg_drive = sum(s.pct_driving for s in sessions) / n
    avg_ext = sum(s.pct_extension for s in sessions) / n
    avg_pull = sum(s.pct_pullback for s in sessions) / n
    avg_fail = sum(s.pct_failed for s in sessions) / n
    avg_acpt = sum(s.pct_accepting for s in sessions) / n

    lines.append("PHASE DISTRIBUTION (AVERAGES):")
    lines.append(f"  ROT={avg_rot:5.1f}%  TEST={avg_test:5.1f}%  DRIVE={avg_drive:5.1f}%")
    lines.append(f"  EXT={avg_ext:5.1f}%  PULL={avg_pull:5.1f}%  FAIL={avg_fail:5.1f}%  ACPT={avg_acpt:5.1f}%")
    lines.append("")

    # Aggregate Liquidity
    lines.append("LIQUIDITY AGGREGATE:")
    agg_liq_states = {}
    all_liq_scores = []
    all_depth = []
    all_stress = []
    for s in sessions:
        for k, v in s.liq_state_counts.items():
            agg_liq_states[k] = agg_liq_states.get(k, 0) + v
        all_liq_scores.extend(s.liq_score_values)
        all_depth.extend(s.liq_depth_values)
        all_stress.extend(s.liq_stress_values)
    if agg_liq_states:
        total = sum(agg_liq_states.values())
        lines.append(f"  States: {format_dict_counts(agg_liq_states, total)}")
        if all_liq_scores:
            lines.append(f"  Avg Score: {sum(all_liq_scores)/len(all_liq_scores):.2f}")
        if all_depth:
            lines.append(f"  Avg Depth Rank: {sum(all_depth)/len(all_depth):.0f}")
        if all_stress:
            lines.append(f"  Avg Stress Rank: {sum(all_stress)/len(all_stress):.0f}")
    else:
        lines.append("  (no liquidity data)")
    lines.append("")

    # Aggregate Volatility
    lines.append("VOLATILITY AGGREGATE:")
    agg_vol_regimes = {}
    all_vol_pctile = []
    total_shocks = 0
    for s in sessions:
        for k, v in s.vol_regime_counts.items():
            agg_vol_regimes[k] = agg_vol_regimes.get(k, 0) + v
        all_vol_pctile.extend(s.vol_pctile_values)
        total_shocks += s.vol_shock_count
    if agg_vol_regimes:
        total = sum(agg_vol_regimes.values())
        lines.append(f"  Regimes: {format_dict_counts(agg_vol_regimes, total)}")
        if all_vol_pctile:
            lines.append(f"  Avg Percentile: {sum(all_vol_pctile)/len(all_vol_pctile):.1f}")
            lines.append(f"  Pctile Dist: {percentile_dist(all_vol_pctile, len(all_vol_pctile))}")
        lines.append(f"  Total Shocks: {total_shocks}")
    else:
        lines.append("  (no volatility data)")
    lines.append("")

    # Aggregate Delta
    lines.append("DELTA AGGREGATE:")
    agg_delta_char = {}
    agg_delta_errors = {}
    all_delta_bar_pct = []
    for s in sessions:
        for k, v in s.delta_char_counts.items():
            agg_delta_char[k] = agg_delta_char.get(k, 0) + v
        for k, v in s.delta_error_counts.items():
            agg_delta_errors[k] = agg_delta_errors.get(k, 0) + v
        all_delta_bar_pct.extend(s.delta_bar_pct_values)
    if agg_delta_char:
        total = sum(agg_delta_char.values())
        lines.append(f"  Character: {format_dict_counts(agg_delta_char, total)}")
        if all_delta_bar_pct:
            lines.append(f"  Bar Pct Dist: {percentile_dist(all_delta_bar_pct, total)}")
    if agg_delta_errors:
        total_errors = sum(agg_delta_errors.values())
        lines.append(f"  Errors ({total_errors}): {format_dict_counts(agg_delta_errors, total_errors)}")
    if not agg_delta_char and not agg_delta_errors:
        lines.append("  (no delta data)")
    lines.append("")

    # Aggregate VOLACC
    lines.append("VOLACC AGGREGATE:")
    agg_volacc_states = {}
    agg_volacc_intensity = {}
    total_ready = 0
    for s in sessions:
        for k, v in s.volacc_state_counts.items():
            agg_volacc_states[k] = agg_volacc_states.get(k, 0) + v
        for k, v in s.volacc_intensity_counts.items():
            agg_volacc_intensity[k] = agg_volacc_intensity.get(k, 0) + v
        total_ready += s.volacc_ready_count
    if agg_volacc_states:
        lines.append(f"  Total Samples: {total_ready}")
        lines.append(f"  States: {format_dict_counts(agg_volacc_states, total_ready)}")
        intensity_order = ['VL', 'LO', 'NM', 'HI', 'VH', 'EX', 'SH']
        sorted_int = [(k, agg_volacc_intensity.get(k, 0)) for k in intensity_order if k in agg_volacc_intensity]
        if sorted_int:
            int_str = " ".join(f"{k}={v} ({100*v/total_ready:.0f}%)" for k, v in sorted_int)
            lines.append(f"  Intensity: {int_str}")
    else:
        lines.append("  (no VOLACC data)")
    lines.append("")

    # Aggregate ENVIRONMENT CONTEXT
    lines.append("ENVIRONMENT AGGREGATE:")
    agg_env_outcome = {}
    agg_env_intent = {}
    agg_env_transition = {}
    total_env = 0
    for s in sessions:
        total_env += len(s.env_samples)
        for k, v in s.env_outcome_counts.items():
            agg_env_outcome[k] = agg_env_outcome.get(k, 0) + v
        for k, v in s.env_intent_counts.items():
            agg_env_intent[k] = agg_env_intent.get(k, 0) + v
        for k, v in s.env_transition_counts.items():
            agg_env_transition[k] = agg_env_transition.get(k, 0) + v
    if total_env > 0:
        lines.append(f"  Total Samples: {total_env}")
        lines.append(f"  Outcome: {format_dict_counts(agg_env_outcome, total_env)}")
        lines.append(f"  Intent: {format_dict_counts(agg_env_intent, total_env)}")
        lines.append(f"  Transition: {format_dict_counts(agg_env_transition, total_env)}")
    else:
        lines.append("  (no environment data)")
    lines.append("")

    # Aggregate PROBES & ENGAGEMENTS
    lines.append("PROBES & ENGAGEMENTS AGGREGATE:")
    total_probes_fired = sum(s.total_probes_fired for s in sessions)
    total_probes_hit = sum(s.total_probes_hit for s in sessions)
    total_probes_miss = sum(s.total_probes_miss for s in sessions)
    total_engagements = sum(s.total_engagements for s in sessions)
    total_escapes = sum(s.total_escapes for s in sessions)
    total_touches_vah = sum(s.vah_touches for s in sessions)
    total_touches_poc = sum(s.poc_touches for s in sessions)
    total_touches_val = sum(s.val_touches for s in sessions)
    total_touches = total_touches_vah + total_touches_poc + total_touches_val
    probe_hit_rate = (100.0 * total_probes_hit / total_probes_fired) if total_probes_fired > 0 else 0.0
    avg_eng_bars = sum(s.avg_engagement_bars * s.total_engagements for s in sessions) / total_engagements if total_engagements > 0 else 0.0

    lines.append(f"  Probes: Fired={total_probes_fired} Hit={total_probes_hit} Miss={total_probes_miss} Rate={probe_hit_rate:.1f}%")
    lines.append(f"  Engagements: {total_engagements} | Escapes: {total_escapes} | AvgBars: {avg_eng_bars:.1f}")
    lines.append(f"  Touches: VAH={total_touches_vah} POC={total_touches_poc} VAL={total_touches_val} Total={total_touches}")

    # Compare with OUTCOME stats
    if total_env > 0:
        engaged_pct = 100.0 * (total_env - agg_env_outcome.get('UNK', 0)) / total_env
        pending_pct = 100.0 * agg_env_outcome.get('PENDING', 0) / total_env
        accepted_pct = 100.0 * agg_env_outcome.get('ACCEPTED', 0) / total_env
        rejected_pct = 100.0 * agg_env_outcome.get('REJECTED', 0) / total_env
        lines.append(f"  Zone Engagement Rate: {engaged_pct:.1f}% of bars (PENDING={pending_pct:.1f}% ACCEPTED={accepted_pct:.1f}% REJECTED={rejected_pct:.1f}%)")
    lines.append("")

    # Aggregate ZONE OUTCOME (Terminal engagement outcomes)
    lines.append("ZONE OUTCOME AGGREGATE (Terminal Engagement Outcomes):")
    agg_zone_outcome = {}
    agg_zone_by_type = {}
    all_vol_ratios = []
    all_bars_engaged = []
    for s in sessions:
        for outcome, count in s.zone_outcome_counts.items():
            agg_zone_outcome[outcome] = agg_zone_outcome.get(outcome, 0) + count
        for zone_type, outcome_counts in s.zone_outcome_by_type.items():
            if zone_type not in agg_zone_by_type:
                agg_zone_by_type[zone_type] = {}
            for outcome, count in outcome_counts.items():
                agg_zone_by_type[zone_type][outcome] = \
                    agg_zone_by_type[zone_type].get(outcome, 0) + count
        all_vol_ratios.extend(s.zone_outcome_vol_ratios)
        all_bars_engaged.extend(s.zone_outcome_bars_engaged)

    total_zone_outcomes = sum(agg_zone_outcome.values())
    if total_zone_outcomes > 0:
        # Overall outcome distribution
        lines.append(f"  Outcomes (n={total_zone_outcomes}): {format_dict_counts(agg_zone_outcome, total_zone_outcomes)}")

        # Per-zone-type breakdown
        lines.append("  By Zone Type:")
        for zone_type in sorted(agg_zone_by_type.keys()):
            outcomes = agg_zone_by_type[zone_type]
            zone_total = sum(outcomes.values())
            outcome_str = " ".join(f"{o}={c}" for o, c in sorted(outcomes.items()))
            lines.append(f"    {zone_type}: {outcome_str} (n={zone_total})")

        # Volume ratio distribution (key for acceptance threshold calibration)
        if all_vol_ratios:
            sorted_ratios = sorted(all_vol_ratios)
            p25 = sorted_ratios[len(sorted_ratios) // 4] if len(sorted_ratios) >= 4 else sorted_ratios[0]
            p50 = sorted_ratios[len(sorted_ratios) // 2] if len(sorted_ratios) >= 2 else sorted_ratios[0]
            p75 = sorted_ratios[3 * len(sorted_ratios) // 4] if len(sorted_ratios) >= 4 else sorted_ratios[-1]
            avg_ratio = sum(all_vol_ratios) / len(all_vol_ratios)
            lines.append(f"  Volume Ratio: avg={avg_ratio:.2f} p25={p25:.2f} p50={p50:.2f} p75={p75:.2f}")
            # Show how many engagements would pass 1.3x threshold (current) vs 0.8x (proposed)
            pass_1_3 = sum(1 for r in all_vol_ratios if r >= 1.3)
            pass_0_8 = sum(1 for r in all_vol_ratios if r >= 0.8)
            lines.append(f"  Would Pass acceptanceVolRatio: @1.3x={pass_1_3} ({100*pass_1_3/len(all_vol_ratios):.1f}%) @0.8x={pass_0_8} ({100*pass_0_8/len(all_vol_ratios):.1f}%)")

        # Bars engaged distribution (key for acceptance min bars)
        if all_bars_engaged:
            sorted_bars = sorted(all_bars_engaged)
            avg_bars = sum(all_bars_engaged) / len(all_bars_engaged)
            p25_bars = sorted_bars[len(sorted_bars) // 4] if len(sorted_bars) >= 4 else sorted_bars[0]
            p50_bars = sorted_bars[len(sorted_bars) // 2] if len(sorted_bars) >= 2 else sorted_bars[0]
            p75_bars = sorted_bars[3 * len(sorted_bars) // 4] if len(sorted_bars) >= 4 else sorted_bars[-1]
            lines.append(f"  Bars Engaged: avg={avg_bars:.1f} p25={p25_bars} p50={p50_bars} p75={p75_bars}")
            pass_3_bars = sum(1 for b in all_bars_engaged if b >= 3)
            lines.append(f"  Would Pass acceptanceMinBars: @3bars={pass_3_bars} ({100*pass_3_bars/len(all_bars_engaged):.1f}%)")
    else:
        lines.append("  (no ZONE_OUTCOME data - run study with updated logging)")
    lines.append("")

    # Session Type Breakdown
    balance_count = sum(1 for s in sessions if classify_session(s) == "BALANCE")
    trend_count = sum(1 for s in sessions if classify_session(s) == "TREND")
    mixed_count = sum(1 for s in sessions if classify_session(s) == "MIXED")
    lines.append(f"SESSION TYPES: Balance={balance_count} Trend={trend_count} Mixed={mixed_count}")
    lines.append("")

    # ==========================================================================
    # CALIBRATION CONCERNS
    # ==========================================================================
    lines.append("-" * 100)
    lines.append("CALIBRATION CONCERNS & TUNING SUGGESTIONS")
    lines.append("-" * 100)
    lines.append("")

    concerns = []

    # Check for high VOID/THIN liquidity
    if agg_liq_states:
        total_liq = sum(agg_liq_states.values())
        void_pct = 100 * agg_liq_states.get('VOID', 0) / total_liq
        thin_pct = 100 * agg_liq_states.get('THIN', 0) / total_liq
        if void_pct > 10:
            concerns.append(f"- HIGH VOID rate ({void_pct:.1f}%): Consider adjusting depth thresholds or check DOM data quality")
        if thin_pct > 40:
            concerns.append(f"- HIGH THIN rate ({thin_pct:.1f}%): Liquidity thresholds may be too aggressive")

    # Check for excessive delta errors
    if agg_delta_errors:
        total_delta = sum(agg_delta_char.values()) + sum(agg_delta_errors.values())
        error_pct = 100 * sum(agg_delta_errors.values()) / total_delta
        if error_pct > 20:
            concerns.append(f"- HIGH DELTA error rate ({error_pct:.1f}%): Engines may be blocking too aggressively")
            for err, cnt in agg_delta_errors.items():
                concerns.append(f"    {err}: {cnt} ({100*cnt/total_delta:.1f}%)")

    # Check for volatility extremes
    if agg_vol_regimes:
        total_vol = sum(agg_vol_regimes.values())
        event_pct = 100 * agg_vol_regimes.get('EVENT', 0) / total_vol
        if event_pct > 15:
            concerns.append(f"- HIGH EVENT regime rate ({event_pct:.1f}%): May be over-detecting volatility spikes")

    # Phase distribution concerns
    if avg_pull < 1.0:
        concerns.append("- PULLBACK phase never fires. Consider reducing directionalAfterglowBars")
    if avg_fail > 10.0:
        concerns.append("- FAILED_AUCTION is high (>10%). Consider adjusting excess detection thresholds")
    if avg_drive < 3.0 and trend_count > 0:
        concerns.append("- DRIVING is low on trend days. Check directional phase detection")

    if not concerns:
        concerns.append("- No major concerns detected. Review individual sessions for edge cases.")

    for c in concerns:
        lines.append(c)

    lines.append("")
    lines.append("=" * 100)

    return "\n".join(lines)


def export_to_csv(sessions: List[SessionStats], output_file: str):
    """Export key metrics to CSV for analysis."""
    import csv

    with open(output_file, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)

        # Header
        writer.writerow([
            'date', 'session_type', 'classification', 'total_bars',
            'pct_rotation', 'pct_driving', 'pct_testing', 'pct_extension', 'pct_pullback', 'pct_failed', 'pct_accepting',
            'liq_void_pct', 'liq_thin_pct', 'liq_normal_pct', 'avg_liq_score', 'avg_depth', 'avg_stress',
            'vol_cmp_pct', 'vol_nrm_pct', 'vol_exp_pct', 'vol_evt_pct', 'vol_shock_count',
            'delta_valid_count', 'delta_error_count', 'delta_convergent_pct',
            'volacc_acc_pct', 'volacc_rej_pct', 'volacc_test_pct',
            'phase_transitions', 'probe_hit_rate', 'total_engagements'
        ])

        for s in sessions:
            classification = classify_session(s)

            # Liquidity percentages
            total_liq = sum(s.liq_state_counts.values()) or 1
            liq_void_pct = 100 * s.liq_state_counts.get('VOID', 0) / total_liq
            liq_thin_pct = 100 * s.liq_state_counts.get('THIN', 0) / total_liq
            liq_normal_pct = 100 * s.liq_state_counts.get('NORMAL', 0) / total_liq
            avg_liq = sum(s.liq_score_values) / len(s.liq_score_values) if s.liq_score_values else 0
            avg_depth = sum(s.liq_depth_values) / len(s.liq_depth_values) if s.liq_depth_values else 0
            avg_stress = sum(s.liq_stress_values) / len(s.liq_stress_values) if s.liq_stress_values else 0

            # Volatility percentages
            total_vol = sum(s.vol_regime_counts.values()) or 1
            vol_cmp_pct = 100 * s.vol_regime_counts.get('COMPRESSION', 0) / total_vol
            vol_nrm_pct = 100 * s.vol_regime_counts.get('NORMAL', 0) / total_vol
            vol_exp_pct = 100 * s.vol_regime_counts.get('EXPANSION', 0) / total_vol
            vol_evt_pct = 100 * s.vol_regime_counts.get('EVENT', 0) / total_vol

            # Delta
            delta_valid = sum(s.delta_char_counts.values())
            delta_errors = sum(s.delta_error_counts.values())
            delta_conv_pct = 100 * s.delta_align_counts.get('CONVERGENT', 0) / delta_valid if delta_valid else 0

            # VOLACC
            total_va = s.volacc_ready_count or 1
            va_acc_pct = 100 * s.volacc_state_counts.get('ACC', 0) / total_va
            va_rej_pct = 100 * s.volacc_state_counts.get('REJ', 0) / total_va
            va_test_pct = 100 * s.volacc_state_counts.get('TEST', 0) / total_va

            writer.writerow([
                s.session_date, s.session_type, classification, s.total_bars,
                f"{s.pct_rotation:.1f}", f"{s.pct_driving:.1f}", f"{s.pct_testing:.1f}",
                f"{s.pct_extension:.1f}", f"{s.pct_pullback:.1f}", f"{s.pct_failed:.1f}", f"{s.pct_accepting:.1f}",
                f"{liq_void_pct:.1f}", f"{liq_thin_pct:.1f}", f"{liq_normal_pct:.1f}",
                f"{avg_liq:.2f}", f"{avg_depth:.0f}", f"{avg_stress:.0f}",
                f"{vol_cmp_pct:.1f}", f"{vol_nrm_pct:.1f}", f"{vol_exp_pct:.1f}", f"{vol_evt_pct:.1f}",
                s.vol_shock_count,
                delta_valid, delta_errors, f"{delta_conv_pct:.1f}",
                f"{va_acc_pct:.1f}", f"{va_rej_pct:.1f}", f"{va_test_pct:.1f}",
                s.phase_transitions, f"{s.probe_hit_rate:.1f}", s.total_engagements
            ])

    print(f"CSV exported to: {output_file}")


def export_to_json(sessions: List[SessionStats], output_file: str):
    """Export full metrics to JSON for programmatic analysis."""
    from dataclasses import asdict

    data = {
        'sessions': [asdict(s) for s in sessions],
        'summary': {
            'total_sessions': len(sessions),
            'balance_sessions': sum(1 for s in sessions if classify_session(s) == "BALANCE"),
            'trend_sessions': sum(1 for s in sessions if classify_session(s) == "TREND"),
            'mixed_sessions': sum(1 for s in sessions if classify_session(s) == "MIXED"),
        }
    }

    # Convert dataclass lists to dicts (they're already dicts via asdict)
    # But we need to handle nested dataclasses
    def clean_for_json(obj):
        if isinstance(obj, dict):
            return {k: clean_for_json(v) for k, v in obj.items()}
        elif isinstance(obj, list):
            return [clean_for_json(item) for item in obj]
        else:
            return obj

    data = clean_for_json(data)

    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=2)

    print(f"JSON exported to: {output_file}")


# =============================================================================
# MAIN
# =============================================================================

def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_session_stats.py <logfile.txt> [options]")
        print("")
        print("Options:")
        print("  --output <file.txt>    Write text report to file")
        print("  --csv <file.csv>       Export key metrics to CSV")
        print("  --json <file.json>     Export full metrics to JSON")
        print("")
        print("Input: Copy Sierra Chart log window contents to a text file")
        print("       (Select all in Log window, Ctrl+C, paste to Notepad, save)")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = None
    csv_file = None
    json_file = None

    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == '--output' and i + 1 < len(sys.argv):
            output_file = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == '--csv' and i + 1 < len(sys.argv):
            csv_file = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == '--json' and i + 1 < len(sys.argv):
            json_file = sys.argv[i + 1]
            i += 2
        else:
            i += 1

    # Read input
    try:
        with open(input_file, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
    except FileNotFoundError:
        print(f"Error: File not found: {input_file}")
        sys.exit(1)

    print(f"Read {len(lines)} lines from {input_file}")

    # Filter to AMT lines
    amt_lines = [l.strip() for l in lines if '[AMT]' in l or '[SESSION]' in l or
                 '[VOL]' in l or '[VOL-PACE]' in l or '[DELTA]' in l or
                 '[VOLACC]' in l or '[OBI-TRACE]' in l]
    print(f"Found {len(amt_lines)} AMT/engine log lines")

    if len(amt_lines) < 10:
        print("Warning: Very few AMT log lines found. Processing all lines...")
        amt_lines = [l.strip() for l in lines]

    # Detect sessions (now returns phase with each boundary)
    sessions_bounds = detect_session_boundaries(amt_lines)
    print(f"Detected {len(sessions_bounds)} session phase(s)")

    # Extract stats per session phase
    all_stats = []
    for start, end, phase in sessions_bounds:
        stats = extract_session_stats(amt_lines, start, end)
        # Use the detected phase (more reliable than last-seen in extraction)
        if phase and phase != "UNKNOWN":
            stats.session_type = phase
        if stats.total_bars > 0:
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

    # CSV export
    if csv_file:
        export_to_csv(all_stats, csv_file)

    # JSON export
    if json_file:
        export_to_json(all_stats, json_file)


if __name__ == "__main__":
    main()
