import os

# Caminhos base
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATASET_RAW_DIR = os.path.join(BASE_DIR, "imagens_treino5")
DATASET_PROC_DIR = os.path.join(BASE_DIR, "dataset_processado_v2")
MODEL_DIR = os.path.join(BASE_DIR, "modelo_v2")

# Resolução de entrada da rede neural
IMG_WIDTH = 96
IMG_HEIGHT = 96
IMG_CHANNELS = 1  # Grayscale

# Parâmetros de filtragem
MIN_THROTTLE = 0.02  # Aceita apenas valores acima da deadband inclusiva do motor

# Parâmetros de treino
BATCH_SIZE = 64
EPOCHS = 50
LEARNING_RATE = 0.001
VAL_SPLIT = 0.2
TEST_SPLIT = 0.2
RANDOM_STATE = 42
DIRECTION_THRESHOLD = 0.05
CALIBRATION_SAMPLES = 200

# Parâmetros de Data Augmentation
AUG_ROTATION_MAX = 15.0          # Graus máximos de rotação (±15°)
AUG_ROTATION_STEER_FACTOR = 0.15 # Compensação de steer por rotação máxima
AUG_ROTATION_ENABLED = False    # Só habilitar após calibrar a compensação física
AUG_BRIGHTNESS_RANGE = 0.15      # Variação de brilho (±0.15 em escala normalizada)
AUG_CONTRAST_RANGE = (0.7, 1.3)  # Fator de contraste min/max

# Diretórios são criados apenas pela etapa que grava seus artefatos.
