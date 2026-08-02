#pragma once

#include <cstdint>
#include <vector>

// Encodes a 16-bit single-component image as a lossless JPEG stream
// (ITU T.81 process 14, SOF3), the compression DNG specifies for
// integer raw data (TIFF Compression = 7). Predictor 1 (left neighbor),
// no point transform, one optimal Huffman table built per image.
std::vector<uint8_t> encodeLosslessJpeg(const uint16_t* img,
                                        uint32_t width, uint32_t height);
