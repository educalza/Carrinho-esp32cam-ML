// ============================================
// IMPLEMENTAÇÃO — Servidores HTTP (Capture + Stream MJPEG)
// ============================================
#include <Arduino.h>
#include "http_handlers.h"
#include "esp_camera.h"
#include "esp_timer.h"

// ----- Constantes do stream MJPEG -----
#define PART_BOUNDARY "frame"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART_HEADER  = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ----- Handles dos servidores -----
static httpd_handle_t camera_httpd = NULL;   // porta 80
static httpd_handle_t stream_httpd = NULL;   // porta 81

httpd_handle_t getMainServerHandle() {
    return camera_httpd;
}

// =============================================
// Handler: GET /capture — frame JPEG único
// =============================================
static esp_err_t capture_handler(httpd_req_t *req) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[HTTP] ERRO: captura de frame falhou");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);

    if (res == ESP_OK) {
        Serial.printf("[HTTP] /capture — %u bytes enviados\n", fb->len);
    }
    return res;
}

// =============================================
// Handler: GET /stream — stream MJPEG contínuo
// =============================================
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[80];

    // Configurar headers do multipart
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "variable");

    Serial.println("[STREAM] Cliente conectado — iniciando stream MJPEG");

    uint32_t frame_count = 0;
    int64_t stream_start = esp_timer_get_time();

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[STREAM] ERRO: captura do frame falhou");
            res = ESP_FAIL;
            break;
        }

        // Enviar boundary do multipart
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        // Enviar header do part (content-type + content-length)
        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART_HEADER, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            esp_camera_fb_return(fb);
            break;
        }

        // Enviar dados JPEG (direto do buffer DMA — zero copy)
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            break;
        }

        frame_count++;

        // Ceder tempo de CPU e rádio Wi-Fi para o WebSocket atender comandos instantaneamente (<50ms)
        vTaskDelay(pdMS_TO_TICKS(15));

        // Log de FPS a cada 100 frames
        if (frame_count % 100 == 0) {
            int64_t elapsed_us = esp_timer_get_time() - stream_start;
            float fps = (float)frame_count * 1000000.0f / (float)elapsed_us;
            Serial.printf("[STREAM] %u frames | %.1f FPS\n", frame_count, fps);
        }
    }

    // Log de encerramento
    int64_t total_us = esp_timer_get_time() - stream_start;
    float avg_fps = (total_us > 0) ? (float)frame_count * 1000000.0f / (float)total_us : 0;
    Serial.printf("[STREAM] Encerrado — %u frames em %.1fs (%.1f FPS medio)\n",
                  frame_count, (float)total_us / 1000000.0f, avg_fps);

    return res;
}

// =============================================
// Inicialização dos servidores HTTP
// =============================================
void startHTTPServers() {
    // ----- Servidor principal (porta 80) -----
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;   // espaço para /capture + /ws + futuros endpoints
    config.stack_size = 8192;       // stack maior para handlers mais complexos

    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        // Registrar /capture
        httpd_uri_t capture_uri = {
            .uri      = "/capture",
            .method   = HTTP_GET,
            .handler  = capture_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(camera_httpd, &capture_uri);

        Serial.println("[HTTP] Servidor principal iniciado na porta 80");
        Serial.println("[HTTP]   /capture — frame JPEG unico");
    } else {
        Serial.println("[HTTP] ERRO: falha ao iniciar servidor na porta 80");
    }

    // ----- Servidor de stream (porta 81) -----
    // Servidor separado para que o stream MJPEG (conexão longa)
    // não bloqueie os endpoints da porta 80.
    config.server_port = 81;
    config.ctrl_port   = 32769;    // porta de controle diferente para o segundo servidor

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_uri_t stream_uri = {
            .uri      = "/stream",
            .method   = HTTP_GET,
            .handler  = stream_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(stream_httpd, &stream_uri);

        Serial.println("[HTTP] Servidor de stream iniciado na porta 81");
        Serial.println("[HTTP]   /stream — stream MJPEG continuo");
    } else {
        Serial.println("[HTTP] ERRO: falha ao iniciar servidor de stream na porta 81");
    }
}
