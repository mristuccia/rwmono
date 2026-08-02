#include "ljpeg_encoder.h"

#include <array>
#include <cstring>

namespace {

// Predictor-1 differences, modulo 2^16 as the spec requires. Per T.81
// H.1.2.1: sample (0,0) is predicted from 2^(P-1); the rest of the first
// line from the left neighbor; the first sample of every other line from
// the sample above; everything else from the selected predictor (left).
template <class F>
void forEachDiff(const uint16_t* img, uint32_t w, uint32_t h, F&& emit) {
  for (uint32_t r = 0; r < h; r++) {
    const uint16_t* row = img + (size_t)r * w;
    const uint16_t* above = row - w;
    for (uint32_t c = 0; c < w; c++) {
      int pred;
      if (c > 0) pred = row[c - 1];
      else if (r > 0) pred = above[0];
      else pred = 32768;
      emit((int16_t)((row[c] - pred) & 0xFFFF));
    }
  }
}

inline int category(int diff) {
  if (diff == -32768) return 16;
  unsigned a = diff < 0 ? -diff : diff;
  return a ? 32 - __builtin_clz(a) : 0;
}

struct HuffTable {
  uint8_t bits[17] = {};     // bits[l] = number of codes of length l
  std::vector<uint8_t> vals; // symbols in code order
  uint16_t code[17] = {};    // per symbol (SSSS 0..16)
  uint8_t len[17] = {};
};

// Optimal code lengths per T.81 Annex K.2, with the reserved extra symbol
// (index 256) that keeps the all-ones code unused, then length-limiting to
// 16 bits and canonical code assignment per Annex C.
HuffTable buildHuffman(const uint64_t freqIn[17]) {
  std::array<uint64_t, 257> freq{};
  std::array<int, 257> others;
  std::array<int, 257> codesize{};
  others.fill(-1);
  for (int i = 0; i < 17; i++) freq[i] = freqIn[i];
  freq[256] = 1;

  while (true) {
    int v1 = -1, v2 = -1;
    uint64_t f1 = UINT64_MAX, f2 = UINT64_MAX;
    for (int i = 0; i < 257; i++)
      if (freq[i] && freq[i] <= f1) { f1 = freq[i]; v1 = i; }
    for (int i = 0; i < 257; i++)
      if (freq[i] && i != v1 && freq[i] <= f2) { f2 = freq[i]; v2 = i; }
    if (v2 < 0) break;

    freq[v1] += freq[v2];
    freq[v2] = 0;
    codesize[v1]++;
    while (others[v1] >= 0) { v1 = others[v1]; codesize[v1]++; }
    others[v1] = v2;
    codesize[v2]++;
    while (others[v2] >= 0) { v2 = others[v2]; codesize[v2]++; }
  }

  std::array<int, 34> counts{};
  for (int i = 0; i < 257; i++)
    if (codesize[i]) counts[codesize[i]]++;

  for (int i = 33; i > 16; i--) {  // Annex K "Adjust_BITS"
    while (counts[i] > 0) {
      int j = i - 2;
      while (counts[j] == 0) j--;
      counts[i] -= 2;
      counts[i - 1] += 1;
      counts[j + 1] += 2;
      counts[j] -= 1;
    }
  }
  {
    int i = 16;
    while (counts[i] == 0) i--;
    counts[i]--;  // drop the reserved symbol's code point
  }

  HuffTable t;
  for (int l = 1; l <= 16; l++) t.bits[l] = (uint8_t)counts[l];
  for (int l = 1; l <= 16; l++)       // symbols sorted by (length, value)
    for (int s = 0; s < 17; s++)
      if (codesize[s] == l) t.vals.push_back((uint8_t)s);

  uint16_t code = 0;
  size_t k = 0;
  for (int l = 1; l <= 16; l++) {
    for (int n = 0; n < t.bits[l]; n++, k++) {
      t.code[t.vals[k]] = code++;
      t.len[t.vals[k]] = (uint8_t)l;
    }
    code <<= 1;
  }
  return t;
}

class BitWriter {
 public:
  explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
  void put(uint32_t bits, int n) {
    if (!n) return;
    acc_ = (acc_ << n) | (bits & ((1u << n) - 1));
    nbits_ += n;
    while (nbits_ >= 8) {
      uint8_t b = (uint8_t)(acc_ >> (nbits_ - 8));
      out_.push_back(b);
      if (b == 0xFF) out_.push_back(0);  // marker stuffing
      nbits_ -= 8;
    }
  }
  void flush() {  // pad the final byte with 1s
    if (nbits_) put((1u << (8 - nbits_)) - 1, 8 - nbits_);
  }

 private:
  std::vector<uint8_t>& out_;
  uint32_t acc_ = 0;
  int nbits_ = 0;
};

void marker(std::vector<uint8_t>& out, uint8_t m) {
  out.push_back(0xFF);
  out.push_back(m);
}

void segment(std::vector<uint8_t>& out, uint8_t m, const std::vector<uint8_t>& payload) {
  marker(out, m);
  uint16_t len = (uint16_t)(payload.size() + 2);
  out.push_back(len >> 8);
  out.push_back(len & 0xFF);
  out.insert(out.end(), payload.begin(), payload.end());
}

}  // namespace

std::vector<uint8_t> encodeLosslessJpeg(const uint16_t* img,
                                        uint32_t width, uint32_t height) {
  uint64_t freq[17] = {};
  forEachDiff(img, width, height, [&](int diff) { freq[category(diff)]++; });
  HuffTable t = buildHuffman(freq);

  std::vector<uint8_t> out;
  out.reserve((size_t)width * height);  // ~50% of raw is typical

  marker(out, 0xD8);  // SOI

  std::vector<uint8_t> dht;
  dht.push_back(0x00);  // DC table, id 0
  for (int l = 1; l <= 16; l++) dht.push_back(t.bits[l]);
  dht.insert(dht.end(), t.vals.begin(), t.vals.end());
  segment(out, 0xC4, dht);

  std::vector<uint8_t> sof = {
      16,                                      // precision
      (uint8_t)(height >> 8), (uint8_t)height,
      (uint8_t)(width >> 8),  (uint8_t)width,
      1,                                       // components
      1, 0x11, 0,                              // id, 1x1 sampling, quant 0
  };
  segment(out, 0xC3, sof);  // SOF3: lossless

  std::vector<uint8_t> sos = {
      1,     // components in scan
      1, 0,  // component 1, DC table 0
      1,     // Ss: predictor 1
      0, 0,  // Se, Ah/Al (no point transform)
  };
  segment(out, 0xDA, sos);

  BitWriter bw(out);
  forEachDiff(img, width, height, [&](int diff) {
    int s = category(diff);
    bw.put(t.code[s], t.len[s]);
    if (s && s < 16) bw.put((uint32_t)(diff < 0 ? diff - 1 : diff), s);
  });
  bw.flush();

  marker(out, 0xD9);  // EOI
  return out;
}
