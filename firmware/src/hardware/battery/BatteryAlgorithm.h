/**
 * @file BatteryAlgorithm.h
 * @brief Pure (no I/O) battery monitoring algorithm
 *
 * Using median-of-N sampling, integer IIR with BAT_IIR_SHIFT,
 * interpolated ADC discharge curve (EMA+gated percent display),
 * active CE slope probe classification,
 * and snapshot heuristics + asymmetric debounce
 * as backup. All math lives here; each port supplies its own ~60-line HAL
 * adapter for ADC reads, GPIO toggles, and microsecond delays.
 *
 * Constraints:
 *   - Header-only.
 *   - Plain C99 + C++ compatible (no namespaces, no constexpr, no <type_traits>).
 *   - No <Arduino.h>, no ulp_riscv_*.
 *   - Zero global state — all state lives in caller-owned structs.
 *   - Zero blocking calls.
 *
 * Cadence is expressed in **service ticks** (kProbePeriodTicks). Each port
 * picks its own service interval (500 ms ULP wake, ~variable main-CPU
 * service rate) — the absolute period is `service_interval * kProbePeriodTicks`.
 *
 * ── Charging / VBUS-present sampling contract ─────────────────────────────────
 * When external power is up (Echo: CHG_GOOD / PGOOD), the resistor divider sits
 * on the charger-managed node — **continuous** ADC reads are not Open-Circuit /
 * rested cell volts. Adapters MUST pause charging for the probe burst, gather
 * `BAT_PROBE_SAMPLES` while paused, release charge, optionally wait for resettling,
 * then run `bat_classify_probe` so `have_probe_sample` + `probe_verdict` drive
 * `bat_sample_trustworthy`. Echo fulfills this via `digitalWrite(CE_PIN, HIGH)`
 * during `kProbeSampling` in `BatteryGauge::service()` (Power.cpp): CE high =
 * BQ24079 disabled, charger off for tens of microseconds of burst + 2 ms
 * settle; idle CE low = charger enabled.
 *
 * ── Pointer / null contract ─────────────────────────────────────────────────
 * All functions in this header assume their pointer arguments are non-NULL
 * and (where applicable) that buffer arguments point to at least the
 * documented number of elements. The header does NOT defend against NULL
 * pointers — callers (the HAL adapters) are responsible. This keeps the
 * ULP build small.
 *
 * ── bat_mv vs bat_pct consistency ────────────────────────────────────────────
 * `bat_mv` is from the instantaneous raw each tick (not IIR-filtered ADC).
 * `bat_pct` is an EMA of the discharge curve keyed off that raw, gated so when
 * the curve says SOC > 0 we never render below the instantaneous curve at the
 * same sample (`max(ema, instant)` — avoids bogus 0% while the divider still
 * reads full). With instant at a true curve 0% we decay on EMA-only so short
 * glitches above empty do not linger. Monotonic charging/discharging clamps
 * live in BatteryGauge::service() once `last_reported_pct` is seeded.
 */
#ifndef BADGE_BATTERY_ALGORITHM_H
#define BADGE_BATTERY_ALGORITHM_H

#include <stdint.h>
#include <stdbool.h>

#include "../CommonFlags.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Tunable constants ───────────────────────────────────────────────────────
// `enum` so they're integer constants in both C and C++ without preprocessor.
enum {
    BAT_SAMPLE_N            = 10,   // must be even (median pair average)
    BAT_RAW_MIN             = 500,
    BAT_PROBE_PERIOD_TICKS  = 5,
    BAT_PROBE_WINDOW_US     = 50000,
    BAT_PROBE_SAMPLES       = 4,    // must be even (median pair average)
    BAT_PROBE_SLOPE_DROP    = 30,
    BAT_PROBE_FLAT_SPREAD   = 15,
    BAT_WAKE_DELTA          = 300,
    BAT_FLOAT_SPREAD        = 100,
    BAT_STAT_HISTORY_BITS   = 8,
    BAT_STAT_HICCUP_FLIPS   = 2,
    BAT_DEBOUNCE_ABSENT     = 3,
    BAT_DEBOUNCE_PRESENT    = 10,

    /* Integer EMA on displayed percent (same recurrence as BAT_IIR on raw ADC).
       Higher shift = smoother / slower. */
    BAT_PCT_EMA_SHIFT       = 4,
    /* After filter reset / boot: snap pct to instantaneous curve this many
       trustworthy ticks so gauge never flashes 00 while display EMA settles. */
    BAT_PCT_BOOTSTRAP_TICKS = 8
};

// Compile-time guard: median-pair averaging requires an even sample count.
// If anyone bumps these to an odd value, bat_median_pair() silently returns
// the wrong answer (average of two off-center elements). The typedef-array
// trick gives a C99-compatible static assert.
typedef char bat_static_assert_sample_n_even[
    ((BAT_SAMPLE_N    & 1) == 0) ? 1 : -1];
typedef char bat_static_assert_probe_n_even[
    ((BAT_PROBE_SAMPLES & 1) == 0) ? 1 : -1];
// stat_history is held in a uint32_t; mask math assumes the window fits.
typedef char bat_static_assert_stat_history_fits[
    (BAT_STAT_HISTORY_BITS > 0 && BAT_STAT_HISTORY_BITS <= 32) ? 1 : -1];

// ── Pure helpers ────────────────────────────────────────────────────────────

static inline void bat_sort_small(uint32_t *buf, uint8_t n) {
    for (uint8_t i = 1; i < n; ++i) {
        uint32_t key = buf[i];
        int j = (int)i - 1;
        while (j >= 0 && buf[j] > key) {
            buf[j + 1] = buf[j];
            --j;
        }
        buf[j + 1] = key;
    }
}

static inline uint32_t bat_median_pair(const uint32_t *sorted, uint8_t n) {
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
}

// Convert raw ADC counts to cell millivolts. Based on values below.
static inline uint32_t bat_adc_to_mv(uint32_t raw) {
        return ((raw * 1739) / 1000) + 36;
}

// Absolute minimum displayed SOC implied by observed terminal mV (coarse band).
// Prevents "0 %" while the divider clearly reads mid/high pack; refined by the
// discharge curve / EMA once trustworthy samples stream.
static inline uint32_t bat_mv_min_display_pct(uint32_t mv) {
    if (mv <= 3300u) return 0;
    if (mv >= 4150u) return 100u;
    return ((mv - 3300u) * 100u) / (4150u - 3300u);
}

// Discharge curve (percentage vs ADC via bat_adc_to_mv). Lower knots unchanged.
// LR 523450 / 523450-class 1000 mAh pouch: datasheets cite nominal 3.7 V and CCCV to
// 4.2 V; on this badge rested "full" is often ~4.05–4.12 V depending on charger
// termination and divider — calibrated here to ~4.09 V rested for 100% SOC so the gauge
// can reach full without assuming 4.20 V terminal.
//
//   3.00 -> r: 1697-1705 f: 1703-1705    0%
//   3.40 -> r: 1928-1941 f: 1933-1935   10%
//   3.55 -> r: 2013-2020 f: 2018-2020   20%
//   3.65 -> r: 2070-2083 f: 2077-2079   30%
//   3.70 -> r: 2099-2108 f: 2105-2107   40%
//   3.75 -> r: 2127-2136 f: 2133-2135   50%
//   3.80 -> r: 2153-2182 f: 2163-2165   60%
//   3.85 -> r: 2189-2197 f: 2193-2195   70%
//   3.95 -> r: 2244-2253 f: 2250-2252   80%
//  ~4.02 -> adc ~2289 (bat_adc_to_mv)   90%   rested OCV bracket below cell "full"
//  ~4.09 -> adc ~2331 (bat_adc_to_mv)  100%   match field rested max on LR 523450

static inline uint32_t bat_raw_to_pct(uint32_t adc_value) {
    static const uint32_t curve_filter[11] = {
        1705, 1933, 2018, 2077, 2105, 2133, 2163, 2193, 2250, 2289, 2331
    };
    static const uint32_t curve_pct[11] = {
        0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100,
    };
    if (adc_value <= curve_filter[0])  return 0;
    if (adc_value >= curve_filter[10]) return 100;
    for (uint32_t i = 1u; i < 11u; i++) {
        if (adc_value <= curve_filter[i]) {
            uint32_t lo  = curve_filter[i - 1u];
            uint32_t hi  = curve_filter[i];
            uint32_t pcl = curve_pct[i - 1u];
            uint32_t pch = curve_pct[i];
            uint32_t span = hi - lo;
            if (span == 0u) return pch;
            // Linear pct in [pcl, pch]; round nearest.
            uint64_t delta = (uint64_t)(adc_value - lo) *
                             (uint64_t)(pch - pcl);
            delta += (uint64_t)span / 2ULL;
            return pcl + (uint32_t)(delta / (uint64_t)span);
        }
    }
    return 100;  // unreachable
}

// ── Probe classifier ────────────────────────────────────────────────────────
// Pure: takes already-collected samples, returns verdict. The I/O of
// CE-toggling + ADC sampling is the adapter's job.
//   1  -> battery present (samples flat AND all >= BAT_RAW_MIN)
//   0  -> battery absent  (any sample < BAT_RAW_MIN, OR falling slope)
//  -1  -> undecided (let heuristics handle this tick)
// `out_median` receives the median-pair average of the samples (caller may
// publish only when verdict == 1; samples buffer is sorted in place).
//
// Preconditions: `samples` non-NULL when n >= 1. `out_median` may be NULL.
static inline int8_t bat_classify_probe(uint32_t *samples,
                                        uint8_t n,
                                        uint32_t *out_median) {
    if (n < 2) {
        // Note: dereferences samples[0] when n == 1 — caller must pass a
        // valid buffer per the file-level pointer contract.
        if (out_median) *out_median = (n == 1) ? samples[0] : 0;
        return -1;
    }

    uint32_t mn = samples[0], mx = samples[0];
    for (uint8_t i = 1; i < n; i++) {
        if (samples[i] < mn) mn = samples[i];
        if (samples[i] > mx) mx = samples[i];
    }
    int32_t slope_drop = (int32_t)samples[0] - (int32_t)samples[n - 1];
    uint32_t spread    = mx - mn;

    bat_sort_small(samples, n);
    if (out_median) {
        // BAT_PROBE_SAMPLES is statically asserted even, so the even-N branch
        // is the only one ever taken in production. The odd-N branch exists
        // for forward-compatibility if a future config uses an odd window.
        if ((n & 1u) == 0u) {
            *out_median = bat_median_pair(samples, n);
        } else {
            *out_median = samples[n / 2];
        }
    }

    if (mn < (uint32_t)BAT_RAW_MIN) return 0;
    if (slope_drop >= (int32_t)BAT_PROBE_SLOPE_DROP) return 0;
    if (spread <= (uint32_t)BAT_PROBE_FLAT_SPREAD) return 1;
    return -1;
}

// ── Filter state machine (IIR on raw ADC; EMA+gated pct for display/UI) ─

typedef struct {
    uint32_t adc_raw;
    uint32_t filtered_raw;  // stored << BAT_IIR_SHIFT; 0 == bootstrap-armed
    uint32_t pct_smooth;    // stored << BAT_PCT_EMA_SHIFT; 0 == pct EMA bootstrap
    uint32_t bat_mv;
    uint32_t bat_pct;
    uint8_t  pct_boot_ticks;  // BAT_PCT_BOOTSTRAP_TICKS until normal EMA
} bat_filter_state_t;

static inline void bat_filter_zero(bat_filter_state_t *s) {
    s->adc_raw        = 0;
    s->filtered_raw   = 0;  // also re-arms the IIR bootstrap sentinel
    s->pct_smooth     = 0;  // pct EMA bootstrap
    s->bat_mv         = 0;
    s->bat_pct        = 0;
    s->pct_boot_ticks = (uint8_t)BAT_PCT_BOOTSTRAP_TICKS;
}

// Feed a TRUSTWORTHY raw sample. Caller decides trustworthiness via
// bat_sample_trustworthy(). bat_mv is recomputed on every call so the
// host always sees the freshest reading even when the IIR holds steady.
//
// Defensive guard: silently drops samples below BAT_RAW_MIN. Two reasons:
//   (1) The IIR uses `filtered_raw == 0` as a bootstrap sentinel; if a
//       buggy adapter ever fed a true zero (or near-zero) on the first
//       call after bat_filter_zero(), the IIR would seed itself with that
//       junk and take ~2^BAT_IIR_SHIFT ticks to recover.
//   (2) Sub-RAW_MIN samples mean "no battery on the line" — they should
//       never reach the IIR even if the trustworthiness predicate is
//       wrong. This keeps both ports identical-behavior even if one
//       adapter forgets the predicate check.
static inline void bat_filter_update(bat_filter_state_t *s, uint32_t raw) {
    if (raw < (uint32_t)BAT_RAW_MIN) return;

    s->adc_raw = raw;
    if (s->filtered_raw == 0) {
        s->filtered_raw = raw << BAT_IIR_SHIFT;
    } else {
        s->filtered_raw = s->filtered_raw -
                          (s->filtered_raw >> BAT_IIR_SHIFT) +
                          raw;
    }

    uint32_t pct_inst = bat_raw_to_pct(raw);
    if (pct_inst > 100u) pct_inst = 100u;

    if (s->pct_boot_ticks > 0u) {
        s->pct_boot_ticks = (uint8_t)(s->pct_boot_ticks - 1u);
        s->bat_pct        = pct_inst;
        s->pct_smooth     = pct_inst << BAT_PCT_EMA_SHIFT;
        s->bat_mv         = bat_adc_to_mv(s->adc_raw);
        return;
    }

    if (s->pct_smooth == 0) {
        s->pct_smooth = pct_inst << BAT_PCT_EMA_SHIFT;
    } else {
        s->pct_smooth =
            s->pct_smooth -
            (s->pct_smooth >> BAT_PCT_EMA_SHIFT) +
            pct_inst;
    }
    uint32_t pct_ema = s->pct_smooth >> BAT_PCT_EMA_SHIFT;
    if (pct_ema > 100u) pct_ema = 100u;

    // When the curve thinks there is measurable charge (>0%), never show lower
    // than this sample's instantaneous reading (guards sluggish EMA + bad
    // interaction with downstream monotonic clamps). True empty: pct_inst==0 —
    // let EMA trail down alone so noisy highs do not plaster "full".
    if (pct_inst != 0u) {
        s->bat_pct = (pct_ema > pct_inst) ? pct_ema : pct_inst;
    } else {
        s->bat_pct = pct_ema;
    }

    // Instantaneous millivolts — not the IIR-filtered ADC.
    s->bat_mv = bat_adc_to_mv(s->adc_raw);
}

static inline void bat_filter_apply_mv_soc_floor(bat_filter_state_t *s) {
    if (s->bat_mv == 0u) return;
    uint32_t floor_pct = bat_mv_min_display_pct(s->bat_mv);
    if (s->adc_raw >= (uint32_t)BAT_RAW_MIN) {
        uint32_t crv = bat_raw_to_pct(s->adc_raw);
        if (crv > floor_pct) floor_pct = crv;
    }
    if (s->bat_pct < floor_pct) s->bat_pct = floor_pct;
    if (s->bat_pct > 100u) s->bat_pct = 100u;
}

// ── Presence state machine ──────────────────────────────────────────────────

typedef struct {
    uint32_t stat_history;       // bit 0 = newest; masked to BAT_STAT_HISTORY_BITS
    uint16_t debounce_count;     // climbs toward target state
    bool     latched;
    uint32_t prev_raw;
    bool     prev_raw_valid;
    bool     prev_latched;       // for present->absent edge detection
    bool     stat_hiccup;        // last computed STAT-flapping verdict
} bat_presence_state_t;

static inline void bat_presence_init(bat_presence_state_t *p) {
    p->stat_history   = 0;
    p->debounce_count = 0;
    // Optimistic default: assume battery is present at boot. On a USB-only
    // boot with no cell, the host may briefly see (bat_present=true,
    // bat_pct=0) for up to BAT_DEBOUNCE_ABSENT ticks before the state
    // machine drops the latch. UI layers should treat bat_pct==0 as
    // "unknown" until the first trustworthy sample lands rather than
    // displaying "0%" outright.
    p->latched        = true;
    p->prev_raw       = 0;
    p->prev_raw_valid = false;
    p->prev_latched   = true;
    p->stat_hiccup    = false;
}

typedef struct {
    bool     pgood;
    bool     stat_raw;
    bool     have_raw;
    uint32_t raw;
    uint32_t adc_spread;     // 0 when no comparable multi-sample window
    int8_t   probe_verdict;  // 1 present, 0 absent, -1 undecided / no probe this tick
} bat_presence_inputs_t;

typedef struct {
    bool target_present;            // post-heuristic target before debounce/override
    bool latched;                   // post-debounce, post-override final verdict
    bool edge_present_to_absent;    // true on the wake the latch dropped
    bool stat_hiccup;               // BQ24079 STAT line flapping (charge fault recovery)
} bat_presence_outputs_t;

static inline void bat_presence_update(bat_presence_state_t *p,
                                       const bat_presence_inputs_t *in,
                                       bat_presence_outputs_t *out) {
    // STAT history: only update while pgood (hiccup mode requires USB).
    // Mask the history register itself so old bits past the analysis window
    // can never leak into (history >> 1) during the popcount step. With
    // BAT_STAT_HISTORY_BITS=8 this also keeps the popcount loop bounded.
    bool stat_hiccup = false;
    uint32_t mask = ((uint32_t)1 << BAT_STAT_HISTORY_BITS) - 1u;
    if (in->pgood) {
        p->stat_history = ((p->stat_history << 1) |
                           (in->stat_raw ? 1u : 0u)) & mask;
        // Adjacent-bit XOR: bit i of xor_bits is set iff history bits i and
        // i+1 differ. With an N-bit window there are N-1 adjacent pairs, so
        // we mask with (mask >> 1) to keep only positions 0..N-2.
        uint32_t xor_bits = (p->stat_history ^ (p->stat_history >> 1)) & (mask >> 1);
        uint8_t flips = 0;
        while (xor_bits) {
            flips += (uint8_t)(xor_bits & 1u);
            xor_bits >>= 1;
        }
        stat_hiccup = (flips >= (uint8_t)BAT_STAT_HICCUP_FLIPS);
    }
    p->stat_hiccup = stat_hiccup;

    bool absent_suspected = false;
    if (in->have_raw) {
        bool raw_implausible = (in->raw < (uint32_t)BAT_RAW_MIN);
        bool wake_delta_high = false;
        if (p->prev_raw_valid) {
            uint32_t delta = (in->raw > p->prev_raw)
                ? (in->raw - p->prev_raw)
                : (p->prev_raw - in->raw);
            wake_delta_high = (delta >= (uint32_t)BAT_WAKE_DELTA);
        }

        // Only commit plausible samples to prev_raw. An implausibly-low
        // reading (battery removed, divider floating) would otherwise
        // poison the next tick's wake-delta calculation: the real
        // re-insertion sample would compare against ~50 instead of the
        // last good ~2200, trip wake_delta_high, and keep the latch
        // (still true under debounce) suspecting absent for several
        // ticks after the cell is back.
        if (!raw_implausible) {
            p->prev_raw       = in->raw;
            p->prev_raw_valid = true;
        }

        absent_suspected = in->pgood && (raw_implausible
                                         || wake_delta_high
                                         || stat_hiccup
                                         || (in->adc_spread >= (uint32_t)BAT_FLOAT_SPREAD));
    }

    bool target_present = in->have_raw ? !absent_suspected : p->latched;
    if (target_present == p->latched) {
        p->debounce_count = 0;
    } else {
        uint16_t threshold = p->latched
            ? (uint16_t)BAT_DEBOUNCE_ABSENT
            : (uint16_t)BAT_DEBOUNCE_PRESENT;
        if (++p->debounce_count >= threshold) {
            p->latched        = target_present;
            p->debounce_count = 0;
        }
    }

    // Probe override: definitive verdict bypasses debounce.
    if (in->probe_verdict == 1 && !p->latched) {
        p->latched        = true;
        p->debounce_count = 0;
    } else if (in->probe_verdict == 0 && p->latched) {
        p->latched        = false;
        p->debounce_count = 0;
    }

    bool edge = (p->prev_latched && !p->latched);
    if (edge) {
        // Clear wake-delta history so the eventual re-insertion doesn't
        // trip a false delta against a pre-removal value.
        p->prev_raw       = 0;
        p->prev_raw_valid = false;
    }
    p->prev_latched = p->latched;

    out->target_present         = target_present;
    out->latched                = p->latched;
    out->edge_present_to_absent = edge;
    out->stat_hiccup            = stat_hiccup;
}

// ── Coordinator predicates ──────────────────────────────────────────────────

// Same rule as the ULP source: only feed the IIR with cell-voltage samples.
//   1. !pgood path: BQ passes the cell straight through, the regular
//      median-of-N raw IS the cell voltage. Gate on target_present.
//   2.  pgood path: only the probe's median (under verdict==1) is the true
//      cell voltage. Those ADC samples MUST be taken while the host has
//      disabled charging / charger IC paused (Echo: CE high during burst).
//      Never widen this predicate to unconditional divider reads — you will
//      lie about SOC while plugged in.
static inline bool bat_sample_trustworthy(bool pgood,
                                          bool have_raw,
                                          bool have_probe_sample,
                                          bool target_present,
                                          int8_t probe_verdict) {
    if (!pgood && have_raw && target_present) return true;
    if (pgood && have_probe_sample && probe_verdict == 1) return true;
    return false;
}

// Compose the shared `flags` word using MASK_* from CommonFlags.h.
//
// Note: `stat_hiccup` is intentionally NOT folded into this word — the
// flags layout is part of the wire/RTC-mailbox contract with the host and
// every consumer of the flag bits already exists. Hiccup is exposed
// instead via `bat_presence_outputs_t::stat_hiccup` (and persisted on the
// state struct) so consumers that care can read it directly without
// changing the flag-bit layout. If a MASK_STAT_HICCUP slot is ever added
// to CommonFlags.h, fold it in here.
static inline uint32_t bat_compose_flags(bool pgood,
                                         bool stat_raw,
                                         bool stat_low,
                                         bool bat_present) {
    uint32_t f = 0;
    if (pgood)       f |= MASK_PGOOD;
    if (stat_raw)    f |= MASK_STAT_RAW;
    if (stat_low)    f |= MASK_STAT_LOW;
    if (bat_present) f |= MASK_BAT_PRESENT;
    return f;
}

// ── Threshold classifier ────────────────────────────────────────────────────
// Three-band UI classifier (pct→band). WiFi association is no longer gated on
// battery band — Power::wifiAllowed is unconditional — but thresholds remain
// useful for telemetry / future UX.
typedef enum {
    BAT_THRESH_NORMIE = 0,  // pct >= 20
    BAT_THRESH_DOM    = 1,  // 10 <= pct < 20
    BAT_THRESH_SUB    = 2   // pct < 10
} battery_threshold_t;

static inline battery_threshold_t bat_classify_threshold(uint32_t pct) {
    if (pct < 10) return BAT_THRESH_SUB;
    if (pct < 20) return BAT_THRESH_DOM;
    return BAT_THRESH_NORMIE;
}

// Bootup fast path: caller has 10 raw ADC samples but no IIR / probe state
// yet. Sort, take median-pair, convert to pct, classify. Samples buffer is
// sorted in place. n must be >= 2 and even.
static inline battery_threshold_t bat_bootup_threshold(uint32_t *samples,
                                                       uint8_t n) {
    bat_sort_small(samples, n);
    uint32_t med = bat_median_pair(samples, n);
    uint32_t pct = bat_raw_to_pct(med);
    return bat_classify_threshold(pct);
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BADGE_BATTERY_ALGORITHM_H
