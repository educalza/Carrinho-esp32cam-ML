// ============================================
// GRAVAÇÃO NO SD CARD — Etapa 3 (v2: steer+throttle)
// Dataset sincronizado (frame JPEG + eixos contínuos)
// ============================================
#ifndef SD_RECORDER_H
#define SD_RECORDER_H

#include <Arduino.h>

// ----- Estado de direção contínuo -----
// Representa os 2 eixos do analógico capturados no instante de cada frame.
// Usados para rotular cada imagem no CSV de treino.
struct DriveState {
    float steer;     // -1.0 (esquerda máx) a +1.0 (direita máx)
    float throttle;  // -1.0 (ré máx) a +1.0 (frente máx)
};

// ----- Inicialização do SD Card -----
// Monta o sistema de arquivos SD_MMC em modo 1-bit.
// DEVE ser chamado ANTES de initCamera() no setup(), porque o GPIO 4
// (LED Flash / HS_DATA1 do SD) precisa ser reconfigurado após a montagem.
// Retorna true se o cartão foi montado com sucesso.
bool initSDCard();

// ----- Controle de sessão de gravação -----

// Cria uma nova pasta /sdcard/session_NNN e abre o arquivo log.csv.
// Retorna true se a sessão foi criada com sucesso.
bool startRecording();

// Finaliza a sessão: fecha o arquivo CSV e reseta contadores.
void stopRecording();

// Retorna true se uma sessão de gravação está ativa.
bool isRecording();

// ----- Gravação de frame + comando -----

// Captura um frame da câmera, salva o JPEG no SD e registra uma linha
// no log.csv com o timestamp, steer e throttle atuais.
// Só faz algo se isRecording() == true.
// Retorna true se o frame foi salvo com sucesso.
bool recordFrame();

// ----- Estado do comando de direção -----

// Atualiza os eixos de direção. Chamado pelo ws_handler quando
// recebe um comando "drive" do PC.
void setCurrentDrive(float steer, float throttle);

// Retorna o estado de direção ativo no momento.
DriveState getCurrentDrive();

// ----- Informações de status -----

// Número do frame atual na sessão ativa.
uint32_t getRecordedFrameCount();

// Número da sessão atual (ou a última criada).
uint16_t getCurrentSessionNumber();

// Espaço livre no SD card em bytes.
uint64_t getSDFreeBytes();

// Espaço total do SD card em bytes.
uint64_t getSDTotalBytes();

// Retorna true se o SD card foi inicializado com sucesso.
bool isSDCardReady();

#endif // SD_RECORDER_H
