#pragma once
#include <stdint.h>

namespace car_ml {
constexpr int kCameraWidth = 320;
constexpr int kCameraHeight = 240;
constexpr int kInputWidth = 96;
constexpr int kInputHeight = 96;
constexpr int kInputSize = kInputWidth * kInputHeight;

// Keep hot lookup data together in internal DRAM (static instance in the sketch).
struct alignas(16) PreprocessingTables {
    int8_t pixel[256];
    uint16_t x[kInputWidth];
    uint32_t row[kInputHeight]; // Byte offsets exceed uint16_t near the bottom.
};

bool preparePreprocessing(PreprocessingTables& tables, float scale, int32_t zeroPoint);
// Caller validates the QVGA grayscale frame and the INT8 destination before calling.
// No allocation or intermediate image; source and destination must not overlap.
void preprocess(const uint8_t* camera, int8_t* destination, const PreprocessingTables& tables);
}
