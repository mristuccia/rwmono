#include "dng_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ljpeg_encoder.h"

namespace {

enum TiffType : uint16_t {
  T_BYTE = 1,
  T_ASCII = 2,
  T_SHORT = 3,
  T_LONG = 4,
  T_RATIONAL = 5,
  T_UNDEFINED = 7,
  T_SRATIONAL = 10,
};

size_t typeSize(uint16_t type) {
  switch (type) {
    case T_BYTE: case T_ASCII: case T_UNDEFINED: return 1;
    case T_SHORT: return 2;
    case T_LONG: return 4;
    case T_RATIONAL: case T_SRATIONAL: return 8;
  }
  return 1;
}

struct Entry {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  std::vector<uint8_t> data;
};

void putU16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(x & 0xff);
  v.push_back(x >> 8);
}

void putU32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
  v.push_back((x >> 16) & 0xff);
  v.push_back((x >> 24) & 0xff);
}

// A little-endian TIFF IFD whose serialized size is fixed once the entry set
// is fixed, so offsets can be laid out before values are final.
class Ifd {
 public:
  void addShort(uint16_t tag, uint16_t value) {
    Entry e{tag, T_SHORT, 1, {}};
    putU16(e.data, value);
    add(std::move(e));
  }
  void addLong(uint16_t tag, uint32_t value) {
    Entry e{tag, T_LONG, 1, {}};
    putU32(e.data, value);
    add(std::move(e));
  }
  void addAscii(uint16_t tag, const std::string& s) {
    Entry e{tag, T_ASCII, (uint32_t)s.size() + 1, {}};
    e.data.assign(s.begin(), s.end());
    e.data.push_back(0);
    add(std::move(e));
  }
  void addBytes(uint16_t tag, uint16_t type, const uint8_t* p, uint32_t count) {
    Entry e{tag, type, count, {}};
    e.data.assign(p, p + count * typeSize(type));
    add(std::move(e));
  }
  void addLongs(uint16_t tag, const uint32_t* v, uint32_t count) {
    Entry e{tag, T_LONG, count, {}};
    for (uint32_t i = 0; i < count; i++) putU32(e.data, v[i]);
    add(std::move(e));
  }
  void addRational(uint16_t tag, uint32_t num, uint32_t den) {
    Entry e{tag, T_RATIONAL, 1, {}};
    putU32(e.data, num);
    putU32(e.data, den);
    add(std::move(e));
  }
  void setLong(uint16_t tag, uint32_t value) {
    for (auto& e : entries_)
      if (e.tag == tag) {
        e.data.clear();
        putU32(e.data, value);
        return;
      }
  }

  // 2 (count) + 12 per entry + 4 (next-IFD pointer) + out-of-line value heap.
  size_t byteSize() const {
    size_t heap = 0;
    for (const auto& e : entries_)
      if (e.data.size() > 4) heap += (e.data.size() + 1) & ~size_t(1);
    return 2 + entries_.size() * 12 + 4 + heap;
  }

  std::vector<uint8_t> serialize(uint32_t baseOffset) const {
    std::vector<Entry> sorted = entries_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Entry& a, const Entry& b) { return a.tag < b.tag; });

    uint32_t heapAt = baseOffset + 2 + (uint32_t)sorted.size() * 12 + 4;
    std::vector<uint8_t> out, heap;
    putU16(out, (uint16_t)sorted.size());
    for (const auto& e : sorted) {
      putU16(out, e.tag);
      putU16(out, e.type);
      putU32(out, e.count);
      if (e.data.size() <= 4) {
        std::vector<uint8_t> v = e.data;
        v.resize(4, 0);
        out.insert(out.end(), v.begin(), v.end());
      } else {
        putU32(out, heapAt + (uint32_t)heap.size());
        heap.insert(heap.end(), e.data.begin(), e.data.end());
        if (heap.size() & 1) heap.push_back(0);
      }
    }
    putU32(out, 0);  // no next IFD
    out.insert(out.end(), heap.begin(), heap.end());
    return out;
  }

 private:
  void add(Entry e) { entries_.push_back(std::move(e)); }
  std::vector<Entry> entries_;
};

void floatToRational(float x, uint32_t& num, uint32_t& den) {
  if (x <= 0) { num = 0; den = 1; return; }
  if (x < 1.0f) {
    num = 1;
    den = (uint32_t)std::lround(1.0 / x);
  } else {
    num = (uint32_t)std::lround(x * 100.0);
    den = 100;
  }
}

}  // namespace

bool writeMonoDng(const std::string& path, const uint16_t* pixels,
                  uint32_t width, uint32_t height, const MonoDngMeta& meta,
                  std::string& err) {
  std::vector<uint8_t> encoded;
  if (meta.losslessJpeg) encoded = encodeLosslessJpeg(pixels, width, height);
  const size_t dataBytes =
      meta.losslessJpeg ? encoded.size() : (size_t)width * height * 2;

  Ifd exif;
  {
    static const uint8_t exifVersion[4] = {'0', '2', '3', '0'};
    exif.addBytes(0x9000, T_UNDEFINED, exifVersion, 4);
    if (meta.exposureTime > 0) {
      uint32_t n, d;
      floatToRational(meta.exposureTime, n, d);
      exif.addRational(0x829A, n, d);
    }
    if (meta.fnumber > 0)
      exif.addRational(0x829D, (uint32_t)std::lround(meta.fnumber * 100), 100);
    if (meta.iso > 0) exif.addShort(0x8827, (uint16_t)std::lround(meta.iso));
    if (!meta.dateTime.empty()) exif.addAscii(0x9003, meta.dateTime);
    if (meta.focalLength > 0)
      exif.addRational(0x920A, (uint32_t)std::lround(meta.focalLength * 100), 100);
  }

  Ifd ifd0;
  ifd0.addLong(254, 0);  // NewSubfileType: main image
  ifd0.addLong(256, width);
  ifd0.addLong(257, height);
  ifd0.addShort(258, 16);  // BitsPerSample
  ifd0.addShort(259, meta.losslessJpeg ? 7 : 1);  // 7 = lossless JPEG
  ifd0.addShort(262, 34892);  // PhotometricInterpretation: LinearRaw
  if (!meta.make.empty()) ifd0.addAscii(271, meta.make);
  if (!meta.model.empty()) ifd0.addAscii(272, meta.model);
  ifd0.addLong(273, 0);  // StripOffsets, patched below
  ifd0.addShort(274, (uint16_t)meta.orientation);
  ifd0.addShort(277, 1);  // SamplesPerPixel
  ifd0.addLong(278, height);  // RowsPerStrip: single strip
  ifd0.addLong(279, (uint32_t)dataBytes);
  ifd0.addRational(282, 300, 1);
  ifd0.addRational(283, 300, 1);
  ifd0.addShort(296, 2);  // ResolutionUnit: inch
  if (!meta.software.empty()) ifd0.addAscii(305, meta.software);
  if (!meta.dateTime.empty()) ifd0.addAscii(306, meta.dateTime);
  ifd0.addLong(34665, 0);  // ExifIFD pointer, patched below
  static const uint8_t dngVersion[4] = {1, 4, 0, 0};
  static const uint8_t dngBackward[4] = {1, 2, 0, 0};
  ifd0.addBytes(50706, T_BYTE, dngVersion, 4);
  ifd0.addBytes(50707, T_BYTE, dngBackward, 4);
  if (!meta.uniqueModel.empty()) ifd0.addAscii(50708, meta.uniqueModel);
  ifd0.addShort(50714, 0);  // BlackLevel
  ifd0.addLong(50717, meta.whiteLevel);
  if (meta.hasDefaultCrop) {
    ifd0.addLongs(50719, meta.cropOrigin, 2);
    ifd0.addLongs(50720, meta.cropSize, 2);
  }

  const uint32_t ifd0At = 8;
  const uint32_t exifAt = ifd0At + (uint32_t)ifd0.byteSize();
  uint32_t dataAt = exifAt + (uint32_t)exif.byteSize();
  dataAt = (dataAt + 1) & ~1u;
  ifd0.setLong(273, dataAt);
  ifd0.setLong(34665, exifAt);

  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    err = "cannot open " + path + " for writing";
    return false;
  }

  std::vector<uint8_t> head;
  head.push_back('I');
  head.push_back('I');
  putU16(head, 42);
  putU32(head, ifd0At);
  auto b0 = ifd0.serialize(ifd0At);
  auto b1 = exif.serialize(exifAt);
  head.insert(head.end(), b0.begin(), b0.end());
  head.insert(head.end(), b1.begin(), b1.end());
  head.resize(dataAt, 0);

  const void* data = meta.losslessJpeg ? (const void*)encoded.data()
                                       : (const void*)pixels;
  bool ok = std::fwrite(head.data(), 1, head.size(), f) == head.size() &&
            std::fwrite(data, 1, dataBytes, f) == dataBytes;
  ok = std::fclose(f) == 0 && ok;
  if (!ok) err = "write failed for " + path;
  return ok;
}
