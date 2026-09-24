#include <assert.h>
#include <limits>
#include <initializer_list>
#include <cstdio>
#include "../firmware/esp32cam_autonomous/driving_policy.h"
#include "../firmware/esp32_motor_controller/control_protocol.h"
#include "../firmware/esp32_motor_controller/motor_config.h"

int main() {
    static_assert(MAX_DECISION_AGE_US < FAILSAFE_TIMEOUT_MS * 1000LL,
                  "Decision deadline must leave margin before motor watchdog");
    // Abrupt direction changes follow the current model, without retained recovery steering.
    for (float prediction : {1.0f, -.75f, 0.0f, .2f, -1.0f, .85f}) {
        const auto command=commandForModel(prediction,true);
        assert(command.steer==prediction && command.throttle>0);
        assert(command.throttle==throttleForSteer(prediction));
        char wire[40];
        std::snprintf(wire,sizeof(wire),"S%.3fT%.3f\n",command.steer,command.throttle);
        motor_control::CommandReceiver receiver;
        float steer=0, throttle=0;
        bool received=false;
        for (const char* c=wire; *c; ++c) received=receiver.feed(*c,steer,throttle) || received;
        assert(received && fabsf(steer-prediction)<.0006f);
        assert(fabsf(throttle-command.throttle)<.0006f);
        const auto disabled=commandForModel(prediction,false);
        assert(disabled.steer==0 && disabled.throttle==0);
    }
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), -1.1f, 1.1f}) {
        const auto command=commandForModel(invalid,true);
        assert(command.steer==0 && command.throttle==0);
    }
    assert(commandForModel(1.0005f,true).steer==1);
    assert(commandForModel(-1.0005f,true).steer==-1);
    DriveGate gate(false);
    assert(!gate.start());
    for (unsigned i=0;i<MODEL_READY_FRAMES;++i) gate.observe(true);
    assert(gate.ready() && !gate.armed() && gate.start());
    gate.observe(false);
    assert(!gate.armed());
    for (unsigned i=0;i<MODEL_READY_FRAMES;++i) gate.observe(true);
    assert(!gate.armed()); // Nenhuma retomada automatica apos decisao invalida.
    assert(gate.start());
    gate.stop();
    assert(!gate.armed() && !gate.start());
    DriveGate automatic(true);
    assert(!automatic.armed());
    automatic.observe(false);
    for (unsigned i=0;i<MODEL_READY_FRAMES;++i) automatic.observe(true);
    assert(automatic.armed());
    automatic.observe(false);
    for (unsigned i=0;i<MODEL_READY_FRAMES;++i) automatic.observe(true);
    assert(!automatic.armed());
    DriveGate stoppedBeforeBoot(true);
    stoppedBeforeBoot.stop();
    for (unsigned i=0; i<MODEL_READY_FRAMES; ++i) stoppedBeforeBoot.observe(true);
    assert(!stoppedBeforeBoot.armed());
    // Battery operation: temporary timing/capture failures pause immediately,
    // and require a fresh consecutive sequence before any motion resumes.
    DriveGate battery(true);
    battery.pause(); // A slow first capture must not cancel battery startup.
    assert(battery.pending() && !battery.armed());
    for (unsigned i=1; i<MODEL_READY_FRAMES; ++i) {
        battery.observe(true);
        assert(!battery.armed());
    }
    battery.observe(true);
    assert(battery.armed());
    battery.pause();
    assert(!battery.armed());
    battery.observe(true);
    battery.pause(); // An intermittent miss restarts the readiness sequence.
    for (unsigned i=1; i<MODEL_READY_FRAMES; ++i) {
        battery.observe(true);
        assert(!battery.armed());
    }
    battery.observe(true);
    assert(battery.armed());
    battery.pause();
    battery.stop(); // STOP cancels a pending automatic resume too.
    for (unsigned i=0; i<MODEL_READY_FRAMES; ++i) {
        battery.pause();
        battery.observe(true);
    }
    for (unsigned i=0; i<MODEL_READY_FRAMES; ++i) battery.observe(true);
    assert(!battery.pending() && !battery.armed());
    const auto stopped=commandForModel(.9f,battery.armed());
    assert(stopped.steer==0 && stopped.throttle==0);
    DriveGate manualOnly(false);
    manualOnly.pause();
    for (unsigned i=0; i<MODEL_READY_FRAMES; ++i) manualOnly.observe(true);
    assert(!manualOnly.armed() && !manualOnly.pending());
    assert(stoppedBeforeBoot.start());
    stoppedBeforeBoot.stop();
    for (unsigned i=0; i<MODEL_READY_FRAMES; ++i) stoppedBeforeBoot.observe(true);
    assert(!stoppedBeforeBoot.armed());
    assert(throttleForSteer(0) > throttleForSteer(.2f));
    assert(throttleForSteer(.2f) > throttleForSteer(.5f));
    assert(throttleForSteer(-.5f) == throttleForSteer(.5f));
    assert(throttleForSteer(std::numeric_limits<float>::quiet_NaN()) == 0);
    // Integra a politica do piloto com o mapeamento efetivo da placa de motores.
    auto pwm = [](float steer) {
        return motor_control::throttleToPwm(throttleForSteer(steer), MOTOR_MIN_PWM,
                                           MOTOR_PWM_MAX, MOTOR_THROTTLE_DEADBAND);
    };
    assert(pwm(0) > pwm(.2f) && pwm(.2f) > pwm(.5f));
}
