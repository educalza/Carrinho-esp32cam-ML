"""Contratos de dados/modelos e métricas, sem importar TensorFlow ou OpenCV."""
import csv
import hashlib
import json
from pathlib import Path

import numpy as np

SCHEMA_VERSION = 2
SPLITS = ("train", "val", "test")
PREPROCESSING = "grayscale_320x240_mean2x2_integer_96x96_float32_div255_v1"


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def save_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False,
                                    allow_nan=False) + "\n", encoding="utf-8")


def split_sessions(session_names, val_fraction, test_fraction, seed, explicit=None):
    """Divide grupos inteiros; frações referem-se a sessões, não a frames."""
    names = sorted(set(session_names))
    if len(names) < 3:
        raise ValueError("São necessárias pelo menos 3 sessões válidas para treino/val/test.")
    if not (0 < val_fraction < 1 and 0 < test_fraction < 1
            and val_fraction + test_fraction < 1):
        raise ValueError("VAL_SPLIT e TEST_SPLIT devem ser >0 e somar menos de 1.")
    if explicit is not None:
        result = {key: list(explicit.get(key, [])) for key in SPLITS}
        flat = sum(result.values(), [])
        if (any(not value for value in result.values()) or len(flat) != len(set(flat))
                or set(flat) != set(names) or set(explicit) != set(SPLITS)):
            raise ValueError("Split explícito deve distribuir cada sessão válida exatamente uma vez.")
        return result
    shuffled = np.random.default_rng(seed).permutation(names).tolist()
    n_val = max(1, int(round(len(names) * val_fraction)))
    n_test = max(1, int(round(len(names) * test_fraction)))
    while n_val + n_test >= len(names):
        if n_val >= n_test and n_val > 1:
            n_val -= 1
        elif n_test > 1:
            n_test -= 1
        else:
            raise ValueError("Sessões insuficientes para as frações solicitadas.")
    return {"val": sorted(shuffled[:n_val]),
            "test": sorted(shuffled[n_val:n_val + n_test]),
            "train": sorted(shuffled[n_val + n_test:])}


def resize_like_firmware(img_gray, out_w=96, out_h=96, src_w=320, src_h=240):
    """Preserva o resize inteiro 2x2 do firmware, recusando resolução incorreta."""
    if img_gray.ndim != 2 or img_gray.shape != (src_h, src_w):
        raise ValueError(f"Esperada imagem grayscale {src_w}x{src_h}; recebida {img_gray.shape}.")
    x = np.arange(out_w) * src_w // out_w
    y = np.arange(out_h) * src_h // out_h
    x1, y1 = np.minimum(x + 1, src_w - 1), np.minimum(y + 1, src_h - 1)
    source = img_gray.astype(np.uint16)
    return ((source[np.ix_(y, x)] + source[np.ix_(y, x1)]
             + source[np.ix_(y1, x)] + source[np.ix_(y1, x1)]) >> 2).astype(np.uint8)


def load_dataset(directory):
    """Recusa arrays antigos, incompletos, misturados ou sem proveniência."""
    directory = Path(directory)
    manifest_path = directory / "manifest.json"
    if not manifest_path.is_file():
        raise ValueError("Dataset sem manifest.json v2. Execute 02_preprocessar.py novamente.")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("schema_version") != SCHEMA_VERSION
            or manifest.get("preprocessing") != PREPROCESSING
            or manifest.get("split_unit") != "session"
            or manifest.get("augmentation_splits") != ["train"]):
        raise ValueError("Manifesto incompatível: refaça o pré-processamento v2.")
    declared = manifest.get("sessions", {})
    names = [name for key in SPLITS for name in declared.get(key, [])]
    if (any(not declared.get(key) for key in SPLITS) or len(names) != len(set(names))):
        raise ValueError("Sessões ausentes ou compartilhadas entre os conjuntos.")
    arrays = {}
    source_splits = {}
    for key in SPLITS:
        metadata = manifest["splits"][key]
        for filename, expected in metadata["sha256"].items():
            if Path(filename).name != filename or sha256_file(directory / filename) != expected:
                raise ValueError(f"Artefato alterado ou inválido: {filename}.")
        required = {f"X_{key}.npy", f"y_{key}.npy", f"sources_{key}.csv"}
        if not required.issubset(metadata["sha256"]):
            raise ValueError(f"Manifesto incompleto: {key}.")
        X = np.load(directory / f"X_{key}.npy", mmap_mode="r", allow_pickle=False)
        y = np.load(directory / f"y_{key}.npy", mmap_mode="r", allow_pickle=False)
        if (X.shape != (metadata["samples"], 96, 96, 1) or y.shape != (len(X),)
                or not len(X) or X.dtype != np.float32 or y.dtype != np.float32):
            raise ValueError(f"Formato de arrays inválido: {key}.")
        with open(directory / f"sources_{key}.csv", newline="", encoding="utf-8") as stream:
            count = 0
            for index, row in enumerate(csv.DictReader(stream)):
                if int(row["index"]) != index or row["session"] not in declared[key]:
                    raise ValueError(f"Proveniência inválida: {key}, linha {index}.")
                if key != "train" and row["augmentation"] != "original":
                    raise ValueError("Validação/teste não podem ter augmentation.")
                source = (row["session"], row["frame"])
                previous = source_splits.setdefault(source, key)
                if previous != key:
                    raise ValueError("Mesmo frame encontrado em conjuntos diferentes.")
                count += 1
        if count != len(X):
            raise ValueError(f"Proveniência não corresponde ao número de amostras: {key}.")
        # Verifica por blocos, sem copiar o dataset inteiro para RAM.
        for start in range(0, len(X), 128):
            block = X[start:start + 128]
            labels = y[start:start + 128]
            if (not np.isfinite(block).all() or np.any(block < 0) or np.any(block > 1)
                    or not np.isfinite(labels).all() or np.any(np.abs(labels) > 1)):
                raise ValueError(f"Pixels/rótulos fora do contrato: {key}.")
        arrays[key] = (X, y)
    return manifest, sha256_file(manifest_path), arrays


def load_model_metadata(model_dir, dataset_sha256, require_tflite=False):
    directory = Path(model_dir)
    path = directory / "training_manifest.json"
    if not path.is_file():
        raise ValueError("Modelo sem training_manifest.json v2; treine novamente com o dataset v2.")
    metadata = json.loads(path.read_text(encoding="utf-8"))
    if metadata.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("Manifesto de treinamento incompatível; treine com o pipeline v2.")
    if metadata.get("dataset_manifest_sha256") != dataset_sha256:
        raise ValueError("Modelo foi treinado com outro dataset; use artefatos da mesma execução.")
    if metadata.get("keras_sha256") != sha256_file(directory / "modelo_linha.keras"):
        raise ValueError("Modelo Keras alterado desde o treinamento registrado.")
    if require_tflite:
        conversion = json.loads((directory / "conversion_manifest.json").read_text(encoding="utf-8"))
        if (conversion.get("schema_version") != SCHEMA_VERSION
                or conversion.get("dataset_manifest_sha256") != dataset_sha256
                or conversion.get("keras_sha256") != metadata["keras_sha256"]
                or conversion.get("tflite_sha256") != sha256_file(directory / "modelo_linha.tflite")
                or conversion.get("calibration_split") != "train"):
            raise ValueError("TFLite sem vínculo válido com o modelo/dataset de treino.")
    return metadata


def regression_metrics(actual, predicted, threshold=0.05):
    actual = np.asarray(actual, dtype=np.float64).reshape(-1)
    predicted = np.asarray(predicted, dtype=np.float64).reshape(-1)
    if (actual.shape != predicted.shape or not len(actual)
            or not np.isfinite(actual).all() or not np.isfinite(predicted).all()):
        raise ValueError("Métricas exigem vetores finitos de mesmo tamanho e não vazios.")
    errors = predicted - actual
    directions = lambda values: np.where(values < -threshold, -1, np.where(values > threshold, 1, 0))
    target_class, pred_class = directions(actual), directions(predicted)
    denominator = float(np.sum((actual - actual.mean()) ** 2))
    classes = {}
    for code, name in [(-1, "left"), (0, "straight"), (1, "right")]:
        mask = target_class == code
        classes[name] = {"count": int(mask.sum()),
                         "mae": float(np.abs(errors[mask]).mean()) if mask.any() else None,
                         "direction_accuracy": float((pred_class[mask] == code).mean()) if mask.any() else None}
    curves = target_class != 0
    return {"samples": len(actual), "mae": float(np.abs(errors).mean()),
            "mse": float(np.mean(errors ** 2)),
            "p95_absolute_error": float(np.quantile(np.abs(errors), 0.95)),
            "r2": 1 - float(np.sum(errors ** 2)) / denominator if denominator > 0 else None,
            "direction_accuracy": float((target_class == pred_class).mean()),
            "curve_direction_accuracy": float((target_class[curves] == pred_class[curves]).mean()) if curves.any() else None,
            "by_direction": classes}


def predict_keras(model, X, batch_size=64):
    return np.concatenate([np.asarray(model(X[start:start + batch_size], training=False)).reshape(-1)
                           for start in range(0, len(X), batch_size)])


def predict_tflite(interpreter, X):
    interpreter.allocate_tensors()
    input_info = interpreter.get_input_details()[0]
    output_info = interpreter.get_output_details()[0]
    if (tuple(input_info["shape"]) != (1, 96, 96, 1)
            or int(np.prod(output_info.get("shape", (1, 1)))) != 1
            or input_info["dtype"] != np.int8 or output_info["dtype"] != np.int8):
        raise ValueError("Contrato TFLite esperado: entrada INT8 [1,96,96,1], saída INT8.")
    in_scale, in_zero = input_info["quantization"]
    out_scale, out_zero = output_info["quantization"]
    if in_scale <= 0 or out_scale <= 0:
        raise ValueError("Escalas INT8 inválidas.")
    predictions = np.empty(len(X), dtype=np.float32)
    for index, frame in enumerate(X):
        # Firmware usa lroundf: arredondamento de empates para longe de zero.
        scaled = frame[np.newaxis] / in_scale
        quantized = np.copysign(np.floor(np.abs(scaled) + 0.5), scaled) + in_zero
        interpreter.set_tensor(input_info["index"], np.clip(quantized, -128, 127).astype(np.int8))
        interpreter.invoke()
        raw = int(interpreter.get_tensor(output_info["index"]).reshape(-1)[0])
        predictions[index] = (raw - out_zero) * out_scale
    return predictions
