# Piloto professor independente

## Revisão 4: início e seleção da faixa

Corrige a aquisição em imagens com uma faixa central e piso escuro na borda:
exige contraste local e prioriza uma faixa com os dois limites visíveis na aquisição.
A imagem real fornecida em 20/09/2026 antes falhava com `mais de uma continuacao
possivel`, zero linhas; após a correção passou com 22 linhas válidas, limiar 94,
centro próximo x=87,63. Isso valida aquela imagem, não todas as condições da pista.

Preparar gravação apenas cria a sessão. Agora a sessão permanece preparada enquanto
o carro ainda está parado aguardando uma linha válida. `Iniciar piloto` informa o
motivo e a contagem de imagens válidas quando recusa a partida. O painel identifica
`versao: 4`, mostra `quadros_processados` para distinguir estado constante de travamento,
atualiza visão/estado numa única consulta e limita a espera dos comandos a cinco segundos.
Ausência de confirmação não garante que o comando não chegou; confira o estado.

Para testar uma imagem em cinza 160x120 no detector C++ fora da placa, compile
`tests/test_teacher.cpp` e execute com `--frame`, enviando 19.200 intensidades decimais
separadas por espaços na entrada padrão. O programa imprime o resultado e retorna
zero somente se a imagem for aceita.

Implementação própria de visão clássica multizona e PD, inspirada conceitualmente em
https://github.com/IgorKoga/Carrinho_ESP32 (commit 8a628c2).
Nenhum código de atuação daquele repositório foi incorporado. Este sketch não usa ML.
Os firmwares existentes e modelos continuam em suas pastas originais.

## Instalação e operação

1. Abra `esp32cam_teacher_recorder.ino` no Arduino IDE. Selecione AI Thinker ESP32-CAM,
   PSRAM habilitada e Arduino ESP32 Core 3.3.11. Use a fiação atual; não conecte os
   motores nem o servo diretamente à ESP32-CAM.
2. Grave este sketch somente na ESP32-CAM. O ESP32 Dev deve estar com o controlador
   atual deste projeto (protocolo S...T..., PWM útil acima de 0,02 e watchdog de 300 ms).
   Se ele já está atualizado, não precisa ser gravado novamente.
3. Conecte celular/computador ao Wi-Fi `Carrinho-Professor`, senha `professor2026`.
   Abra `http://192.168.4.1`. Não precisa de roteador nem Internet.
4. Primeiro teste com rodas suspensas: uma linha deslocada à esquerda deve produzir
   direção negativa e esterçamento físico para esquerda. A inversão mecânica continua
   sendo responsabilidade do Dev. Ajuste a montagem da câmera antes de coletar.
5. Posicione em linha preta sobre piso claro. Câmera horizontal, sem espelhamento,
   imagem QVGA 320x240. As zonas usam y=100,70,40 na imagem reduzida 160x120.
   O detector acompanha linhas horizontais de y=112 até y=28 (passos de 4).
   Precisa de evidência próxima e ao menos 24 pixels de extensão vertical.
6. Aguarde `pronto=true`. Para coletar, pressione Preparar gravação antes de Iniciar
   piloto. Também é possível pilotar sem gravação/cartão.
7. PARAR desarma e encerra a coleta. Aguarde `pendentes=0` antes de retirar o cartão
   ou desligar. Cada nova preparação cria uma sessão nova, sem substituir anteriores.
8. Para voltar ao controle manual ou à ML, grave novamente o sketch correspondente.

## Comportamento e limitações

Partida sempre manual, após cinco observações válidas. Perda/ambiguidade visual
zera a aceleração imediatamente e pausa a atuação por até 120 ms: se a linha voltar
nesse prazo, retoma; se a perda persistir, desarma e exige novo START. Imagem velha,
ciclo acima de 200 ms ou falha de escrita desarmam imediatamente. Depois de desarmado,
reencontrar a linha não o rearma. Se a captura travar, o watchdog já existente no Dev interrompe
os motores. O botão web depende de a tarefa atender a requisição; não é um botão
físico de emergência. Perder todas as associações Wi-Fi também para; fechar apenas
o navegador não desconecta o Wi-Fi e não para o carro.

A detecção usa limiar por percentis na região completa, evitando perder o contraste
quando uma linha de amostragem fica totalmente coberta pelo cruzamento. Aceita
faixas escuras de 2 a 52 pixels, inclusive cortadas pela borda, contraste mínimo 25
e continuidade ao acompanhar o traçado de baixo para cima. Usa ao menos cinco linhas
válidas cobrindo 20 pixels ou mais, com primeira observação em y>=64. Uma ou duas
linhas de amostragem ausentes podem ser ignoradas. A referência temporal só é usada
se vier de observação válida com menos de 200 ms.
Curvas podem sair do campo de visão superior: o segmento visível estima a direção
adiante, com aceleração reduzida. Uma faixa transversal de até 48 pixels pode ser
atravessada se houver saída visível compatível com a trajetória de entrada. A regra
é seguir essa continuação, sem escolher deliberadamente uma saída à esquerda/direita.
Quando o cruzamento cobre a base, a trajetória válida anterior fornece a referência
para a saída que permanece visível; não exige uma faixa estreita na base nessa fase.
Não há avanço às cegas se faltar a linha: cruzamentos totalmente ocupando a imagem,
sem saída visível ou bifurcações ambíguas ainda param. O painel mostra o diagnóstico
visual e distingue falha de percepção de atraso na captura/processamento. A primeira
falha durante a condução congela a imagem em cinza (160x120) e seu motivo. Com o
piloto parado, use **Ver imagem da parada**. Para atualizar a imagem depois, use
**Liberar captura de diagnóstico**. Essa visualização é servida somente parado para
não atrasar a condução. Em falha de captura sem novo frame decodificado, a imagem
mostrada é a última disponível, não um frame da falha. Se voltar a parar, uma captura
desse painel com a imagem e o motivo permite calibrar com evidência da pista real.
O suporte informado é uma medida geométrica, não uma probabilidade calibrada.
Sombras e desenhos podem confundir o detector, inclusive uma sombra transversal
com formato de cruzamento. Valores iniciais exigem ensaio na pista real.

PD normalizado: Kp=1,4; Kd=0,025 com tempo medido em segundos, mais antecipação
de 0,35 vezes o desvio entre extremidades observadas. Aceleração 0,20
na reta até 0,10 nas curvas (~PWM 120 até 103 com o controlador atual). PWM não
mede velocidade física. Os parâmetros estão em `teacher_policy.h` e exigem nova
compilação. Em cruzamentos/curvas parcialmente visíveis usa aceleração 0,10.
Não há streaming nem ajuste ao vivo, para reduzir
o trabalho concorrente com captura e SD. O painel oferece estado e controle.

## Dataset

Cartão: `/teacher/session_NNN/frame_NNNNNN.jpg` e `log.csv`. JPEG QVGA original,
sem overlay; a decisão é calculada decodificando exatamente esse JPEG. O professor
envia o comando e enfileira uma cópia da mesma imagem. Uma tarefa separada escreve
no SD. No máximo dois frames ficam pendentes; se a fila encher, a amostra é descartada
e contabilizada, sem bloquear a decisão. Falha de escrita para o piloto e exige nova
sessão. Aguarde a fila terminar antes de desligar para preservar as últimas amostras.

O CSV mantém `timestamp_ms,frame,steer,throttle,label_valid` e acrescenta
`label_method=teacher_same_frame`, instante/idade da decisão, centros das zonas,
curvatura e suporte. A direção registrada é o comando solicitado, não uma medição
do ângulo físico do servo. Como PD usa histórico, a mesma imagem isolada pode gerar
pequenas diferenças de rótulo conforme a trajetória anterior.

Copie a pasta `teacher` do cartão para uma nova pasta de dados no computador.
O preprocessador reconhece `teacher_same_frame`, inclusive com `--require-aligned`.
Ele exige `label_valid=1`, decisão posterior à captura, idade menor que 200 ms e
consistência entre os timestamps e a idade (tolerância de 1 ms pelo arredondamento).
Amostras com esses dados inválidos são descartadas também no modo padrão.
Os arquivos `sources_*.csv` preservam o método, instante/idade da decisão e confiança
da observação original. Aqui o comando é calculado DEPOIS da imagem; não invente
`command_age_ms=0`. Use diretórios novos para processamento/modelo e sessões inteiras
reservadas para validação/teste. Não misture automaticamente com datasets existentes.

Valide primeiro algumas voltas do professor. Dados automáticos reproduzem seus erros;
complemente com sessões manuais de recuperação. Não há garantia de melhoria do modelo
sem comparar validação e condução real.

## Verificação local

`tests/test_teacher.cpp` cobre imagens uniformes, linha central, curvas dos dois lados,
linhas ambíguas, sombra larga, redução de aceleração, atraso e rearme manual.
Também cobre curvas fortes que saem da imagem, cruzamento movendo-se pela imagem
até cobrir a base, cruzamento sem saída, oclusão transversal excessiva, faixa larga,
linha tocando a borda, ausência do trecho mais próximo e recuperação/expiração da pausa.
A compilação Arduino não inclui a pasta tests. Compilação e testes de software não
substituem medição de latência com seu SD nem calibração na pista.
