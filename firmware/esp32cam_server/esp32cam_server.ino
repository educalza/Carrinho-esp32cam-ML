// ============================================================
//  ESP32-CAM Carrinho Autônomo ML — Etapa 2+3+4
//  Firmware: Servidor Wi-Fi + Gravação SD + Relay Serial
// ============================================================
//
//  Endpoints:
//    📹 GET  http://<IP>:81/stream   — stream MJPEG contínuo
//    📷 GET  http://<IP>/capture     — frame JPEG único
//    🔌 WS   ws://<IP>/ws            — controle bidirecional (JSON)
//
//  Relay Serial (→ 2º ESP32 motor controller):
//    TX: GPIO 13  →  RX2 do 2º ESP32
//    RX: GPIO 12  ←  TX2 do 2º ESP32
//    Formato: S<steer>T<throttle>\n
//
//  Dependências (Arduino Library Manager):
//    - ArduinoJson 7.x (Benoît Blanchon)
//
//  Configurações do Arduino IDE:
//    Board:            AI Thinker ESP32-CAM
//    Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA)
//    PSRAM:            Enabled
//    Flash Mode:       DIO
//    Upload Speed:     115200
// ============================================================

#include <WiFi.h>
#include "esp_camera.h"

#include "wifi_config.h"
#include "camera_config.h"
#include "http_handlers.h"
#include "ws_handler.h"
#include "sd_recorder.h"

// ----- Serial2 para comunicação com 2º ESP32 (controlador de motores) -----
// No ESP32-CAM (AI-Thinker), GPIO 12 e 13 ficam livres quando SD_MMC
// está em modo 1-bit (modo usado para liberar GPIO 4 como LED Flash).
#define SERIAL2_RX_PIN  12    // Recebe do 2º ESP32 (não usado por enquanto)
#define SERIAL2_TX_PIN  13    // Envia comandos de drive para o 2º ESP32

// =============================================
// Utilitário: piscar LED Flash N vezes
// =============================================
void blinkLED(int times, int onMs = 100, int offMs = 100) {
    for (int i = 0; i < times; i++) {
        digitalWrite(LED_FLASH_GPIO, HIGH);
        delay(onMs);
        digitalWrite(LED_FLASH_GPIO, LOW);
        if (i < times - 1) delay(offMs);
    }
}

// =============================================
// Setup
// =============================================
void setup() {
    // ----- Serial (USB/debug) -----
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    delay(500);   // aguardar estabilização da serial
    Serial.println();
    Serial.println("========================================");
    Serial.println("  ESP32-CAM Carrinho Autonomo ML");
    Serial.println("  Firmware v3.0 — Etapa 2+3+4");
    Serial.println("========================================");

    // ----- Inicializar SD Card (ANTES da câmera) -----
    // O SD_MMC no ESP32-CAM compartilha o GPIO 4 (HS_DATA1) com o LED Flash.
    // Inicializando em modo 1-bit, GPIO 4 fica livre para uso como LED,
    // e GPIO 12/13 ficam livres para Serial2.
    Serial.println("[INIT] Inicializando SD Card...");
    if (!initSDCard()) {
        Serial.println("[INIT] AVISO: SD Card nao disponivel");
        Serial.println("[INIT] Gravacao de dataset desabilitada");
        Serial.println("[INIT] Servidor Wi-Fi continuara funcionando normalmente");
    }

    // ----- LED Flash (desligado por padrão) -----
    pinMode(LED_FLASH_GPIO, OUTPUT);
    digitalWrite(LED_FLASH_GPIO, LOW);
    Serial.println("[INIT] LED Flash configurado (GPIO 4) — desligado");

    // Sinal visual de BOOT: 2 piscadas rápidas
    blinkLED(2);
    Serial.println("[INIT] LED: 2 piscadas — placa ligou");

    // ----- Serial2 para relay ao 2º ESP32 -----
    Serial2.begin(115200, SERIAL_8N1, SERIAL2_RX_PIN, SERIAL2_TX_PIN);
    Serial.printf("[INIT] Serial2 iniciada (TX=GPIO%d, RX=GPIO%d) — relay para motor ESP32\n",
                  SERIAL2_TX_PIN, SERIAL2_RX_PIN);

    // ----- Inicializar câmera -----
    Serial.println("[INIT] Inicializando camera...");
    if (!initCamera()) {
        Serial.println("[INIT] FALHA CRITICA: camera nao inicializou!");
        Serial.println("[INIT] Verifique:");
        Serial.println("  - Cabo flat da camera conectado corretamente");
        Serial.println("  - PSRAM habilitada no Arduino IDE (Tools > PSRAM > Enabled)");
        Serial.println("  - Modelo da placa correto (AI Thinker ESP32-CAM)");

        // Sinalizar erro piscando LED 3 vezes
        for (int i = 0; i < 3; i++) {
            digitalWrite(LED_FLASH_GPIO, HIGH);
            delay(200);
            digitalWrite(LED_FLASH_GPIO, LOW);
            delay(200);
        }

        Serial.println("[INIT] Reiniciando em 5 segundos...");
        delay(5000);
        ESP.restart();
    }

    // ----- Conectar Wi-Fi (modo STA) -----
    Serial.printf("[WIFI] Conectando a \"%s\"", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");

        if (millis() - wifiStart > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println();
            Serial.println("[WIFI] FALHA: timeout de conexao!");
            Serial.println("[WIFI] Verifique:");
            Serial.printf("  - SSID: \"%s\"\n", WIFI_SSID);
            Serial.println("  - Senha correta em wifi_config.h");
            Serial.println("  - Roteador ligado e ao alcance");
            Serial.println("[WIFI] Reiniciando em 5 segundos...");
            delay(5000);
            ESP.restart();
        }
    }

    Serial.println();
    Serial.printf("[WIFI] Conectado! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());

    // Sinal visual de Wi-Fi OK: 4 piscadas rápidas
    blinkLED(4);
    Serial.println("[WIFI] LED: 4 piscadas — Wi-Fi conectado");
    Serial.printf("[WIFI] Hostname: %s\n", WIFI_HOSTNAME);

    // ----- Iniciar servidores -----
    startHTTPServers();
    setupWebSocket(getMainServerHandle());

    // ----- Banner final -----
    String ip = WiFi.localIP().toString();
    Serial.println();
    Serial.println("============================================");
    Serial.println("  Servidor pronto! Endpoints disponiveis:");
    Serial.println("--------------------------------------------");
    Serial.printf("  Stream:  http://%s:81/stream\n", ip.c_str());
    Serial.printf("  Capture: http://%s/capture\n", ip.c_str());
    Serial.printf("  WS:      ws://%s/ws\n", ip.c_str());
    Serial.println("--------------------------------------------");
    Serial.printf("  SD Card: %s\n", isSDCardReady() ? "PRONTO" : "INDISPONIVEL");
    if (isSDCardReady()) {
        Serial.printf("  SD Livre: %llu MB\n", getSDFreeBytes() / (1024 * 1024));
    }
    Serial.printf("  Serial2: TX=GPIO%d  RX=GPIO%d (→ motor ESP32)\n",
                  SERIAL2_TX_PIN, SERIAL2_RX_PIN);
    Serial.println("--------------------------------------------");
    Serial.printf("  Heap livre:  %u bytes\n", ESP.getFreeHeap());
    Serial.printf("  PSRAM livre: %u bytes\n", ESP.getFreePsram());
    Serial.println("============================================");
    Serial.println();
    Serial.println("Comandos WS disponiveis:");
    Serial.println("  {\"cmd\": \"drive\", \"steer\": 0.0, \"throttle\": 0.8}");
    Serial.println("  {\"cmd\": \"rec_start\"}");
    Serial.println("  {\"cmd\": \"rec_stop\"}");
    Serial.println("  {\"cmd\": \"get_status\"}");
    Serial.println("  steer:    -1.0 (esq) a +1.0 (dir)");
    Serial.println("  throttle: -1.0 (re)  a +1.0 (frente)");
    Serial.println();
}

// =============================================
// Loop
// =============================================
void loop() {
    // Verificar conexão Wi-Fi e reconectar se necessário
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 10000) {   // checar a cada 10s
        lastWifiCheck = millis();

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WIFI] Conexao perdida! Reconectando...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

            unsigned long reconnStart = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - reconnStart < WIFI_CONNECT_TIMEOUT_MS) {
                delay(500);
                Serial.print(".");
            }

            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("\n[WIFI] Reconectado! IP: %s\n", WiFi.localIP().toString().c_str());
            } else {
                Serial.println("\n[WIFI] Reconexao falhou, tentando novamente em 10s...");
            }
        }
    }

    // ----- Gravação de frames no SD -----
    // Quando a gravação está ativa, captura frames continuamente
    // e salva no SD com steer/throttle atuais.
    if (isRecording()) {
        recordFrame();
        // Sem delay adicional — a velocidade de gravação no SD
        // naturalmente limita o FPS (tipicamente 10-20 FPS em QVGA)
    }

    // Yield para o FreeRTOS — os handlers HTTP e WS rodam em tasks separadas
    delay(1);   // delay mínimo para não bloquear o scheduler
}
