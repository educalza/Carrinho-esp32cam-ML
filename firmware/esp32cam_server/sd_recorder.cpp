// ============================================
// IMPLEMENTAÇÃO — Gravação no SD Card (Etapa 3 v3)
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
// Formato do log.csv (v3 — comando alinhado ao inicio da captura):
//   timestamp_ms,frame,steer,throttle,command_timestamp_ms,command_age_ms,label_valid
//   12345,frame_00001.jpg,0.00,0.80,12330,15,1
//
// ============================================

#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include <math.h>
#include "sd_recorder.h"

// ----- Estado interno -----
static bool       sdReady       = false;
static std::atomic<bool> recording{false};
static std::atomic<uint32_t> frameCount{0};
static std::atomic<uint32_t> rejectedFrameCount{0};
static std::atomic<uint16_t> sessionNumber{0};
static File       csvFile;
static SemaphoreHandle_t recordingMutex = nullptr;
static portMUX_TYPE driveMux = portMUX_INITIALIZER_UNLOCKED;
static DriveHistory driveHistory;
static uint64_t recordingStartUs = 0;  // protegido por recordingMutex

// Mutex de tarefa: o SD pode bloquear, mas nunca dentro de critical section.
// Uma chamada rec_stop aguarda a escrita em andamento antes de fechar o CSV.
class RecordingLock {
public:
    RecordingLock() { xSemaphoreTake(recordingMutex, portMAX_DELAY); }
    ~RecordingLock() { xSemaphoreGive(recordingMutex); }
    RecordingLock(const RecordingLock &) = delete;
    RecordingLock &operator=(const RecordingLock &) = delete;
};

static void closeRecordingLocked() {
    recording.store(false);
    if (csvFile) {
        csvFile.flush();
        csvFile.close();
    }
}

// Caminho da sessão ativa (ex: "/session_042")
static char sessionPath[32];

// =============================================
// Inicialização do SD Card
// =============================================

bool initSDCard() {
    // setup() chama antes de iniciar as tarefas HTTP/WS.
    if (!recordingMutex) recordingMutex = xSemaphoreCreateMutex();
    if (!recordingMutex) {
        Serial.println("[SD] ERRO: memoria insuficiente para mutex de gravacao");
        return false;
    }
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

    Serial.printf("[SD] Ultima sessao encontrada: %u\n", sessionNumber.load());

    sdReady = true;
    return true;
}

// =============================================
// Controle de sessão
// =============================================

bool startRecording() {
    sensor_t* sensor = esp_camera_sensor_get();
    if (!sensor || sensor->status.framesize != FRAMESIZE_QVGA) {
        Serial.println("[SD] Gravacao exige camera em QVGA (320x240).");
        return false;
    }
    if (!sdReady) {
        Serial.println("[SD] ERRO: cartao nao inicializado!");
        return false;
    }

    RecordingLock lock;

    if (recording) {
        Serial.println("[SD] AVISO: ja esta gravando! Parando sessao anterior...");
        closeRecordingLocked();
    }

    // Incrementar número de sessão
    sessionNumber++;
    frameCount = 0;
    rejectedFrameCount = 0;

    // Criar pasta da sessão
    snprintf(sessionPath, sizeof(sessionPath), "/session_%03u", sessionNumber.load());

    if (!SD_MMC.mkdir(sessionPath)) {
        Serial.printf("[SD] ERRO: falha ao criar pasta %s\n", sessionPath);
        return false;
    }

    // Abrir arquivo CSV
    char csvPath[48];
    snprintf(csvPath, sizeof(csvPath), "%s/log.csv", sessionPath);

    csvFile = SD_MMC.open(csvPath, FILE_WRITE);
    if (!csvFile) {
        Serial.printf("[SD] ERRO: falha ao criar %s\n", csvPath);
        return false;
    }

    // As quatro primeiras colunas continuam compativeis com datasets v2.
    if (csvFile.println("timestamp_ms,frame,steer,throttle,command_timestamp_ms,command_age_ms,label_valid") == 0) {
        closeRecordingLocked();
        return false;
    }
    csvFile.flush();

    recordingStartUs = static_cast<uint64_t>(esp_timer_get_time());
    recording = true;

    Serial.println("========================================");
    Serial.printf("[SD] GRAVACAO INICIADA — Sessao %03u\n", sessionNumber.load());
    Serial.printf("[SD] Pasta: %s\n", sessionPath);
    Serial.printf("[SD] Espaco livre: %llu MB\n",
                  (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024));
    Serial.println("========================================");

    return true;
}

void stopRecording() {
    if (!recordingMutex) return;
    RecordingLock lock;
    if (!recording) {
        Serial.println("[SD] AVISO: nenhuma gravacao ativa para parar");
        return;
    }

    closeRecordingLocked();

    Serial.println("========================================");
    Serial.printf("[SD] GRAVACAO ENCERRADA — Sessao %03u\n", sessionNumber.load());
    Serial.printf("[SD] Frames salvos: %u | Sem rotulo sincronizado: %u\n",
                  frameCount.load(), rejectedFrameCount.load());
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
    const uint16_t captureSession = sessionNumber.load();

    // A captura pode aguardar a camera. Nao impedir rec_stop nesse intervalo.
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[SD] ERRO: captura do frame falhou!");
        return false;
    }

    const uint64_t captureUs = static_cast<uint64_t>(fb->timestamp.tv_sec) * 1000000ULL
                             + static_cast<uint64_t>(fb->timestamp.tv_usec);
    DriveSample label = {};
    portENTER_CRITICAL(&driveMux);
    const bool hasLabel = driveHistory.sampleAt(captureUs, label);
    portEXIT_CRITICAL(&driveMux);

    RecordingLock lock;
    // Nao deixar um frame pendente entrar em outra sessao ou depois de stop.
    if (!recording || captureSession != sessionNumber.load()) {
        esp_camera_fb_return(fb);
        return false;
    }
    if (!hasLabel || captureUs < recordingStartUs || fb->format != PIXFORMAT_JPEG ||
        fb->width != 320 || fb->height != 240) {
        ++rejectedFrameCount;
        esp_camera_fb_return(fb);
        return false;
    }
    const uint32_t nextFrame = frameCount.load() + 1;

    // Montar nome do arquivo de imagem
    char frameName[24];
    snprintf(frameName, sizeof(frameName), "frame_%05u.jpg", nextFrame);

    char framePath[64];
    snprintf(framePath, sizeof(framePath), "%s/%s", sessionPath, frameName);

    // Salvar JPEG no SD
    File imgFile = SD_MMC.open(framePath, FILE_WRITE);
    if (!imgFile) {
        Serial.printf("[SD] ERRO: falha ao criar %s\n", framePath);
        esp_camera_fb_return(fb);
        return false;
    }

    size_t bytesWritten = imgFile.write(fb->buf, fb->len);
    imgFile.close();

    if (bytesWritten != fb->len) {
        Serial.printf("[SD] ERRO: escrita incompleta (%u de %u bytes)\n",
                      static_cast<unsigned>(bytesWritten), static_cast<unsigned>(fb->len));
        SD_MMC.remove(framePath);
        esp_camera_fb_return(fb);
        return false;
    }

    // O rotulo foi escolhido ANTES da escrita, pelo timestamp do frame.
    char row[160];
    const int rowLength = snprintf(row, sizeof(row), "%llu,%s,%.2f,%.2f,%llu,%llu,1\n",
        static_cast<unsigned long long>(captureUs / 1000), frameName,
        label.drive.steer, label.drive.throttle,
        static_cast<unsigned long long>(label.timestampUs / 1000),
        static_cast<unsigned long long>((captureUs - label.timestampUs) / 1000));
    if (rowLength <= 0 || static_cast<size_t>(rowLength) >= sizeof(row) || !csvFile ||
        csvFile.write(reinterpret_cast<const uint8_t *>(row), rowLength) != static_cast<size_t>(rowLength)) {
        Serial.println("[SD] ERRO: escrita do CSV falhou; encerrando sessao");
        SD_MMC.remove(framePath);
        closeRecordingLocked();
        esp_camera_fb_return(fb);
        return false;
    }
    frameCount.store(nextFrame);

    // Flush a cada 10 frames para equilibrar performance vs. seguranca.
    if (nextFrame % 10 == 0) csvFile.flush();

    // Log periódico a cada 50 frames
    if (nextFrame % 50 == 0) {
        Serial.printf("[SD] Sessao %03u: %u frames | steer=%.2f throttle=%.2f | %u bytes\n",
                      sessionNumber.load(), nextFrame,
                      label.drive.steer, label.drive.throttle, static_cast<unsigned>(fb->len));
    }

    esp_camera_fb_return(fb);
    return true;
}

// =============================================
// Getters / Setters
// =============================================

void setCurrentDrive(float steer, float throttle) {
    if (!isfinite(steer) || !isfinite(throttle)) return;
    const DriveState drive = {
        constrain(steer, -1.0f, 1.0f), constrain(throttle, -1.0f, 1.0f)
    };
    portENTER_CRITICAL(&driveMux);
    driveHistory.push(drive, static_cast<uint64_t>(esp_timer_get_time()));
    portEXIT_CRITICAL(&driveMux);
}

DriveState getCurrentDrive() {
    portENTER_CRITICAL(&driveMux);
    const DriveState result = driveHistory.latest();
    portEXIT_CRITICAL(&driveMux);
    return result;
}

uint32_t getRecordedFrameCount() {
    return frameCount;
}

uint32_t getRejectedFrameCount() {
    return rejectedFrameCount;
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
