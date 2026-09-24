// Host: g++ -std=c++11 -Wall -Wextra -pedantic tests/test_motor_control.cpp -o <test>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include "../firmware/esp32_motor_controller/control_protocol.h"
#include "../firmware/esp32_motor_controller/motor_config.h"

static int pwm(float throttle) {
    return motor_control::throttleToPwm(throttle, MOTOR_MIN_PWM, MOTOR_PWM_MAX,
                                        MOTOR_THROTTLE_DEADBAND);
}

static int receive(motor_control::CommandReceiver& receiver, const std::string& bytes,
                   float& steer, float& throttle) {
    int accepted = 0;
    for (char byte : bytes) accepted += receiver.feed(byte, steer, throttle) ? 1 : 0;
    return accepted;
}

int main() {
    assert(pwm(0) == 0);
    assert(pwm(0.02f) == 0);
    assert(pwm(-0.02f) == 0);
    assert(pwm(0.021f) == MOTOR_MIN_PWM);
    assert(pwm(0.048f) == 95);
    assert(pwm(0.038f) == 93);
    assert(pwm(0.032f) == 92);
    // O ponto médio cai entre os dois inteiros representáveis pelo PWM de 8 bits.
    assert(pwm(0.51f) >= 172 && pwm(0.51f) <= 173);
    assert(pwm(1) == MOTOR_PWM_MAX);
    assert(pwm(-1) == MOTOR_PWM_MAX);
    assert(pwm(5) == MOTOR_PWM_MAX);
    assert(pwm(std::numeric_limits<float>::quiet_NaN()) == 0);
    assert(pwm(std::numeric_limits<float>::infinity()) == 0);
    int previous = 0;
    for (int i = 0; i <= 1000; ++i) {
        const float throttle = i / 1000.0f;
        const int result = pwm(throttle);
        assert(result >= previous && result <= MOTOR_PWM_MAX);
        assert(result == pwm(-throttle));
        previous = result;
    }

    float steer = 0.0f;
    float throttle = 0.0f;
    assert(motor_control::parseCommand("S-0.500T0.048", steer, throttle));
    assert(steer == -0.5f && throttle == 0.048f);
    assert(motor_control::parseCommand("S+1T-1", steer, throttle));
    assert(steer == 1.0f && throttle == -1.0f);
    assert(motor_control::parseCommand("S0.00T0.00", steer, throttle));

    const char* invalid[] = {
        nullptr, "", "garbageS0T0", " S0T0", "S0 T0", "S0T 0", "S0T0 ",
        "S0T0garbage", "ST0", "S0T", "S.T0", "S0T-", "S0T1.001", "S-1.001T0",
        "SnanT0", "S0Tinf", "S0T-NaN", "S0x0p0T0", "S0T0x1p0", "S0T1e-1",
        "S0T0S0T0", "S0T0\n", "S9999999999999999999999999999999999999999T0"
    };
    for (const char* line : invalid) {
        steer = 0.123f;
        throttle = 0.456f;
        assert(!motor_control::parseCommand(line, steer, throttle));
        assert(steer == 0.123f && throttle == 0.456f);
    }

    motor_control::CommandReceiver receiver;
    assert(receive(receiver, "S-0.5", steer, throttle) == 0);
    assert(receive(receiver, "00T0.048\r\n", steer, throttle) == 1);
    assert(steer == -0.5f && throttle == 0.048f);
    assert(receive(receiver, "\r\n\n", steer, throttle) == 0);
    assert(receive(receiver, "S0T1\nS1T0\n", steer, throttle) == 2);
    // Prefixo numericamente válido que ultrapassa o buffer não pode ser aplicado.
    assert(receive(receiver, "S0T0." + std::string(100, '0') + "\n", steer, throttle) == 0);
    assert(receive(receiver, "S0T0.032\n", steer, throttle) == 1);
    // Um NUL/control byte no fio invalida o quadro inteiro, inclusive o prefixo.
    std::string injected = "S0T1";
    injected += '\0';
    injected += "S0T1\n";
    assert(receive(receiver, injected, steer, throttle) == 0);
    assert(receive(receiver, "S0T1\t\n", steer, throttle) == 0);
    assert(receive(receiver, "S0T0\n", steer, throttle) == 1);

    const uint32_t lastCommand = 1000;
    assert(!motor_control::commandExpired(1299, lastCommand, FAILSAFE_TIMEOUT_MS));
    assert(motor_control::commandExpired(1300, lastCommand, FAILSAFE_TIMEOUT_MS));
    assert(motor_control::commandExpired(1500, lastCommand, FAILSAFE_TIMEOUT_MS));
    // O clock millis() cruza UINT32_MAX após ~49 dias.
    const uint32_t beforeWrap = UINT32_MAX - 99;
    assert(!motor_control::commandExpired(199, beforeWrap, FAILSAFE_TIMEOUT_MS));
    assert(motor_control::commandExpired(200, beforeWrap, FAILSAFE_TIMEOUT_MS));
    // Pacotes inválidos não fornecem a confirmação que renova o watchdog.
    uint32_t lastValid = 1000;
    if (receive(receiver, "S0Tnan\n", steer, throttle)) lastValid = 1290;
    assert(motor_control::commandExpired(1300, lastValid, FAILSAFE_TIMEOUT_MS));
    std::cout << "motor_control: PWM, parser, framing and watchdog passed\n";
}
