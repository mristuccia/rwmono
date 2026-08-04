// rwmono — convert a Bayer RAW (e.g. Hasselblad .3FR) to a monochrome DNG
// without demosaicing.
//
// Modes:
//   bin   2x2 super-pixel binning: each RGGB quad collapses to one output
//         pixel. Half linear resolution, zero interpolation.
//         --weights g     (G1+G2)/2, pure green channel (default)
//         --weights luma  (R' + G1 + G2 + B')/4 with R/B equalized to G
//   flat  Full-resolution mosaic kept as-is; per-channel gains equalize the
//         CFA channels (white-balances the mosaic) so a neutral subject gives
//         a flat mono image at native resolution.
//   quincunx
//         The green checkerboard re-indexed onto a 45deg-rotated square grid:
//         every green sample maps to exactly one output pixel, nothing is
//         interpolated. The scene appears rotated 45deg inside a black
//         bounding box; pixel pitch is sqrt(2) x native. Gr/Gb imbalance is
//         measured per file and equalized (disable with --no-grgb).

#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "dng_writer.h"

namespace {

void usage() {
  std::fprintf(stderr,
      "usage: rwmono <bin|flat|quincunx> <input raw> [options]\n"
      "  -o <file>          output DNG path (default: input name + _<mode>.dng)\n"
      "  --weights <g|luma> bin mode only: G-only or (R+2G+B)/4 luma (default g)\n"
      "  --diamond          Sensor+-style 4-green diamond aperture:\n"
      "                     bin      same output size, 4-tap diamond instead of\n"
      "                              the 2-tap diagonal pair\n"
      "                     quincunx additionally decimate 2x on the green\n"
      "                              lattice (half linear resolution)\n"
      "  --wb <asshot|R,G,B> channel gains for equalization (default asshot)\n"
      "  --no-grgb          quincunx mode only: skip Gr/Gb equalization\n"
      "  --derotate         quincunx: bicubic-resample back to an upright frame\n"
      "  --autocrop         quincunx: tag the largest inscribed rectangle as\n"
      "                     DefaultCrop (crops away most of the frame!)\n"
      "  --uncompressed     disable lossless JPEG compression\n");
}

bool parseGains(const std::string& s, float g[3]) {
  return std::sscanf(s.c_str(), "%f,%f,%f", &g[0], &g[1], &g[2]) == 3 &&
         g[0] > 0 && g[1] > 0 && g[2] > 0;
}

std::string formatTime(time_t t) {
  if (t <= 0) return {};
  char buf[32];
  struct tm tmv;
  localtime_r(&t, &tmv);
  std::strftime(buf, sizeof buf, "%Y:%m:%d %H:%M:%S", &tmv);
  return buf;
}

int flipToOrientation(int flip) {
  switch (flip) {
    case 3: return 3;   // 180
    case 5: return 8;   // 90 CCW
    case 6: return 6;   // 90 CW
    default: return 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string mode = argv[1];
  const std::string input = argv[2];
  if (mode != "bin" && mode != "flat" && mode != "quincunx") {
    usage();
    return 2;
  }

  std::string output;
  std::string weights = "g";
  std::string wb = "asshot";
  bool grgbFix = true;
  bool derotate = false;
  bool autocrop = false;
  bool compress = true;
  bool diamond = false;
  for (int i = 3; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) output = argv[++i];
    else if (a == "--weights" && i + 1 < argc) weights = argv[++i];
    else if (a == "--wb" && i + 1 < argc) wb = argv[++i];
    else if (a == "--no-grgb") grgbFix = false;
    else if (a == "--derotate") derotate = true;
    else if (a == "--autocrop") autocrop = true;
    else if (a == "--diamond") diamond = true;
    else if (a == "--uncompressed") compress = false;
    else {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      usage();
      return 2;
    }
  }
  if (weights != "g" && weights != "luma") {
    std::fprintf(stderr, "--weights must be g or luma\n");
    return 2;
  }
  if ((derotate || autocrop) && mode != "quincunx") {
    std::fprintf(stderr, "--derotate/--autocrop only apply to quincunx mode\n");
    return 2;
  }
  if (diamond && mode == "flat") {
    std::fprintf(stderr, "--diamond only applies to bin and quincunx modes\n");
    return 2;
  }
  if (diamond && mode == "bin" && weights == "luma") {
    std::fprintf(stderr, "--diamond is a green-lattice aperture; use --weights g\n");
    return 2;
  }
  if (output.empty()) {
    std::string base = input;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > base.find_last_of('/') + 0)
      base.resize(dot);
    output = base + "_" + mode + (diamond ? "-diamond" : "") +
             (derotate ? "-derot" : "") + ".dng";
  }

  LibRaw lr;
  int rc = lr.open_file(input.c_str());
  if (rc != LIBRAW_SUCCESS) {
    std::fprintf(stderr, "cannot open %s: %s\n", input.c_str(), libraw_strerror(rc));
    return 1;
  }
  rc = lr.unpack();
  if (rc != LIBRAW_SUCCESS) {
    std::fprintf(stderr, "cannot unpack %s: %s\n", input.c_str(), libraw_strerror(rc));
    return 1;
  }

  const auto& S = lr.imgdata.sizes;
  const auto& C = lr.imgdata.color;
  const auto& P = lr.imgdata.idata;
  const uint16_t* raw = lr.imgdata.rawdata.raw_image;

  if (!raw) {
    std::fprintf(stderr, "no 16-bit Bayer data in %s\n", input.c_str());
    return 1;
  }
  if (!P.filters || P.filters == 9) {
    std::fprintf(stderr, "%s is not a Bayer-mosaic raw (X-Trans/mono not supported)\n",
                 input.c_str());
    return 1;
  }

  const int W = S.width, H = S.height;
  const size_t stride = S.raw_pitch ? S.raw_pitch / 2 : S.raw_width;

  // Per-channel black levels: LibRaw exposes a common base, per-channel
  // deltas, and optionally a repeating pattern block in cblack[6...].
  auto blackFor = [&](int r, int c, int ch) -> float {
    float b = (float)C.black + (float)C.cblack[ch];
    if (C.cblack[4] && C.cblack[5])
      b += (float)C.cblack[6 + (r % C.cblack[4]) * C.cblack[5] + (c % C.cblack[5])];
    return b;
  };

  // Channel gains normalized so green stays at 1.0.
  float gains[4] = {1, 1, 1, 1};
  if (wb == "asshot") {
    float g = C.cam_mul[1] > 0 ? C.cam_mul[1] : 1.0f;
    gains[0] = C.cam_mul[0] / g;
    gains[2] = C.cam_mul[2] / g;
    gains[3] = C.cam_mul[3] > 0 ? C.cam_mul[3] / g : 1.0f;
  } else {
    float g3[3];
    if (!parseGains(wb, g3)) {
      std::fprintf(stderr, "--wb must be 'asshot' or R,G,B (e.g. 2.63,1,1.62)\n");
      return 2;
    }
    gains[0] = g3[0] / g3[1];
    gains[2] = g3[2] / g3[1];
  }

  // Common saturation point: green's usable range. Channels with gain > 1
  // exceed it after equalization and are clipped so highlights clip together.
  const float blackG = (float)C.black + (float)C.cblack[1];
  const float sat = (float)C.maximum - blackG;
  const float scale = 65535.0f / sat;

  std::printf("input:    %s\n", input.c_str());
  std::printf("camera:   %s %s\n", P.make, P.model);
  std::printf("mosaic:   %d x %d visible (raw %d x %d), pattern %c%c%c%c\n",
              W, H, S.raw_width, S.raw_height,
              "RGBG"[lr.COLOR(0, 0)], "RGBG"[lr.COLOR(0, 1)],
              "RGBG"[lr.COLOR(1, 0)], "RGBG"[lr.COLOR(1, 1)]);
  std::printf("levels:   black %u (+%u,%u,%u,%u), white %u\n",
              C.black, C.cblack[0], C.cblack[1], C.cblack[2], C.cblack[3],
              C.maximum);
  std::printf("gains:    R %.4f  G 1.0000  B %.4f  G2 %.4f (%s)\n",
              gains[0], gains[2], gains[3], wb.c_str());

  auto rawAt = [&](int r, int c) -> float {
    return (float)raw[(size_t)(r + S.top_margin) * stride + (c + S.left_margin)];
  };
  auto clamp16 = [](float v) -> uint16_t {
    return (uint16_t)std::lround(std::min(std::max(v, 0.0f), 65535.0f));
  };

  uint32_t outW, outH;
  std::vector<uint16_t> out;
  bool cropSet = false;
  uint32_t cropO[2] = {0, 0}, cropS[2] = {0, 0};

  if (mode == "quincunx") {
    // Green sites live on one checkerboard parity. Re-index them with the
    // diagonal basis e1=(1,1), e2=(-1,1): output row i = (dr+dc)/2, output
    // col j = (dc-dr)/2 relative to the first green site. Every green sample
    // maps to exactly one output pixel; no value is ever interpolated.
    const int c0 = (lr.COLOR(0, 0) == 1 || lr.COLOR(0, 0) == 3) ? 0 : 1;

    // Pass 1: measure Gr/Gb balance (the two greens sit on alternating
    // output diagonals, so any imbalance would render as a lattice).
    double sumGr = 0, sumGb = 0;
    size_t nGr = 0, nGb = 0;
    for (int r = 0; r < H; r++) {
      for (int c = (r % 2 == 0) ? c0 : 1 - c0; c < W; c += 2) {
        int ch = lr.COLOR(r, c);
        if (ch == 1) { sumGr += rawAt(r, c); nGr++; }
        else if (ch == 3) { sumGb += rawAt(r, c); nGb++; }
      }
    }
    float ratio = 1.0f;
    if (nGr && nGb) {
      double mGr = sumGr / nGr - blackG;
      double mGb = sumGb / nGb - ((double)C.black + (double)C.cblack[3]);
      if (mGr > 0 && mGb > 0) ratio = (float)(mGr / mGb);
    }
    const float gbGain = grgbFix ? ratio : 1.0f;
    std::printf("gr/gb:    measured Gr/Gb %.5f, %s\n", ratio,
                grgbFix ? "equalizing Gb to Gr" : "correction disabled");

    // Output bounds: i in [0, (H-1+W-1-c0)/2], j spans the diamond.
    const int iMax = (H - 1 + W - 1 - c0) / 2;
    const int jMin = -((H - 1 + c0) / 2);
    const int jMax = (W - 1 - c0) / 2;
    outW = (uint32_t)(jMax - jMin + 1);
    outH = (uint32_t)(iMax + 1);
    out.assign((size_t)outW * outH, 0);

    for (int r = 0; r < H; r++) {
      for (int c = (r % 2 == 0) ? c0 : 1 - c0; c < W; c += 2) {
        int ch = lr.COLOR(r, c);
        if (ch != 1 && ch != 3) continue;  // safety: skip non-green
        float v = rawAt(r, c) - blackFor(r, c, ch);
        if (ch == 3) v *= gbGain;
        int i = (r + c - c0) / 2;
        int j = (c - c0 - r) / 2 - jMin;
        out[(size_t)i * outW + j] = clamp16(v * scale);
      }
    }

    if (diamond) {
      // Phase One's Sensor+ introduced the diamond super-pixel to stop binned
      // apertures leaving gaps. It bins each green plane within itself,
      // because its output must stay in colour; a monochrome target does not
      // need that, so the natural diamond here is the four *nearest* greens
      // (either Gr or Gb) surrounding one R/B site -- half the footprint.
      // Under the re-indexing above those four are exactly a 2x2 block, so a
      // plain non-overlapping 2x2 decimation tiles the green lattice with no
      // gaps and no overlap. Still an average of measured photosites only --
      // nothing is interpolated. See NOTICE.
      const uint32_t bW = outW / 2, bH = outH / 2;
      std::vector<uint16_t> b((size_t)bW * bH);
      for (uint32_t i = 0; i < bH; i++) {
        const uint16_t* r0 = &out[(size_t)(2 * i) * outW];
        const uint16_t* r1 = &out[(size_t)(2 * i + 1) * outW];
        for (uint32_t j = 0; j < bW; j++) {
          const uint32_t s = (uint32_t)r0[2 * j] + r0[2 * j + 1] +
                             r1[2 * j] + r1[2 * j + 1];
          b[(size_t)i * bW + j] = (uint16_t)((s + 2) / 4);
        }
      }
      out = std::move(b);
      outW = bW;
      outH = bH;
      std::printf("diamond:  2x2 green-lattice bin -> %u x %u\n", outW, outH);
    }

    // Pitch of the current grid in green-lattice units: 1 normally, 2 after
    // the diamond decimation.
    const float qs = diamond ? 2.0f : 1.0f;

    if (derotate) {
      // One spatial resample back to an upright frame: output pixel (x,y)
      // corresponds to scene point (r,c) = (y*sqrt2, x*sqrt2); map that into
      // quincunx grid coordinates and sample with bicubic Catmull-Rom.
      const uint32_t W2 = (uint32_t)std::floor((W - 1) / (M_SQRT2 * qs));
      const uint32_t H2 = (uint32_t)std::floor((H - 1) / (M_SQRT2 * qs));
      std::vector<uint16_t> up((size_t)W2 * H2);
      auto wcr = [](float t, float w[4]) {
        w[0] = ((-0.5f * t + 1.0f) * t - 0.5f) * t;
        w[1] = (1.5f * t - 2.5f) * t * t + 1.0f;
        w[2] = ((-1.5f * t + 2.0f) * t + 0.5f) * t;
        w[3] = (0.5f * t - 0.5f) * t * t;
      };
      const int qw = (int)outW, qh = (int)outH;
      // A binned pixel (I,J) covers quincunx cells 2I..2I+1, so its centre
      // sits at 2I+0.5 -- hence the half-cell shift before the qs division.
      const float half = (qs - 1.0f) * 0.5f;
      for (uint32_t y = 0; y < H2; y++) {
        const float rr = (float)(y * M_SQRT2 * qs);
        for (uint32_t x = 0; x < W2; x++) {
          const float cc = (float)(x * M_SQRT2 * qs);
          const float fi = ((rr + cc - c0) * 0.5f - half) / qs;
          const float fj = ((cc - c0 - rr) * 0.5f - jMin - half) / qs;
          const int i0 = (int)std::floor(fi), j0 = (int)std::floor(fj);
          float wi[4], wj[4];
          wcr(fi - i0, wi);
          wcr(fj - j0, wj);
          float acc = 0;
          for (int di = -1; di <= 2; di++) {
            const int si = std::min(std::max(i0 + di, 0), qh - 1);
            const uint16_t* qrow = &out[(size_t)si * outW];
            float rowAcc = 0;
            for (int dj = -1; dj <= 2; dj++) {
              const int sj = std::min(std::max(j0 + dj, 0), qw - 1);
              rowAcc += wj[dj + 1] * qrow[sj];
            }
            acc += wi[di + 1] * rowAcc;
          }
          up[(size_t)y * W2 + x] = clamp16(acc);
        }
      }
      out = std::move(up);
      outW = W2;
      outH = H2;
    } else if (autocrop) {
      // Largest axis-aligned rectangle inscribed in the diamond is the
      // centered square with half-side (H-1)/4 (H being the short mosaic
      // dimension) -- a large part of the frame necessarily falls outside.
      const int s2 = (int)((H - 1) / (4.0f * qs));
      const int ic = ((int)outH - 1) / 2, jc = ((int)outW - 1) / 2;
      cropO[0] = (uint32_t)(jc - s2);
      cropO[1] = (uint32_t)(ic - s2);
      cropS[0] = cropS[1] = (uint32_t)(2 * s2);
      cropSet = true;
    }
  } else if (mode == "flat") {
    outW = W;
    outH = H;
    out.resize((size_t)outW * outH);
    for (int r = 0; r < H; r++) {
      for (int c = 0; c < W; c++) {
        int ch = lr.COLOR(r, c);
        float v = (rawAt(r, c) - blackFor(r, c, ch)) * gains[ch];
        out[(size_t)r * outW + c] = clamp16(v * scale);
      }
    }
  } else if (mode == "bin" && diamond) {
    // The same diamond aperture at bin's output scale: instead of the two greens that
    // happen to fall inside a quad, average the four greens that surround one
    // R site. The centroid lands on the R lattice -- an upright square grid of
    // pitch 2, the same geometry bin already outputs -- and the aperture is a
    // diamond matched to the green lattice's Brillouin zone rather than a
    // 2-tap diagonal. Two Gr and two Gb per super-pixel, so the Gr/Gb
    // imbalance cancels in the mean without an explicit correction.
    outW = W / 2;
    outH = H / 2;
    out.resize((size_t)outW * outH);
    int dr0 = 0, dc0 = 0;
    for (int dr = 0; dr < 2; dr++)
      for (int dc = 0; dc < 2; dc++)
        if (lr.COLOR(dr, dc) == 0) { dr0 = dr; dc0 = dc; }
    std::printf("diamond:  4-green diamond centred on R sites at (%d,%d)\n",
                dr0, dc0);
    for (uint32_t r = 0; r < outH; r++) {
      for (uint32_t c = 0; c < outW; c++) {
        const int rr = 2 * (int)r + dr0, cc = 2 * (int)c + dc0;
        // Mirror at the border rather than clamp: r-1 and r+1 share a parity,
        // so the substitute site is still green.
        const int rm = (rr - 1 >= 0) ? rr - 1 : rr + 1;
        const int rp = (rr + 1 < H) ? rr + 1 : rr - 1;
        const int cm = (cc - 1 >= 0) ? cc - 1 : cc + 1;
        const int cp = (cc + 1 < W) ? cc + 1 : cc - 1;
        const float v = (rawAt(rm, cc) - blackFor(rm, cc, lr.COLOR(rm, cc)) +
                         rawAt(rp, cc) - blackFor(rp, cc, lr.COLOR(rp, cc)) +
                         rawAt(rr, cm) - blackFor(rr, cm, lr.COLOR(rr, cm)) +
                         rawAt(rr, cp) - blackFor(rr, cp, lr.COLOR(rr, cp))) *
                        0.25f;
        out[(size_t)r * outW + c] = clamp16(v * scale);
      }
    }
  } else {  // bin
    outW = W / 2;
    outH = H / 2;
    out.resize((size_t)outW * outH);
    const bool luma = (weights == "luma");
    for (uint32_t r = 0; r < outH; r++) {
      for (uint32_t c = 0; c < outW; c++) {
        float sumG = 0, sumOther = 0;
        int nG = 0;
        for (int dr = 0; dr < 2; dr++) {
          for (int dc = 0; dc < 2; dc++) {
            int rr = 2 * r + dr, cc = 2 * c + dc;
            int ch = lr.COLOR(rr, cc);
            float v = rawAt(rr, cc) - blackFor(rr, cc, ch);
            if (ch == 1 || ch == 3) {
              sumG += v;
              nG++;
            } else {
              // Equalize R/B to green and clip to green's saturation so a
              // blown highlight can't lift the average above white.
              sumOther += std::min(v * gains[ch], sat);
            }
          }
        }
        float v = luma ? (sumG + sumOther) / 4.0f
                       : (nG ? sumG / nG : 0.0f);
        out[(size_t)r * outW + c] = clamp16(v * scale);
      }
    }
  }

  MonoDngMeta meta;
  meta.make = P.make;
  meta.model = P.model;
  meta.uniqueModel = std::string(P.make) + " " + P.model +
                     " (rwmono " + mode + (mode == "bin" ? "/" + weights : "") +
                     (diamond ? "/diamond" : "") + (derotate ? "/derot" : "") + ")";
  meta.software = "rwmono 0.1";
  meta.dateTime = formatTime(lr.imgdata.other.timestamp);
  meta.orientation = flipToOrientation(S.flip);
  meta.whiteLevel = 65535;
  meta.iso = lr.imgdata.other.iso_speed;
  meta.exposureTime = lr.imgdata.other.shutter;
  meta.fnumber = lr.imgdata.other.aperture;
  meta.focalLength = lr.imgdata.other.focal_len;
  meta.losslessJpeg = compress;
  if (cropSet) {
    meta.hasDefaultCrop = true;
    meta.cropOrigin[0] = cropO[0];
    meta.cropOrigin[1] = cropO[1];
    meta.cropSize[0] = cropS[0];
    meta.cropSize[1] = cropS[1];
  }

  std::string err;
  if (!writeMonoDng(output, out.data(), outW, outH, meta, err)) {
    std::fprintf(stderr, "error: %s\n", err.c_str());
    return 1;
  }

  uint16_t mn = 65535, mx = 0;
  double mean = 0;
  for (uint16_t v : out) {
    mn = std::min(mn, v);
    mx = std::max(mx, v);
    mean += v;
  }
  mean /= (double)out.size();
  std::printf("output:   %s\n", output.c_str());
  std::printf("          %u x %u, 16-bit mono, min %u  max %u  mean %.1f\n",
              outW, outH, mn, mx, mean);
  if (compress) {
    FILE* f = std::fopen(output.c_str(), "rb");
    if (f) {
      std::fseek(f, 0, SEEK_END);
      long sz = std::ftell(f);
      std::fclose(f);
      std::printf("          lossless JPEG: %.1f MB (%.0f%% of uncompressed)\n",
                  sz / 1048576.0,
                  100.0 * sz / ((double)outW * outH * 2));
    }
  }
  if (cropSet)
    std::printf("          DefaultCrop: %u,%u + %ux%u\n", cropO[0], cropO[1],
                cropS[0], cropS[1]);
  return 0;
}
