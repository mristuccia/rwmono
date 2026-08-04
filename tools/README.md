# tools — measurement harness

The scripts that produce the numbers in §5 of the white paper. They synthesise
Bayer captures of analytically known ground truth, write them as real CFA DNGs,
run the actual `rwmono` binary on them, and measure the results — so they test
the shipping code path, not a reimplementation of it.

Requires `numpy`; `Pillow` is optional (visual crops only). No other dependencies.

```sh
cmake -B build && cmake --build build     # the harness runs build/rwmono
python3 tools/evaluate.py                 # SNR, aliasing, MTF for every mode
python3 tools/decisive.py                 # is --diamond a prefilter, or just blur?
```

Set `RWMONO=/path/to/rwmono` to test a different binary.

## The photon model

Section 5's simulation, shared by all scripts (`evaluate.py`):

| | value |
|---|---|
| relative photosite sensitivity | R 1/2.63, G 1, B 1/1.62, mono 2.0 |
| green full well | 49 650 e⁻ |
| read noise | 3 e⁻ RMS |
| black / white level | 512 / 65 535 |

A green photosite on an 18 % patch therefore holds 8 937 e⁻, whose √N is 94.5 —
the unit in which every SNR figure in the paper is expressed. As a validation,
`evaluate.py` reproduces the published §5.3 table to under 1 %:

| | paper | harness |
|---|---|---|
| mono sensor | 133.7 | 133.7 |
| `bin g` | 133.2 | 134.0 |
| `bin luma` | 150.3 | 151.4 |
| `flat` | 75.6 | 75.5 |
| `quincunx --derot` | 116.0 | 116.4 |

## The scripts

- **`dngio.py`** — minimal uncompressed TIFF/DNG writer and reader. Writes Bayer
  CFA DNGs that LibRaw opens, reads back the mono DNGs rwmono produces with
  `--uncompressed`.
- **`evaluate.py`** — the main harness. SNR on an 18 % patch, beyond-Nyquist
  aliasing energy on a zone plate, and MTF from sinusoidal gratings at known
  frequencies (horizontal and diagonal, via lock-in detection rather than a
  slanted edge, so the measurement frequency is exact).
- **`decisive.py`** — separates anti-aliasing from blur. Sweeps a post-blur on
  `bin g` and traces the (in-band contrast, aliasing) trade-off curve; a point
  below that curve cannot be reached by any post-processing.
- **`realcheck.py`** — runs the same noise check against a real capture and
  locates the tiles where `bin g` and `--diamond` differ most.

## Caveats

- The MTF tables report *aliased* amplitude beyond a method's own grid Nyquist
  (the measurement frequency is folded), which is informative but is not MTF.
  For pitch-2 methods the 0.25 cyc/native-px column is degenerate — it sits
  exactly at output Nyquist, where the quadrature detector has no phase
  reference — and should be ignored.
- Diagonal columns beyond a method's Nyquist assume the alias reappears at the
  same orientation, which 2-D folding does not guarantee. The zone plate is the
  trustworthy aliasing metric; the gratings are the trustworthy MTF metric.
- `flat` has output pitch 1, so its beyond-Nyquist band is empty by
  construction and it reports no aliasing figure.
