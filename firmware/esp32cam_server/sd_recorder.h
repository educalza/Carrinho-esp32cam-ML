// ============================================
// GRAVAÇÃO NO SD CARD — Etapa 3 (v3: rotulo sincronizado)
// Dataset sincronizado (frame JPEG + eixos contínuos)
// ============================================
#ifndef SD_RECORDER_H
#define SD_RECORDER_H

#include <Arduino.h>
#include "drive_history.h"

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

// Finaliza a sessao: aguarda escrita pendente e fecha o arquivo CSV.
// Os contadores finais permanecem disponiveis para o status.
void stopRecording();

// Retorna true se uma sessão de gravação está ativa.
bool isRecording();

// ----- Gravação de frame + comando -----

// Captura um frame da câmera, salva o JPEG no SD e registra uma linha
// no log.csv com o timestamp da captura e o ultimo comando recebido ate
// esse instante. Frames sem comando ou com comando >300 ms sao descartados.
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

// Frames descartados por falta de rotulo sincronizado na sessao.
uint32_t getRejectedFrameCount();

// Número da sessão atual (ou a última criada).
uint16_t getCurrentSessionNumber();

// Espaço livre no SD card em bytes.
uint64_t getSDFreeBytes();

// Espaço total do SD card em bytes.
uint64_t getSDTotalBytes();

// Retorna true se o SD card foi inicializado com sucesso.
bool isSDCardReady();

#endif // SD_RECORDER_H
