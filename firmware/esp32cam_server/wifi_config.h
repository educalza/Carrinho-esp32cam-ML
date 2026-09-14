// ============================================
// CONFIGURAÇÃO WI-FI — ALTERE ANTES DE COMPILAR
// ============================================
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

// Credenciais da rede doméstica
#define WIFI_SSID               "Edilson Ligga 2.4g"          // ← substitua pelo nome da sua rede
#define WIFI_PASSWORD           "z1w1w2g1"          // ← substitua pela senha da sua rede

// Timeout de conexão (ms)
#define WIFI_CONNECT_TIMEOUT_MS  10000               // 10 segundos

// Hostname na rede local (aparece no roteador)
#define WIFI_HOSTNAME           "esp32cam-carrinho"

#endif // WIFI_CONFIG_H
