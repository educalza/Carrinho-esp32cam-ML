// ============================================
// HANDLERS HTTP — Stream MJPEG e Captura
// ============================================
#ifndef HTTP_HANDLERS_H
#define HTTP_HANDLERS_H

#include "esp_http_server.h"

// Inicia dois servidores HTTP:
//   - Porta 80: servidor principal (endpoint /capture + futuro WebSocket)
//   - Porta 81: servidor de stream (endpoint /stream MJPEG)
void startHTTPServers();

// Retorna o handle do servidor principal (porta 80).
// Usado pelo ws_handler para registrar o WebSocket no mesmo servidor.
httpd_handle_t getMainServerHandle();

#endif // HTTP_HANDLERS_H
