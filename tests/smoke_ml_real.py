"""Verificacao curta com OpenCV/TensorFlow reais e dados sinteticos isolados.

Nao mede qualidade de pilotagem e nao publica nenhum modelo no firmware.
Saidas ficam em .build/ml-smoke-*/ para inspecao.
"""
import csv
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
os.environ.setdefault("MPLBACKEND", "Agg")
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ml_pipeline"))
import cv2
import numpy as np
import config


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "ml_pipeline" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    build = ROOT / ".build"
    build.mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="ml-smoke-", dir=build))
    for number in range(5):
        session = output / "raw" / f"session_{number:03}"
        session.mkdir(parents=True)
        with (session / "log.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.writer(stream)
            writer.writerow(["timestamp_ms", "frame", "steer", "throttle",
                             "command_timestamp_ms", "command_age_ms", "label_valid"])
            for frame, steer in enumerate([-0.6, -0.3, 0.0, 0.1, 0.4, 0.7]):
                image = np.full((240, 320), 180 + number * 5, dtype=np.uint8)
                x = int(150 + steer * 90)
                image[:, x:x+18] = 35 + number
                filename = f"frame_{frame:05}.jpg"
                assert cv2.imwrite(str(session / filename), image)
                writer.writerow([1000 + frame * 50, filename, steer, 0.4,
                                 990 + frame * 50, 10, 1])
    config.EPOCHS = 2
    config.BATCH_SIZE = 8
    config.CALIBRATION_SAMPLES = 12
    dataset, model = output / "dataset", output / "model"
    module("02_preprocessar").preprocessar_dataset(output / "raw", dataset, require_aligned=True)
    module("03_treinar_modelo").treinar(dataset, model)
    module("04_converter_tflite").converter_para_tflite(dataset, model)
    module("avaliar_precisao").avaliar_modelo(dataset, model)
    report = json.loads((model / "avaliacao_test.json").read_text(encoding="utf-8"))
    assert report["tflite"]["samples"] == 6
    assert np.isfinite(report["tflite"]["mae"])
    assert (model / "modelo_linha.h").is_file()
    print(f"SMOKE OK: {output}")


if __name__ == "__main__":
    main()
