"""Train an image classifier on Edge Impulse and download the Arduino library for ESP32-S3.

Uses EI_API_KEY from .env. Data must already be uploaded (dashboard "Edge Impulse 업로드").
Usage:  python train_ei.py [--cycles 30] [--lr 0.0005] [--model transfer_mobilenetv1_a25_d100]
"""
import argparse
import sys
import time
from pathlib import Path

import requests
from dotenv import dotenv_values

ROOT = Path(__file__).resolve().parent
API = "https://studio.edgeimpulse.com/v1/api"
H = {"x-api-key": dotenv_values(ROOT / ".env")["EI_API_KEY"]}
DSP_ID, LEARN_ID = 2, 3

parser = argparse.ArgumentParser()
parser.add_argument("--cycles", type=int, default=30)
parser.add_argument("--lr", type=float, default=0.0005)
parser.add_argument("--model", default="transfer_mobilenetv1_a25_d100")  # MobileNetV2 lost ~25% accuracy after int8 quantization
parser.add_argument("--size", type=int, default=96, help="input image size (square)")
parser.add_argument("--neurons", type=int, default=16)
parser.add_argument("--dropout", type=float, default=0.1)
parser.add_argument("--no-build", action="store_true", help="skip Arduino library build")
args = parser.parse_args()


def call(method, path, **kw):
    r = requests.request(method, f"{API}/{path}", headers=H, timeout=120, **kw)
    r.raise_for_status()
    data = r.json() if "json" in r.headers.get("Content-Type", "") else r.content
    if isinstance(data, dict) and not data.get("success", True):
        sys.exit(f"API error on {path}: {data.get('error')}")
    return data


def wait_job(pid, job_id, title):
    print(f"[{title}] job {job_id}")
    printed = 0
    while True:
        time.sleep(5)
        status = call("GET", f"{pid}/jobs/{job_id}/status")["job"]
        logs = call("GET", f"{pid}/jobs/{job_id}/stdout").get("stdout", [])
        for line in reversed(logs[: len(logs) - printed] if printed else logs):
            text = line.get("data", "").rstrip()
            if text:
                print("   ", text)
        printed = len(logs)
        if status.get("finished"):
            if not status.get("finishedSuccessful"):
                sys.exit(f"[{title}] failed")
            print(f"[{title}] done")
            return


pid = call("GET", "projects")["projects"][0]["id"]
print(f"project {pid}")
counts = {c: call("GET", f"{pid}/raw-data/count", params={"category": c})["count"] for c in ("training", "testing")}
print(f"samples: {counts}")

# 1. impulse: image (NxN) -> Image (RGB) -> Transfer learning
call("POST", f"{pid}/impulse", json={
    "inputBlocks": [{"id": 1, "type": "image", "name": "Image data", "title": "Image data",
                     "imageWidth": args.size, "imageHeight": args.size, "resizeMode": "fit-short"}],
    "dspBlocks": [{"id": DSP_ID, "type": "image", "name": "Image", "title": "Image", "axes": ["image"], "input": 1}],
    "learnBlocks": [{"id": LEARN_ID, "type": "keras-transfer-image", "name": "Transfer learning",
                     "title": "Transfer learning (Images)", "dsp": [DSP_ID]}],
})
call("POST", f"{pid}/dsp/{DSP_ID}", json={"config": {"channels": "RGB"}})

# 2. features
job = call("POST", f"{pid}/jobs/generate-features", json={"dspId": DSP_ID})
wait_job(pid, job["id"], "features")

# 3. train
job = call("POST", f"{pid}/jobs/train/keras/{LEARN_ID}", json={
    "mode": "visual",
    "trainingCycles": args.cycles,
    "learningRate": args.lr,
    "trainTestSplit": 0.2,
    "augmentationPolicyImage": "all",
    "visualLayers": [{"type": args.model, "neurons": args.neurons, "dropoutRate": args.dropout}],
})
wait_job(pid, job["id"], "train")

meta = call("GET", f"{pid}/training/keras/{LEARN_ID}/metadata")
for m in meta.get("modelValidationMetrics", []):
    print(f"validation {m['type']}: accuracy {m['accuracy'] * 100:.1f}%  loss {m['loss']:.3f}")
    if m.get("confusionMatrix"):
        print(f"   confusion matrix (rows=actual {meta.get('labels') or meta.get('classNames')}): {m['confusionMatrix']}")

if args.no_build:
    sys.exit(0)

# 4. Arduino library (int8, TFLite Micro)
job = call("POST", f"{pid}/jobs/build-ondevice-model", params={"type": "arduino"},
           json={"engine": "tflite", "modelType": "int8"})  # EON arena is too small for ESP-NN buffers on ESP32-S3
wait_job(pid, job["id"], "build")
zip_bytes = call("GET", f"{pid}/deployment/download", params={"type": "arduino", "modelType": "int8", "engine": "tflite"})
out = ROOT / "ei_model" / "ei-arduino-library.zip"
out.parent.mkdir(exist_ok=True)
out.write_bytes(zip_bytes)
print(f"saved {out} ({len(zip_bytes) / 1024:.0f} KB)")
