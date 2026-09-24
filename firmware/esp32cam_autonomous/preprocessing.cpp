#include "preprocessing.h"
#include "performance_options.h"
#include <math.h>

#if defined(ESP32)
#include "esp_attr.h"
#define CAR_PREPROCESS_CODE IRAM_ATTR
#else
#define CAR_PREPROCESS_CODE
#endif

namespace car_ml {
bool preparePreprocessing(PreprocessingTables& tables, float scale, int32_t zeroPoint) {
    if (!isfinite(scale) || scale <= 0 || zeroPoint < -128 || zeroPoint > 127) return false;
    for (int i = 0; i < 256; ++i) {
        const float normalized = float(i) / 255.0f;
        const float scaled = normalized / scale;
        // Values >=255 always saturate for an INT8 zero point. Avoid lround overflow.
        const int32_t q = scaled >= 255.0f ? 127 : int32_t(lroundf(scaled)) + zeroPoint;
        tables.pixel[i] = int8_t(q < -128 ? -128 : (q > 127 ? 127 : q));
    }
    for (int x = 0; x < kInputWidth; ++x) tables.x[x] = x * kCameraWidth / kInputWidth;
    for (int y = 0; y < kInputHeight; ++y)
        tables.row[y] = (y * kCameraHeight / kInputHeight) * kCameraWidth;
    return true;
}

// Small hot loop in IRAM: camera PSRAM reads no longer evict this code from flash cache.
// Preserve the training pipeline's integer 2x2 average, coordinates and quantization.
void CAR_ML_FAST_CODE CAR_PREPROCESS_CODE preprocess(const uint8_t* camera, int8_t* destination,
                                    const PreprocessingTables& tables) {
    for (int y = 0; y < kInputHeight; ++y) {
        const uint8_t* row0 = camera + tables.row[y];
        const uint8_t* row1 = row0 + kCameraWidth;
        for (int x = 0; x < kInputWidth; ++x) {
            const unsigned sx = tables.x[x];
            const unsigned sum = unsigned(row0[sx]) + row0[sx + 1] + row1[sx] + row1[sx + 1];
            *destination++ = tables.pixel[sum >> 2];
        }
    }
}
}
