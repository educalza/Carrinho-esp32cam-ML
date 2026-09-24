#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <vector>
#include "../firmware/esp32cam_autonomous/preprocessing.h"

// Independent scalar reference of the original firmware/training resize contract.
static int8_t reference(const uint8_t* image, int x, int y, float scale, int zero) {
    const int sx = x * 320 / 96, sy = y * 240 / 96;
    const unsigned sum = unsigned(image[sy*320+sx]) + image[sy*320+sx+1] +
                         image[(sy+1)*320+sx] + image[(sy+1)*320+sx+1];
    const float normalized = float(sum / 4) / 255.0f;
    const int q = int(std::lround(normalized / scale)) + zero;
    return int8_t(std::max(-128, std::min(127, q)));
}

int main() {
    car_ml::PreprocessingTables tables;
    assert(!car_ml::preparePreprocessing(tables, 0, -128));
    assert(!car_ml::preparePreprocessing(tables, -1, -128));
    assert(!car_ml::preparePreprocessing(tables, std::numeric_limits<float>::quiet_NaN(), -128));
    assert(!car_ml::preparePreprocessing(tables, std::numeric_limits<float>::infinity(), -128));
    assert(!car_ml::preparePreprocessing(tables, 1.f/255, -129));
    assert(!car_ml::preparePreprocessing(tables, 1.f/255, 128));

    // Include unaligned camera memory and sentinels around both buffers.
    std::vector<uint8_t> source(320*240+2, 0x5a);
    std::array<int8_t, 96*96+2> destination;
    uint8_t* frame = source.data()+1;
    std::mt19937 random(2041);
    unsigned frames = 0;
    for (float scale : {1.f/255, 1.f/128, 1.f/17, .01f}) for (int zero : {-128, -64, 0, 127}) {
        assert(car_ml::preparePreprocessing(tables, scale, zero));
        // All gray levels plus gradients, checkerboards, impulses, stripes and random images.
        for (int pattern=0; pattern<280; ++pattern) {
            for (int y=0; y<240; ++y) for (int x=0; x<320; ++x) {
                frame[y*320+x] = pattern<256 ? pattern :
                    pattern==256 ? x%256 : pattern==257 ? y :
                    pattern==258 ? ((x+y)%2)*255 : pattern==259 ? (x%7==0)*255 :
                    pattern==260 ? ((x==317 && y==238) ? 255 : 0) : random()%256;
            }
            destination.fill(0x55);
            car_ml::preprocess(frame, destination.data()+1, tables);
            for (int y=0; y<96; ++y) for (int x=0; x<96; ++x)
                assert(destination[1+y*96+x] == reference(frame,x,y,scale,zero));
            assert(destination.front()==0x55 && destination.back()==0x55);
            assert(source.front()==0x5a && source.back()==0x5a);
            ++frames;
        }
    }
    // Tiny but positive quantization scales must saturate without rounding overflow.
    assert(car_ml::preparePreprocessing(tables, std::numeric_limits<float>::min(), -128));
    assert(tables.pixel[0]==-128 && tables.pixel[1]==127 && tables.pixel[255]==127);
    std::printf("Preprocessing: %u frames, %u INT8 pixels identical to reference\n", frames, frames*96*96);
}
