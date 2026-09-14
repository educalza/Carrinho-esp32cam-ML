// ============================================
// CONFIGURAÇÃO DE PINOS — 2º ESP32 (Motor Controller)
// ============================================
//
// Ajuste os pinos abaixo conforme a sua fiação.
// Pinos padrão: ESP32 DevKit V1
//
// ============================================
#ifndef MOTOR_CONFIG_H
#define MOTOR_CONFIG_H

// ============================
// Servo de Direção (Esterçamento)
// ============================
#define SERVO_PIN           18      // Sinal PWM para o servo

// Ângulos limite do servo (ajuste conforme mecânica do chassi)
#define SERVO_MIN_ANGLE     45      // Ângulo mínimo
#define SERVO_CENTER_ANGLE  90      // Ângulo central (reto)
#define SERVO_MAX_ANGLE     135     // Ângulo máximo
#define SERVO_INVERTED      true    // true = inverte esquerda/direita mecanicamente

// ============================
// Ponte H L298N — Motor Esquerdo (A)
// ============================
#define MOTOR_A_EN          25      // ENA — PWM de velocidade
#define MOTOR_A_IN1         26      // IN1 — Direção A
#define MOTOR_A_IN2         27      // IN2 — Direção B

// ============================
// Ponte H L298N — Motor Direito (B)
// ============================
#define MOTOR_B_EN          32      // ENB — PWM de velocidade
#define MOTOR_B_IN3         33      // IN3 — Direção A
#define MOTOR_B_IN4         14      // IN4 — Direção B

// ============================
// Serial (comunicação com ESP32-CAM)
// ============================
// Usa Serial2 (UART2) do ESP32
#define SERIAL_CAM_RX       16      // RX2 — recebe do TX (GPIO 13) do ESP32-CAM
#define SERIAL_CAM_TX       17      // TX2 — envia para RX (GPIO 12) do ESP32-CAM
#define SERIAL_CAM_BAUD     115200

// ============================
// Parâmetros de Controle
// ============================

// PWM dos motores
#define MOTOR_PWM_FREQ      1000    // Frequência PWM em Hz
#define MOTOR_PWM_RES       8       // Resolução em bits (0-255)
#define MOTOR_PWM_MAX       255     // Valor máximo de PWM (2^8 - 1)

// Velocidade mínima para os motores girarem (compensa atrito estático)
// Se throttle * 255 < MOTOR_MIN_PWM, aplica MOTOR_MIN_PWM
// Ajuste conforme seus motores — motores baratos costumam precisar de ~60-80
#define MOTOR_MIN_PWM       50

// Timeout de segurança (failsafe)
// Se nenhum comando chegar em FAILSAFE_TIMEOUT_MS, para os motores
#define FAILSAFE_TIMEOUT_MS 500

// Canais LEDC para PWM (ESP32 tem 16 canais: 0-15)
#define LEDC_CHANNEL_MOTOR_A  0
#define LEDC_CHANNEL_MOTOR_B  1
#define LEDC_CHANNEL_SERVO    2

// Servo PWM
#define SERVO_PWM_FREQ      50      // 50 Hz padrão para servos
#define SERVO_PWM_RES       16      // 16-bit para boa resolução angular
// Duty cycle para servos típicos (em ticks de 16-bit a 50Hz):
// 0.5ms (0°)   = ~1638
// 1.5ms (90°)  = ~4915
// 2.5ms (180°) = ~8192
#define SERVO_MIN_DUTY      1638    // ~0.5ms → 0°
#define SERVO_MAX_DUTY      8192    // ~2.5ms → 180°

#endif // MOTOR_CONFIG_H
