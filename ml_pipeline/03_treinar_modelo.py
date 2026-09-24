"""Treina usando exclusivamente treino e validação, preservando o teste final."""
import argparse
import os
from pathlib import Path

import numpy as np

import config
from pipeline_utils import load_dataset, save_json, sha256_file


def criar_modelo(tf):
    layers = tf.keras.layers
    model = tf.keras.Sequential([
        layers.Input(shape=(config.IMG_HEIGHT, config.IMG_WIDTH, config.IMG_CHANNELS)),
        layers.Conv2D(4, (5, 5), strides=4, activation="relu", padding="same"),
        layers.Conv2D(8, (3, 3), strides=2, activation="relu", padding="same"),
        layers.Conv2D(12, (3, 3), strides=2, activation="relu", padding="same"),
        layers.GlobalAveragePooling2D(),
        layers.Dense(16, activation="relu"),
        layers.Dense(1, activation="tanh"),
    ])
    model.compile(optimizer=tf.keras.optimizers.Adam(config.LEARNING_RATE),
                  loss="mse", metrics=["mae"])
    return model


def batches(X, y, batch_size, seed, shuffle):
    """Lê somente um lote do memmap; embaralha índices em vez dos pixels."""
    rng = np.random.default_rng(seed)
    while True:
        indices = rng.permutation(len(X)) if shuffle else np.arange(len(X))
        for start in range(0, len(indices), batch_size):
            selected = indices[start:start + batch_size]
            yield np.asarray(X[selected]), np.asarray(y[selected]).reshape(-1, 1)


def treinar(dataset_dir=config.DATASET_PROC_DIR, model_dir=config.MODEL_DIR):
    if config.EPOCHS <= 0 or config.BATCH_SIZE <= 0:
        raise ValueError("EPOCHS e BATCH_SIZE devem ser positivos.")
    directory = Path(model_dir)
    if directory.exists():
        raise FileExistsError(f"{directory} já existe. Escolha --model-dir novo para preservar os modelos.")
    manifest, dataset_hash, arrays = load_dataset(dataset_dir)
    os.environ.setdefault("TF_DETERMINISTIC_OPS", "1")
    import tensorflow as tf
    tf.keras.utils.set_random_seed(config.RANDOM_STATE)
    tf.config.experimental.enable_op_determinism()
    for gpu in tf.config.list_physical_devices("GPU"):
        tf.config.experimental.set_memory_growth(gpu, True)
    directory.mkdir(parents=True)
    model = criar_modelo(tf)
    model.summary()
    X_train, y_train = arrays["train"]
    X_val, y_val = arrays["val"]
    signature = (tf.TensorSpec((None, 96, 96, 1), tf.float32), tf.TensorSpec((None, 1), tf.float32))

    def dataset(X, y, shuffle):
        return tf.data.Dataset.from_generator(
            lambda: batches(X, y, config.BATCH_SIZE, config.RANDOM_STATE, shuffle),
            output_signature=signature).prefetch(1)

    model_path = directory / "modelo_linha.keras"
    callbacks = [
        tf.keras.callbacks.ModelCheckpoint(str(model_path), monitor="val_loss", save_best_only=True),
        tf.keras.callbacks.EarlyStopping(monitor="val_loss", patience=15, restore_best_weights=True,
                                        min_delta=1e-4),
        tf.keras.callbacks.ReduceLROnPlateau(monitor="val_loss", factor=0.5, patience=5, min_lr=1e-6),
        tf.keras.callbacks.TerminateOnNaN(),
    ]
    print(f"Treino: máximo {config.EPOCHS} épocas; batch {config.BATCH_SIZE}; seed {config.RANDOM_STATE}")
    history = model.fit(dataset(X_train, y_train, True),
                        steps_per_epoch=(len(X_train) + config.BATCH_SIZE - 1) // config.BATCH_SIZE,
                        validation_data=dataset(X_val, y_val, False),
                        validation_steps=(len(X_val) + config.BATCH_SIZE - 1) // config.BATCH_SIZE,
                        epochs=config.EPOCHS, callbacks=callbacks, shuffle=False)
    if not all(np.isfinite(values).all() for values in history.history.values()):
        raise ValueError("Treinamento produziu NaN/Inf; nenhum manifesto válido foi emitido.")
    # A avaliação e conversão sempre carregam o checkpoint de menor val_loss do disco.
    save_json(directory / "training_manifest.json", {
        "schema_version": 2, "dataset_manifest_sha256": dataset_hash,
        "keras_sha256": sha256_file(model_path), "seed": config.RANDOM_STATE,
        "epochs_requested": config.EPOCHS, "epochs_completed": len(history.history["loss"]),
        "batch_size": config.BATCH_SIZE, "learning_rate": config.LEARNING_RATE,
        "tensorflow_version": tf.__version__, "numpy_version": np.__version__,
        "train_mean_steer": float(np.mean(y_train)),
        "test_used_for_training": False, "sessions": manifest["sessions"],
        "history": {key: [float(value) for value in values] for key, values in history.history.items()},
    })
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(1, 2, figsize=(12, 5))
    for axis, metric in zip(axes, ("loss", "mae")):
        axis.plot(history.history[metric], label="Treino")
        axis.plot(history.history[f"val_{metric}"], label="Validação")
        axis.set(title=metric.upper(), xlabel="Época")
        axis.legend()
        axis.grid(alpha=0.3)
    figure.tight_layout()
    figure.savefig(directory / "historico_treino.png", dpi=120)
    plt.close(figure)
    print(f"Melhor checkpoint e proveniência salvos em {directory}; teste continua reservado.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-dir", default=config.DATASET_PROC_DIR)
    parser.add_argument("--model-dir", default=config.MODEL_DIR)
    args = parser.parse_args()
    treinar(args.dataset_dir, args.model_dir)
