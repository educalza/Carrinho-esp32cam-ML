#pragma once
#include <stdint.h>
#include <math.h>
#include "autonomous_config.h"

inline float throttleForSteer(float steer) {
    if (!isfinite(steer)) return 0.0f;
    const float magnitude = fabsf(steer);
    const float base = magnitude >= 0.30f ? SPEED_HARD_CURVE :
                       magnitude >= 0.12f ? SPEED_SOFT_CURVE : SPEED_STRAIGHT;
    const float throttle = base * SPEED_SCALE;
    return throttle < 0 ? 0 : (throttle > 1 ? 1 : throttle);
}

// Every enabled command follows the current prediction; no line detector or recovery state.
struct DriveCommand { float steer; float throttle; };
inline DriveCommand commandForModel(float prediction, bool enabled) {
    if (!enabled || !isfinite(prediction) || prediction < -1.001f || prediction > 1.001f)
        return {0, 0};
    const float steer=prediction < -1 ? -1 : (prediction > 1 ? 1 : prediction);
    return {steer, throttleForSteer(steer)};
}

class DriveGate {
public:
    explicit DriveGate(bool autoArm = AUTO_ARM_ON_BOOT) : autoArm_(autoArm) {}
    void observe(bool valid) {
        if (!valid) {
            if (armed_) autoArm_ = false;
            armed_ = false;
            goodFrames_ = 0;
            return;
        }
        if (goodFrames_ < MODEL_READY_FRAMES) ++goodFrames_;
        if (autoArm_ && ready()) {
            armed_ = true;
            autoArm_ = false;
        }
    }
    bool start() {
        if (!ready()) return false;
        autoArm_ = false;
        armed_ = true;
        return true;
    }
    void stop() {
        autoArm_ = false; // Operator STOP and hard faults remain latched.
        armed_ = false;
        goodFrames_ = 0;
    }
    // No movement while paused. Only an already requested run can recover.
    void pause() {
        autoArm_ = autoArm_ || armed_;
        armed_ = false;
        goodFrames_ = 0;
    }
    bool pending() const { return autoArm_ && !armed_; }
    bool ready() const { return goodFrames_ >= MODEL_READY_FRAMES; }
    bool armed() const { return armed_; }
private:
    bool autoArm_;
    bool armed_ = false;
    unsigned goodFrames_ = 0;
};
