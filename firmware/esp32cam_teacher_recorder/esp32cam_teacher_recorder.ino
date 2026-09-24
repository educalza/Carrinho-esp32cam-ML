// Independent classical-vision teacher. Existing firmware and models are untouched.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include <atomic>
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "teacher_policy.h"

WebServer web(80); // Handlers execute on the control task: single command authority.
teacher::Controller controller;
teacher::Gate gate;
uint8_t* rgb = nullptr;
uint8_t gray[teacher::W*teacher::H];
bool sdReady = false, recording = false;
std::atomic<bool> writeFault{false};
std::atomic<unsigned> pending{0}, saved{0};
unsigned session = 0, frameId = 0, dropped = 0;
unsigned processedFrames = 0;
uint64_t lastDecision = 0;
const char* visionStatus = "aguardando imagem";
const char* lastStop = "nenhuma falha registrada";
teacher::Observation lastGood;
uint64_t lastGoodUs = 0;
uint8_t diagnosticGray[teacher::W*teacher::H];
bool diagnosticReady=false, diagnosticFrozen=false;
int diagnosticThreshold=0;
teacher::Observation observation;
teacher::Command command;
struct Record { uint8_t* jpeg; size_t length; uint64_t captured, decided; unsigned session, id; teacher::Observation vision; teacher::Command drive; };
QueueHandle_t records;

void stopDrive() {
    gate.stop(); controller.reset(); recording = false; command = {};
    lastGood={}; lastGoodUs=0;
    Serial2.print("S0.000T0.000\n");
}
void writer(void*) {
    Record r;
    for (;;) {
        if (xQueueReceive(records, &r, portMAX_DELAY) != pdTRUE) continue;
        char imagePath[80], csvPath[64], row[320];
        snprintf(imagePath,sizeof(imagePath),"/teacher/session_%03u/frame_%06u.jpg",r.session,r.id);
        snprintf(csvPath,sizeof(csvPath),"/teacher/session_%03u/log.csv",r.session);
        File image = SD_MMC.open(imagePath,FILE_WRITE);
        bool ok = image && image.write(r.jpeg,r.length)==r.length;
        if (image) image.close();
        free(r.jpeg);
        if (ok) {
            File csv = SD_MMC.open(csvPath,FILE_APPEND);
            const int n = snprintf(row,sizeof(row),"%llu,frame_%06u.jpg,%.3f,%.3f,1,teacher_same_frame,%llu,%llu,%.2f,%.2f,%.2f,%.4f,%.3f\n",
                r.captured/1000,r.id,r.drive.steer,r.drive.throttle,r.decided/1000,(r.decided-r.captured)/1000,
                r.vision.nearX,r.vision.midX,r.vision.farX,r.vision.bend,r.vision.confidence);
            ok = csv && n>0 && n<(int)sizeof(row) && csv.write((const uint8_t*)row,n)==(size_t)n;
            if (csv) csv.close();
        }
        if (ok) ++saved;
        else { SD_MMC.remove(imagePath); writeFault = true; }
        --pending;
    }
}
bool newSession() {
    if (!sdReady || gate.armed() || pending.load()) return false;
    char path[64];
    do { snprintf(path,sizeof(path),"/teacher/session_%03u",++session); } while (SD_MMC.exists(path));
    if (!SD_MMC.mkdir(path)) return false;
    strlcat(path,"/log.csv",sizeof(path));
    File csv = SD_MMC.open(path,FILE_WRITE);
    bool ok = csv && csv.println("timestamp_ms,frame,steer,throttle,label_valid,label_method,decision_timestamp_ms,decision_age_ms,near_x,mid_x,far_x,curvature,confidence")>0;
    if (csv) csv.close();
    if (!ok) return false;
    frameId=0; saved=0; dropped=0; writeFault=false; recording=true;
    return true;
}
void enqueueFrame(camera_fb_t* fb, uint64_t captured, uint64_t decided) {
    if (!recording) return;
    if (pending.load() >= 2) { ++dropped; return; }
    auto* copy = (uint8_t*)ps_malloc(fb->len);
    if (!copy) { ++dropped; return; }
    memcpy(copy,fb->buf,fb->len);
    Record r{copy,fb->len,captured,decided,session,++frameId,observation,command};
    ++pending;
    if (xQueueSend(records,&r,0)!=pdTRUE) { --pending; free(copy); ++dropped; }
}
bool cameraBegin() {
    camera_config_t c = {};
    c.pin_pwdn=32; c.pin_reset=-1; c.pin_xclk=0; c.pin_sccb_sda=26; c.pin_sccb_scl=27;
    c.pin_d0=5; c.pin_d1=18; c.pin_d2=19; c.pin_d3=21; c.pin_d4=36; c.pin_d5=39; c.pin_d6=34; c.pin_d7=35;
    c.pin_vsync=25; c.pin_href=23; c.pin_pclk=22;
    c.ledc_timer=LEDC_TIMER_0; c.ledc_channel=LEDC_CHANNEL_0; c.xclk_freq_hz=20000000;
    c.pixel_format=PIXFORMAT_JPEG; c.frame_size=FRAMESIZE_QVGA; c.jpeg_quality=12;
    c.fb_count=1; c.fb_location=CAMERA_FB_IN_PSRAM; c.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
    return esp_camera_init(&c)==ESP_OK;
}
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="pt-br"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>Piloto professor</title>
<style>body{font:18px system-ui;max-width:700px;margin:30px auto;padding:16px;background:#14202a;color:#fff}button{padding:18px;margin:6px;font:inherit}pre{white-space:pre-wrap}</style>
<h1>Piloto professor — revisão 4</h1><p>Preparar gravação cria uma sessão no SD. O piloto só pode iniciar quando pronto=true (cinco imagens válidas). Pare antes de retirar o cartão.</p>
<button onclick="send('record')">Preparar gravação</button><button onclick="send('start')">Iniciar piloto</button><button onclick="send('stop')">PARAR</button>
<p id="message"></p><pre id="status"></pre><p>Sem resposta ou conexão perdida: confira fisicamente a parada. Gravações pendentes devem chegar a zero antes de retirar o SD.</p>
<p id="vision"></p>
<p>Com o piloto parado, veja a imagem processada. A primeira falha fica congelada para análise.</p>
<button onclick="diagnostic()">Ver imagem da parada</button><button onclick="refreshDiagnostic()">Liberar captura de diagnóstico</button>
<p id="diagMessage"></p><canvas id="frame" width="160" height="120" style="width:100%;image-rendering:pixelated"></canvas>
<script>async function request(url,options={}){const controller=new AbortController();const timer=setTimeout(()=>controller.abort(),5000);try{const r=await fetch(url,{...options,cache:'no-store',signal:controller.signal});const body=await r.arrayBuffer();return {ok:r.ok,status:r.status,text:()=>new TextDecoder().decode(body),json:()=>JSON.parse(new TextDecoder().decode(body)),arrayBuffer:()=>body}}finally{clearTimeout(timer)}}
async function send(action){document.getElementById('message').textContent='Enviando comando...';try{let r=await request('/'+action,{method:'POST',body:''});document.getElementById('message').textContent=(r.ok?'':'Comando recusado: ')+r.text()}catch(e){document.getElementById('message').textContent='Sem confirmação em 5 segundos. Confira a conexão e o estado do piloto antes de tentar novamente.'}}
async function diagnostic(){try{let r=await fetch('/diagnostic');if(!r.ok){document.getElementById('diagMessage').textContent=await r.text();return}let bytes=new Uint8Array(await r.arrayBuffer());if(bytes.length!==19200)throw Error();let ctx=document.getElementById('frame').getContext('2d');let im=ctx.createImageData(160,120);for(let i=0;i<bytes.length;i++){im.data[i*4]=im.data[i*4+1]=im.data[i*4+2]=bytes[i];im.data[i*4+3]=255}ctx.putImageData(im,0,0);document.getElementById('diagMessage').textContent='Imagem sem overlay usada pelo detector. Salve uma captura desta tela se a parada persistir.'}catch(e){document.getElementById('diagMessage').textContent='Falha ao obter imagem'}}
async function refreshDiagnostic(){let r=await fetch('/diagnostic-reset',{method:'POST'});document.getElementById('diagMessage').textContent=await r.text()}
async function poll(){try{let r=await request('/status');if(!r.ok)throw Error();let d=r.json();document.getElementById('status').textContent=JSON.stringify(d,null,2);document.getElementById('vision').textContent='Agora: '+d.visao+' | Última falha: '+d.ultima_falha+' | Atualizado às '+new Date().toLocaleTimeString()}catch(e){document.getElementById('status').textContent='Sem atualização do ESP32 (5 s). Verifique o Wi-Fi.'}setTimeout(poll,500)}poll();</script></html>)HTML";
void setup() {
    Serial.begin(115200);
    Serial2.begin(115200,SERIAL_8N1,12,13);
    stopDrive();
    if (!psramFound()) { Serial.println("PSRAM obrigatoria"); return; }
    sdReady = SD_MMC.begin("/sdcard",true);
    if (sdReady && !SD_MMC.exists("/teacher")) sdReady=SD_MMC.mkdir("/teacher");
    rgb=(uint8_t*)ps_malloc(320*240*3);
    records=xQueueCreate(2,sizeof(Record));
    if (!rgb || !records || !cameraBegin()) { Serial.println("Falha de inicializacao"); free(rgb); rgb=nullptr; return; }
    if (xTaskCreatePinnedToCore(writer,"teacher_sd",6144,nullptr,1,nullptr,0)!=pdPASS) { free(rgb); rgb=nullptr; return; }
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Carrinho-Professor","professor2026");
    web.on("/",HTTP_GET,[]{ web.sendHeader("Cache-Control","no-store");web.send_P(200,"text/html; charset=utf-8",PAGE); });
    web.on("/vision",HTTP_GET,[]{char info[480];snprintf(info,sizeof(info),"Agora: %s | Ultima falha: %s | Linhas apoiadas: %u | Limiar: %d | Imagem congelada: %s (limiar %d) | Pausa de leitura: %s",visionStatus,lastStop,observation.rows,observation.threshold,diagnosticFrozen?"sim":"nao",diagnosticThreshold,gate.paused()?"sim":"nao");web.send(200,"text/plain; charset=utf-8",info);});
    web.on("/diagnostic",HTTP_GET,[]{
        if(gate.armed() || !diagnosticReady) {web.send(409,"text/plain","Pare o piloto e aguarde uma imagem.");return;}
        web.setContentLength(sizeof(diagnosticGray)); web.send(200,"application/octet-stream","");
        web.sendContent((const char*)diagnosticGray,sizeof(diagnosticGray));
    });
    web.on("/diagnostic-reset",HTTP_POST,[]{
        if(gate.armed()) {web.send(409,"text/plain","Pare o piloto primeiro.");return;}
        diagnosticFrozen=false;lastStop="nenhuma falha registrada";
        web.send(200,"text/plain","Captura liberada. Aguarde uma imagem e pressione Ver imagem.");
    });
    web.on("/stop",HTTP_POST,[]{stopDrive(); web.send(200,"text/plain","Parado. Aguarde pendentes=0 para retirar o SD.");});
    web.on("/record",HTTP_POST,[]{bool ok=newSession(); web.send(ok?200:409,"text/plain; charset=utf-8",ok?"Sessão criada no SD. Aguarde pronto=true antes de iniciar o piloto.":"Pare o piloto, aguarde gravações e verifique o SD.");});
    web.on("/start",HTTP_POST,[]{
        if(writeFault.load()) {web.send(409,"text/plain; charset=utf-8","Falha no SD. Prepare uma nova sessão após verificar o cartão.");return;}
        if(!gate.ready()) {char response[240];snprintf(response,sizeof(response),"Piloto não iniciado: %s. Imagens válidas consecutivas: %u/5.",visionStatus,gate.validFrames());web.send(409,"text/plain; charset=utf-8",response);return;}
        gate.start();web.send(200,"text/plain; charset=utf-8","Piloto habilitado. Confira ativo=true e aceleração no painel.");
    });
    web.on("/status",HTTP_GET,[]{char s[800]; snprintf(s,sizeof(s),"{\"versao\":4,\"quadros_processados\":%u,\"imagens_validas\":%u,\"ativo\":%s,\"linha\":%s,\"pronto\":%s,\"gravando\":%s,\"sd\":%s,\"falha_sd\":%s,\"salvos\":%u,\"descartados\":%u,\"pendentes\":%u,\"direcao\":%.3f,\"aceleracao\":%.3f,\"suporte_linha\":%.2f,\"visao\":\"%s\",\"ultima_falha\":\"%s\"}",processedFrames,gate.validFrames(),gate.armed()?"true":"false",observation.valid?"true":"false",gate.ready()?"true":"false",recording?"true":"false",sdReady?"true":"false",writeFault?"true":"false",saved.load(),dropped,pending.load(),command.steer,command.throttle,observation.confidence,visionStatus,lastStop);web.sendHeader("Cache-Control","no-store");web.send(200,"application/json",s);});
    web.begin(); Serial.println("Conecte ao Wi-Fi Carrinho-Professor / professor2026. Abra http://192.168.4.1");
}
void loop() {
    if (!rgb) { stopDrive(); delay(100); return; }
    web.handleClient();
    // Loss of the operator's Wi-Fi association stops motion. Browser closure alone does not.
    if (WiFi.softAPgetStationNum()==0 || writeFault.load()) stopDrive();
    camera_fb_t* fb=esp_camera_fb_get();
    const uint64_t now=esp_timer_get_time();
    const uint64_t captured=fb ? (uint64_t)fb->timestamp.tv_sec*1000000ULL+fb->timestamp.tv_usec : 0;
    bool valid=fb && fb->buf && fb->width==320 && fb->height==240 && fb->format==PIXFORMAT_JPEG && captured && now>=captured && now-captured<200000;
    if (valid) valid=fmt2rgb888(fb->buf,fb->len,PIXFORMAT_JPEG,rgb);
    const bool decoded=valid;
    if (valid) {
        for (int y=0;y<120;++y) for(int x=0;x<160;++x) {
            const uint8_t* p=rgb+(y*2*320+x*2)*3;
            gray[y*160+x]=(77*p[0]+150*p[1]+29*p[2])>>8;
        }
        const bool recent=lastGoodUs && now>=lastGoodUs && now-lastGoodUs<200000;
        observation=teacher::inspect(gray,sizeof(gray),recent?&lastGood:nullptr);
    } else observation={};
    const uint64_t decided=esp_timer_get_time();
    const float dt=lastDecision ? (decided-lastDecision)/1000000.0f : 0.03f;
    const bool timely=captured && decided>=captured && decided-captured<200000 && dt<=0.2f;
    visionStatus=!valid?"Falha de captura, formato ou imagem atrasada":
        !timely?"Processamento ou ciclo acima de 200 ms":
        observation.reason;
    lastDecision=decided;
    ++processedFrames;
    const bool wasArmed=gate.armed();
    const bool usable=valid && observation.valid && timely;
    if(decoded && !diagnosticFrozen) {
        memcpy(diagnosticGray,gray,sizeof(gray));diagnosticReady=true;diagnosticThreshold=observation.threshold;
    }
    if(wasArmed && !usable && !diagnosticFrozen) {
        lastStop=visionStatus;diagnosticFrozen=diagnosticReady;
    }
    // Hard timing/camera failures stop immediately. A visual miss commands ZERO
    // throttle immediately, but permits recovery for 120 ms without pressing START.
    if(!valid || !timely) {gate.stop();lastGood={};lastGoodUs=0;}
    else gate.observe(observation.valid,millis());
    if(usable) {lastGood=observation;lastGoodUs=decided;}
    if (!gate.armed() || !usable) controller.reset();
    command=gate.armed() && usable?controller.update(observation,dt):teacher::Command{};
    Serial2.printf("S%.3fT%.3f\n",command.steer,command.throttle);
    if (command.throttle>0) enqueueFrame(fb,captured,decided);
    if (wasArmed && !gate.armed()) recording=false;
    if (fb) esp_camera_fb_return(fb);
    delay(1);
}
