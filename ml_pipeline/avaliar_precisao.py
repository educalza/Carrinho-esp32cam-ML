"""Compara Keras, INT8 e referências simples nas sessões de teste reservadas."""
import argparse
import csv
import json
from pathlib import Path

import numpy as np

import config
from pipeline_utils import (load_dataset, load_model_metadata, predict_keras, predict_tflite,
                            regression_metrics, save_json, sha256_file)


def avaliar_modelo(dataset_dir=config.DATASET_PROC_DIR, model_dir=config.MODEL_DIR, split="test"):
    directory = Path(model_dir)
    manifest, dataset_hash, arrays = load_dataset(dataset_dir)
    metadata = load_model_metadata(directory, dataset_hash, require_tflite=True)
    import tensorflow as tf
    X, y = arrays[split]
    model = tf.keras.models.load_model(directory / "modelo_linha.keras")
    keras_pred = predict_keras(model, X, config.BATCH_SIZE)
    interpreter = tf.lite.Interpreter(model_path=str(directory / "modelo_linha.tflite"))
    tflite_pred = predict_tflite(interpreter, X)
    metrics = lambda prediction: regression_metrics(y, prediction, config.DIRECTION_THRESHOLD)
    report = {
        "schema_version": 2, "split": split, "dataset_manifest_sha256": dataset_hash,
        "keras_sha256": metadata["keras_sha256"],
        "tflite_sha256": sha256_file(directory / "modelo_linha.tflite"),
        "sessions": manifest["sessions"][split], "direction_threshold": config.DIRECTION_THRESHOLD,
        "keras": metrics(keras_pred), "tflite": metrics(tflite_pred),
        "baseline_zero": metrics(np.zeros(len(y))),
        "baseline_train_mean": metrics(np.full(len(y), metadata["train_mean_steer"])),
        "keras_tflite_prediction_mae": float(np.abs(keras_pred - tflite_pred).mean()),
        "by_session": {},
    }
    report["quantization_mae_delta"] = report["tflite"]["mae"] - report["keras"]["mae"]
    with open(Path(dataset_dir) / f"sources_{split}.csv", newline="", encoding="utf-8") as stream:
        source_sessions = np.array([row["session"] for row in csv.DictReader(stream)])
    for session in sorted(set(source_sessions)):
        mask = source_sessions == session
        report["by_session"][session] = {
            "keras": regression_metrics(y[mask], keras_pred[mask], config.DIRECTION_THRESHOLD),
            "tflite": regression_metrics(y[mask], tflite_pred[mask], config.DIRECTION_THRESHOLD),
        }
    report_path = directory / f"avaliacao_{split}.json"
    save_json(report_path, report)
    print(json.dumps({key: report[key] for key in
                      ("split", "keras", "tflite", "baseline_zero", "baseline_train_mean",
                       "quantization_mae_delta")}, indent=2, ensure_ascii=False))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(1, 2, figsize=(12, 5))
    for axis, name, prediction in zip(axes, ("keras", "tflite"), (keras_pred, tflite_pred)):
        axis.scatter(y, prediction, alpha=0.2, s=10)
        axis.plot([-1, 1], [-1, 1], "r--")
        axis.set(title=f"{name.upper()} | MAE {report[name]['mae']:.4f}",
                 xlabel="Direção humana", ylabel="Direção prevista", xlim=(-1.1, 1.1), ylim=(-1.1, 1.1))
    figure.tight_layout()
    figure.savefig(directory / f"precisao_{split}.png", dpi=120)
    plt.close(figure)
    print(f"Relatório: {report_path}. Compare curvas e sessões, além da média global.")
    if split == "test":
        print("Teste reservado foi consultado. Para novos ajustes, use validação e reserve novas sessões para o próximo teste final.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-dir", default=config.DATASET_PROC_DIR)
    parser.add_argument("--model-dir", default=config.MODEL_DIR)
    parser.add_argument("--split", choices=("val", "test"), default="test")
    args = parser.parse_args()
    avaliar_modelo(args.dataset_dir, args.model_dir, args.split)
