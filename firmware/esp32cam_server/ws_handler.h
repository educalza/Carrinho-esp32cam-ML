// ============================================
// HANDLER WEBSOCKET — Controle bidirecional
// ============================================
#ifndef WS_HANDLER_H
#define WS_HANDLER_H

#include "esp_http_server.h"

// Registra o handler WebSocket no endpoint /ws do servidor HTTP fornecido.
// Deve ser chamado APÓS startHTTPServers(), passando getMainServerHandle().
void setupWebSocket(httpd_handle_t server);

#endif // WS_HANDLER_H
