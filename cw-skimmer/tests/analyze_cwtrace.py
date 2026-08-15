#!/usr/bin/env python3
"""Analyze a .cwtrace scope capture and offline-decode Morse with WPM timing.

Usage:
  python3 tests/analyze_cwtrace.py captures/cwtrace_....cwtrace
  python3 tests/analyze_cwtrace.py captures/cwtrace_....cwtrace --wpm 18
  python3 tests/analyze_cwtrace.py --synth-bel --wpm 18   # self-test BEL
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

MORSE = {
    ".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E",
    "..-.": "F", "--.": "G", "....": "H", "..": "I", ".---": "J",
    "-.-": "K", ".-..": "L", "--": "M", "-.": "N", "---": "O",
    ".--.": "P", "--.-": "Q", ".-.": "R", "...": "S", "-": "T",
    "..-": "U", "...-": "V", ".--": "W", "-..-": "X", "-.--": "Y",
    "--..": "Z",
    "-----": "0", ".----": "1", "..---": "2", "...--": "3", "....-": "4",
    ".....": "5", "-....": "6", "--...": "7", "---..": "8", "----.": "9",
}


def load_cwtrace(path: Path):
    meta = {}
    samples = []  # (t_ms, power_db, above)
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" in line and not line[0].isdigit() and not line.startswith("-"):
                # meta key=value (time lines start with digits)
                k, _, v = line.partition("=")
                if k and not k[0].isdigit() and " " not in k:
                    meta[k] = v
                    continue
            parts = line.split()
            if len(parts) >= 3:
                try:
                    t = int(parts[0])
                    p = float(parts[1])
                    a = int(parts[2]) != 0
                    samples.append((t, p, a))
                except ValueError:
                    continue
    return meta, samples


def decode_stream(above_series, times_ms, wpm: float):
    """above_series: list of bool, times_ms aligned."""
    unit = 1.2 / max(5.0, wpm)  # seconds
    pending = ""
    text = []
    elements = []  # (kind, duration_s, symbol)

    if not above_series:
        return "", elements, unit

    state = above_series[0]
    start_t = times_ms[0]

    def close_mark(dur):
        nonlocal pending
        if dur < unit * 0.25:
            return
        is_dah = dur >= unit * 2.0
        sym = "-" if is_dah else "."
        pending += sym
        elements.append(("mark", dur, sym))

    def close_space(dur):
        nonlocal pending
        elements.append(("space", dur, ""))
        if not pending:
            if dur >= unit * 5.5 and text and text[-1] != " ":
                text.append(" ")
            return
        if dur < unit * 2.0:
            return
        ch = MORSE.get(pending, "?")
        text.append(ch)
        elements.append(("letter", dur, ch + f" [{pending}]"))
        pending = ""
        if dur >= unit * 5.5:
            text.append(" ")

    for i in range(1, len(above_series)):
        if above_series[i] == state:
            continue
        dur = max(0.0005, (times_ms[i] - start_t) / 1000.0)
        if state:
            close_mark(dur)
        else:
            close_space(dur)
        state = above_series[i]
        start_t = times_ms[i]

    # flush trailing space
    if not state and pending:
        dur = max(0.0005, (times_ms[-1] - start_t) / 1000.0)
        if dur >= unit * 2.0:
            close_space(dur)

    return "".join(text).strip(), elements, unit


def synth_bel(wpm: float, sample_ms: float = 22.0):
    """Synthesize high/low for 'BEL' at given WPM."""
    unit_ms = 1200.0 / wpm
    # B=-... E=. L=.-..
    letters = ["-...", ".", ".-.."]
    pattern = []  # list of (is_mark, units)
    for li, letter in enumerate(letters):
        for ei, ch in enumerate(letter):
            pattern.append((True, 3.0 if ch == "-" else 1.0))
            if ei + 1 < len(letter):
                pattern.append((False, 1.0))  # element gap
        if li + 1 < len(letters):
            pattern.append((False, 3.0))  # letter gap

    samples = []
    t = 0.0
    # lead-in silence
    for _ in range(10):
        samples.append((int(t), -90.0, False))
        t += sample_ms
    for is_mark, units in pattern:
        dur = units * unit_ms
        end = t + dur
        while t < end:
            p = -40.0 if is_mark else -90.0
            samples.append((int(t), p, is_mark))
            t += sample_ms
    for _ in range(20):
        samples.append((int(t), -90.0, False))
        t += sample_ms
    return samples


def analyze(samples, wpm: float, label: str = ""):
    if not samples:
        print("No samples")
        return
    times = [s[0] for s in samples]
    powers = [s[1] for s in samples]
    above = [s[2] for s in samples]
    dur_s = (times[-1] - times[0]) / 1000.0
    n_high = sum(1 for a in above if a)
    print(f"=== {label or 'capture'} ===")
    print(f"samples={len(samples)}  duration={dur_s:.2f}s  high_frac={n_high/len(samples):.2%}")
    print(f"power min={min(powers):.1f} max={max(powers):.1f} mean={sum(powers)/len(powers):.1f} dB")
    print(f"median sample step ≈ {dur_s*1000/max(1,len(samples)-1):.1f} ms")

    # mark/space run lengths
    runs = []
    st = above[0]
    t0 = times[0]
    for i in range(1, len(above)):
        if above[i] != st:
            runs.append((st, (times[i] - t0) / 1000.0))
            st = above[i]
            t0 = times[i]
    marks = [d for hi, d in runs if hi]
    spaces = [d for hi, d in runs if not hi]
    if marks:
        print(f"marks n={len(marks)}  min={min(marks)*1000:.0f} med={sorted(marks)[len(marks)//2]*1000:.0f} max={max(marks)*1000:.0f} ms")
    if spaces:
        print(f"spaces n={len(spaces)} min={min(spaces)*1000:.0f} med={sorted(spaces)[len(spaces)//2]*1000:.0f} max={max(spaces)*1000:.0f} ms")

    text, elements, unit = decode_stream(above, times, wpm)
    print(f"WPM={wpm:.1f}  dit={unit*1000:.0f} ms")
    print(f"decoded: [{text}]")
    print("first 30 elements:")
    for e in elements[:30]:
        print(f"  {e[0]:6s} {e[1]*1000:7.0f} ms  {e[2]}")


def auto_decode(samples):
    """Match GUI ThresholdMorseWindow: cluster marks + merge short holes."""
    if not samples:
        return "", 18.0, 0.08
    times = [s[0] for s in samples]
    above = [s[2] for s in samples]
    runs = []
    st, t0 = above[0], times[0]
    for i in range(1, len(above)):
        if above[i] != st:
            runs.append((st, (times[i] - t0) / 1000.0))
            st, t0 = above[i], times[i]
    runs.append((st, (times[-1] - t0) / 1000.0))

    marks = sorted(d for hi, d in runs if hi and d >= 0.03)
    if len(marks) < 2:
        unit = 1.2 / 18.0
    else:
        best_gap, split = 0.0, len(marks)
        for i in range(1, len(marks)):
            g = marks[i] - marks[i - 1]
            if g > best_gap:
                best_gap, split = g, i
        dits = marks[:split] if best_gap >= 0.08 and 0 < split < len(marks) else marks[: (len(marks) + 1) // 2]
        unit = sorted(dits)[len(dits) // 2]
        unit = max(0.03, min(0.3, unit))

    hole = max(0.05, unit * 0.4)
    merged = []
    i = 0
    while i < len(runs):
        hi, d = runs[i]
        if merged and merged[-1][0] and (not hi) and d < hole and i + 1 < len(runs) and runs[i + 1][0]:
            merged[-1] = (True, merged[-1][1] + d + runs[i + 1][1])
            i += 2
            continue
        if hi and d < max(0.025, unit * 0.25):
            i += 1
            continue
        merged.append((hi, d))
        i += 1

    pending = ""
    out = []
    for hi, d in merged:
        if hi:
            pending += "-" if d >= unit * 2 else "."
        else:
            if not pending:
                if d >= unit * 5 and out and out[-1] != " ":
                    out.append(" ")
                continue
            if d < unit * 2:
                continue
            out.append(MORSE.get(pending, "?"))
            pending = ""
            if d >= unit * 5:
                out.append(" ")
    if pending:
        out.append(MORSE.get(pending, "?"))
    return "".join(out), 1.2 / unit, unit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?", help=".cwtrace file")
    ap.add_argument("--wpm", type=float, default=18.0)
    ap.add_argument("--synth-bel", action="store_true")
    ap.add_argument("--try-wpm", action="store_true", help="try several WPM values")
    ap.add_argument("--auto", action="store_true", help="auto WPM (GUI algorithm)")
    args = ap.parse_args()

    if args.synth_bel:
        samples = synth_bel(args.wpm)
        analyze(samples, args.wpm, f"synth BEL @ {args.wpm} WPM")
        if args.try_wpm:
            for w in range(10, 36, 2):
                t, _, _ = decode_stream([s[2] for s in samples], [s[0] for s in samples], w)
                print(f"  wpm={w:2d} -> [{t}]")
        return 0

    if not args.path:
        ap.error("path or --synth-bel required")
    path = Path(args.path)
    meta, samples = load_cwtrace(path)
    print("meta:", meta)
    if args.auto:
        text, wpm, unit = auto_decode(samples)
        print(f"AUTO unit={unit*1000:.0f} ms (~{wpm:.1f} WPM)  decoded=[{text}]")
        analyze(samples, wpm, path.name)
    elif args.try_wpm:
        for w in range(8, 41, 2):
            t, _, _ = decode_stream([s[2] for s in samples], [s[0] for s in samples], w)
            print(f"  wpm={w:2d} -> [{t}]")
    else:
        analyze(samples, args.wpm, path.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
