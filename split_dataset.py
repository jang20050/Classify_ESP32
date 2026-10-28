"""Copy data/<class>/*.jpg into dataset/{train,test}/<class>/ (originals are kept).

Usage:  python split_dataset.py [--test 0.2] [--seed 42]
"""
import argparse
import random
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DATA_DIR = ROOT / "data"
OUT_DIR = ROOT / "dataset"

parser = argparse.ArgumentParser()
parser.add_argument("--test", type=float, default=0.2, help="test ratio (default 0.2)")
parser.add_argument("--seed", type=int, default=42)
args = parser.parse_args()

if OUT_DIR.exists():
    shutil.rmtree(OUT_DIR)  # rebuilt from data/ every run

rng = random.Random(args.seed)
for class_dir in sorted(p for p in DATA_DIR.iterdir() if p.is_dir()):
    files = sorted(class_dir.glob("*.jpg"))
    rng.shuffle(files)
    n_test = round(len(files) * args.test)
    for split, chunk in (("test", files[:n_test]), ("train", files[n_test:])):
        dst = OUT_DIR / split / class_dir.name
        dst.mkdir(parents=True, exist_ok=True)
        for f in chunk:
            shutil.copy2(f, dst / f.name)
    print(f"{class_dir.name:12s} train {len(files) - n_test:4d}  test {n_test:4d}")
print(f"-> {OUT_DIR}")
