// ============================================
// IMPLEMENTAÇÃO — Gravação no SD Card (Etapa 3 v2)
// ============================================
//
// Usa SD_MMC em modo 1-bit para evitar conflito com LED Flash (GPIO 4).
//
// Estrutura no cartão:
//   /sdcard/session_001/
//       frame_00001.jpg
//       frame_00002.jpg
//       log.csv
//   /sdcard/session_002/
//       ...
//
// Formato do log.csv (v2 — eixos contínuos):
//   timestamp_ms,frame,steer,throttle
//   12345,frame_00001.jpg,0.00,0.80
//   12410,frame_00002.jpg,-0.45,0.75
//
// ============================================

#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "esp_camera.h"
#include "sd_recorder.h"

// ----- Estado interno -----
static bool       sdReady       = false;
static bool       recording     = false;
static DriveState currentDrive  = {0.0f, 0.0f};
static uint32_t   frameCount    = 0;
static uint16_t   sessionNumber = 0;
static File       csvFile;

// Caminho da sessão ativa (ex: "/session_042")
static char sessionPath[32];

// =============================================
// Inicialização do SD Card
// =============================================

bool initSDCard() {
    Serial.println("[SD] Inicializando SD_MMC (modo 1-bit)...");

    // Modo 1-bit: usa apenas GPIO 2 (DATA0), GPIO 14 (CLK), GPIO 15 (CMD)
    // Isso libera o GPIO 4 (HS_DATA1) para uso como LED Flash.
    // Também libera GPIO 12 e 13 para Serial2 (comunicação com 2º ESP32).
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("[SD] ERRO: falha ao montar SD_MMC!");
        Serial.println("[SD] Verifique:");
        Serial.println("  - Cartao MicroSD inserido corretamente");
        Serial.println("  - Formatado em FAT32");
        Serial.println("  - Contatos limpos");
        sdReady = false;
        return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
        Serial.println("[SD] ERRO: nenhum cartao detectado!");
        sdReady = false;
        return false;
    }

    // Informações do cartão
    const char* typeStr = "DESCONHECIDO";
    switch (cardType) {
        case CARD_MMC:  typeStr = "MMC";    break;
        case CARD_SD:   typeStr = "SD";     break;
        case CARD_SDHC: typeStr = "SDHC";   break;
    }

    uint64_t totalBytes = SD_MMC.totalBytes();
    uint64_t usedBytes  = SD_MMC.usedBytes();

    Serial.printf("[SD] Cartao montado! Tipo: %s\n", typeStr);
    Serial.printf("[SD] Total: %llu MB | Usado: %llu MB | Livre: %llu MB\n",
                  totalBytes / (1024 * 1024),
                  usedBytes  / (1024 * 1024),
                  (totalBytes - usedBytes) / (1024 * 1024));

    // Descobrir o próximo número de sessão disponível
    // Varrer /sdcard/ procurando pastas session_NNN existentes
    sessionNumber = 0;
    File root = SD_MMC.open("/");
    if (root) {
        File entry;
        while ((entry = root.openNextFile())) {
            const char* name = entry.name();
            // entry.name() retorna o caminho sem "/" inicial em algumas versões
            // Procurar por "session_" no nome
            const char* prefix = strstr(name, "session_");
            if (prefix && entry.isDirectory()) {
                uint16_t num = (uint16_t)atoi(prefix + 8);   // pular "session_"
                if (num > sessionNumber) {
                    sessionNumber = num;
                }
            }
            entry.close();
        }
        root.close();
    }

    Serial.printf("[SD] Ultima sessao encontrada: %u\n", sessionNumber);

    sdReady = true;
    return true;
}

// =============================================
// Controle de sessão
// =============================================

bool startRecording() {
    if (!sdReady) {
        Serial.println("[SD] ERRO: cartao nao inicializado!");
        return false;
    }

    if (recording) {
        Serial.println("[SD] AVISO: ja esta gravando! Parando sessao anterior...");
        stopRecording();
    }

    // Incrementar número de sessão
    sessionNumber++;
    frameCount = 0;

    // Criar pasta da sessão
    snprintf(sessionPath, sizeof(sessionPath), "/session_%03u", sessionNumber);

    if (!SD_MMC.mkdir(sessionPath)) {
        Serial.printf("[SD] ERRO: falha ao criar pasta %s\n", sessionPath);
        sessionNumber--;
        return false;
    }

    // Abrir arquivo CSV
    char csvPath[48];
    snprintf(csvPath, sizeof(csvPath), "%s/log.csv", sessionPath);

    csvFile = SD_MMC.open(csvPath, FILE_WRITE);
    if (!csvFile) {
        Serial.printf("[SD] ERRO: falha ao criar %s\n", csvPath);
        sessionNumber--;
        return false;
    }

    // Escrever cabeçalho do CSV (v2: eixos contínuos)
    csvFile.println("timestamp_ms,frame,steer,throttle");
    csvFile.flush();

    recording = true;

    Serial.println("========================================");
    Serial.printf("[SD] GRAVACAO INICIADA — Sessao %03u\n", sessionNumber);
    Serial.printf("[SD] Pasta: %s\n", sessionPath);
    Serial.printf("[SD] Espaco livre: %llu MB\n",
                  (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024));
    Serial.println("========================================");

    return true;
}

void stopRecording() {
    if (!recording) {
        Serial.println("[SD] AVISO: nenhuma gravacao ativa para parar");
        return;
    }

    recording = false;

    // Fechar CSV
    if (csvFile) {
        csvFile.flush();
        csvFile.close();
    }

    Serial.println("========================================");
    Serial.printf("[SD] GRAVACAO ENCERRADA — Sessao %03u\n", sessionNumber);
    Serial.printf("[SD] Total de frames salvos: %u\n", frameCount);
    Serial.printf("[SD] Espaco livre: %llu MB\n",
                  (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024));
    Serial.println("========================================");
}

bool isRecording() {
    return recording;
}

// =============================================
// Gravação de frame + log
// =============================================

bool recordFrame() {
    if (!recording || !sdReady) return false;

    // Capturar frame da câmera
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[SD] ERRO: captura do frame falhou!");
        return false;
    }

    frameCount++;
    unsigned long timestamp = millis();

    // Montar nome do arquivo de imagem
    char frameName[24];
    snprintf(frameName, sizeof(frameName), "frame_%05u.jpg", frameCount);

    char framePath[64];
    snprintf(framePath, sizeof(framePath), "%s/%s", sessionPath, frameName);

    // Salvar JPEG no SD
    File imgFile = SD_MMC.open(framePath, FILE_WRITE);
    if (!imgFile) {
        Serial.printf("[SD] ERRO: falha ao criar %s\n", framePath);
        esp_camera_fb_return(fb);
        frameCount--;
        return false;
    }

    size_t bytesWritten = imgFile.write(fb->buf, fb->len);
    imgFile.close();

    if (bytesWritten != fb->len) {
        Serial.printf("[SD] ERRO: escrita incompleta (%u de %u bytes)\n",
                      bytesWritten, fb->len);
        esp_camera_fb_return(fb);
        return false;
    }

    // Registrar linha no CSV (v2: steer + throttle contínuos)
    DriveState driveSnapshot = currentDrive;   // snapshot atômico
    if (csvFile) {
        csvFile.printf("%lu,%s,%.2f,%.2f\n",
                       timestamp, frameName,
                       driveSnapshot.steer, driveSnapshot.throttle);

        // Flush a cada 10 frames para equilibrar performance vs. segurança
        if (frameCount % 10 == 0) {
            csvFile.flush();
        }
    }

    // Log periódico a cada 50 frames
    if (frameCount % 50 == 0) {
        Serial.printf("[SD] Sessao %03u: %u frames | steer=%.2f throttle=%.2f | %u bytes\n",
                      sessionNumber, frameCount,
                      driveSnapshot.steer, driveSnapshot.throttle, fb->len);
    }

    esp_camera_fb_return(fb);
    return true;
}

// =============================================
// Getters / Setters
// =============================================

void setCurrentDrive(float steer, float throttle) {
    currentDrive.steer    = constrain(steer, -1.0f, 1.0f);
    currentDrive.throttle = constrain(throttle, -1.0f, 1.0f);
}

DriveState getCurrentDrive() {
    return currentDrive;
}

uint32_t getRecordedFrameCount() {
    return frameCount;
}

uint16_t getCurrentSessionNumber() {
    return sessionNumber;
}

uint64_t getSDFreeBytes() {
    if (!sdReady) return 0;
    return SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

uint64_t getSDTotalBytes() {
    if (!sdReady) return 0;
    return SD_MMC.totalBytes();
}

bool isSDCardReady() {
    return sdReady;
}
