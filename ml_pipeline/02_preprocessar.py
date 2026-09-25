"""Divide sessões ANTES de ampliar imagens; grava arrays com memória limitada."""
import argparse
import csv
import json
from pathlib import Path
import shutil
import tempfile

import numpy as np

import config
from pipeline_utils import (PREPROCESSING, SCHEMA_VERSION, SPLITS, resize_like_firmware,
                            save_json, sha256_file, split_sessions)


def augment_brightness_contrast(img, rng):
    alpha = rng.uniform(*config.AUG_CONTRAST_RANGE)
    beta = rng.uniform(-config.AUG_BRIGHTNESS_RANGE, config.AUG_BRIGHTNESS_RANGE)
    return np.clip(alpha * img + beta, 0, 1).astype(np.float32)


def augment_rotation(img, steer, rng):
    import cv2
    angle = float(rng.uniform(-config.AUG_ROTATION_MAX, config.AUG_ROTATION_MAX))
    h, w = img.shape
    matrix = cv2.getRotationMatrix2D((w / 2, h / 2), angle, 1)
    rotated = cv2.warpAffine(img, matrix, (w, h), borderMode=cv2.BORDER_REPLICATE)
    label = np.clip(steer + angle / config.AUG_ROTATION_MAX * config.AUG_ROTATION_STEER_FACTOR, -1, 1)
    return rotated, float(label)


def variants(image, steer, split, rng):
    yield image, steer, "original"
    if split != "train":
        return
    flipped = np.fliplr(image)
    yield flipped, -steer, "flip"
    yield augment_brightness_contrast(image, rng), steer, "brightness_contrast"
    yield augment_brightness_contrast(flipped, rng), -steer, "flip_brightness_contrast"
    if config.AUG_ROTATION_ENABLED:
        rotated, label = augment_rotation(image, steer, rng)
        yield rotated, label, "rotation"
        rotated, label = augment_rotation(flipped, -steer, rng)
        yield rotated, label, "flip_rotation"


def index_samples(raw_dir, require_aligned=False):
    """Valida JPEGs antes de dimensionar os arquivos; não retém imagens em RAM."""
    import cv2
    raw_dir = Path(raw_dir).resolve()
    samples, skipped, legacy = [], 0, 0
    log_hashes, seen = {}, set()
    for session in sorted(raw_dir.glob("session_*")):
        log_path = session / "log.csv"
        if not log_path.is_file():
            continue
        log_hashes[session.name] = sha256_file(log_path)
        with log_path.open(newline="", encoding="utf-8-sig") as stream:
            reader = csv.DictReader(stream)
            reader.fieldnames = [name.strip() for name in (reader.fieldnames or [])]
            if not {"frame", "steer", "throttle"}.issubset(reader.fieldnames):
                raise ValueError(f"CSV sem colunas obrigatórias: {log_path}")
            for row in reader:
                try:
                    steer, throttle = float(row["steer"]), float(row["throttle"])
                    frame = row["frame"].strip()
                    if not np.isfinite([steer, throttle]).all() or abs(steer) > 1 or abs(throttle) > 1:
                        raise ValueError("rótulo inválido")
                    if throttle <= config.MIN_THROTTLE:
                        skipped += 1
                        continue
                    if "label_valid" in row and int(row["label_valid"]) != 1:
                        skipped += 1
                        continue
                    if "command_age_ms" in row and not 0 <= float(row["command_age_ms"]) <= 300:
                        skipped += 1
                        continue
                    method = row.get("label_method", "").strip()
                    if method not in ("", "capture_history"):
                        skipped += 1
                        continue
                    aligned = "label_valid" in row and "command_age_ms" in row
                    alignment = "capture_history" if aligned else "legacy_unknown"
                    if require_aligned and not aligned:
                        skipped += 1
                        continue
                    image_path = (session / frame).resolve()
                    if image_path.parent != session.resolve():
                        raise ValueError("frame deve estar na própria sessão")
                    identity = (session.name, frame)
                    if identity in seen:
                        raise ValueError("frame duplicado no CSV")
                    seen.add(identity)
                    image = cv2.imread(str(image_path), cv2.IMREAD_GRAYSCALE)
                    if image is None or image.shape != (240, 320):
                        skipped += 1
                        continue
                    legacy += not aligned
                    samples.append({"session": session.name, "frame": frame,
                                    "path": str(image_path), "steer": steer,
                                    "timestamp_ms": row.get("timestamp_ms", ""),
                                    "command_age_ms": row.get("command_age_ms", ""),
                                    "alignment": alignment})
                except (ValueError, TypeError) as error:
                    raise ValueError(f"{log_path}, linha {reader.line_num}: {error}") from error
    if legacy:
        print(f"ATENÇÃO: {legacy} frames antigos sem comprovação de alinhamento temporal.")
    return samples, {"skipped_rows": skipped, "legacy_alignment_frames": legacy,
                     "log_sha256": log_hashes}


def write_split(directory, split, samples, rng):
    import cv2
    factor = (6 if config.AUG_ROTATION_ENABLED else 4) if split == "train" else 1
    count = len(samples) * factor
    X = np.lib.format.open_memmap(directory / f"X_{split}.npy", mode="w+", dtype=np.float32,
                                  shape=(count, config.IMG_HEIGHT, config.IMG_WIDTH, 1))
    y = np.lib.format.open_memmap(directory / f"y_{split}.npy", mode="w+", dtype=np.float32,
                                  shape=(count,))
    columns = ["index", "session", "frame", "augmentation", "timestamp_ms", "command_age_ms", "alignment"]
    with (directory / f"sources_{split}.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        index = 0
        for sample in samples:
            gray = cv2.imread(sample["path"], cv2.IMREAD_GRAYSCALE)
            if gray is None:
                raise ValueError(f"Imagem desapareceu durante o processamento: {sample['path']}")
            normalized = resize_like_firmware(gray).astype(np.float32) / 255.0
            for image, label, augmentation in variants(normalized, sample["steer"], split, rng):
                X[index, :, :, 0], y[index] = image, label
                writer.writerow({"index": index, "augmentation": augmentation,
                                 **{key: sample[key] for key in columns[1:] if key != "augmentation"}})
                index += 1
    X.flush()
    y.flush()
    del X, y
    files = [f"X_{split}.npy", f"y_{split}.npy", f"sources_{split}.csv"]
    return {"samples": count, "source_frames": len(samples),
            "sha256": {name: sha256_file(directory / name) for name in files}}


def preprocessar_dataset(raw_dir=config.DATASET_RAW_DIR, output_dir=config.DATASET_PROC_DIR,
                        split_file=None, require_aligned=False):
    output = Path(output_dir).resolve()
    if output.exists():
        raise FileExistsError(f"Saída já existe: {output}. Escolha --output-dir novo; nenhum dado foi substituído.")
    if (config.IMG_HEIGHT, config.IMG_WIDTH, config.IMG_CHANNELS) != (96, 96, 1):
        raise ValueError("O contrato do firmware atual exige entrada 96x96x1.")
    samples, summary = index_samples(raw_dir, require_aligned)
    explicit = json.loads(Path(split_file).read_text(encoding="utf-8")) if split_file else None
    sessions = split_sessions([sample["session"] for sample in samples], config.VAL_SPLIT,
                              config.TEST_SPLIT, config.RANDOM_STATE, explicit)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=output.name + "_", dir=output.parent))
    try:
        rng = np.random.default_rng(config.RANDOM_STATE)
        metadata = {}
        for split in SPLITS:
            selected = [sample for sample in samples if sample["session"] in sessions[split]]
            print(f"{split}: {len(sessions[split])} sessões, {len(selected)} frames originais")
            metadata[split] = write_split(temporary, split, selected, rng)
        save_json(temporary / "manifest.json", {
            "schema_version": SCHEMA_VERSION, "preprocessing": PREPROCESSING,
            "raw_dir": str(Path(raw_dir).resolve()), "seed": config.RANDOM_STATE,
            "split_unit": "session", "sessions": sessions, "augmentation_splits": ["train"],
            "augmentation": {"rotation_enabled": config.AUG_ROTATION_ENABLED,
                             "rotation_max": config.AUG_ROTATION_MAX,
                             "rotation_steer_factor": config.AUG_ROTATION_STEER_FACTOR,
                             "brightness_range": config.AUG_BRIGHTNESS_RANGE,
                             "contrast_range": list(config.AUG_CONTRAST_RANGE)},
            "filter": {"min_throttle": config.MIN_THROTTLE, "max_command_age_ms": 300,
                       "require_aligned": require_aligned},
            "splits": metadata, "source_summary": summary})
        temporary.rename(output)
    except BaseException:
        if temporary.resolve().parent == output.parent and temporary.name.startswith(output.name + "_"):
            # Preserva a exceção original se o Windows ainda mantiver um memmap aberto.
            shutil.rmtree(temporary, ignore_errors=True)
        raise
    print(f"Dataset independente concluído em {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-dir", default=config.DATASET_RAW_DIR)
    parser.add_argument("--output-dir", default=config.DATASET_PROC_DIR)
    parser.add_argument("--split-file", help="JSON com listas train, val, test de nomes de sessões")
    parser.add_argument("--require-aligned", action="store_true",
                        help="Exige alinhamento comprovado pelo historico de comandos")
    args = parser.parse_args()
    preprocessar_dataset(args.raw_dir, args.output_dir, args.split_file, args.require_aligned)
