"""Contratos de dados reais e métricas; não exige treino nem TensorFlow."""
import csv
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

import numpy as np

PIPELINE = Path(__file__).resolve().parents[1] / "ml_pipeline"
sys.path.insert(0, str(PIPELINE))
import config
from pipeline_utils import (load_dataset, load_model_metadata, predict_tflite, regression_metrics,
                            resize_like_firmware, save_json, sha256_file, split_sessions)


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, PIPELINE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


preprocess = load_script("02_preprocessar")
train = load_script("03_treinar_modelo")
convert = load_script("04_converter_tflite")


class SplitTests(unittest.TestCase):
    def test_sessions_never_overlap_and_are_reproducible(self):
        names = [f"session_{index:03d}" for index in range(10)]
        first = split_sessions(names, 0.2, 0.2, 42)
        self.assertEqual(first, split_sessions(list(reversed(names)), 0.2, 0.2, 42))
        self.assertEqual([len(first[key]) for key in ("train", "val", "test")], [6, 2, 2])
        flat = sum(first.values(), [])
        self.assertEqual(set(flat), set(names))
        self.assertEqual(len(flat), len(set(flat)))

    def test_three_sessions_are_minimum(self):
        self.assertTrue(all(split_sessions(["a", "b", "c"], 0.2, 0.2, 42).values()))
        with self.assertRaises(ValueError):
            split_sessions(["a", "b"], 0.2, 0.2, 42)

    def test_explicit_holdout_must_be_complete_and_disjoint(self):
        valid = {"train": ["a"], "val": ["b"], "test": ["c"]}
        self.assertEqual(split_sessions(["a", "b", "c"], 0.2, 0.2, 42, valid), valid)
        with self.assertRaises(ValueError):
            split_sessions(["a", "b", "c"], 0.2, 0.2, 42,
                           {"train": ["a"], "val": ["a"], "test": ["c"]})


class ImageTests(unittest.TestCase):
    def test_resize_matches_integer_firmware_reference(self):
        frame = np.random.default_rng(42).integers(0, 256, (240, 320), dtype=np.uint8)
        actual = resize_like_firmware(frame)
        expected = np.empty((96, 96), dtype=np.uint8)
        for y in range(96):
            for x in range(96):
                sx, sy = x * 320 // 96, y * 240 // 96
                expected[y, x] = sum(int(frame[yy, xx]) for yy in (sy, min(sy + 1, 239))
                                     for xx in (sx, min(sx + 1, 319))) // 4
        np.testing.assert_array_equal(actual, expected)
        with self.assertRaises(ValueError):
            resize_like_firmware(frame[:100])

    def test_augmentation_only_train_with_correct_flip_label(self):
        frame = np.tile(np.linspace(0, 1, 96, dtype=np.float32), (96, 1))
        for key in ("val", "test"):
            self.assertEqual(len(list(preprocess.variants(frame, 0.4, key, np.random.default_rng(3)))), 1)
        augmented = list(preprocess.variants(frame, 0.4, "train", np.random.default_rng(3)))
        self.assertEqual(len(augmented), 4)
        np.testing.assert_array_equal(augmented[1][0], np.fliplr(frame))
        self.assertEqual(augmented[1][1], -0.4)
        self.assertTrue(all(np.min(image) >= 0 and np.max(image) <= 1 for image, _, _ in augmented))


class PipelineIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.raw = self.root / "raw"
        # Simula apenas a leitura JPEG; todo split/resize/augmentation/I/O é executado.
        self.cv2 = types.SimpleNamespace(IMREAD_GRAYSCALE=0,
            imread=lambda path, _: np.load(path, allow_pickle=False))
        self.reader_patch = patch.dict(sys.modules, {"cv2": self.cv2})
        self.reader_patch.start()
        self.addCleanup(self.reader_patch.stop)
        for index in range(3):
            session = self.raw / f"session_{index:03d}"
            session.mkdir(parents=True)
            for frame in range(2):
                with (session / f"frame_{frame:05d}.jpg").open("wb") as stream:
                    np.save(stream, np.full((240, 320), index * 30 + frame, dtype=np.uint8))
            (session / "log.csv").write_text(
                "timestamp_ms,frame,steer,throttle,command_age_ms,label_valid\n"
                "100,frame_00000.jpg,-0.4,0.3,20,1\n"
                "150,frame_00001.jpg,0.5,0.4,10,1\n", encoding="utf-8")

    def test_teacher_alignment_and_provenance_survive_processing(self):
        for session in self.raw.glob("session_*"):
            (session / "log.csv").write_text(
                "timestamp_ms,frame,steer,throttle,label_valid,label_method,decision_timestamp_ms,decision_age_ms,confidence\n"
                "100,frame_00000.jpg,-0.4,0.3,1,teacher_same_frame,120,20,0.8\n"
                "150,frame_00001.jpg,0.5,0.4,1,teacher_same_frame,171,20,0.9\n", encoding="utf-8")
        output = self.root / "teacher"
        preprocess.preprocessar_dataset(self.raw, output, require_aligned=True)
        for split in ("train", "val", "test"):
            with (output / f"sources_{split}.csv").open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertTrue(rows)
            self.assertTrue(all(row["alignment"] == "teacher_same_frame" for row in rows))
            self.assertTrue(all(row["label_method"] == "teacher_same_frame" for row in rows))
            self.assertTrue(all(row["decision_age_ms"] == "20" for row in rows))
            self.assertTrue(all(row["confidence"] in ("0.8", "0.9") for row in rows))

    def test_teacher_invalid_timing_is_rejected_even_without_strict_mode(self):
        session = self.raw / "session_000"
        for decided, age in (("300", "200"), ("99", "0"), ("120", "0"),
                             ("nan", "20"), ("120", "nan"), ("", "20")):
            with self.subTest(decided=decided, age=age):
                (session / "log.csv").write_text(
                    "timestamp_ms,frame,steer,throttle,label_valid,label_method,decision_timestamp_ms,decision_age_ms\n"
                    f"100,frame_00000.jpg,0.4,0.3,1,teacher_same_frame,{decided},{age}\n", encoding="utf-8")
                samples, _ = preprocess.index_samples(self.raw)
                self.assertFalse(any(row["session"] == "session_000" for row in samples))

    def build(self):
        output = self.root / "processed"
        preprocess.preprocessar_dataset(self.raw, output)
        return output

    def test_end_to_end_manifest_and_no_cross_split_augmentation(self):
        output = self.build()
        manifest, digest, arrays = load_dataset(output)
        self.assertEqual(len(digest), 64)
        self.assertEqual([len(arrays[key][0]) for key in ("train", "val", "test")], [8, 2, 2])
        self.assertEqual(manifest["source_summary"]["legacy_alignment_frames"], 0)
        del arrays
        with self.assertRaises(FileExistsError):
            preprocess.preprocessar_dataset(self.raw, output)

    def test_tampered_array_is_rejected(self):
        output = self.build()
        with (output / "y_train.npy").open("ab") as stream:
            stream.write(b"changed")
        with self.assertRaisesRegex(ValueError, "alterado"):
            load_dataset(output)

    def test_old_arrays_without_manifest_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "manifest.json"):
            load_dataset(self.root)

    def test_invalid_alignment_is_filtered(self):
        log_path = self.raw / "session_000" / "log.csv"
        log_path.write_text(log_path.read_text().replace(",20,1", ",301,1"), encoding="utf-8")
        samples, summary = preprocess.index_samples(self.raw)
        self.assertEqual(len(samples), 5)
        self.assertEqual(summary["skipped_rows"], 1)

    def test_reverse_is_filtered_instead_of_aborting_collection(self):
        log_path = self.raw / "session_000" / "log.csv"
        with log_path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            columns = reader.fieldnames
            rows = list(reader)
        rows[0]["throttle"] = "-0.5"
        with log_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
        samples, summary = preprocess.index_samples(self.raw)
        self.assertEqual(len(samples), 5)
        self.assertEqual(summary["skipped_rows"], 1)

    def test_motor_deadband_is_not_treated_as_forward_motion(self):
        log_path = self.raw / "session_000" / "log.csv"
        with log_path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            columns = reader.fieldnames
            rows = list(reader)
        rows[0]["throttle"] = "0.02"
        rows[1]["throttle"] = "0.01"
        with log_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
        samples, summary = preprocess.index_samples(self.raw)
        self.assertEqual(len(samples), 4)
        self.assertEqual(summary["skipped_rows"], 2)

    def test_overlapping_sessions_in_manifest_are_rejected(self):
        output = self.build()
        path = output / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["sessions"]["val"] = manifest["sessions"]["train"]
        save_json(path, manifest)
        with self.assertRaisesRegex(ValueError, "compartilhadas"):
            load_dataset(output)

    def test_model_must_match_dataset_and_model_bytes(self):
        model = self.root / "modelo_linha.keras"
        model.write_bytes(b"test model")
        with self.assertRaisesRegex(ValueError, "training_manifest"):
            load_model_metadata(self.root, "dataset")
        metadata = {"schema_version": 2, "dataset_manifest_sha256": "dataset",
                    "keras_sha256": sha256_file(model)}
        save_json(self.root / "training_manifest.json", metadata)
        self.assertEqual(load_model_metadata(self.root, "dataset"), metadata)
        with self.assertRaisesRegex(ValueError, "outro dataset"):
            load_model_metadata(self.root, "other")
        model.write_bytes(b"different model")
        with self.assertRaisesRegex(ValueError, "alterado"):
            load_model_metadata(self.root, "dataset")


class MetricsTests(unittest.TestCase):
    def test_constant_target_has_no_nan_or_fake_curve_accuracy(self):
        metrics = regression_metrics(np.zeros(3), np.zeros(3))
        self.assertEqual(metrics["mae"], 0)
        self.assertEqual(metrics["direction_accuracy"], 1)
        self.assertIsNone(metrics["r2"])
        self.assertIsNone(metrics["curve_direction_accuracy"])
        json.dumps(metrics, allow_nan=False)

    def test_direction_metrics_measure_curves_separately(self):
        metrics = regression_metrics([-0.8, 0, 0.8], [0, 0, 0])
        self.assertAlmostEqual(metrics["direction_accuracy"], 1 / 3)
        self.assertEqual(metrics["curve_direction_accuracy"], 0)
        self.assertEqual(metrics["by_direction"]["right"]["count"], 1)

    def test_calibration_is_reproducible_unique_and_bounded(self):
        indices = convert.calibration_indices(7, 200, 42)
        self.assertEqual(len(set(indices)), 7)
        np.testing.assert_array_equal(indices, convert.calibration_indices(7, 200, 42))

    def test_training_batches_read_every_frame_once_per_epoch(self):
        X = np.arange(10, dtype=np.float32).reshape(10, 1)
        batches = train.batches(X, X.ravel(), 4, 42, True)
        first = np.concatenate([next(batches)[0].ravel() for _ in range(3)])
        second = np.concatenate([next(batches)[0].ravel() for _ in range(3)])
        np.testing.assert_array_equal(np.sort(first), np.arange(10))
        np.testing.assert_array_equal(np.sort(second), np.arange(10))
        self.assertFalse(np.array_equal(first, second))

    def test_int8_rounding_matches_firmware_and_output_does_not_overflow(self):
        class Interpreter:
            def allocate_tensors(self):
                pass
            def get_input_details(self):
                return [{"shape": (1, 96, 96, 1), "dtype": np.int8,
                         "quantization": (0.25, -128), "index": 0}]
            def get_output_details(self):
                return [{"dtype": np.int8, "quantization": (0.01, -128), "index": 1}]
            def set_tensor(self, index, value):
                self.input = value
            def invoke(self):
                pass
            def get_tensor(self, index):
                return np.array([[127]], dtype=np.int8)
        interpreter = Interpreter()
        prediction = predict_tflite(interpreter, np.full((1, 96, 96, 1), 0.125, dtype=np.float32))
        self.assertTrue(np.all(interpreter.input == -127))
        self.assertAlmostEqual(float(prediction[0]), 2.55, places=5)


if __name__ == "__main__":
    unittest.main()
