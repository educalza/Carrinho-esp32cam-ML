# 📌 Mapa de Pinagem — Carrinho Autônomo ML

---

## Board 1: ESP32-CAM (AI-Thinker)

> Responsável por: **Câmera, Wi-Fi, gravação SD, relay serial**

### Pinos Ocupados pela Câmera (NÃO MEXER)

| GPIO | Função |
|------|--------|
| 0 | XCLK (clock da câmera) |
| 5 | D0 (dado câmera Y2) |
| 18 | D1 (dado câmera Y3) |
| 19 | D2 (dado câmera Y4) |
| 21 | D3 (dado câmera Y5) |
| 36 | D4 (dado câmera Y6) |
| 39 | D5 (dado câmera Y7) |
| 34 | D6 (dado câmera Y8) |
| 35 | D7 (dado câmera Y9) |
| 25 | VSYNC |
| 23 | HREF |
| 22 | PCLK |
| 26 | SIOD (I2C SDA do sensor) |
| 27 | SIOC (I2C SCL do sensor) |
| 32 | PWDN (power down câmera) |

### Pinos Ocupados pelo SD Card (modo 1-bit)

| GPIO | Função |
|------|--------|
| 2 | SD_MMC DATA0 |
| 14 | SD_MMC CLK |
| 15 | SD_MMC CMD |

### Pinos LIVRES (usados pelo projeto)

| GPIO | Função no Projeto | Conecta a |
|------|-------------------|-----------|
| **4** | LED Flash | — (integrado na placa) |
| **13** | Serial2 TX para 2º ESP32 | GPIO 16 (RX2) do 2º ESP32 |
| **12** | Serial2 RX do 2º ESP32 | GPIO 17 (TX2) do 2º ESP32 |

### Pinos reservados pelo sistema (NÃO USAR)

| GPIO | Motivo |
|------|--------|
| 1 | TX0 (Serial USB — debug/upload) |
| 3 | RX0 (Serial USB — debug/upload) |
| 16 | Conectado à PSRAM (indisponível) |

AVISO - GPIO 12: Este pino controla a voltagem do flash (VDD_SDIO).
Se estiver HIGH durante o boot, o ESP32 pode não iniciar.
Desconecte o fio do GPIO 12 na hora de dar upload.

---

## Board 2: ESP32 DevKit V1 (Controlador de Motores)

> Responsável por: **Servo de direção, 2 motores DC via ponte H L298N**

### Pinos usados pelo projeto

| GPIO | Função no Projeto | Conecta a |
|------|-------------------|-----------|
| **16** | Serial2 RX (do ESP32-CAM) | GPIO 13 (TX2) do ESP32-CAM |
| **17** | Serial2 TX (para ESP32-CAM) | GPIO 12 (RX2) do ESP32-CAM |
| **18** | Servo PWM (sinal) | Fio de sinal do servo |
| **25** | Motor A — ENA (PWM velocidade) | Pino ENA da ponte H L298N |
| **26** | Motor A — IN1 (direção) | Pino IN1 da ponte H L298N |
| **27** | Motor A — IN2 (direção) | Pino IN2 da ponte H L298N |
| **32** | Motor B — ENB (PWM velocidade) | Pino ENB da ponte H L298N |
| **33** | Motor B — IN3 (direção) | Pino IN3 da ponte H L298N |
| **14** | Motor B — IN4 (direção) | Pino IN4 da ponte H L298N |

### Pinos LIVRES (disponíveis para expansão futura)

| GPIO | Notas |
|------|-------|
| 2 | LED integrado na maioria dos DevKits |
| 4, 5, 12, 13, 15, 19, 21, 22, 23 | Disponíveis para sensores, LEDs, etc. |
| 34, 35, 36, 39 | Somente entrada (bons para sensores analógicos) |

---

## Diagrama de Fiação Completo

```
 ESP32-CAM (AI-Thinker)
 ──────────────────────
 [Câmera CSI] - Cabo flat (já conectado)
 [MicroSD]    - GPIO 2/14/15 (SD_MMC 1-bit)
 [LED Flash]  - GPIO 4

 GPIO 13 (TX2) ──── fio azul ────► GPIO 16 (RX2) ─┐
 GPIO 12 (RX2) ◄─── fio verde ─── GPIO 17 (TX2) ──┤
 GND ────────────── fio preto ───► GND ────────────┤
                                                    │
                   ESP32 DevKit V1 (Motor Controller)
                   ──────────────────────────────────
                   GPIO 18 ── fio laranja ──► SERVO (sinal)
                                              SERVO VCC ◄── 5V
                                              SERVO GND ◄── GND

                   GPIO 25 ── fio amarelo ──► L298N ENA
                   GPIO 26 ── fio amarelo ──► L298N IN1
                   GPIO 27 ── fio amarelo ──► L298N IN2

                   GPIO 32 ── fio branco ───► L298N ENB
                   GPIO 33 ── fio branco ───► L298N IN3
                   GPIO 14 ── fio branco ───► L298N IN4

                   GND ───── fio preto ─────► L298N GND ◄── Bateria GND


 Ponte H L298N
 ─────────────
 12V (VMS)   ◄── Bateria Li-ion 2S (~7.4V)
 GND         ◄── GND da bateria
 5V (saída)  ──► Pode alimentar os ESP32s (via VIN)

 OUT1 / OUT2 ──► Motor DC Esquerdo (Motor A)
 OUT3 / OUT4 ──► Motor DC Direito  (Motor B)

 ⚠️  Remover os jumpers dos pinos ENA e ENB para
     usar PWM do ESP32 em vez de velocidade fixa!
```

---

## Lembretes Importantes

1. **Jumpers ENA/ENB do L298N**: O módulo vem com jumpers que fixam
   velocidade em 100%. REMOVA-OS e conecte os fios dos GPIO 25 e 32.

2. **GND compartilhado**: O GND da bateria, do L298N e dos dois ESP32s
   devem ser TODOS interligados. Sem GND comum os sinais não funcionam.

3. **GPIO 12 no boot**: Desconecte o fio do GPIO 12 do ESP32-CAM antes
   de fazer upload via USB. Reconecte depois.

---

## Resumo Rápido (Cola para Bancada)

```
ESP32-CAM           2º ESP32 DevKit
─────────           ───────────────
GPIO 13 (TX) ─────► GPIO 16 (RX)     Serial
GPIO 12 (RX) ◄───── GPIO 17 (TX)     Serial
GND ─────────────── GND               GND comum

                    GPIO 18 ──► Servo (sinal)
                    GPIO 25 ──► L298N ENA
                    GPIO 26 ──► L298N IN1
                    GPIO 27 ──► L298N IN2
                    GPIO 32 ──► L298N ENB
                    GPIO 33 ──► L298N IN3
                    GPIO 14 ──► L298N IN4
```
