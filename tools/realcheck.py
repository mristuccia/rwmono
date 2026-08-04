"""Check the synthetic conclusions against a real capture.

    rwmono bin IMG.3FR --uncompressed -o a.dng
    rwmono bin IMG.3FR --diamond --uncompressed -o b.dng
    python3 tools/realcheck.py a.dng b.dng

Compares noise on the flattest tiles against the predicted sqrt(2), and locates
the tiles where the two differ most -- i.e. where the aliasing lived.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dngio import read_mono_dng

D = os.path.dirname(os.path.abspath(__file__))
if len(sys.argv) != 3:
    sys.exit(__doc__)
g, blk, wht = read_mono_dng(sys.argv[1])
d, _, _ = read_mono_dng(sys.argv[2])
assert g.shape == d.shape, "both inputs must be the same size (bin vs bin --diamond)"
h, w = g.shape
P = 96

# tile both images and collect per-tile statistics
hh, ww = h // P, w // P
tg = g[:hh * P, :ww * P].reshape(hh, P, ww, P).transpose(0, 2, 1, 3).reshape(hh, ww, -1)
td = d[:hh * P, :ww * P].reshape(hh, P, ww, P).transpose(0, 2, 1, 3).reshape(hh, ww, -1)

# --- noise: flattest tiles (smooth sky / wall), high-pass to drop gradients
def hp_std(t):
    x = t.reshape(t.shape[0], t.shape[1], P, P)
    lap = (x[:, :, 1:-1, 1:-1] * 4 - x[:, :, :-2, 1:-1] - x[:, :, 2:, 1:-1]
           - x[:, :, 1:-1, :-2] - x[:, :, 1:-1, 2:])
    return lap.std(axis=(2, 3)) / np.sqrt(20.0)      # unit-gain normalisation

ng, nd = hp_std(tg), hp_std(td)
mean_g = tg.mean(axis=2)
flat = np.argsort(ng.ravel())[:40]
print("noise on the 40 flattest 96x96 tiles (high-pass sigma, 16-bit DN)")
print(f"  mean signal level     {mean_g.ravel()[flat].mean():10.0f} DN")
print(f"  bin g                 {ng.ravel()[flat].mean():10.2f}")
print(f"  bin g --diamond       {nd.ravel()[flat].mean():10.2f}")
print(f"  ratio                 {ng.ravel()[flat].mean() / nd.ravel()[flat].mean():10.3f}"
      f"   (sqrt2 = 1.414 predicted)")

# --- where do the two differ most? that is where aliasing lived
diff = np.abs(tg.astype(np.float32) - td).mean(axis=2)
idx = np.unravel_index(np.argsort(diff.ravel())[-6:], diff.shape)
print("\ntiles with the largest bin g vs --diamond difference (aliasing hot spots)")
for r, c in zip(*idx):
    print(f"  tile ({r:3d},{c:3d}) at px ({c*P:5d},{r*P:5d})  mean|diff| "
          f"{diff[r, c]:7.1f} DN = {diff[r, c]/(wht-blk)*100:.2f}%")

try:
    from PIL import Image
    r, c = idx[0][-1], idx[1][-1]
    y0, x0 = max(r * P - 100, 0), max(c * P - 100, 0)
    n = 320
    def to8(im):
        v = np.clip((im[y0:y0 + n, x0:x0 + n] - blk) / (wht - blk), 0, 1) ** (1 / 2.2)
        v = (v - v.min()) / max(v.ptp(), 1e-6)
        return (np.kron(v, np.ones((2, 2))) * 255).astype(np.uint8)
    strip = np.hstack([to8(g), np.full((n * 2, 10), 255, np.uint8), to8(d)])
    Image.fromarray(strip).save(os.path.join(D, "real_crop.png"))
    print(f"\nwrote real_crop.png (2x nearest): bin g | --diamond  at ({x0},{y0})")
except ImportError:
    pass
