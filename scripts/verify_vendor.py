"""Verify exact vendored dependency hashes, without network access."""
import hashlib
import json
import pathlib

root = pathlib.Path(__file__).resolve().parents[1] / "vendor"
manifest = json.loads((root / "dependencies.json").read_text(encoding="utf-8"))
for name, metadata in manifest.items():
    actual = hashlib.sha256((root / name).read_bytes()).hexdigest()
    if actual != metadata["sha256"]:
        raise SystemExit(f"Hash mismatch: {name}")
print(f"Verified {len(manifest)} vendored files")
