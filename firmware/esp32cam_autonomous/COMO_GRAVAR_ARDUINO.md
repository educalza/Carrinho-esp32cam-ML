# Gravar e verificar o piloto automático

## Dependências

- Arduino ESP32 Core: compilação de referência com **3.3.11**.
- Biblioteca **tflm_esp32 2.0.0**, que fornece `tflm_esp32.h`. Instalar somente EloquentTinyML ou uma biblioteca com outro nome não garante esse header.
- Sketch: `esp32cam_autonomous.ino`, mantendo todos os arquivos da pasta, inclusive `optimized_conv.h`, `optimized_conv.cpp`, `preprocessing.h`, `preprocessing.cpp`, `performance_options.h`, `build_opt.h` e a subpasta `src/esp_nn`. Abra o sketch nessa pasta completa; copiar somente o `.ino` não inclui as otimizações.

O modelo existente foi preservado. Para gerar um novo modelo com validação independente, siga [o pipeline v2](../../ml_pipeline/README.md). Só substitua o header após avaliar o resultado INT8.

## Configurações de placa

Selecione **AI Thinker ESP32-CAM**, PSRAM habilitada, CPU de 240 MHz e uma partição de aplicativo que comporte o resultado da compilação. Use DIO se essa for a configuração compatível com sua placa. Para esta alteração, mantenha o firmware existente do ESP32 Dev Module; somente o ESP32-CAM precisa ser atualizado.

Para gravar, coloque GPIO 0 em GND durante o reset/upload, conforme o gravador utilizado. Ao concluir, remova essa ligação e reinicie. A gravação não é executada pelos testes do repositório.

## Habilitação e diagnóstico

A direção vem exclusivamente da previsão atual do modelo, limitada à faixa -1 a +1 e enviada com três casas decimais. Foram removidos o detector heurístico de fita e todas as manobras de recuperação: busca, ré, escolha de lado pelo histórico, alinhamento visual e transição de retorno ao ML. Todo frame recente e válido passa pela inferência, mesmo sem fita visível. A ausência de fita não causa parada nem aciona outra direção.

A partida automática permanece habilitada: `AUTO_ARM_ON_BOOT=true` e `MODEL_READY_FRAMES=3` exigem três decisões consecutivas com captura, inferência e tempos válidos. Isso não confirma presença da pista. Para exigir partida manual, use `AUTO_ARM_ON_BOOT=false`. No monitor serial a 115200 baud, com final de linha, `START` habilita após essa sequência e `STOP` desabilita, inclusive durante a espera inicial.

A velocidade normal permanece em `autonomous_config.h`: `SPEED_SCALE=0.45`, com comandos de acelerador 0.225 na reta, 0.180 na curva suave e 0.144 na curva fechada, conforme a magnitude prevista. Throttle e PWM não são velocidade física medida. Não há aceleração especial de recuperação nem throttle negativo.

Confirme o firmware pela mensagem:
```text
[CTRL] Direcao exclusiva do modelo; recuperacao e detector de linha desativados
```

O log `[ML] modo=MODELO` mostra inferência, idade da imagem, período, direção, acelerador e estado (`ATIVO`, `AGUARDANDO` ou `BLOQUEADO`). O campo de detecção `linha=x/4` deixou de existir no log atual. Os motivos de parada continuam disponíveis por `DIAG`; novos registros mostram `fase=ml_puro` e `linha=n/a`. Registros antigos permanecem legíveis. Logs de ciclo são descartados quando falta espaço na UART.

As proteções de captura, contrato do modelo, inferência e tempo permanecem. Idade e intervalo máximos: 250 ms; watchdog independente dos motores: 300 ms. Falhas temporárias de captura ou atraso pausam e permitem retomar após três decisões válidas, se a partida já estava autorizada. STOP e falhas graves bloqueiam até START. A ausência de linha não participa dessas verificações. Remover a recuperação não corrige uma eventual reinicialização da placa nem garante acerto das previsões nas curvas.

O modelo, o pré-processamento, as convoluções ESP-NN e o perfil rápido de câmera permanecem. A arena tem 25 KiB; o uso real aparece na inicialização. Se um modelo novo exceder a arena ou tiver contrato incompatível, a inicialização é bloqueada.

## Compilação pela linha de comando

Na raiz do repositório, ajustando a pasta de bibliotecas da sua instalação:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32cam --libraries C:/Users/eduar/Documents/Arduino/libraries --build-path .build/autonomous firmware/esp32cam_autonomous
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/esp32_motor_controller
```

`compile` verifica o programa sem gravá-lo na placa. Consulte [IMPLEMENTACAO_ML.md](../../IMPLEMENTACAO_ML.md) para a ordem de coleta, treino, avaliação e teste na pista.

## Consultar depois uma parada ocorrida na bateria

O firmware guarda a primeira causa de cada episódio de parada após ter enviado comando de movimento, para a frente. O registro inclui a fase, os tempos disponíveis e o último comando de movimento; o campo de suporte visual é mantido apenas para ler registros antigos. Uma nova parada após retomada substitui o registro anterior. Ele fica na memória interna (NVS), sem cartão SD; gravações ocorrem apenas parado e com intervalo mínimo de cinco segundos.

Após uma parada que queira investigar, mantenha a placa alimentada e parada por pelo menos cinco segundos antes de desligar. Ao ligar no cabo, abra o monitor a 115200 baud: a inicialização imprime `[DIAG] Ultima parada salva`. Para consultar novamente, envie `DIAG` com final de linha enquanto estiver parado. Se estiver habilitado, envie `STOP` antes; uma parada manual após novo movimento passa a ser o evento mais recente.

O instante é o tempo desde a inicialização em que ocorreu o evento, não o horário do relógio. Valores -1 significam medição indisponível. O registro distingue paradas do operador, atrasos e outras falhas. `tempo de busca da linha` e outros motivos dos limites antigos nao sao mais gerados pelo piloto atual. Eles ainda podem aparecer em `Ultima parada salva`, pois o upload normal nao apaga o evento persistente do firmware anterior. Registros antigos permanecem visiveis ate uma nova parada. Se desligar antes da gravacao pendente, podera aparecer o evento anterior; reinicializacao abrupta ou falta de energia nao gera por si so um registro de parada.

Estas alterações exigem gravar apenas o firmware autônomo no ESP32-CAM; não alteram o protocolo da placa de motores nem exigem novo treinamento. A direção vem do modelo e o acelerador continua no protocolo que o controlador existente já aceita.

## Convoluções otimizadas com o modelo atual

`ML_USE_ESP_NN=true` em `autonomous_config.h` seleciona a convolução INT8 otimizada da Espressif para ESP32 comum. As camadas restantes continuam na biblioteca `tflm_esp32` 2.0.0. A integração é local ao sketch: não modifica bibliotecas globais, a câmera, o pré-processamento, a velocidade nem a política de direção. O header do modelo continua idêntico ao de `modelo_v3` (SHA256 `f835b35dae58bbae79324b01cc249774e020db102483add12ce391a6d3fae751`). Não há novo treinamento ou conversão.

A cópia mínima do ESP-NN está em `src/esp_nn`, com licença Apache-2.0 e versão fixada no commit `07df856b8108f5ce5712a7ebcd834c0d9207997d`. `UPSTREAM.json` registra origem, hashes dos arquivos originais e mudanças locais: includes relativos e atributo de otimização nas duas funções de convolução, sem alterar a aritmética. Não é necessário instalar outra biblioteca pelo gerenciador Arduino. O kernel genérico otimizado do ESP32 foi escolhido explicitamente; não são usadas instruções exclusivas de ESP32-S3.

O adaptador preserva a preparação de tensores, escalas e arredondamento do TFLM. Convoluções incompatíveis (como dilatação, grupos, lote maior que um ou 1x1 com padding) usam a implementação de referência. Durante a inicialização, antes de habilitar movimento, cinco entradas sintéticas passam pelo modelo inteiro. Cada convolução otimizada é comparada byte a byte com a referência usando a mesma entrada, pesos e parâmetros. Uma divergência ou falta de espaço para essa verificação bloqueia a inicialização. Isso verifica equivalência nesses casos, não substitui avaliação na pista. A comparação é desligada antes de pilotar e não entra no benchmark normal.

No monitor serial, espere:

```text
[ML] backend=ESP-NN conv INT8 | CPU=240MHz
[ML] ESP-NN verificado: 15 convolucoes identicas; 0 chamadas de referencia
```

Os números 15 e 0 correspondem às três convoluções do modelo atual, verificadas em cinco entradas. Confira também a mensagem de arena em SRAM interna. Depois compare `[BENCH] infer=... ciclo=...` e os valores `idade` e `periodo` do log normal com o firmware anterior. A compilação confirma a integração; o ganho de tempo só pode ser medido na placa. Maior FPS, sozinho, não garante que o carrinho complete a curva.

Para comparação A/B, altere apenas `ML_USE_ESP_NN` para `false`, recompile e grave: isso restaura as convoluções de referência sem trocar o modelo. Use as mesmas configurações da placa, iluminação e alimentação. Para voltar à otimização, restaure `true` e grave novamente. Atualize somente o ESP32-CAM; o Dev Module não muda.

Teste nativo no Windows (GCC/MinGW e fontes instaladas do TFLM):

```powershell
./tests/test_esp_nn_conv.ps1
```

O teste cobre convoluções com as geometrias do modelo, padding assimétrico, canais não múltiplos de quatro, bias ausente, saturação, escala por canal e o adaptador real. Também verifica retorno à referência e rejeição de divergência. As localizações do compilador e das fontes podem ser passadas pelos parâmetros `CompilerDirectory` e `TflmSource`.

## Localidade de memória e compilação para desempenho

O pré-processamento agora escreve diretamente no tensor INT8. A imagem intermediária `model_gray` foi removida: são 9.216 escritas a menos por frame. As tabelas de coordenadas e quantização ficam juntas na DRAM interna e o mapa horizontal usa inteiros de 16 bits; os offsets verticais continuam em 32 bits, pois o quadro tem 76.800 bytes. O campo de visão, as coordenadas, a média inteira 2x2, a normalização e o arredondamento foram preservados. `preprocessing.cpp` coloca apenas o pequeno loop de pixels na IRAM, reduzindo a dependência do cache de instruções durante leituras da PSRAM. Isso não torna a PSRAM acessível com cache desabilitado.

A verificação inicial do ESP-NN continua ativa. Seu buffer de 9 KiB é alocado apenas durante a inicialização e liberado ao terminar, inclusive se houver divergência. Falta de memória impede a partida, sem pular a verificação. Não há alocações no processamento de cada frame.

`ML_MODEL_IN_INTERNAL_RAM=true` tenta copiar os 6.592 bytes do modelo atual para SRAM interna alinhada a 16 bytes, depois de reservar a arena. O interpretador e os pesos usam essa cópia durante toda a execução, reduzindo leituras da flash. Se não houver um bloco disponível, o mesmo modelo continua na flash. O arquivo original não é alterado. A cópia consome RAM: a liberação dos 9 KiB de verificação não representa 9 KiB líquidos extras quando o modelo está na SRAM. Para comparar somente a localização dos pesos, use `ML_MODEL_IN_INTERNAL_RAM=false`, mantendo as demais opções.

`CAR_ML_OPTIMIZE_HOT_PATHS=1` em `performance_options.h` aplica `-O2` por atributo GCC apenas ao loop de pré-processamento e às duas funções locais de convolução ESP-NN. O restante do programa e a TFLM mantêm `-Os`, explicitado em `build_opt.h` para evitar reaproveitar flags de uma compilação anterior. Não há edição de bibliotecas instaladas nem uso de `-ffast-math`. Para comparar somente a otimização dessas rotinas, defina `CAR_ML_OPTIMIZE_HOT_PATHS` como `0`, compile e grave novamente; restaure `1` para voltar ao perfil local de desempenho.

A aplicação global de `-O2` foi descartada após erro interno do compilador Xtensa no `unpack.cpp` da TFLM com o Core 3.3.11. A opção por função evita recompilar esse componente com flags diferentes, mantendo a melhoria concentrada nos trechos usados pelo modelo.

Confira na inicialização:

```text
[MEM] modelo=SRAM bytes=6592 | pre=IRAM, direto INT8
[BUILD] O2 local: preprocessamento e convolucoes ESP-NN; TFLM padrao
[MEM] verificacao liberada: 9216 bytes | heap interno livre=... maior_bloco=...
```

`modelo=flash` indica que a cópia foi desabilitada ou não coube; isso não troca o modelo. O heap mostrado já considera as alocações de câmera, arena e modelo e é mais útil que a estimativa de variáveis globais do compilador. A mensagem ESP-NN continua mostrando a verificação byte a byte antes da partida.

Os testes nativos comparam 4.480 imagens sintéticas (41.287.680 pixels INT8) com o pré-processamento anterior, incluindo todos os tons de cinza, gradientes, bordas, padrões aleatórios, escalas e zero points diferentes. O resultado deve ser idêntico com a otimização local habilitada ou desabilitada:

```powershell
g++ -std=c++17 -Os -Wall -Wextra -pedantic tests/test_preprocessing.cpp firmware/esp32cam_autonomous/preprocessing.cpp -o .build/test_preprocessing.exe
./.build/test_preprocessing.exe
./tests/test_esp_nn_conv.ps1
```

Repita o teste de pré-processamento acrescentando `-DCAR_ML_OPTIMIZE_HOT_PATHS=0` à compilação para conferir a alternativa padrão. Para convoluções, use `./tests/test_esp_nn_conv.ps1 -HotPathOptimization 0`. O script aceita também `-Optimization O2` para comparação nativa, sem alterar o firmware.

Para medir na placa, compare `pre`, `infer` e `ciclo` do `[BENCH]`, além de `idade` e `periodo` do `[ML]`, com a mesma pista, iluminação e velocidade. Use várias janelas após a inicialização. Um ganho de CPU pode reduzir a idade da decisão sem aumentar o FPS, se a câmera continuar impondo a cadência. Não há promessa de ganho em milissegundos sem esse ensaio.

Esta alteração mantém um framebuffer grayscale, o perfil de câmera, `SPEED_SCALE=0.45`, o protocolo dos motores e a direção exclusivamente pela ML. Exige somente gravar o ESP32-CAM com a pasta completa; não exige treinar ou converter novamente.

Referências: [memória interna, IRAM e constantes na flash](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32/api-guides/memory-types.html) e [otimizações de desempenho recomendadas pela Espressif](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-guides/performance/speed.html).

## Perfil de captura mais rápido e medição da câmera

`CAMERA_FAST_PROFILE=true` em `autonomous_config.h` habilita um ensaio de maior cadência da OV2640. A resolução permanece 320x240, grayscale, com um framebuffer, XCLK de 20 MHz, exposição e ganho automáticos conforme o driver. O modelo, o pré-processamento, a velocidade e a política de direção não são alterados.

Após o driver configurar a câmera, o firmware verifica o sensor e lê CLKRC do banco do sensor por `get_reg(0x111, 0xff)`. No perfil conhecido, o campo divisor passa de 3 (divisão por 4) para 1 (divisão por 2), preservando os demais bits. Não é overclock da CPU nem aumento do XCLK externo. O clock interno e a transferência da imagem podem ficar mais rápidos; a taxa final depende do sensor, exposição e capacidade de captura do ESP32. A alteração de tempo de linha pode também mudar a aparência da imagem enquanto os automatismos se ajustam, portanto valide a detecção na pista.

A alteração só é aplicada ao PID OV2640, com divisor original 3 e duplicador desligado. Outros estados permanecem no padrão, com motivo no log. O valor gravado é relido; se falhar, tenta restaurar e conferir o valor original. Se não conseguir confirmar a restauração, bloqueia a inicialização. Essa checagem confirma o registrador, não a qualidade da imagem nem a estabilidade física. Falhas de captura, imagens incompatíveis e atrasos continuam sujeitos às proteções existentes; não há troca automática de perfil enquanto dirige.

Exemplo esperado na inicialização:

```text
[CAM] perfil=rapido CLKRC=3->1 XCLK=20MHz motivo=aplicado
```

A cada aproximadamente dois segundos aparece uma janela de medições (valores abaixo apenas ilustrativos):

```text
[CAM] perfil=rapido n=24 get=...ms inicio_rel=...ms entrega=...ms max=...ms dt=...ms falhas=0 invalidos=0 atrasados=0
```

- `n`: quantidade de quadros válidos para medição nessa janela.
- `get`: duração média da chamada que obtém o quadro, incluindo espera.
- `inicio_rel`: média do timestamp de início atribuído pelo driver menos o instante do pedido. Positivo significa início depois do pedido; negativo significa que o quadro já tinha começado antes. Não é uma medida direta de exposição física.
- `entrega`: idade média do quadro ao retornar da câmera, antes do pré-processamento e da inferência. Em cada amostra, `get = inicio_rel + entrega`.
- `max`: maior idade na entrega nessa janela.
- `dt`: intervalo médio entre timestamps dos quadros recebidos. Pode incluir quadros não recebidos e pausas; não é uma medição direta de todos os VSYNCs do sensor. `-1` indica que ainda não há intervalo disponível.
- `falhas`: capturas sem framebuffer; `invalidos`: formato/dimensões/dados ou timestamps inválidos, repetidos ou regressivos; `atrasados`: quadros válidos entregues acima do limite de idade.

Os contadores se referem à janela, não ao total desde o boot. A janela `[CAM]` é diferente da média de 100 inferências do `[BENCH]`. A ausência de linha não é contabilizada como imagem inválida, e estes contadores não detectam todo ruído ou corrupção visual. Os logs continuam sem bloquear a UART; uma janela pode não ser impressa se faltar espaço.

Para comparar, grave e teste primeiro o perfil rápido. Confira `perfil=rapido`, os contadores, `[BENCH]`, `idade` e `periodo` do ML, além da estabilidade na pista. Para voltar exatamente ao clock escolhido pelo driver, configure `CAMERA_FAST_PROFILE=false` e grave novamente. Mantenha `ML_USE_ESP_NN=true` nos dois testes, a mesma iluminação e a mesma velocidade. Se a imagem ou detecção piorar, ou aparecerem erros de captura, volte ao padrão mesmo que o FPS aumente.

Esta mudança exige somente atualizar o ESP32-CAM com a pasta completa do sketch, agora também contendo `camera_profile.h` e `camera_timing.h`. Não exige treinar, converter ou atualizar o Dev Module. O ganho real de cadência só pode ser confirmado na placa.

Referências para o acesso bancado e divisor: [driver oficial OV2640](https://github.com/espressif/esp32-camera/blob/master/sensors/ov2640.c) e [datasheet do sensor](https://docs.espressif.com/projects/esp-dev-kits/en/latest/_static/esp32-s2-kaluga-1/datasheet/Camera_OV2640.pdf).
