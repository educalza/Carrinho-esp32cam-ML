#pragma once

// Apply O2 only to our hot routines. Global O2 triggers a compiler ICE in the
// installed TFLM unpack.cpp with the ESP32 Core 3.3.11 toolchain.
// Set to 0 for A/B comparison; no global library or compiler settings are changed.
#ifndef CAR_ML_OPTIMIZE_HOT_PATHS
#define CAR_ML_OPTIMIZE_HOT_PATHS 1
#endif

#if CAR_ML_OPTIMIZE_HOT_PATHS && defined(__GNUC__) && !defined(__clang__)
#define CAR_ML_FAST_CODE __attribute__((optimize("O2")))
#else
#define CAR_ML_FAST_CODE
#endif
