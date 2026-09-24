#ifndef CONTROL_PROTOCOL_H
#define CONTROL_PROTOCOL_H

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace motor_control {

// O throttle representa a faixa de PWM utilizável, não velocidade física.
inline int throttleToPwm(float throttle, int minimumPwm, int maximumPwm,
                         float deadband) {
    if (!std::isfinite(throttle)) return 0;
    float magnitude = std::fabs(throttle);
    if (magnitude <= deadband) return 0;
    if (magnitude > 1.0f) magnitude = 1.0f;
    const float fraction = (magnitude - deadband) / (1.0f - deadband);
    return minimumPwm + static_cast<int>(std::lround(fraction * (maximumPwm - minimumPwm)));
}

// Apenas decimal (sinal opcional, ao menos um dígito), sem espaços,
// notação hexadecimal/científica, NaN, infinito ou sufixos silenciosos.
inline bool parseDecimal(const char* begin, char delimiter, float& value,
                         const char*& next) {
    const char* cursor = begin;
    if (*cursor == '+' || *cursor == '-') ++cursor;
    bool hasDigit = false;
    while (*cursor >= '0' && *cursor <= '9') {
        hasDigit = true;
        ++cursor;
    }
    if (*cursor == '.') {
        ++cursor;
        while (*cursor >= '0' && *cursor <= '9') {
            hasDigit = true;
            ++cursor;
        }
    }
    if (!hasDigit || *cursor != delimiter) return false;

    errno = 0;
    char* end = nullptr;
    const float parsed = std::strtof(begin, &end);
    if (end != cursor || errno == ERANGE || !std::isfinite(parsed) ||
        parsed < -1.0f || parsed > 1.0f) return false;
    value = parsed;
    next = delimiter == '\0' ? cursor : cursor + 1;
    return true;
}

inline bool parseCommand(const char* line, float& steer, float& throttle) {
    if (line == nullptr || *line != 'S') return false;
    const char* next = nullptr;
    float parsedSteer = 0.0f;
    float parsedThrottle = 0.0f;
    if (!parseDecimal(line + 1, 'T', parsedSteer, next) ||
        !parseDecimal(next, '\0', parsedThrottle, next)) return false;
    // Saídas só mudam se o quadro inteiro for válido.
    steer = parsedSteer;
    throttle = parsedThrottle;
    return true;
}

class CommandReceiver {
public:
    // Retorna true somente ao terminar um comando válido. CRLF é aceito.
    bool feed(char byte, float& steer, float& throttle) {
        if (byte == '\n' || byte == '\r') {
            buffer_[length_] = '\0';
            const bool valid = !discard_ && length_ > 0 &&
                               parseCommand(buffer_, steer, throttle);
            length_ = 0;
            discard_ = false;
            return valid;
        }
        // Não aproveitar um prefixo truncado nem bytes após um NUL injetado.
        if (byte < 32 || byte > 126 || length_ >= sizeof(buffer_) - 1) {
            discard_ = true;
        }
        if (!discard_) buffer_[length_++] = byte;
        return false;
    }

private:
    char buffer_[64] = {};
    std::size_t length_ = 0;
    bool discard_ = false;
};

inline bool commandExpired(uint32_t now, uint32_t lastCommand, uint32_t timeout) {
    return static_cast<uint32_t>(now - lastCommand) >= timeout;
}

} // namespace motor_control
#endif
