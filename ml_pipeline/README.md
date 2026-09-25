# Pipeline ML v2

O pipeline separa sessões inteiras em treino, validação e teste **antes** de gerar variações de imagens. Os padrões usam aproximadamente 60%/20%/20% das sessões, com seed 42. A porcentagem de frames pode ser diferente porque as sessões têm tamanhos distintos. São necessárias pelo menos três sessões válidas; para uma avaliação útil, grave várias sessões com iluminação, posição inicial e curvas variadas.

Os arquivos antigos em `dataset_processado`, `modelo` e o header já instalado no firmware ficam preservados. As novas saídas são `dataset_processado_v2` e `modelo_v2`. O preprocessamento e treinamento recusam substituir diretórios existentes: para uma nova execução, escolha novos caminhos pelos argumentos correspondentes.

## Execução

Use um ambiente Python com as dependências já utilizadas pelo projeto: NumPy, OpenCV, TensorFlow e Matplotlib. O analisador `01_analisar_dataset.py` também usa pandas. Nenhuma biblioteca nova foi acrescentada. Não se deve executar o treino no runtime auxiliar do Codex sem essas dependências.

Na raiz do projeto:

```powershell
python ml_pipeline/02_preprocessar.py
python ml_pipeline/03_treinar_modelo.py
python ml_pipeline/04_converter_tflite.py
python ml_pipeline/avaliar_precisao.py
```

Antes do teste final, é possível conferir a validação com `python ml_pipeline/avaliar_precisao.py --split val`. Use a validação para escolher configurações; consulte o teste uma vez, após a seleção. Se ajustar o sistema a partir do teste, essas sessões deixam de representar um teste independente: reserve novas sessões para a próxima avaliação final.

Para outras pastas, passe `--raw-dir` e `--output-dir` ao preprocessamento, e os mesmos `--dataset-dir` e `--model-dir` às três etapas seguintes. `config.py` contém a fonte padrão (`imagens_treino3_deadzone`), épocas, tamanho do lote e seed efetivamente usados pelo treino. As versões do TensorFlow e NumPy ficam registradas no manifesto do treinamento; a reprodutibilidade numérica pressupõe ambiente compatível.

## Coleta e separação

- As imagens precisam ser grayscale ou JPEG convertido para grayscale, com resolução original 320×240. O redimensionamento usa a média inteira 2×2 do firmware e normalização por 255. JPEG introduz diferenças em relação à captura grayscale usada no piloto; mantenha câmera, exposição e montagem consistentes e confirme o resultado na placa.
- Somente o treino recebe flip horizontal e variação de brilho/contraste. O flip inverte o sinal da direção. A rotação com compensação heurística do comando fica desligada (`AUG_ROTATION_ENABLED=False`) até haver calibração física.
- O filtro aceita somente `throttle > 0.02`, coerente com a deadband do motor; descarta parada/ré, imagens ausentes ou de resolução incorreta, e registros novos com `label_valid != 1` ou idade do comando acima de 300 ms. Rótulos inválidos e frames duplicados no mesmo log interrompem a etapa com indicação da linha.
- Logs antigos continuam legíveis, mas não é possível corrigir retroativamente o atraso entre imagem e comando. Eles aparecem como `legacy_unknown` na proveniência. `--require-aligned` aceita somente o histórico de comandos válido produzido pelo servidor de coleta. Timestamp, idade do comando e alinhamento são preservados em `sources_*.csv`. Recoletar é preferível quando os logs antigos não comprovam alinhamento.
- O servidor de coleta exige QVGA (320×240) ao iniciar a gravação e bloqueia mudanças para outras resoluções enquanto grava. Frames pendentes de outra resolução são rejeitados.
- Não divida uma mesma gravação entre várias sessões para colocá-las em conjuntos diferentes; cópias de sessões também não constituem dados independentes. Para testar outra pista/dia/iluminação, reserve todas as sessões desse cenário no mesmo conjunto.

Para fixar manualmente as sessões, crie um JSON e use `--split-file caminho.json`. Todas as sessões válidas precisam aparecer exatamente uma vez:

```json
{
  "train": ["session_001", "session_002", "session_003"],
  "val": ["session_004"],
  "test": ["session_005"]
}
```

Cada conjunto produz `X_*.npy`, `y_*.npy` e `sources_*.csv`; cada linha da proveniência identifica sessão, frame e augmentation. `manifest.json` registra separação, filtros, configurações e hashes dos arquivos. Arrays grandes são gravados/lidos por memória mapeada e o treino carrega lotes; não é necessário montar todas as imagens em RAM. A validação do contrato lê os arquivos por blocos e pode levar algum tempo em datasets grandes.

## Avaliação e publicação

O modelo Keras é selecionado pela menor perda na validação. A calibração INT8 utiliza uma amostra aleatória reproduzível **somente do treino**. A conversão verifica Keras e INT8 em toda a validação, sem usar teste para ajustar quantização. Modelos sem proveniência v2 ou que não correspondam ao dataset são recusados.

O teste final gera `modelo_v2/avaliacao_test.json` e `precisao_test.png`, contendo:

- MAE, MSE, percentil 95 do erro e R² para Keras e INT8;
- acerto direcional geral, acerto em curvas e resultados separados para esquerda/reta/direita;
- resultados de cada sessão, para evitar que uma sessão longa esconda falhas em outra;
- referências simples: sempre seguir reto e sempre prever a média das direções do treino;
- aumento de MAE após quantização e diferença entre as previsões Keras/INT8.

Métricas sem amostras aplicáveis (por exemplo, nenhuma curva) aparecem como `null`, sem divisão por zero. O limiar esquerda/reta/direita é `DIRECTION_THRESHOLD`, padrão 0,05. Bons números em imagens não demonstram estabilidade em movimento: confirme também latência na placa, recuperação de deslocamento, curvas e perda da linha em pista.

Revise especialmente o resultado INT8, suas curvas e suas sessões. Só depois copie manualmente `modelo_v2/modelo_linha.h` para `firmware/esp32cam_autonomous/modelo_linha.h` e compile o firmware. A conversão **não copia automaticamente** o header para o carrinho. Não há promessa de qualidade dos modelos antigos: eles precisam ser treinados novamente com a separação corrigida.

## Verificação do código

```powershell
python -B -m unittest discover -s tests -p test_ml_pipeline.py -v
```

Esses testes usam somente NumPy e a biblioteca padrão. Cobrem separação e proveniência, rejeição de artefatos incompatíveis, paridade do resize, augmentation, quantização e métricas. A leitura JPEG é simulada no teste de integração; não substitui executar as etapas com OpenCV/TensorFlow reais e testar o firmware na placa.

Para uma verificação curta com as bibliotecas reais, execute `python -B tests/smoke_ml_real.py`. Ele gera cinco sessões sintéticas, treina por duas épocas, converte para INT8 e avalia o teste reservado em uma pasta isolada `.build/ml-smoke-*`. Não sobrescreve modelos e não mede qualidade de pilotagem.
