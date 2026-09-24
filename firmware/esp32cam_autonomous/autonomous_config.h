#pragma once
#include <stdint.h>

// true: ESP-NN nas convolucoes INT8; false: baseline TFLM para comparar na placa.
constexpr bool ML_USE_ESP_NN = true;

// Copia imutavel do modelo em SRAM interna para reduzir acessos a flash/cache.
// Se nao houver espaco, continua com o mesmo modelo na flash. false permite comparar.
constexpr bool ML_MODEL_IN_INTERNAL_RAM = true;

// Perfil experimental OV2640: divisor interno /2 em vez de /4, QVGA grayscale.
// false preserva os registradores definidos pelo driver para comparar na placa.
constexpr bool CAMERA_FAST_PROFILE = true;

// Throttle normalizado na faixa util de PWM do controlador (0.00 a 1.00)
constexpr float SPEED_SCALE = 0.60f;       // Escala do comando; nao mede velocidade fisica.
constexpr float SPEED_STRAIGHT = 0.50f;
constexpr float SPEED_SOFT_CURVE = 0.40f;
constexpr float SPEED_HARD_CURVE = 0.28f;

// Partida e retomada apos falha temporaria: imagens recentes e inferencias validas, sem detector de fita.
constexpr bool AUTO_ARM_ON_BOOT = true;
constexpr unsigned MODEL_READY_FRAMES = 3;
constexpr int64_t MAX_DECISION_AGE_US = 250000; // 184 ms observados; ainda abaixo do watchdog de 300 ms.
