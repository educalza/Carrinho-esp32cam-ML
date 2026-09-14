// ============================================
// CONFIGURAÇÃO DA CÂMERA — AI-THINKER ESP32-CAM
// ============================================
#ifndef CAMERA_CONFIG_H
#define CAMERA_CONFIG_H

#include "esp_camera.h"

// ----- Pinos do AI-Thinker ESP32-CAM -----
#define PWDN_GPIO_NUM      32
#define RESET_GPIO_NUM     -1
#define XCLK_GPIO_NUM       0
#define SIOD_GPIO_NUM      26
#define SIOC_GPIO_NUM      27

#define Y9_GPIO_NUM        35
#define Y8_GPIO_NUM        34
#define Y7_GPIO_NUM        39
#define Y6_GPIO_NUM        36
#define Y5_GPIO_NUM        21
#define Y4_GPIO_NUM        19
#define Y3_GPIO_NUM        18
#define Y2_GPIO_NUM         5

#define VSYNC_GPIO_NUM     25
#define HREF_GPIO_NUM      23
#define PCLK_GPIO_NUM      22

// ----- LED Flash -----
#define LED_FLASH_GPIO      4

// ----- Parâmetros padrão da câmera -----
#define DEFAULT_FRAME_SIZE    FRAMESIZE_QVGA  // 320x240 (latência ultra-baixa e ideal para ML)
#define DEFAULT_JPEG_QUALITY  14              // Qualidade balanceada com tamanho leve (~7KB)
#define XCLK_FREQ_HZ         20000000        // 20 MHz

// ----- Declaração -----
// Inicializa a câmera com a configuração de pinos e parâmetros acima.
// Retorna true se bem-sucedido, false em caso de erro.
bool initCamera();

// Retorna o nome textual de uma resolução (ex: "VGA", "QVGA").
const char* getFrameSizeName(framesize_t fs);

#endif // CAMERA_CONFIG_H
