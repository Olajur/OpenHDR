"""Checks an OpenHDR frame dump of sdr_test against the expected inverse tone mapping curve.

    python tools/analyze_dump.py [dump dir] [--peak N] [--paper N] [--pq]

The dump dir defaults to %LOCALAPPDATA%\\OpenHDR\\dumps. Prints the output luminance (nits) along the
grey ramp and compares it with the curve computed here from the itm.hlsl default parameters.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # also works under python -I
from pfm import read_pfm  # noqa: E402


def pq_to_scrgb(e):
    """An HDR10 (PQ) code value to scRGB units (80 nits). Grey stays grey in BT.2020, so the ramp
    check works on PQ output channel by channel."""
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    p = max(min(e, 1.0), 0.0) ** (1 / m2)
    return 10000 * (max(p - c1, 0) / (c2 - c3 * p)) ** (1 / m1) / 80


def expected_nits(code, peak, paper, shoulder=0.25, power=2.0, gamma=2.0):
    y = code ** gamma  # contrast 0 decodes with gamma 2.0 (see itm_params.cpp)
    if y > shoulder:
        x = min((y - shoulder) / (1 - shoulder), 1.0)
        y += max(peak / paper - 1, 0) * x ** power
    return min(y * paper, peak)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?", default=os.path.join(os.environ["LOCALAPPDATA"], "OpenHDR", "dumps"))
    ap.add_argument("--peak", type=float, default=400)
    ap.add_argument("--paper", type=float, default=50 / 0.18)
    ap.add_argument("--plain", action="store_true", help="expect plain SDR at paper white (conversion off)")
    ap.add_argument("--contrast", type=int, default=0, help="ContrastLevel the dump was made with")
    ap.add_argument("--pq", action="store_true", help="the output is HDR10 (OutputFormat=hdr10), not scRGB")
    args = ap.parse_args()

    w, h, out = read_pfm(os.path.join(args.dir, "output.pfm"))
    iw, ih, inp = read_pfm(os.path.join(args.dir, "input.pfm"))
    if args.pq:
        out = [[pq_to_scrgb(v) for v in row] for row in out]
    print(f"output {w}x{h}, input {iw}x{ih}")
    if (w, h) != (iw, ih):
        print("FAIL: input/output size mismatch")
        return 1

    row = int(h * 0.04)  # inside the smooth grey ramp
    worst = 0.0
    print(" code   out nits  expected  diff%")
    # Left 60% of the row only: the settings panel may cover the top-right corner.
    for i in range(7):
        x = int(i / 10 * (w - 1))
        code = inp[row][x * 3]
        r, g, b = out[row][x * 3:x * 3 + 3]
        nits = (0.2126 * r + 0.7152 * g + 0.0722 * b) * 80
        gamma = 2.0 + 0.008 * args.contrast
        exp = min(code ** 2.2 * args.paper, args.peak) if args.plain else expected_nits(code, args.peak, args.paper, gamma=gamma)
        diff = 0.0 if exp < 0.5 else abs(nits - exp) / exp * 100
        worst = max(worst, diff)
        print(f" {code:5.3f}  {nits:8.2f}  {exp:8.2f}  {diff:5.2f}")

    flat = [v for r in out for v in r]
    print(f"output max {max(flat) * 80:.1f} nits, min {min(flat) * 80:.3f} nits, "
          f"NaN/Inf: {sum(1 for v in flat if v != v or abs(v) == float('inf'))}")
    # Dither adds up to +-1 code value, which is a few % in the darker steps.
    ok = worst < 6
    print("PASS" if ok else "FAIL", f"(worst deviation {worst:.2f}%)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
