#ifndef DRIVE_HISTORY_H
#define DRIVE_HISTORY_H

#include <stddef.h>
#include <stdint.h>

struct DriveState {
    float steer;     // -1.0 (esquerda) a +1.0 (direita)
    float throttle;  // -1.0 (re) a +1.0 (frente)
};

struct DriveSample {
    DriveState drive;
    uint64_t timestampUs;
};

// Mesmo relogio do camera_fb_t::timestamp: microssegundos desde o boot.
// A sincronizacao entre tarefas pertence ao chamador, nunca ao SD.
class DriveHistory {
public:
    static constexpr size_t CAPACITY = 64;
    static constexpr uint64_t MAX_COMMAND_AGE_US = 300000;

    void push(const DriveState &drive, uint64_t timestampUs) {
        samples_[next_] = {drive, timestampUs};
        next_ = (next_ + 1) % CAPACITY;
        if (count_ < CAPACITY) ++count_;
    }

    DriveState latest() const {
        if (count_ == 0) return {0.0f, 0.0f};
        return samples_[(next_ + CAPACITY - 1) % CAPACITY].drive;
    }

    // Ignora comandos recebidos depois do inicio da captura. Se o
    // ultimo comando anterior ja expirou, nenhum mais antigo e valido.
    bool sampleAt(uint64_t captureUs, DriveSample &sample) const {
        for (size_t offset = 0; offset < count_; ++offset) {
            const auto &candidate = samples_[
                (next_ + CAPACITY - 1 - offset) % CAPACITY];
            if (candidate.timestampUs > captureUs) continue;
            if (captureUs - candidate.timestampUs > MAX_COMMAND_AGE_US) {
                return false;
            }
            sample = candidate;
            return true;
        }
        return false;
    }

private:
    DriveSample samples_[CAPACITY] = {};
    size_t next_ = 0;
    size_t count_ = 0;
};

#endif
