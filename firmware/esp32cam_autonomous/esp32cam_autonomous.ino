// ============================================================================
//  ESP32-CAM Carrinho Autônomo — Inferência INT8 com parada supervisionada
// ============================================================================
//
//  Configuração de inferência (tempos devem ser medidos na placa):
//    1. Câmera em QVGA (320x240 Grayscale) com campo de visão completo (sem crop).
//    2. Pré-processamento por Lookup Table (LUT) inteira pré-computada.
//    3. Modelo TinyML INT8; direção aprendida sem atenuação artificial.
//    4. Tensor Arena de 25 KB, preferencialmente na SRAM interna.
//    5. Taxa de envio de dados contínua para o 2º ESP32 a cada frame.
// ============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdarg.h>
#include "esp_camera.h"
#include "camera_pins.h"
#include "modelo_linha.h"

// TensorFlow Lite Micro Includes para ESP32
#include <tflm_esp32.h>
#include "driving_policy.h"
#include "optimized_conv.h"
#include "camera_profile.h"
#include "camera_timing.h"
#include "preprocessing.h"
#include "performance_options.h"

// -------------------------------------------------------------
// Constantes da Rede Neural
// -------------------------------------------------------------
constexpr int MODEL_INPUT_WIDTH = car_ml::kInputWidth;
constexpr int MODEL_INPUT_HEIGHT = car_ml::kInputHeight;
constexpr int MODEL_INPUT_SIZE = car_ml::kInputSize;

// Tamanho da Tensor Arena na SRAM interna (apenas 25 KB para a nova CNN)
const int kTensorArenaSize = 25 * 1024;
uint8_t* tensor_arena = nullptr;

// Ponteiros TFLite
const tflite::Model* tflite_model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* model_input = nullptr;
TfLiteTensor* model_output = nullptr;

// -------------------------------------------------------------
// Tabelas de Pré-processamento Pré-computadas (Lookup Tables)
// -------------------------------------------------------------
static car_ml::PreprocessingTables preprocessingTables;
// Interpreter keeps pointers into this immutable copy throughout its lifetime.
static uint8_t* modelInternal = nullptr;
static DriveGate driveGate;
static car_camera::Timing cameraTiming;
static bool cameraFastActive=false;

struct StopSnapshot {
    uint32_t magic;
    uint32_t uptimeMs;
    int64_t periodUs, ageUs, inferUs;
    int32_t rows;
    float steer, throttle;
    char reason[80];
    char phase[20];
};
static StopSnapshot stopSnapshot = {};
static bool haveStopSnapshot=false, stopDirty=false, stopEpisode=false, hasMoved=false;
static bool persistAttempted=false;
static uint32_t lastPersistAttempt=0;
static int64_t diagnosticPeriodUs=0, diagnosticAgeUs=-1, diagnosticInferUs=-1;
static float lastMovingSteer=0, lastMovingThrottle=0;

// Diagnostic output must never wait for UART space in the driving loop.
void diagnostic(const char* format, ...) {
    char buffer[224];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0 && length < (int)sizeof(buffer) && Serial.availableForWrite() >= length)
        Serial.write((const uint8_t*)buffer, length);
}

void reportCameraTiming() {
    static uint32_t lastLog=0;
    const uint32_t now=millis();
    if (now-lastLog<2000) return;
    lastLog=now;
    const auto& w=cameraTiming.window();
    if (!w.frames && !w.failures && !w.invalid) return;
    const double divisor=w.frames ? double(w.frames)*1000 : 1;
    diagnostic("[CAM] perfil=%s n=%lu get=%.1fms inicio_rel=%.1fms entrega=%.1fms max=%.1fms dt=%.1fms falhas=%lu invalidos=%lu atrasados=%lu\n",
        cameraFastActive ? "rapido" : "padrao",(unsigned long)w.frames,
        w.frames ? w.getUs/divisor : -1.0,w.frames ? w.startRelativeUs/divisor : -1.0,
        w.frames ? w.deliveryAgeUs/divisor : -1.0,w.frames ? w.maxDeliveryAgeUs/1000.0 : -1.0,
        w.gaps ? w.frameGapUs/(double(w.gaps)*1000) : -1.0,
        (unsigned long)w.failures,(unsigned long)w.invalid,(unsigned long)w.late);
    cameraTiming.clearWindow();
}

// Writes happen only after zero throttle, at most once per five seconds.
// Separate NVS namespace survives normal power cycling and firmware uploads.
void persistStopSnapshot() {
    if (!stopDirty || driveGate.armed()) return;
    const uint32_t now=millis();
    if (persistAttempted && now-lastPersistAttempt<5000) return;
    persistAttempted=true; lastPersistAttempt=now;
    Preferences preferences;
    if (preferences.begin("ml_diag",false)) {
        if (preferences.putBytes("laststop",&stopSnapshot,sizeof(stopSnapshot))==sizeof(stopSnapshot)) stopDirty=false;
        preferences.end();
    }
    if (stopDirty) diagnostic("[DIAG] falha ao salvar parada; nova tentativa enquanto parado\n");
}
void captureStopSnapshot(const char* reason) {
    if (!hasMoved || stopEpisode) return; // Preserve the first cause of this stop episode.
    stopEpisode=true;
    stopSnapshot={};
    stopSnapshot.magic=0x4D4C5301;
    stopSnapshot.uptimeMs=millis();
    stopSnapshot.periodUs=diagnosticPeriodUs;
    stopSnapshot.ageUs=diagnosticAgeUs;
    stopSnapshot.inferUs=diagnosticInferUs;
    stopSnapshot.rows=-1; // Detector desativado; preserva o formato dos registros antigos.
    stopSnapshot.steer=lastMovingSteer;
    stopSnapshot.throttle=lastMovingThrottle;
    snprintf(stopSnapshot.reason,sizeof(stopSnapshot.reason),"%s",reason);
    snprintf(stopSnapshot.phase,sizeof(stopSnapshot.phase),"%s","ml_puro");
    haveStopSnapshot=stopDirty=true;
}
void printStopSnapshot() {
    if (!haveStopSnapshot) { Serial.println("[DIAG] Nenhuma parada apos movimento registrada."); return; }
    Serial.printf("[DIAG] Ultima parada%s: %s | fase=%s | instante=%lums\n",
                  stopDirty?" (ainda nao salva)":" salva",stopSnapshot.reason,stopSnapshot.phase,
                  (unsigned long)stopSnapshot.uptimeMs);
    char rows[16];
    if (stopSnapshot.rows < 0) snprintf(rows,sizeof(rows),"n/a");
    else snprintf(rows,sizeof(rows),"%ld/4",(long)stopSnapshot.rows);
    Serial.printf("[DIAG] periodo=%.1fms idade=%.1fms infer=%.1fms linha=%s ultimo S=%.3f T=%.3f\n",
                  stopSnapshot.periodUs/1000.0,stopSnapshot.ageUs<0 ? -1.0 : stopSnapshot.ageUs/1000.0,
                  stopSnapshot.inferUs<0 ? -1.0 : stopSnapshot.inferUs/1000.0,
                  rows,stopSnapshot.steer,stopSnapshot.throttle);
}
void loadStopSnapshot() {
    Preferences preferences;
    if (preferences.begin("ml_diag",true)) {
        if (preferences.getBytesLength("laststop")==sizeof(stopSnapshot) &&
            preferences.getBytes("laststop",&stopSnapshot,sizeof(stopSnapshot))==sizeof(stopSnapshot) &&
            stopSnapshot.magic==0x4D4C5301) {
            stopSnapshot.reason[sizeof(stopSnapshot.reason)-1]=0;
            stopSnapshot.phase[sizeof(stopSnapshot.phase)-1]=0;
            haveStopSnapshot=true;
        }
        preferences.end();
    }
    printStopSnapshot();
}

void sendDrive(float steer, float throttle) {
    Serial2.printf("S%.3fT%.3f\n", steer, throttle);
    if (throttle!=0) {
        hasMoved=true; stopEpisode=false;
        lastMovingSteer=steer; lastMovingThrottle=throttle;
    }
}

void stopDriving(const char* reason) {
    const bool wasArmed = driveGate.armed();
    sendDrive(0, 0);
    captureStopSnapshot(reason);
    driveGate.stop();
    persistStopSnapshot();
    static unsigned long lastLog = 0;
    if (wasArmed || millis() - lastLog >= 1000) {
        diagnostic("[STOP] %s | bloqueado; enviar START apos recuperar\n", reason);
        lastLog = millis();
    }
}

void pauseDriving(const char* reason) {
    const bool wasArmed = driveGate.armed();
    sendDrive(0, 0);
    captureStopSnapshot(reason);
    driveGate.pause();
    persistStopSnapshot();
    static unsigned long lastLog = 0;
    if (wasArmed || millis() - lastLog >= 1000) {
        diagnostic("[PAUSA] %s | %s\n", reason,
                   driveGate.pending() ? "aguardando 3 decisoes validas" : "START necessario");
        lastLog = millis();
    }
}

// Leitura limitada; comandos inteiros com nova linha. STOP também é lido após Invoke.
void pollOperator() {
    static char command[12];
    static size_t length = 0;
    static bool overflow = false;
    unsigned budget = 64;
    while (Serial.available() && budget--) {
        const char c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (length && !overflow) {
                command[length] = '\0';
                if (strcmp(command, "STOP") == 0) stopDriving("operador");
                else if (strcmp(command, "DIAG") == 0) {
                    if (driveGate.armed()) diagnostic("[DIAG] Envie STOP antes de consultar o registro\n");
                    else printStopSnapshot();
                }
                else if (strcmp(command, "START") == 0)
                    Serial.println(driveGate.start() ? "[CTRL] Habilitado" : "[CTRL] Aguarde frames validos");
            }
            length = 0;
            overflow = false;
        } else if (length < sizeof(command)-1 && c != '\0') command[length++] = c;
        else overflow = true;
    }
}

// Utilitários de Diagnóstico e LED
// -------------------------------------------------------------
void blinkLED(int times, int ms = 100) {
    pinMode(LED_FLASH_GPIO, OUTPUT);
    for (int i = 0; i < times; i++) {
        digitalWrite(LED_FLASH_GPIO, HIGH);
        delay(ms);
        digitalWrite(LED_FLASH_GPIO, LOW);
        delay(ms);
    }
}

// -------------------------------------------------------------
// Inicialização do Sensor de Câmera OV2640 em QVGA Grayscale
// -------------------------------------------------------------
bool setupCamera() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;

    // Grayscale em QVGA (320x240): campo de visão amplo idêntico ao dataset de treino
    config.pixel_format = PIXFORMAT_GRAYSCALE;
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 12; // Inicializado, mesmo sem JPEG.
    config.fb_count     = 1;  // Múltiplos buffers são recomendados apenas em JPEG.
    config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[ERRO] Falha ao inicializar camera: 0x%x\n", err);
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 0);
    }

    const auto clock=car_camera::configureClock(s,CAMERA_FAST_PROFILE);
    cameraFastActive=clock.fast;
    Serial.printf("[CAM] perfil=%s CLKRC=%d->%d XCLK=20MHz motivo=%s\n",
                  clock.fast ? "rapido" : "padrao",clock.original,clock.effective,clock.reason);
    if (!clock.ok) {
        Serial.println("[ERRO] Clock da camera nao confirmado; piloto bloqueado");
        return false;
    }

    Serial.println("[CAM] Camera QVGA (320x240 Grayscale) pronta!");
    return true;
}

// -------------------------------------------------------------
// Inicialização do TensorFlow Lite Micro
// -------------------------------------------------------------
bool setupTFLite() {
    Serial.println("[ML] Carregando modelo TensorFlow Lite...");
    tflite_model = tflite::GetModel(g_model);
    if (tflite_model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.printf("[ERRO] Versao do schema TFLite incompativel: %d (esperado %d)\n",
                      tflite_model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    // Solicita memória interna explicitamente; usa PSRAM somente como fallback.
    tensor_arena = (uint8_t*) heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (tensor_arena) {
        Serial.printf("[ML] Tensor Arena alocada na SRAM interna rapida (%d KB)!\n", kTensorArenaSize / 1024);
    } else if (psramFound()) {
        tensor_arena = (uint8_t*) ps_malloc(kTensorArenaSize);
        if (tensor_arena) {
            Serial.printf("[ML] Tensor Arena alocada na PSRAM (%d KB)\n", kTensorArenaSize / 1024);
        }
    }

    if (!tensor_arena) {
        Serial.println("[ERRO CRITICO] Falha ao alocar Tensor Arena na memoria!");
        return false;
    }

    // Arena has priority; weights use only spare internal memory, never PSRAM.
    if (ML_MODEL_IN_INTERNAL_RAM) {
        modelInternal = (uint8_t*)heap_caps_aligned_alloc(16, g_model_len,
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (modelInternal) {
            memcpy(modelInternal, g_model, g_model_len);
            tflite_model = tflite::GetModel(modelInternal);
        }
    }
    Serial.printf("[MEM] modelo=%s bytes=%u | pre=IRAM, direto INT8\n",
                  modelInternal ? "SRAM" : "flash", g_model_len);
#if CAR_ML_OPTIMIZE_HOT_PATHS
    Serial.println("[BUILD] O2 local: preprocessamento e convolucoes ESP-NN; TFLM padrao");
#else
    Serial.println("[BUILD] rotinas locais com otimizacao padrao do compilador");
#endif

    // Registrador com os operadores exigidos pelo modelo
    static tflite::MicroMutableOpResolver<16> resolver;
    resolver.AddConv2D(ML_USE_ESP_NN ? car_ml::RegisterOptimizedConv() : tflite::Register_CONV_2D());
    resolver.AddMean();  // GlobalAveragePooling2D compila como MEAN no TFLite
    resolver.AddMaxPool2D();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddAdd();
    resolver.AddMul();
    resolver.AddQuantize();
    resolver.AddDequantize();
    resolver.AddShape();
    resolver.AddPack();
    resolver.AddStridedSlice();
    resolver.AddRelu();
    resolver.AddTanh();

    // Instancia o interpretador TFLite
    static tflite::MicroInterpreter static_interpreter(
        tflite_model, resolver, tensor_arena, kTensorArenaSize
    );
    interpreter = &static_interpreter;

    // Aloca os tensores da rede
    TfLiteStatus allocate_status = interpreter->AllocateTensors();
    if (allocate_status != kTfLiteOk) {
        Serial.println("[ERRO] Falha em AllocateTensors()!");
        return false;
    }

    model_input = interpreter->input(0);
    model_output = interpreter->output(0);

    // Recusa contratos incompatíveis antes de escrever no buffer INT8.
    if (!model_input || !model_output || !model_input->dims || !model_output->dims ||
        model_input->type != kTfLiteInt8 || model_output->type != kTfLiteInt8 ||
        model_input->dims->size != 4 || model_input->dims->data[0] != 1 ||
        model_input->dims->data[1] != MODEL_INPUT_HEIGHT ||
        model_input->dims->data[2] != MODEL_INPUT_WIDTH || model_input->dims->data[3] != 1 ||
        model_input->bytes != MODEL_INPUT_SIZE || model_output->bytes != 1 ||
        !isfinite(model_input->params.scale) || model_input->params.scale <= 0 ||
        !isfinite(model_output->params.scale) || model_output->params.scale <= 0) {
        Serial.println("[ERRO] Modelo deve ser INT8 [1,96,96,1] com uma saida e escalas validas");
        return false;
    }

    Serial.printf("[ML] Modelo pronto! Entrada: [%d, %d, %d] | Saida: %d valor(es)\n",
                  model_input->dims->data[1],
                  model_input->dims->data[2],
                  model_input->dims->data[3],
                  (int)model_output->bytes);

    // ---------------------------------------------------------
    // Inicialização da Tabela LUT e Mapeamento de Coordenadas
    // ---------------------------------------------------------
    if (!car_ml::preparePreprocessing(preprocessingTables, model_input->params.scale,
                                      model_input->params.zero_point)) {
        Serial.println("[ML] Quantizacao de entrada invalida");
        return false;
    }
    Serial.printf("[ML] LUT pronta | arena usada: %u bytes\n", (unsigned)interpreter->arena_used_bytes());

    Serial.printf("[ML] backend=%s | CPU=%uMHz\n", ML_USE_ESP_NN ? "ESP-NN conv INT8" : "TFLM referencia", getCpuFrequencyMhz());
    if (ML_USE_ESP_NN) {
        // Temporary verification memory is released before driving (no per-frame allocation).
        uint8_t* verification = (uint8_t*)heap_caps_malloc(MODEL_INPUT_SIZE,
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!verification && psramFound())
            verification = (uint8_t*)heap_caps_malloc(MODEL_INPUT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!verification) {
            Serial.println("[ML] Sem memoria para verificar ESP-NN; piloto bloqueado");
            return false;
        }
        car_ml::SetConvVerification(verification, MODEL_INPUT_SIZE);
        uint32_t randomState=0x12345678;
        for (int pattern=0; pattern<5; ++pattern) {
            for (int i=0; i<MODEL_INPUT_SIZE; ++i) {
                randomState=randomState*1664525u+1013904223u;
                model_input->data.int8[i]=pattern==0 ? -128 : pattern==1 ? 127 :
                    pattern==2 ? 0 : pattern==3 ? (i%256)-128 : int(randomState>>24)-128;
            }
            if (interpreter->Invoke()!=kTfLiteOk) {
                car_ml::SetConvVerification(nullptr,0);
                heap_caps_free(verification);
                Serial.println("[ML] Falha na verificacao ESP-NN; piloto bloqueado");
                return false;
            }
        }
        car_ml::SetConvVerification(nullptr,0);
        heap_caps_free(verification);
        Serial.printf("[MEM] verificacao liberada: %u bytes | heap interno livre=%u maior_bloco=%u\n",
                      (unsigned)MODEL_INPUT_SIZE,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        Serial.printf("[ML] ESP-NN verificado: %u convolucoes identicas; %u chamadas de referencia\n",
                      car_ml::OptimizedConvCalls(),car_ml::ReferenceConvCalls());
        if (!car_ml::OptimizedConvCalls()) {
            Serial.println("[ML] Modelo sem convolucoes compativeis com ESP-NN");
            return false;
        }
    }

    return true;
}

// -------------------------------------------------------------
// Setup Principal
// -------------------------------------------------------------
void setup() {
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=======================================================");
    Serial.println("  ESP32-CAM — PILOTO EXCLUSIVO DO MODELO");
    Serial.println("=======================================================");

    // Iniciar Serial2 (comunicação com placa dos motores)
    Serial2.begin(115200, SERIAL_8N1, SERIAL2_RX_PIN, SERIAL2_TX_PIN);
    sendDrive(0, 0);
    pinMode(LED_FLASH_GPIO, OUTPUT);
    digitalWrite(LED_FLASH_GPIO, LOW);
    Serial.printf("[INIT] Serial2 iniciada (TX=GPIO%d, RX=GPIO%d) a 115200 bps\n",
                  SERIAL2_TX_PIN, SERIAL2_RX_PIN);

    loadStopSnapshot();

    // Inicializar Câmera
    if (!setupCamera()) {
        blinkLED(5, 100);
        while(true) delay(1000);
    }

    // Inicializar TensorFlow Lite
    if (!setupTFLite()) {
        blinkLED(3, 300);
        while(true) delay(1000);
    }

    // Sinal de pronto: 2 piscadas
    blinkLED(2, 150);
    // Descarta buffers acumulados durante a inicialização e as piscadas.
    for (int warmup = 0; warmup < 2; ++warmup) {
        camera_fb_t* frame = esp_camera_fb_get();
        if (frame) esp_camera_fb_return(frame);
    }
    Serial.println("[CTRL] Direcao exclusiva do modelo; recuperacao e detector de linha desativados");
    Serial.println("[INIT] Pronto. Partida apos inferencias validas; START habilita / STOP para (com nova linha).\n");
}

// -------------------------------------------------------------
// Loop Principal de Pilotagem Autônoma (com Benchmark de Alta Precisão)
// -------------------------------------------------------------
void loop() {
    persistStopSnapshot();
    reportCameraTiming(); // Non-blocking, including capture failures.
    int64_t t0 = esp_timer_get_time(); // microssegundos
    static int64_t previousLoopUs = 0;
    const int64_t periodUs = previousLoopUs ? t0 - previousLoopUs : 0;
    diagnosticPeriodUs=periodUs;
    diagnosticAgeUs=diagnosticInferUs=-1;
    const bool periodValid = !previousLoopUs || periodUs <= MAX_DECISION_AGE_US;
    if (!periodValid) pauseDriving("intervalo de controle excedido");
    previousLoopUs = t0;
    pollOperator();

    // 1. Capturar frame mais recente da câmera
    int64_t t_cam_start = esp_timer_get_time();
    camera_fb_t* fb = esp_camera_fb_get();
    const int64_t receivedUs=esp_timer_get_time();
    if (!fb) {
        cameraTiming.failure();
        pauseDriving("falha de captura");
        delay(2);
        return;
    }
    int64_t t_cam_us = receivedUs - t_cam_start;
    const int64_t capturedUs = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
    const int64_t ageUs = receivedUs - capturedUs;
    diagnosticAgeUs=ageUs;
    if (fb->format != PIXFORMAT_GRAYSCALE || fb->width != 320 || fb->height != 240 ||
        fb->len < 320 * 240 || !fb->buf || capturedUs <= 0 || ageUs < 0) {
        cameraTiming.invalidFrame();
        esp_camera_fb_return(fb);
        stopDriving("frame invalido");
        return;
    }
    if (!cameraTiming.observe(t_cam_start,receivedUs,capturedUs)) {
        esp_camera_fb_return(fb);
        stopDriving("timestamp da camera repetido/regressivo");
        return;
    }
    if (ageUs > MAX_DECISION_AGE_US) {
        cameraTiming.lateFrame();
        esp_camera_fb_return(fb);
        pauseDriving("frame atrasado");
        return;
    }

    // 2. Pré-processar e quantizar na entrada do modelo (via LUT)
    int64_t t_pre_start = esp_timer_get_time();
    car_ml::preprocess(fb->buf, model_input->data.int8, preprocessingTables);
    esp_camera_fb_return(fb);
    int64_t t_pre_us = esp_timer_get_time() - t_pre_start;
    pollOperator(); // STOP recebido durante a captura tem prioridade.
    const int64_t visionUs = esp_timer_get_time();
    diagnosticAgeUs=visionUs-capturedUs;
    if (visionUs - capturedUs > MAX_DECISION_AGE_US) {
        pauseDriving("imagem atrasada apos processamento");
        return;
    }
    // 3. Executar Inferência da Rede Neural (Medição em microssegundos)
    int64_t t_infer_start = esp_timer_get_time();
    TfLiteStatus invoke_status = interpreter->Invoke();
    int64_t t_infer_us = esp_timer_get_time() - t_infer_start;
    diagnosticInferUs=t_infer_us;
    diagnosticAgeUs=esp_timer_get_time()-capturedUs;

    if (invoke_status != kTfLiteOk) {
        stopDriving("erro de inferencia");
        return;
    }

    // 4. Ler Saída do Steer da IA (-1.0 a +1.0)
    int8_t out_int8 = model_output->data.int8[0];
    float raw_steer = (out_int8 - model_output->params.zero_point) * model_output->params.scale;
    if (!isfinite(raw_steer) || raw_steer < -1.001f || raw_steer > 1.001f) {
        stopDriving("predicao invalida");
        return;
    }
    if (esp_timer_get_time() - capturedUs > MAX_DECISION_AGE_US) {
        pauseDriving("decisao atrasada");
        return;
    }
    if (periodValid) driveGate.observe(true);
    pollOperator(); // STOP recebido durante captura/inferencia tem prioridade.
    // 5. Direcao exclusivamente da previsao atual, sem detector ou recuperacao.
    const DriveCommand command = commandForModel(raw_steer, driveGate.armed());
    const float steer = command.steer;
    const float throttle = command.throttle;
    sendDrive(steer, throttle);

    int64_t t_total_us = esp_timer_get_time() - t0;

    // 7. Estatísticas e Benchmark Acumulado sobre 100 Inferências
    static int benchmark_samples = 0;
    static int64_t sum_infer_us = 0;
    static int64_t sum_cam_us = 0;
    static int64_t sum_pre_us = 0;
    static int64_t sum_total_us = 0;
    static int64_t min_infer_us = 99999999;
    static int64_t max_infer_us = 0;

    // Ignora a 1ª inferência (warm-up) para não poluir a média
    static bool warmup_done = false;
    if (!warmup_done) {
        warmup_done = true;
        return;
    }

    benchmark_samples++;
    sum_infer_us += t_infer_us;
    sum_cam_us   += t_cam_us;
    sum_pre_us   += t_pre_us;
    sum_total_us += t_total_us;

    if (t_infer_us < min_infer_us) min_infer_us = t_infer_us;
    if (t_infer_us > max_infer_us) max_infer_us = t_infer_us;

    // Log limitado para não bloquear o ciclo por tráfego de debug.
    static unsigned long lastStateLog = 0;
    if (millis() - lastStateLog >= 500) {
        diagnostic("[ML] modo=MODELO infer=%.1fms idade=%.1fms periodo=%.1fms S=%.3f T=%.3f %s\n",
                      t_infer_us / 1000.0f, (esp_timer_get_time()-capturedUs)/1000.0f,
                      periodUs/1000.0f, steer, throttle,
                      driveGate.armed() ? "ATIVO" : (driveGate.pending() ? "AGUARDANDO" : "BLOQUEADO"));
        lastStateLog = millis();
    }


    // A cada 100 frames: Relatório Completo de Benchmark
    if (benchmark_samples >= 100) {
        float avg_infer_ms = (sum_infer_us / 100.0f) / 1000.0f;
        float min_infer_ms = min_infer_us / 1000.0f;
        float max_infer_ms = max_infer_us / 1000.0f;
        float avg_cam_ms   = (sum_cam_us / 100.0f) / 1000.0f;
        float avg_pre_us   = (sum_pre_us / 100.0f);
        float avg_total_ms = (sum_total_us / 100.0f) / 1000.0f;
        float real_fps     = (avg_total_ms > 0) ? (1000.0f / avg_total_ms) : 0.0f;

        diagnostic("[BENCH] infer=%.2fms min=%.2f max=%.2f cam=%.2fms pre=%.1fus ciclo=%.2fms fps=%.1f\n",
                   avg_infer_ms, min_infer_ms, max_infer_ms, avg_cam_ms,
                   avg_pre_us, avg_total_ms, real_fps);

        // Reset dos acumuladores
        benchmark_samples = 0;
        sum_infer_us = 0;
        sum_cam_us = 0;
        sum_pre_us = 0;
        sum_total_us = 0;
        min_infer_us = 99999999;
        max_infer_us = 0;
    }
}
