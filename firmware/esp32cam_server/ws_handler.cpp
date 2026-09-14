// ============================================
// IMPLEMENTAÇÃO — Handler WebSocket (ESP-IDF nativo)
// ============================================
//
// Usa o suporte a WebSocket embutido no esp_http_server (ESP-IDF 4.4+),
// sem necessidade de bibliotecas externas de WS.
// Requer ArduinoJson 7.x para parse/serialize de mensagens JSON.
//
// Protocolo:
//   PC → ESP32:  {"cmd": "get_status"}
//                {"cmd": "set_config", "framesize": 8, "quality": 10}
//                {"cmd": "led", "state": true}
//                {"cmd": "rec_start"}
//                {"cmd": "rec_stop"}
//                {"cmd": "drive", "steer": -0.5, "throttle": 0.8}
//   ESP32 → PC:  {"type": "status", ...}
//                {"type": "config_ack", ...}
//                {"type": "led_ack", ...}
//                {"type": "rec_ack", ...}
//                {"type": "drive_ack", ...}
//                {"type": "error", "msg": "..."}
//
// Relay Serial (ESP32-CAM → 2º ESP32):
//   Cada comando "drive" é retransmitido via Serial2 no formato:
//     S<steer>T<throttle>\n
//   Exemplo: S-0.50T0.80\n
// ============================================

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "ws_handler.h"
#include "camera_config.h"
#include "sd_recorder.h"
#include "esp_camera.h"

// ----- Estado do LED -----
static bool ledState = false;

// ----- Referência ao Serial2 (inicializado no .ino) -----
// Declarado como extern para acesso ao Serial2 configurado no sketch principal.
// O Serial2 é usado para retransmitir comandos de drive ao 2º ESP32.
extern HardwareSerial Serial2;

// =============================================
// Utilitário: enviar texto via WebSocket
// =============================================
static esp_err_t ws_send_text(httpd_req_t *req, const char *text) {
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)text;
    ws_pkt.len     = strlen(text);
    ws_pkt.type    = HTTPD_WS_TYPE_TEXT;
    return httpd_ws_send_frame(req, &ws_pkt);
}

// =============================================
// Utilitário: retransmitir drive para 2º ESP32 via Serial2
// =============================================
static void relayDriveToMotorESP(float steer, float throttle) {
    // Formato compacto: S<steer>T<throttle>\n
    // Exemplo: S-0.50T0.80\n
    char buf[32];
    snprintf(buf, sizeof(buf), "S%.2fT%.2f\n", steer, throttle);
    Serial2.print(buf);
}

// =============================================
// Comando: get_status
// =============================================
static void handleGetStatus(httpd_req_t *req) {
    sensor_t *s = esp_camera_sensor_get();

    JsonDocument doc;
    doc["type"]        = "status";
    doc["framesize"]   = getFrameSizeName(s ? (framesize_t)s->status.framesize : DEFAULT_FRAME_SIZE);
    doc["framesize_val"] = s ? s->status.framesize : 8;
    doc["quality"]     = s ? s->status.quality : DEFAULT_JPEG_QUALITY;
    doc["rssi"]        = WiFi.RSSI();
    doc["uptime_s"]    = (unsigned long)(millis() / 1000);
    doc["free_heap"]   = ESP.getFreeHeap();
    doc["psram_free"]  = ESP.getFreePsram();
    doc["led"]         = ledState;

    // ----- Informações do SD e gravação -----
    doc["sd_ready"]     = isSDCardReady();
    doc["recording"]    = isRecording();
    doc["session"]      = getCurrentSessionNumber();
    doc["rec_frames"]   = getRecordedFrameCount();

    // Eixos de direção atuais
    DriveState ds = getCurrentDrive();
    doc["steer"]    = ds.steer;
    doc["throttle"] = ds.throttle;

    if (isSDCardReady()) {
        doc["sd_free_mb"]  = (uint32_t)(getSDFreeBytes() / (1024 * 1024));
        doc["sd_total_mb"] = (uint32_t)(getSDTotalBytes() / (1024 * 1024));
    }

    char buffer[512];
    serializeJson(doc, buffer, sizeof(buffer));
    ws_send_text(req, buffer);
}

// =============================================
// Comando: set_config
// =============================================
static void handleSetConfig(httpd_req_t *req, JsonDocument &cmdDoc) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Sensor nao disponivel\"}");
        return;
    }

    JsonDocument respDoc;
    respDoc["type"] = "config_ack";
    bool changed = false;

    // Alterar resolução
    if (cmdDoc["framesize"].is<int>()) {
        int fs = cmdDoc["framesize"];
        if (fs >= 0 && fs <= 13) {
            s->set_framesize(s, (framesize_t)fs);
            respDoc["framesize"]      = fs;
            respDoc["framesize_name"] = getFrameSizeName((framesize_t)fs);
            changed = true;
            Serial.printf("[WS] Resolucao alterada para: %s (%d)\n",
                          getFrameSizeName((framesize_t)fs), fs);
        } else {
            ws_send_text(req, "{\"type\":\"error\",\"msg\":\"framesize invalido (0-13)\"}");
            return;
        }
    }

    // Alterar qualidade JPEG
    if (cmdDoc["quality"].is<int>()) {
        int q = cmdDoc["quality"];
        if (q >= 4 && q <= 63) {
            s->set_quality(s, q);
            respDoc["quality"] = q;
            changed = true;
            Serial.printf("[WS] Qualidade JPEG alterada para: %d\n", q);
        } else {
            ws_send_text(req, "{\"type\":\"error\",\"msg\":\"quality invalida (4-63)\"}");
            return;
        }
    }

    if (!changed) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Nenhum parametro valido fornecido (framesize, quality)\"}");
        return;
    }

    char buffer[200];
    serializeJson(respDoc, buffer, sizeof(buffer));
    ws_send_text(req, buffer);
}

// =============================================
// Comando: led
// =============================================
static void handleLed(httpd_req_t *req, JsonDocument &cmdDoc) {
    if (!cmdDoc["state"].is<bool>()) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Campo 'state' (bool) ausente\"}");
        return;
    }

    ledState = cmdDoc["state"].as<bool>();
    digitalWrite(LED_FLASH_GPIO, ledState ? HIGH : LOW);

    JsonDocument respDoc;
    respDoc["type"]  = "led_ack";
    respDoc["state"] = ledState;

    char buffer[64];
    serializeJson(respDoc, buffer, sizeof(buffer));
    ws_send_text(req, buffer);

    Serial.printf("[WS] LED Flash: %s\n", ledState ? "LIGADO" : "DESLIGADO");
}

// =============================================
// Comando: rec_start — inicia gravação no SD
// =============================================
static void handleRecStart(httpd_req_t *req) {
    if (!isSDCardReady()) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"SD card nao inicializado\"}");
        return;
    }

    if (startRecording()) {
        JsonDocument respDoc;
        respDoc["type"]    = "rec_ack";
        respDoc["action"]  = "started";
        respDoc["session"] = getCurrentSessionNumber();

        char buffer[128];
        serializeJson(respDoc, buffer, sizeof(buffer));
        ws_send_text(req, buffer);
    } else {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Falha ao iniciar gravacao\"}");
    }
}

// =============================================
// Comando: rec_stop — para gravação no SD
// =============================================
static void handleRecStop(httpd_req_t *req) {
    if (!isRecording()) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Nenhuma gravacao ativa\"}");
        return;
    }

    uint32_t totalFrames = getRecordedFrameCount();
    uint16_t session     = getCurrentSessionNumber();
    stopRecording();

    JsonDocument respDoc;
    respDoc["type"]    = "rec_ack";
    respDoc["action"]  = "stopped";
    respDoc["session"] = session;
    respDoc["frames"]  = totalFrames;

    char buffer[128];
    serializeJson(respDoc, buffer, sizeof(buffer));
    ws_send_text(req, buffer);
}

// =============================================
// Comando: drive — eixos steer + throttle contínuos
// =============================================
static void handleDrive(httpd_req_t *req, JsonDocument &cmdDoc) {
    // Validar que steer e throttle são números
    if (!cmdDoc["steer"].is<float>() && !cmdDoc["steer"].is<int>()) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Campo 'steer' (float) ausente\"}");
        return;
    }
    if (!cmdDoc["throttle"].is<float>() && !cmdDoc["throttle"].is<int>()) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Campo 'throttle' (float) ausente\"}");
        return;
    }

    float steer    = constrain(cmdDoc["steer"].as<float>(), -1.0f, 1.0f);
    float throttle = constrain(cmdDoc["throttle"].as<float>(), -1.0f, 1.0f);

    // 1. Atualizar estado local (para gravação no SD)
    setCurrentDrive(steer, throttle);

    // 2. Retransmitir imediatamente para o 2º ESP32 via Serial2
    relayDriveToMotorESP(steer, throttle);
}

// =============================================
// Handler principal do WebSocket
// =============================================
static esp_err_t ws_handler(httpd_req_t *req) {
    // Handshake HTTP → WebSocket (primeiro request GET)
    if (req->method == HTTP_GET) {
        Serial.println("[WS] Cliente conectado");
        return ESP_OK;
    }

    // ---- Receber frame WebSocket ----

    // Primeira chamada: descobrir tamanho do payload
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        Serial.printf("[WS] Erro ao receber frame: %d\n", ret);
        return ret;
    }

    // Frame vazio (ping/pong já tratado pelo framework)
    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    // Limitar tamanho máximo de mensagem (proteção contra OOM)
    if (ws_pkt.len > 1024) {
        Serial.println("[WS] Mensagem muito grande, ignorando");
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Mensagem excede 1024 bytes\"}");
        return ESP_OK;
    }

    // Segunda chamada: receber payload
    uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
    if (!buf) {
        Serial.println("[WS] Erro de alocacao de memoria");
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        Serial.printf("[WS] Erro ao receber payload: %d\n", ret);
        free(buf);
        return ret;
    }

    // ---- Processar comando JSON ----

    JsonDocument cmdDoc;
    DeserializationError jsonErr = deserializeJson(cmdDoc, (char *)buf);

    if (jsonErr) {
        Serial.printf("[WS] JSON invalido: %s\n", jsonErr.c_str());
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"JSON invalido\"}");
        free(buf);
        return ESP_OK;
    }

    const char *cmd = cmdDoc["cmd"];
    if (!cmd) {
        ws_send_text(req, "{\"type\":\"error\",\"msg\":\"Campo 'cmd' ausente\"}");
        free(buf);
        return ESP_OK;
    }

    // ---- Roteamento de comandos ----

    if (strcmp(cmd, "get_status") == 0) {
        handleGetStatus(req);
    } else if (strcmp(cmd, "set_config") == 0) {
        handleSetConfig(req, cmdDoc);
    } else if (strcmp(cmd, "led") == 0) {
        handleLed(req, cmdDoc);
    } else if (strcmp(cmd, "rec_start") == 0) {
        handleRecStart(req);
    } else if (strcmp(cmd, "rec_stop") == 0) {
        handleRecStop(req);
    } else if (strcmp(cmd, "drive") == 0) {
        handleDrive(req, cmdDoc);
    } else {
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf),
                 "{\"type\":\"error\",\"msg\":\"Comando desconhecido: %s\"}", cmd);
        ws_send_text(req, errBuf);
        Serial.printf("[WS] Comando desconhecido: %s\n", cmd);
    }

    free(buf);
    return ESP_OK;
}

// =============================================
// Registro do WebSocket no servidor HTTP
// =============================================
void setupWebSocket(httpd_handle_t server) {
    if (!server) {
        Serial.println("[WS] ERRO: servidor HTTP nulo, WebSocket nao registrado");
        return;
    }

    // Registrar handler com flag is_websocket = true
    // Isso usa o suporte nativo do esp_http_server (ESP-IDF 4.4+)
    httpd_uri_t ws_uri;
    memset(&ws_uri, 0, sizeof(httpd_uri_t));
    ws_uri.uri       = "/ws";
    ws_uri.method    = HTTP_GET;
    ws_uri.handler   = ws_handler;
    ws_uri.user_ctx  = NULL;
    ws_uri.is_websocket            = true;
    ws_uri.handle_ws_control_frames = false;   // framework trata ping/pong
    ws_uri.supported_subprotocol    = NULL;

    esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
    if (err == ESP_OK) {
        Serial.println("[WS] WebSocket registrado em /ws (porta 80)");
    } else {
        Serial.printf("[WS] ERRO ao registrar WebSocket: %d\n", err);
    }
}
