#pragma once

#include <cstdint>
#include <string>

// Metadata carried into the output DNG. All fields optional; empty/zero
// fields are simply omitted from the file.
struct MonoDngMeta {
  std::string make;
  std::string model;
  std::string uniqueModel;   // DNG UniqueCameraModel
  std::string software;
  std::string dateTime;      // "YYYY:MM:DD HH:MM:SS", used for DateTime + DateTimeOriginal
  int orientation = 1;       // TIFF orientation code
  uint32_t whiteLevel = 65535;
  bool losslessJpeg = true;  // Compression 7 (SOF3) vs 1 (none)
  bool hasDefaultCrop = false;
  uint32_t cropOrigin[2] = {0, 0};  // x, y
  uint32_t cropSize[2] = {0, 0};    // w, h
  float iso = 0.0f;
  float exposureTime = 0.0f; // seconds
  float fnumber = 0.0f;
  float focalLength = 0.0f;  // mm
};

// Writes a monochrome linear-raw DNG: 16-bit, single sample per pixel,
// PhotometricInterpretation = LinearRaw, no CFA tags. Per the DNG spec,
// ColorMatrix1 is required only for non-monochrome files, so it is omitted.
// Returns false and sets `err` on failure.
bool writeMonoDng(const std::string& path, const uint16_t* pixels,
                  uint32_t width, uint32_t height, const MonoDngMeta& meta,
                  std::string& err);
