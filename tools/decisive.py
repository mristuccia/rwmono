"""Is --diamond a genuine pre-decimation anti-alias filter, or just blur?

Post-filter `bin g` with the diamond's exact transfer function. That gives an
image with identical in-band MTF to `bin g --diamond` by construction. Any
remaining difference in beyond-Nyquist energy is aliasing that the prefilter
removed before folding -- which no post-processing can undo.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dngio import write_cfa_dng, read_mono_dng
from evaluate import synth_cfa, run, zone_plate, BLACK, WHITE, D


def diamond_tf(h, w):
    """Transfer function of the 4-tap half-pixel diamond, in output coords."""
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    return (np.cos(np.pi * fx) + np.cos(np.pi * fy)) / 2.0


def apply_tf(img, tf):
    return np.real(np.fft.ifft2(np.fft.fft2(img) * tf))


scene, rmax = zone_plate()
src = os.path.join(D, "syn_zone2.dng")
write_cfa_dng(src, synth_cfa(scene), BLACK, WHITE)

g, blk, wht = run("bin", src, os.path.join(D, "a.dng"))
d, _, _ = run("bin", src, os.path.join(D, "b.dng"), "--diamond")
gp = apply_tf(g, diamond_tf(*g.shape))          # bin g, MTF-matched to diamond

h, w = g.shape
cy, cx = (h - 1) / 2.0, (w - 1) / 2.0
y, x = np.mgrid[0:h, 0:w].astype(np.float64)
r_native = np.hypot(x - cx, y - cy) * 2.0       # pitch 2
r_nyq = 0.25 * (rmax / 0.5)

print(f"{'region (scene freq)':<34}{'bin g':>12}{'bin g blurred':>16}{'--diamond':>12}")
for lo, hi, lab in [(0.00, 0.90, "in band       f < 0.225"),
                    (1.15, 1.60, "just past Nyq 0.29-0.40"),
                    (1.60, 1.95, "far past Nyq  0.40-0.49")]:
    band = (r_native > r_nyq * lo) & (r_native < r_nyq * hi) & (r_native < rmax * 0.98)
    if lo == 0:
        band &= r_native > r_nyq * 0.3
    s = lambda im: im[band].std() / (wht - blk) * 100
    print(f"{lab:<34}{s(g):12.3f}{s(gp):16.3f}{s(d):12.3f}")

# Rigorous version: sweep post-blur strength on bin g and trace the
# (in-band contrast, aliasing) trade-off curve. If the diamond point sits
# below that curve, no post-filter of bin g can reproduce it.
inband = (r_native > r_nyq * 0.3) & (r_native < r_nyq * 0.9)
past = (r_native > r_nyq * 1.15) & (r_native < rmax * 0.95)
sig = lambda im: (im[inband].std() / (wht - blk) * 100,
                  im[past].std() / (wht - blk) * 100)

fy = np.fft.fftfreq(h)[:, None]
fx = np.fft.fftfreq(w)[None, :]
print("\npost-blur sweep on bin g   (Gaussian sigma in output px)")
print(f"{'sigma':>8}{'in-band':>12}{'past Nyq':>12}")
for s in [0, 0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1.0, 1.4]:
    tf = np.exp(-2 * (np.pi * s) ** 2 * (fx ** 2 + fy ** 2))
    a, b = sig(apply_tf(g, tf))
    print(f"{s:8.1f}{a:12.3f}{b:12.3f}")
a, b = sig(d)
print(f"{'--diamond':>8}{a:12.3f}{b:12.3f}   <- target")

# ---- save a visual crop --------------------------------------------------
try:
    from PIL import Image
    n = 460
    y0, x0 = int(cy - n // 2), int(cx - n // 2)
    def to8(im):
        v = np.clip((im - blk) / (wht - blk), 0, 1) ** (1 / 2.2)
        return (v[y0:y0 + n, x0:x0 + n] * 255).astype(np.uint8)
    strip = np.hstack([to8(g), np.full((n, 12), 255, np.uint8),
                       to8(gp), np.full((n, 12), 255, np.uint8), to8(d)])
    Image.fromarray(strip).save(os.path.join(D, "zoneplate.png"))
    print("\nwrote zoneplate.png  (bin g | bin g MTF-matched blur | --diamond)")
except ImportError:
    print("\n(PIL not available, skipping visual)")
os.remove(src)
