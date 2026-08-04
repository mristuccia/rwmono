"""Evaluate rwmono's --diamond (Sensor+ green aperture) against the existing modes.

Synthesises Bayer captures of known ground truth, runs the real binary, and
measures MTF, beyond-Nyquist aliasing energy and SNR.

Photon model matches the white paper's section 5:
  relative photosite sensitivity  R 1/2.63, G 1, B 1/1.62, mono 2.0
  green full-well 49650 e-, read noise 3 e- RMS
so a green photosite at an 18% patch holds 8937 e- (sqrt = 94.5).
"""
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dngio import write_cfa_dng, read_mono_dng

D = os.path.dirname(os.path.abspath(__file__))
RW = os.environ.get("RWMONO", os.path.join(D, os.pardir, "build", "rwmono"))
RW = os.path.abspath(RW)

SENS = {0: 1 / 2.63, 1: 1.0, 2: 1 / 1.62, 3: 1.0}   # R G B G2
FULLWELL_G = 49650.0
READ_NOISE = 3.0
BLACK = 512
WHITE = 65535
N = 1024                                            # native mosaic side


def synth_cfa(scene, noise=False, seed=0):
    """scene: float HxW in [0,1] scene radiance (neutral grey). -> uint16 CFA."""
    h, w = scene.shape
    r = np.arange(h)[:, None]
    c = np.arange(w)[None, :]
    # RGGB
    ch = np.where((r % 2 == 0) & (c % 2 == 0), 0,
         np.where((r % 2 == 1) & (c % 2 == 1), 2, 1))
    sens = np.select([ch == 0, ch == 2], [SENS[0], SENS[2]], default=1.0)
    e = scene * FULLWELL_G * sens
    if noise:
        rng = np.random.default_rng(seed)
        e = rng.poisson(np.clip(e, 0, None)).astype(np.float64)
        e += rng.normal(0, READ_NOISE, e.shape)
    dn = e * (WHITE - BLACK) / FULLWELL_G + BLACK
    return np.clip(np.rint(dn), 0, 65535).astype(np.uint16)


def run(mode, src, out, *flags):
    cmd = [RW, mode, src, "-o", out, "--uncompressed", "--wb", "2.63,1,1.62", *flags]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(p.stdout + p.stderr)
    return read_mono_dng(out)


# Each entry: label, mode, flags, output pixel pitch in native photosites.
METHODS = [
    ("bin g",                    "bin",      (),                          2.0),
    ("bin g --diamond",          "bin",      ("--diamond",),              2.0),
    ("bin luma",                 "bin",      ("--weights", "luma"),       2.0),
    ("flat",                     "flat",     (),                          1.0),
    ("quincunx --derot",         "quincunx", ("--derotate",),             np.sqrt(2)),
    ("quincunx --diamond --derot", "quincunx", ("--diamond", "--derotate"), 2 * np.sqrt(2)),
]


# ----------------------------------------------------------------- MTF -----
def grating(freq, angle_deg, n=N):
    """Sinusoid at freq cycles/native-px travelling along `angle`."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    a = np.deg2rad(angle_deg)
    phase = 2 * np.pi * freq * (x * np.cos(a) + y * np.sin(a))
    return 0.5 + 0.35 * np.cos(phase)


def measure_amplitude(img, freq_out, angle_deg):
    """Amplitude of a sinusoid of known output-frequency via lock-in detection."""
    h, w = img.shape
    m = min(h, w)
    y0, x0 = (h - m) // 2 + m // 4, (w - m) // 2 + m // 4
    s = m // 2
    sub = img[y0:y0 + s, x0:x0 + s]
    sub = sub - sub.mean()
    y, x = np.mgrid[0:s, 0:s].astype(np.float64)
    a = np.deg2rad(angle_deg)
    ph = 2 * np.pi * freq_out * (x * np.cos(a) + y * np.sin(a))
    # quadrature projection -> amplitude independent of phase
    ci = (sub * np.cos(ph)).mean()
    si = (sub * np.sin(ph)).mean()
    return 2 * np.hypot(ci, si)


def mtf_curve(angle_deg, freqs, tag):
    """Return {label: [mtf at each freq]} normalised by the lowest frequency."""
    res = {lab: [] for lab, *_ in METHODS}
    for f in freqs:
        scene = grating(f, angle_deg)
        src = os.path.join(D, f"syn_{tag}_{f:.3f}.dng")
        write_cfa_dng(src, synth_cfa(scene), BLACK, WHITE)
        for lab, mode, flags, pitch in METHODS:
            out = os.path.join(D, "o.dng")
            img, blk, wht = run(mode, src, out, *flags)
            # frequency expressed in cycles per *output* pixel
            fo = f * pitch
            if fo > 0.5:            # beyond output Nyquist -> alias, record folded
                fo = abs(((fo + 0.5) % 1.0) - 0.5)
            amp = measure_amplitude(img, fo, angle_deg)
            res[lab].append(amp)
        os.remove(src)
    for lab in res:
        v = np.array(res[lab])
        res[lab] = v / v[0]
    return res


# ------------------------------------------------------------- aliasing -----
def zone_plate(n=N, fmax=0.5):
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    cy = cx = (n - 1) / 2.0
    r2 = (x - cx) ** 2 + (y - cy) ** 2
    rmax = n / 2.0
    R = rmax / fmax
    return 0.5 + 0.35 * np.cos(np.pi * r2 / R), rmax


def aliasing_test():
    scene, rmax = zone_plate()
    src = os.path.join(D, "syn_zone.dng")
    write_cfa_dng(src, synth_cfa(scene), BLACK, WHITE)
    rows = []
    for lab, mode, flags, pitch in METHODS:
        img, blk, wht = run(mode, src, os.path.join(D, "o.dng"), *flags)
        h, w = img.shape
        cy, cx = (h - 1) / 2.0, (w - 1) / 2.0
        y, x = np.mgrid[0:h, 0:w].astype(np.float64)
        # radius in native-photosite units
        rr = np.hypot(x - cx, y - cy) * pitch
        nyq = 0.5 / pitch                       # output Nyquist, cyc/native-px
        r_nyq = nyq * (rmax / 0.5)              # radius where scene f == nyq
        band = (rr > r_nyq * 1.25) & (rr < rmax * 0.95)
        v = img[band] / (wht - blk)
        rows.append((lab, v.std() * 100, band.sum()))
    os.remove(src)
    return rows


# ------------------------------------------------------------------ SNR -----
def snr_test(level=0.18):
    scene = np.full((N, N), level)
    src = os.path.join(D, "syn_flat.dng")
    write_cfa_dng(src, synth_cfa(scene, noise=True, seed=7), BLACK, WHITE)
    rows = []
    for lab, mode, flags, pitch in METHODS:
        img, blk, wht = run(mode, src, os.path.join(D, "o.dng"), *flags)
        h, w = img.shape
        s = min(h, w) // 3
        sub = img[h // 2 - s // 2:h // 2 + s // 2, w // 2 - s // 2:w // 2 + s // 2]
        snr = sub.mean() / sub.std()
        # matched scale: resample to bin's grid (pitch 2). Finer grids average
        # down by the real (possibly correlated) noise, so measure it directly.
        if pitch < 2.0:
            k = int(round(2.0 / pitch))
            hh, ww = sub.shape[0] // k * k, sub.shape[1] // k * k
            ds = sub[:hh, :ww].reshape(hh // k, k, ww // k, k).mean(axis=(1, 3))
            snr_m = ds.mean() / ds.std()
        else:
            snr_m = snr                       # coarser than the grid: upsampling
        rows.append((lab, snr, snr_m))
    os.remove(src)

    # true monochrome sensor reference, same area, same photons, no CFA
    rng = np.random.default_rng(7)
    e = rng.poisson(np.full((N, N), level * FULLWELL_G * 2.0))
    e = e + rng.normal(0, READ_NOISE, e.shape)
    mono_pp = e.mean() / e.std()
    ds = e.reshape(N // 2, 2, N // 2, 2).mean(axis=(1, 3))
    rows.insert(0, ("true mono sensor", mono_pp, ds.mean() / ds.std()))
    return rows


if __name__ == "__main__":
    print("=== SNR, 18% patch " + "=" * 46)
    print(f"{'method':<30}{'per-pixel':>12}{'@bin grid':>12}")
    for lab, a, b in snr_test():
        print(f"{lab:<30}{a:12.1f}{b:12.1f}")

    print("\n=== beyond-Nyquist aliasing energy (zone plate) " + "=" * 18)
    print(f"{'method':<30}{'std %':>12}{'px':>10}")
    for lab, s, n in aliasing_test():
        print(f"{lab:<30}{s:12.3f}{n:10d}")

    freqs = [0.02, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45]
    for angle, name in ((0, "horizontal"), (45, "diagonal")):
        print(f"\n=== MTF, {name} gratings (cyc/native px) " + "=" * 22)
        res = mtf_curve(angle, freqs, name)
        print(f"{'method':<30}" + "".join(f"{f:>7.2f}" for f in freqs))
        for lab, *_ in METHODS:
            print(f"{lab:<30}" + "".join(f"{v:>7.3f}" for v in res[lab]))
