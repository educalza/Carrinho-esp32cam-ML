// ============================================
// IMPLEMENTAÇÃO — Inicialização da câmera
// ============================================
#include <Arduino.h>
#include "camera_config.h"

bool initCamera() {
    camera_config_t config;

    // Pinos de controle
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    // Pinos de dados (D0–D7)
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    // Pinos de sincronização
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href  = HREF_GPIO_NUM;
    config.pin_pclk  = PCLK_GPIO_NUM;

    // Clock e formato
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.ledc_timer   = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    // Ajuste baseado na presença de PSRAM
    if (psramFound()) {
        config.frame_size  = DEFAULT_FRAME_SIZE;
        config.jpeg_quality = DEFAULT_JPEG_QUALITY;
        config.fb_count    = 2;   // double buffering para stream suave
        Serial.println("[CAM] PSRAM encontrada — double buffering ativado");
    } else {
        config.frame_size  = FRAMESIZE_SVGA;   // resolução reduzida sem PSRAM
        config.jpeg_quality = 12;
        config.fb_count    = 1;
        Serial.println("[CAM] PSRAM nao encontrada — resolucao reduzida");
    }

    // Inicializar driver da câmera
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] ERRO ao inicializar camera: 0x%x\n", err);
        if (err == ESP_ERR_NOT_FOUND) {
            Serial.println("[CAM] Verifique o cabo flat da camera e o modelo da placa");
        }
        return false;
    }

    // Ajustes finos do sensor (opcional — descomente conforme necessário)
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, DEFAULT_FRAME_SIZE);
        s->set_quality(s, DEFAULT_JPEG_QUALITY);
        // s->set_vflip(s, 1);    // Espelhar verticalmente
        // s->set_hmirror(s, 1);  // Espelhar horizontalmente
        // s->set_brightness(s, 1);  // -2 a 2
        // s->set_contrast(s, 0);    // -2 a 2
    }

    Serial.println("[CAM] Camera inicializada com sucesso!");
    Serial.printf("[CAM] Resolucao: %s | Qualidade JPEG: %d\n",
                  getFrameSizeName(DEFAULT_FRAME_SIZE), DEFAULT_JPEG_QUALITY);
    return true;
}

const char* getFrameSizeName(framesize_t fs) {
    switch (fs) {
        case FRAMESIZE_96X96:   return "96x96";
        case FRAMESIZE_QQVGA:   return "QQVGA (160x120)";
        case FRAMESIZE_QCIF:    return "QCIF (176x144)";
        case FRAMESIZE_HQVGA:   return "HQVGA (240x176)";
        case FRAMESIZE_240X240: return "240x240";
        case FRAMESIZE_QVGA:    return "QVGA (320x240)";
        case FRAMESIZE_CIF:     return "CIF (400x296)";
        case FRAMESIZE_HVGA:    return "HVGA (480x320)";
        case FRAMESIZE_VGA:     return "VGA (640x480)";
        case FRAMESIZE_SVGA:    return "SVGA (800x600)";
        case FRAMESIZE_XGA:     return "XGA (1024x768)";
        case FRAMESIZE_HD:      return "HD (1280x720)";
        case FRAMESIZE_SXGA:    return "SXGA (1280x1024)";
        case FRAMESIZE_UXGA:    return "UXGA (1600x1200)";
        default:                return "DESCONHECIDA";
    }
}
