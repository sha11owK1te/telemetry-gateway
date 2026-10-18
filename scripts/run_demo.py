"""Start the compiled gateway and Python simulator together; Ctrl+C stops both."""
import argparse
import pathlib
import subprocess
import sys
import time
import urllib.error
import urllib.request
import webbrowser


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be 1..65535")
    root = pathlib.Path(__file__).resolve().parents[1]
    name = "gateway.exe" if sys.platform == "win32" else "gateway"
    binary = next((root / "build" / sub / name for sub in ("", "Release", "Debug")
                   if (root / "build" / sub / name).exists()), None)
    if binary is None:
        raise SystemExit("Build first: python scripts/build.py (or follow README for CMake)")
    data = root / "data"; data.mkdir(exist_ok=True)
    base = f"http://127.0.0.1:{args.port}"
    gateway = simulator = None
    try:
        gateway = subprocess.Popen([str(binary), "--port", str(args.port), "--db", str(data / "telemetry.db"), "--web", str(root / "web")])
        deadline = time.monotonic() + 15
        while True:
            if gateway.poll() is not None:
                raise RuntimeError("Gateway startup failed; check port and database path")
            try:
                with urllib.request.urlopen(base + "/api/health", timeout=0.5): break
            except (urllib.error.URLError, TimeoutError):
                if time.monotonic() > deadline: raise RuntimeError("Gateway startup timed out")
                time.sleep(0.1)
        simulator = subprocess.Popen([sys.executable, str(root / "scripts/simulator.py"), "--url", base])
        print("\nOpen " + base + " | Ctrl+C stops the demo", flush=True)
        if not args.no_browser: webbrowser.open(base)
        while gateway.poll() is None and simulator.poll() is None: time.sleep(0.3)
    except KeyboardInterrupt:
        print("\nStopping demo...")
    finally:
        for process in (simulator, gateway):
            if process is not None and process.poll() is None:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()


if __name__ == "__main__":
    main()
