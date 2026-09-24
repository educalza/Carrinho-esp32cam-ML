#include <assert.h>
#include "../drive_history.h"

int main() {
    DriveHistory history;
    DriveSample selected = {};
    assert(!history.sampleAt(1000000, selected));
    history.push({-0.5f, 0.8f}, 1000000);
    history.push({0.5f, 0.6f}, 1100000);
    assert(!history.sampleAt(999999, selected));
    assert(history.sampleAt(1050000, selected));
    assert(selected.drive.steer == -0.5f);
    assert(selected.timestampUs == 1000000);
    assert(history.sampleAt(1100000, selected));
    assert(selected.drive.steer == 0.5f);
    assert(history.sampleAt(1400000, selected));
    assert(!history.sampleAt(1400001, selected));

    // Encher o buffer elimina os comandos antigos, sem usar um comando futuro.
    for (size_t i = 0; i < DriveHistory::CAPACITY + 5; ++i) {
        history.push({static_cast<float>(i), 0.4f}, 2000000 + i * 10000);
    }
    assert(!history.sampleAt(2049999, selected));
    assert(history.sampleAt(2050000, selected));
    assert(selected.drive.steer == 5.0f);
    assert(history.sampleAt(2655000, selected));
    assert(selected.drive.steer == 65.0f);

    // Mantem precisao e ordem depois do rollover de millis() (49 dias).
    const uint64_t longUptime = (UINT64_C(1) << 32) * 1000 + 1000000;
    history.push({0.2f, 0.3f}, longUptime);
    assert(history.sampleAt(longUptime + 250000, selected));
    assert(selected.timestampUs == longUptime);
    assert(!history.sampleAt(longUptime + 300001, selected));
    return 0;
}
