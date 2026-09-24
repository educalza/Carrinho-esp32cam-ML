#pragma once
#include <stddef.h>
#include <stdint.h>
#include "tensorflow/lite/micro/kernels/conv.h"

namespace car_ml {
TFLMRegistration RegisterOptimizedConv();
// Verification is used only before driving; scratch may be reused afterwards.
void SetConvVerification(uint8_t* scratch, size_t size);
unsigned OptimizedConvCalls();
unsigned ReferenceConvCalls();
}
