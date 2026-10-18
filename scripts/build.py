"""Portable direct build without CMake. Needs GCC/Clang or --zig /path/to/zig."""
import argparse
import os
import pathlib
import shlex
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", default=os.environ.get("ZIG_EXE"))
    args = parser.parse_args()
    zig_path = str(pathlib.Path(args.zig).resolve()) if args.zig else None
    root = pathlib.Path(__file__).resolve().parents[1]
    os.chdir(root)
    build = root / "build"; build.mkdir(exist_ok=True)
    os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", str(build / "zig-global-cache"))
    os.environ.setdefault("ZIG_LOCAL_CACHE_DIR", str(build / "zig-local-cache"))
    if args.zig:
        compiler = zig_path
        cc, cxx = [compiler, "cc"], [compiler, "c++"]
    else:
        cc = shlex.split(os.environ.get("CC", "gcc"))
        cxx = shlex.split(os.environ.get("CXX", "g++"))
        if not shutil.which(cxx[0]):
            parser.error("C++ compiler missing. Install GCC/Clang, or pass --zig <path to zig executable>.")
    def run(command):
        print("+ " + subprocess.list2cmdline(command), flush=True)
        subprocess.run(command, check=True)
    run(cc + ["-std=c99", "-O2", "-DSQLITE_THREADSAFE=1", "-DSQLITE_OMIT_LOAD_EXTENSION", "-c", "vendor/sqlite3.c", "-o", "build/sqlite3.o"])
    run(cc + ["-std=c99", "-O2", "-c", "vendor/serial-frame/serial_frame.c", "-o", "build/serial_frame.o"])
    flags = ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Ivendor", "-Isrc"]
    output = "build/gateway.exe" if sys.platform == "win32" else "build/gateway"
    libraries = ["-lws2_32", "-lcrypt32"] if sys.platform == "win32" else ["-pthread", "-ldl"]
    run(cxx + flags + ["src/main.cpp", "src/gateway.cpp", "build/sqlite3.o", "build/serial_frame.o"] + libraries + ["-o", output])
    print("Built " + str(root / output))


if __name__ == "__main__":
    main()
