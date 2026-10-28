"""XIAO ESP32-S3 Sense data-collection dashboard.

Run:  python dashboard/app.py   ->  http://localhost:5000
"""
import io
import json
import os
import re
import threading
import zipfile
from datetime import datetime
from pathlib import Path

import requests
from dotenv import load_dotenv
from flask import Flask, Response, abort, jsonify, request, send_file, send_from_directory

ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = ROOT / "data"
CONFIG_FILE = DATA_DIR / "config.json"
UPLOADED_FILE = DATA_DIR / ".ei_uploaded.json"

load_dotenv(ROOT / ".env")
EI_API_KEY = os.getenv("EI_API_KEY", "")
EI_INGESTION_URL = "https://ingestion.edgeimpulse.com/api/{category}/files"

# Edge Impulse takes the label from an HTTP header, so keep labels ASCII
LABEL_RE = re.compile(r"^[A-Za-z0-9_-]{1,40}$", re.ASCII)
FILE_RE = re.compile(r"^[A-Za-z0-9_.-]+\.jpg$", re.ASCII)

DATA_DIR.mkdir(exist_ok=True)
app = Flask(__name__, static_folder="static", static_url_path="/static")
_lock = threading.Lock()


# ---------- helpers ----------
def load_json(path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return default


def save_json(path, obj):
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False), encoding="utf-8")


def esp_host():
    return load_json(CONFIG_FILE, {}).get("esp32_host") or os.getenv("ESP32_HOST", "esp32cam.local")


def class_dir(label):
    if not LABEL_RE.match(label or ""):
        abort(400, "label must be 1-40 chars of A-Z a-z 0-9 _ -")
    return DATA_DIR / label


def image_path(label, filename):
    if not FILE_RE.match(filename or ""):
        abort(400, "bad filename")
    path = class_dir(label) / filename
    if not path.is_file():
        abort(404)
    return path


def list_images(label):
    d = class_dir(label)
    if not d.is_dir():
        return []
    return sorted((p.name for p in d.glob("*.jpg")), reverse=True)  # newest first


def list_classes():
    return sorted(p.name for p in DATA_DIR.iterdir() if p.is_dir() and LABEL_RE.match(p.name))


def uploaded_set(label):
    return set(load_json(UPLOADED_FILE, {}).get(label, []))


# ---------- pages ----------
@app.get("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


# ---------- camera ----------
@app.get("/api/config")
def get_config():
    return jsonify(esp32_host=esp_host(), ei_key_loaded=bool(EI_API_KEY))


@app.post("/api/config")
def set_config():
    host = (request.json or {}).get("esp32_host", "").strip()
    host = re.sub(r"^https?://", "", host).rstrip("/")
    if not host:
        abort(400, "esp32_host required")
    cfg = load_json(CONFIG_FILE, {})
    cfg["esp32_host"] = host
    save_json(CONFIG_FILE, cfg)
    return jsonify(esp32_host=host)


@app.get("/api/stream")
def stream():
    """Proxy the ESP32 MJPEG stream (port 81) so the browser only talks to this server."""
    host = esp_host().split(":")[0]
    try:
        r = requests.get(f"http://{host}:81/stream", stream=True, timeout=(3, 10))
    except requests.RequestException as e:
        return Response(f"stream unavailable: {e}", status=502)

    def generate():
        try:
            yield from r.iter_content(chunk_size=4096)
        except requests.RequestException:
            pass
        finally:
            r.close()

    return Response(generate(), content_type=r.headers.get("Content-Type"),
                    headers={"Cache-Control": "no-store"})


@app.get("/api/camera/status")
def camera_status():
    try:
        r = requests.get(f"http://{esp_host()}/status", timeout=3)
        r.raise_for_status()
        return jsonify(online=True, **r.json())
    except (requests.RequestException, ValueError) as e:
        return jsonify(online=False, error=str(e))


@app.post("/api/camera/control")
def camera_control():
    body = request.json or {}
    try:
        r = requests.get(f"http://{esp_host()}/control",
                         params={"var": body.get("var"), "val": int(body.get("val"))}, timeout=3)
        return jsonify(ok=r.ok, msg=r.text), (200 if r.ok else 400)
    except (requests.RequestException, TypeError, ValueError) as e:
        return jsonify(ok=False, msg=str(e)), 502


@app.post("/api/capture")
def capture():
    label = (request.json or {}).get("label", "")
    d = class_dir(label)
    try:
        r = requests.get(f"http://{esp_host()}/capture", timeout=5)
        r.raise_for_status()
    except requests.RequestException as e:
        return jsonify(ok=False, error=f"capture failed: {e}"), 502
    if not r.content.startswith(b"\xff\xd8"):
        return jsonify(ok=False, error="ESP32 did not return a JPEG"), 502

    d.mkdir(exist_ok=True)
    name = f"{label}.{datetime.now().strftime('%Y%m%d_%H%M%S_%f')[:-3]}.jpg"
    (d / name).write_bytes(r.content)
    return jsonify(ok=True, label=label, file=name, size=len(r.content),
                   frame=r.headers.get("X-Frame-Size"), count=len(list_images(label)))


# ---------- classes ----------
@app.get("/api/classes")
def get_classes():
    return jsonify([
        {"name": c, "count": len(imgs := list_images(c)),
         "uploaded": len(uploaded_set(c) & set(imgs))}
        for c in list_classes()
    ])


@app.post("/api/classes")
def create_class():
    label = (request.json or {}).get("name", "").strip()
    class_dir(label).mkdir(exist_ok=True)
    return jsonify(name=label)


@app.delete("/api/classes/<label>")
def delete_class(label):
    d = class_dir(label)
    if not d.is_dir():
        abort(404)
    for p in d.glob("*"):
        p.unlink()
    d.rmdir()
    with _lock:
        up = load_json(UPLOADED_FILE, {})
        up.pop(label, None)
        save_json(UPLOADED_FILE, up)
    return jsonify(ok=True)


# ---------- images ----------
@app.get("/api/images/<label>")
def get_images(label):
    up = uploaded_set(label)
    return jsonify([{"file": f, "uploaded": f in up} for f in list_images(label)])


@app.get("/images/<label>/<filename>")
def get_image(label, filename):
    return send_file(image_path(label, filename), mimetype="image/jpeg")


@app.post("/api/images/<label>/delete")
def delete_images(label):
    files = (request.json or {}).get("files", [])
    deleted = 0
    for f in files:
        try:
            image_path(label, f).unlink()
            deleted += 1
        except Exception:
            pass
    return jsonify(deleted=deleted, count=len(list_images(label)))


# ---------- download ----------
def zip_response(labels, zipname):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:  # JPEGs are already compressed
        for label in labels:
            for f in list_images(label):
                zf.write(DATA_DIR / label / f, f"{label}/{f}")
    buf.seek(0)
    return send_file(buf, mimetype="application/zip", as_attachment=True, download_name=zipname)


@app.get("/api/download/<label>")
def download_class(label):
    class_dir(label)
    return zip_response([label], f"{label}.zip")


@app.get("/api/download")
def download_all():
    return zip_response(list_classes(), f"dataset_{datetime.now():%Y%m%d_%H%M}.zip")


# ---------- Edge Impulse upload ----------
@app.post("/api/ei/upload/<label>")
def ei_upload(label):
    """Upload images of a class to Edge Impulse (ingestion API). Already-uploaded files are skipped."""
    if not EI_API_KEY:
        return jsonify(ok=False, error="EI_API_KEY not set in .env"), 400
    category = (request.json or {}).get("category", "split")
    if category not in ("training", "testing", "split"):
        abort(400, "category must be training|testing|split")

    done = uploaded_set(label)
    images = list_images(label)
    pending = [f for f in images if f not in done]
    ok, failed = [], []
    for i in range(0, len(pending), 20):
        batch = pending[i:i + 20]
        handles = [open(DATA_DIR / label / f, "rb") for f in batch]
        try:
            r = requests.post(
                EI_INGESTION_URL.format(category=category),
                headers={"x-api-key": EI_API_KEY, "x-label": label, "x-disallow-duplicates": "1"},
                files=[("data", (f, h, "image/jpeg")) for f, h in zip(batch, handles)],
                timeout=60,
            )
            r.raise_for_status()
            results = r.json().get("files", [])
            for f, res in zip(batch, results):
                # A duplicate is already in the project, so count it as uploaded
                if res.get("success") or "duplicate" in str(res.get("error", "")).lower():
                    ok.append(f)
                else:
                    failed.append({"file": f, "error": res.get("error")})
        except (requests.RequestException, ValueError) as e:
            failed += [{"file": f, "error": str(e)} for f in batch]
        finally:
            for h in handles:
                h.close()

    with _lock:
        up = load_json(UPLOADED_FILE, {})
        up[label] = sorted(set(up.get(label, [])) | set(ok))
        save_json(UPLOADED_FILE, up)
    return jsonify(ok=not failed, uploaded=len(ok), skipped=len(images) - len(pending), failed=failed)


@app.errorhandler(400)
@app.errorhandler(404)
def json_error(e):
    return jsonify(ok=False, error=e.description), e.code


if __name__ == "__main__":
    print(f"ESP32 host: {esp_host()}   data dir: {DATA_DIR}")
    app.run(host="0.0.0.0", port=5000, threaded=True)
