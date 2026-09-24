#pragma once
#include <stdint.h>

namespace car_camera {
struct TimingWindow {
    uint32_t frames=0, gaps=0, failures=0, invalid=0, late=0;
    int64_t getUs=0, startRelativeUs=0, deliveryAgeUs=0, frameGapUs=0, maxDeliveryAgeUs=0;
};
class Timing {
public:
    bool observe(int64_t request, int64_t received, int64_t captured) {
        if (request<0 || received<request || captured<=0 || captured>received ||
            (previousCapture_ && captured<=previousCapture_)) { ++window_.invalid; return false; }
        ++window_.frames;
        window_.getUs+=received-request;
        window_.startRelativeUs+=captured-request; // Signed: a frame can start before the request.
        const int64_t age=received-captured;
        window_.deliveryAgeUs+=age;
        if(age>window_.maxDeliveryAgeUs) window_.maxDeliveryAgeUs=age;
        if(previousCapture_) { ++window_.gaps; window_.frameGapUs+=captured-previousCapture_; }
        previousCapture_=captured;
        return true;
    }
    void failure() { ++window_.failures; }
    void invalidFrame() { ++window_.invalid; }
    void lateFrame() { ++window_.late; }
    const TimingWindow& window() const { return window_; }
    void clearWindow() { window_=TimingWindow(); } // Keep interval across reporting windows.
private:
    int64_t previousCapture_=0;
    TimingWindow window_;
};
}
