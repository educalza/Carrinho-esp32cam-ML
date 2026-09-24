// ============================================================
//  2º ESP32 — Controlador de Motores (Servo + Ponte H L298N)
// ============================================================
//
//  Recebe comandos do ESP32-CAM via Serial (UART2) no formato:
//    S<steer>T<throttle>\n
//  Exemplo:
//    S-0.500T0.800\n → servo à esquerda, 80% da faixa útil de PWM à frente
//    S0.00T0.00\n   → servo centralizado, motores parados
//





//  Hardware:
//    - 1x Servo de direção (PWM 50Hz)
//    - 2x Motores DC via Ponte H L298N (PWM 1kHz)
//    - Serial2 conectada ao ESP32-CAM
//
//  Failsafe: Se nenhum comando válido chegar em 300ms, para os motores
//            automaticamente (servo permanece na última posição).
//
//  Configurações do Arduino IDE:
//    Board:            ESP32 Dev Module (ou sua variante)
//    Partition Scheme: Default 4MB with spiffs
//    Upload Speed:     921600
// ============================================================

#include <Arduino.h>
#include <esp_arduino_version.h>
#include "motor_config.h"
#include "control_protocol.h"

// Compatibilidade entre ESP32 Arduino Core 2.x e 3.x para LEDC (PWM)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    // ESP32 Core 3.x: ledcAttach(pin, freq, res) e ledcWrite(pin, duty)
    #define PWM_WRITE(pin, channel, duty)       ledcWrite(pin, duty)
#else
    // ESP32 Core 2.x: ledcSetup(channel, freq, res), ledcAttachPin(pin, channel) e ledcWrite(channel, duty)
    #define PWM_WRITE(pin, channel, duty)       ledcWrite(channel, duty)
#endif

static bool attachPwm(int pin, int channel, int frequency, int resolution) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    (void)channel;
    return ledcAttach(pin, frequency, resolution);
#else
    if (ledcSetup(channel, frequency, resolution) <= 0.0) return false;
    ledcAttachPin(pin, channel);
    return true;
#endif
}

// =============================================
// Estado atual
// =============================================
static float currentSteer    = 0.0f;
static float currentThrottle = 0.0f;
static unsigned long lastCmdTime = 0;
static bool failsafeActive = false;
static bool hardwareReady = false;

// =============================================
// Inicialização do Servo (LEDC PWM)
// =============================================
bool initServo() {
    if (!attachPwm(SERVO_PIN, LEDC_CHANNEL_SERVO, SERVO_PWM_FREQ, SERVO_PWM_RES)) {
        Serial.println("[SERVO] Falha ao configurar PWM; movimento bloqueado");
        return false;
    }

    // Posição inicial: centro
    setServoAngle(SERVO_CENTER_ANGLE);

    Serial.printf("[SERVO] Inicializado no GPIO %d | Centro: %d°\n",
                  SERVO_PIN, SERVO_CENTER_ANGLE);
    return true;
}

// =============================================
// Controle do Servo
// =============================================
void setServoAngle(int angle) {
    angle = constrain(angle, 0, 180);

    // Mapear ângulo (0-180) para duty cycle (SERVO_MIN_DUTY - SERVO_MAX_DUTY)
    uint32_t duty = map(angle, 0, 180, SERVO_MIN_DUTY, SERVO_MAX_DUTY);
    PWM_WRITE(SERVO_PIN, LEDC_CHANNEL_SERVO, duty);
}

// Converte steer (-1.0 a +1.0) para ângulo do servo
void setServoFromSteer(float steer) {
    if (SERVO_INVERTED) {
        steer = -steer;
    }
    // steer = -1.0 → SERVO_MIN_ANGLE (esquerda)
    // steer =  0.0 → SERVO_CENTER_ANGLE (reto)
    // steer = +1.0 → SERVO_MAX_ANGLE (direita)
    int angle;
    if (steer < 0) {
        angle = map(steer * 100, -100, 0, SERVO_MIN_ANGLE, SERVO_CENTER_ANGLE);
    } else {
        angle = map(steer * 100, 0, 100, SERVO_CENTER_ANGLE, SERVO_MAX_ANGLE);
    }
    setServoAngle(angle);
}

// =============================================
// Inicialização dos Motores DC (LEDC PWM + GPIO)
// =============================================
bool initMotors() {
    // Motor A
    pinMode(MOTOR_A_IN1, OUTPUT);
    pinMode(MOTOR_A_IN2, OUTPUT);
    digitalWrite(MOTOR_A_IN1, LOW);
    digitalWrite(MOTOR_A_IN2, LOW);

    // Motor B
    pinMode(MOTOR_B_IN3, OUTPUT);
    pinMode(MOTOR_B_IN4, OUTPUT);
    digitalWrite(MOTOR_B_IN3, LOW);
    digitalWrite(MOTOR_B_IN4, LOW);

    const bool motorAReady = attachPwm(MOTOR_A_EN, LEDC_CHANNEL_MOTOR_A, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
    const bool motorBReady = attachPwm(MOTOR_B_EN, LEDC_CHANNEL_MOTOR_B, MOTOR_PWM_FREQ, MOTOR_PWM_RES);
    if (!motorAReady || !motorBReady) {
        Serial.println("[MOTOR] Falha ao configurar PWM; movimento bloqueado");
        return false;
    }
    PWM_WRITE(MOTOR_A_EN, LEDC_CHANNEL_MOTOR_A, 0);
    PWM_WRITE(MOTOR_B_EN, LEDC_CHANNEL_MOTOR_B, 0);

    Serial.printf("[MOTOR] Motor A: EN=GPIO%d IN1=GPIO%d IN2=GPIO%d\n",
                  MOTOR_A_EN, MOTOR_A_IN1, MOTOR_A_IN2);
    Serial.printf("[MOTOR] Motor B: EN=GPIO%d IN3=GPIO%d IN4=GPIO%d\n",
                  MOTOR_B_EN, MOTOR_B_IN3, MOTOR_B_IN4);
    return true;
}

// =============================================
// Controle de um motor individual
// =============================================
// speed: -1.0 (ré máx) a +1.0 (frente máx), faixa útil de PWM normalizada.
void setMotor(int pinEN, int channelEN, int pinFwd, int pinRev, float speed) {
    const int pwm = motor_control::throttleToPwm(speed, MOTOR_MIN_PWM, MOTOR_PWM_MAX,
                                                MOTOR_THROTTLE_DEADBAND);

    if (pwm == 0) {
        // Parado — freio por inércia (coast)
        digitalWrite(pinFwd, LOW);
        digitalWrite(pinRev, LOW);
        PWM_WRITE(pinEN, channelEN, 0);
    } else if (speed > 0) {
        // Frente
        digitalWrite(pinFwd, HIGH);
        digitalWrite(pinRev, LOW);
        PWM_WRITE(pinEN, channelEN, pwm);
    } else {
        // Ré
        digitalWrite(pinFwd, LOW);
        digitalWrite(pinRev, HIGH);
        PWM_WRITE(pinEN, channelEN, pwm);
    }
}

// Aplica throttle igualmente nos dois motores
void setMotorsFromThrottle(float throttle) {
    setMotor(MOTOR_A_EN, LEDC_CHANNEL_MOTOR_A, MOTOR_A_IN1, MOTOR_A_IN2, throttle);
    setMotor(MOTOR_B_EN, LEDC_CHANNEL_MOTOR_B, MOTOR_B_IN3, MOTOR_B_IN4, throttle);
}

// Parar os dois motores imediatamente
void stopMotors() {
    setMotor(MOTOR_A_EN, LEDC_CHANNEL_MOTOR_A, MOTOR_A_IN1, MOTOR_A_IN2, 0);
    setMotor(MOTOR_B_EN, LEDC_CHANNEL_MOTOR_B, MOTOR_B_IN3, MOTOR_B_IN4, 0);
}

// =============================================
// Parser do protocolo serial
// =============================================
// Formato esperado: S<steer>T<throttle>\n
// Exemplo: S-0.50T0.80\n
// Retorna true se o parse foi bem-sucedido
bool parseSerialCommand(const char* line, float &steer, float &throttle) {
    return motor_control::parseCommand(line, steer, throttle);
}

// =============================================
// Setup
// =============================================
void setup() {
    // ----- Serial USB (debug) -----
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("========================================");
    Serial.println("  Motor Controller — 2o ESP32");
    Serial.println("  Firmware v1.0 — Servo + Ponte H L298N");
    Serial.println("========================================");

    // ----- Serial2 (comunicação com ESP32-CAM) -----
    Serial2.begin(SERIAL_CAM_BAUD, SERIAL_8N1, SERIAL_CAM_RX, SERIAL_CAM_TX);
    Serial.printf("[SERIAL] UART2 iniciada (RX=GPIO%d TX=GPIO%d) a %d baud\n",
                  SERIAL_CAM_RX, SERIAL_CAM_TX, SERIAL_CAM_BAUD);

    // ----- Inicializar saídas sem autorizar movimento em caso de falha -----
    const bool motorsReady = initMotors();
    const bool servoReady = initServo();
    hardwareReady = motorsReady && servoReady;
    if (!hardwareReady) {
        Serial.println("[CTRL] Inicialização incompleta. Corrija o PWM e reinicie.");
        return;
    }
    stopMotors();

    // ----- Pronto -----
    Serial.println();
    Serial.println("============================================");
    Serial.println("  Motor Controller pronto!");
    Serial.println("  Aguardando comandos via Serial2...");
    Serial.printf("  Formato: S<steer>T<throttle>\\n\n");
    Serial.printf("  Failsafe: %d ms sem comando = PARADA\n", FAILSAFE_TIMEOUT_MS);
    Serial.println("============================================");
    Serial.println();

    lastCmdTime = millis();
}

// =============================================
// Loop
// =============================================

// Buffer de leitura serial
static motor_control::CommandReceiver receiver;

void checkFailsafe() {
    if (!failsafeActive && motor_control::commandExpired(millis(), lastCmdTime,
                                                        FAILSAFE_TIMEOUT_MS)) {
        stopMotors();
        currentThrottle = 0.0f;
        failsafeActive = true;
        Serial.printf("[CTRL] FAILSAFE: %dms sem comando válido — motores parados\n",
                      FAILSAFE_TIMEOUT_MS);
    }
}

void loop() {
    if (!hardwareReady) {
        delay(10);
        return;
    }
    checkFailsafe();
    // ----- Ler comandos da Serial2 -----
    // Limita o trabalho por loop: ruído contínuo não pode adiar o failsafe.
    int bytesRead = 0;
    while (Serial2.available() && bytesRead++ < 128) {
        const char c = static_cast<char>(Serial2.read());
        float steer, throttle;
        if (receiver.feed(c, steer, throttle)) {
            currentSteer    = steer;
            currentThrottle = throttle;
            lastCmdTime     = millis();
            setServoFromSteer(steer);
            setMotorsFromThrottle(throttle);
            if (failsafeActive) {
                failsafeActive = false;
                Serial.println("[CTRL] Comando recebido — saindo do failsafe");
            }
        }
    }

    // ----- Failsafe: timeout sem comandos -----
    checkFailsafe();

    // ----- Log periódico de estado (a cada 2 segundos) -----
    static unsigned long lastStatusLog = 0;
    if (millis() - lastStatusLog > 2000) {
        lastStatusLog = millis();
        Serial.printf("[CTRL] steer=%.2f throttle=%.2f | failsafe=%s\n",
                      currentSteer, currentThrottle,
                      failsafeActive ? "ATIVO" : "ok");
    }

    delay(1);   // yield
}
