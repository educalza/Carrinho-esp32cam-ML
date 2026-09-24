# Correções do controle e pipeline ML

Este documento registra a implementação inicial. O piloto atual usa exclusivamente a direção do modelo: o detector visual e as manobras de recuperação descritos no histórico abaixo foram removidos. A configuração atual é `SPEED_SCALE=0.45`, partida automática após três decisões válidas e limite de tempo de 250 ms. Para gravação e comportamento atual, consulte [o guia do firmware autônomo](firmware/esp32cam_autonomous/COMO_GRAVAR_ARDUINO.md). Esta mudança exige atualizar somente o ESP32-CAM e não exige treinar ou converter novamente.

As seis frentes foram implementadas em conjunto, com testes separados e preservação dos datasets e modelos existentes. A qualidade na pista ainda exige novo treinamento e ensaios com o carrinho; os testes de software não demonstram desempenho físico.

## Mudanças de comportamento

| Problema | Correção | Como verificar |
|---|---|---|
| Velocidades colapsavam no PWM mínimo | Throttle representa a faixa útil entre PWM mínimo e máximo, após deadband 0,02; serial do piloto usa três casas | Com os valores atuais: reta 95, curva suave 93, curva fechada 92 |
| Imagens relacionadas em treino e validação | Separação por sessões antes de augmentation; teste separado; manifestos de origem | Conferir `manifest.json` e `sources_*.csv` do dataset v2 |
| Direção aprendida era atenuada | Removidos expo cúbico, deadband e filtro fixo do piloto | Comparar direção prevista com a enviada; mantém limites e inversão do servo |
| Carrinho continuava sem pista | Detector visual independente, bloqueio de movimento, START/STOP, parada por falha ou atraso | Linha deve estar detectável antes de START; retirar a pista deve parar e exigir novo START |
| Imagem recebia comando posterior | Histórico protegido entre tarefas e associação pelo timestamp real da câmera | Novas colunas do CSV documentam instante e idade do comando |
| Configuração incompleta da câmera | Estrutura zerada, formato/localização/buffers explícitos e checagem de frame/tensor | Compilar e conferir logs de inicialização e inferência |

## Ordem de uso

1. Grave o controlador de motores atualizado no ESP32 Dev Module. A mesma posição do acelerador agora produz PWM diferente: recalibre a velocidade manual antes de coletar. `MOTOR_MIN_PWM` é ponto inicial de calibração, não medição automática de atrito.
2. Grave o servidor atualizado no ESP32-CAM e colete novas sessões. Para cada frame, registra-se o comando mais recente recebido até o início da captura, desde que tenha no máximo 300 ms. A idade não mede o atraso mecânico do servo nem a reação do piloto humano ao vídeo.
3. Execute o pipeline v2 conforme [ml_pipeline/README.md](ml_pipeline/README.md). Use `--require-aligned` para aceitar somente a nova coleta. Não é possível corrigir o desalinhamento antigo só modificando o código.
4. Compare o modelo INT8 com o Keras e as referências simples nas sessões reservadas. Observe curvas e cada sessão, além da média global. Depois da seleção, copie o novo header para o sketch autônomo.
5. Compile e grave o piloto autônomo, confira logs com as rodas suspensas e então faça ensaios lentos de pista, inclusive saída da linha e parada.

Os antigos `dataset_processado`, `modelo` e `firmware/esp32cam_autonomous/modelo_linha.h` foram preservados. O header atual ainda contém o modelo anterior, cuja avaliação estava sujeita ao vazamento descrito. A conversão v2 não o substitui automaticamente.

## Piloto e perda de linha

`autonomous_config.h` concentra velocidade, limites da proteção visual e habilitação. O padrão inicia parado; envie `START` no monitor serial a 115200 baud, com nova linha, após três decisões válidas consecutivas. `STOP` desabilita o movimento. Para uso sem cabo, `AUTO_ARM_ON_BOOT=true` permite uma única habilitação após a inicialização; uma falha exige START ou reinício, sem retomada automática.

A proteção visual procura uma faixa escura sobre superfície mais clara em quatro linhas da metade inferior da imagem. Ela é uma heurística conservadora; **não é confiança da rede nem classificação treinada de “fora da pista”**. Sombras, juntas do piso e reflexos podem causar falsos positivos ou paradas indevidas. Foi exercitada com imagens sintéticas e uma amostra do dataset, mas os limiares precisam ser confirmados na pista. Para proteção mais robusta, colete também cenas sem linha, sombras e cruzamentos, rotule presença/ausência e avalie um detector específico em sessões independentes.

O piloto verifica formato 320×240 grayscale, tamanho do buffer, contrato INT8 96×96×1 e idade da imagem até a decisão. Acima de 200 ms, envia parada; o controlador tem timeout independente de 300 ms. O timeout corta PWM e deixa o carro desacelerar por inércia. O comando STOP da serial depende de o loop retornar; a placa de motores cobre um travamento por ausência de comandos.

`SPEED_SCALE=0.10` foi preservado. A separação atual de 95/93/92 é pequena; o código deixou de anulá-la, mas só medição na pista permite escolher uma diferença física útil. PWM não é velocidade medida. O protocolo normaliza esforço na faixa útil e o projeto não tem controle de velocidade com encoders.

## Testes reproduzíveis

Na raiz, com Python e um compilador C++ disponíveis:

```powershell
python -B -m unittest discover -s tests -p test_ml_pipeline.py -v
g++ -std=c++11 -Wall -Wextra -pedantic tests/test_motor_control.cpp -o tests/test_motor_control.exe
./tests/test_motor_control.exe
g++ -std=c++11 -Wall -Wextra -pedantic tests/test_driving_policy.cpp -o tests/test_driving_policy.exe
./tests/test_driving_policy.exe
g++ -std=c++11 -Wall -Wextra -pedantic firmware/esp32cam_server/tests/test_drive_history.cpp -o tests/test_drive_history.exe
./tests/test_drive_history.exe
python -B tests/smoke_ml_real.py
```

O último comando executa duas épocas com dados sintéticos e passa por OpenCV, TensorFlow, conversão INT8 e avaliação reais. Os resultados ficam isolados em `.build/ml-smoke-*`; são verificação de funcionamento, não um modelo para pilotar. Os outros testes cobrem separação, filtros, proveniência, quantização, métricas, parser, timeout, histórico e política de parada.

Os arquivos gerados em `.build` e executáveis dos testes são descartáveis e ignorados pelo Git. Nenhum firmware é gravado nas placas pelos testes.

## Verificação realizada nesta implementação

- 18 testes Python passaram; os três programas de teste C++ passaram (motor/protocolo, histórico temporal e política do piloto).
- O fluxo real de OpenCV/TensorFlow concluiu pré-processamento, duas épocas, conversão INT8 e avaliação em dados sintéticos isolados.
- Os três sketches compilaram com Arduino ESP32 Core 3.3.11; o piloto utiliza tflm_esp32 2.0.0. A biblioteca TFLM emite avisos de API C++ depreciada, sem impedir a compilação.
- A indexação da coleta atual aceitou 8.242 frames de 28 sessões e descartou 17 registros. Todos os aceitos têm alinhamento temporal antigo/desconhecido. Houve aviso do decodificador sobre JPEG corrompido: aceitar dimensões e rótulos não certifica a qualidade visual de cada imagem.
- O detector visual foi exercitado também em 33 imagens amostradas do projeto; 23 foram aceitas e 10 rejeitadas. Sem rótulos de presença/ausência, isso não é medida de acurácia. A amostra inclui reflexos e faixas próximas à borda, reforçando a necessidade de calibrar e medir falsos positivos/negativos na pista.

Não foram realizados upload, medições na placa, treinamento completo com imagens reais ou substituição do modelo embarcado. A próxima etapa é a coleta sincronizada, seguida de treinamento v2 e ensaio de pista.
