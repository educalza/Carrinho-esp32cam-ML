"""Quantização INT8 calibrada somente com treino; não publica no firmware."""
import argparse
from pathlib import Path

import numpy as np

import config
from pipeline_utils import (load_dataset, load_model_metadata, predict_keras, predict_tflite,
                            regression_metrics, save_json, sha256_file)


def calibration_indices(count, limit, seed):
    if count < 1 or limit < 1:
        raise ValueError("Calibração exige pelo menos uma amostra de treino.")
    return np.random.default_rng(seed).choice(count, size=min(count, limit), replace=False)


def converter_para_tflite(dataset_dir=config.DATASET_PROC_DIR, model_dir=config.MODEL_DIR):
    directory = Path(model_dir)
    _, dataset_hash, arrays = load_dataset(dataset_dir)
    metadata = load_model_metadata(directory, dataset_hash)
    outputs = [directory / name for name in
               ("modelo_linha.tflite", "modelo_linha.h", "conversion_manifest.json")]
    if any(path.exists() for path in outputs):
        raise FileExistsError("Conversão já existe. Preserve-a e use outro diretório de modelo para nova execução.")
    import tensorflow as tf
    model = tf.keras.models.load_model(directory / "modelo_linha.keras")
    X_train, _ = arrays["train"]
    selected = calibration_indices(len(X_train), config.CALIBRATION_SAMPLES, config.RANDOM_STATE)

    def representative_data_gen():
        for index in selected:
            yield [np.asarray(X_train[index:index + 1], dtype=np.float32)]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_data_gen
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    converter._experimental_disable_per_channel_quantization_for_dense_layers = True
    content = converter.convert()
    interpreter = tf.lite.Interpreter(model_content=content)
    X_val, y_val = arrays["val"]
    keras_pred = predict_keras(model, X_val, config.BATCH_SIZE)
    tflite_pred = predict_tflite(interpreter, X_val)
    keras_metrics = regression_metrics(y_val, keras_pred, config.DIRECTION_THRESHOLD)
    tflite_metrics = regression_metrics(y_val, tflite_pred, config.DIRECTION_THRESHOLD)
    outputs[0].write_bytes(content)
    with outputs[1].open("w", encoding="utf-8") as stream:
        stream.write("// Gerado pelo pipeline v2; revisar avaliação antes de publicar.\n")
        stream.write("#ifndef MODELO_LINHA_H\n#define MODELO_LINHA_H\n\n")
        stream.write(f"const unsigned int g_model_len = {len(content)};\n")
        stream.write("alignas(16) const unsigned char g_model[] = {\n")
        for offset in range(0, len(content), 12):
            stream.write("    " + ", ".join(f"0x{value:02x}" for value in content[offset:offset + 12]) + ",\n")
        stream.write("};\n\n#endif // MODELO_LINHA_H\n")
    save_json(outputs[2], {
        "schema_version": 2, "dataset_manifest_sha256": dataset_hash,
        "keras_sha256": metadata["keras_sha256"], "tflite_sha256": sha256_file(outputs[0]),
        "header_sha256": sha256_file(outputs[1]), "tensorflow_version": tf.__version__,
        "direction_threshold": config.DIRECTION_THRESHOLD,
        "calibration_split": "train", "calibration_indices": selected.tolist(),
        "validation": {"keras": keras_metrics, "tflite": tflite_metrics,
                       "quantization_mae_delta": tflite_metrics["mae"] - keras_metrics["mae"],
                       "keras_tflite_prediction_mae": float(np.abs(keras_pred - tflite_pred).mean())},
    })
    print(f"INT8: {len(content) / 1024:.1f} KiB; val MAE Keras={keras_metrics['mae']:.4f}, "
          f"TFLite={tflite_metrics['mae']:.4f}.")
    print("Execute avaliar_precisao.py para o teste reservado. Header do firmware não foi alterado.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-dir", default=config.DATASET_PROC_DIR)
    parser.add_argument("--model-dir", default=config.MODEL_DIR)
    args = parser.parse_args()
    converter_para_tflite(args.dataset_dir, args.model_dir)
