#!/usr/bin/env python3
"""
Host tests for the ESP firmware's protocol core (lib/Fm30Core); no ESP8266,
STM32 or FM30 needed.

  1. test_core         unit tests: sanitizer, command validator, link queue
                       (fake UART and clock: timeouts, retry, stale replies,
                       banner, unsolicited JSON, overflow)
  2. mock selftest     tools/mock_stm32.py checked against cli.c behavior
  3. test_integration  the real link + validator talking to the mock over TCP

Uses g++ or clang++ if one is on PATH, otherwise MSVC (found with vswhere).

    python tools/run_host_tests.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INC = ROOT / "lib" / "Fm30Core" / "src"
SRC = sorted(INC.glob("*.cpp"))
TESTS = {"test_core": ROOT / "test" / "host" / "test_core.cpp",
         "test_integration": ROOT / "test" / "host" / "test_integration.cpp"}
MOCK = ROOT / "tools" / "mock_stm32.py"
MOCK_PORT = 7031


def find_vcvars():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    path = subprocess.check_output(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        text=True).strip()
    bat = Path(path) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    return bat if path and bat.exists() else None


def build(tmp):
    exes = {name: Path(tmp) / (name + (".exe" if os.name == "nt" else "")) for name in TESTS}
    cxx = shutil.which("g++") or shutil.which("clang++")
    if cxx:
        for name, src in TESTS.items():
            extra = ["-lws2_32"] if os.name == "nt" else ["-pthread"]
            subprocess.check_call([cxx, "-std=c++17", "-Wall", "-Wextra", "-O1", "-g", f"-I{INC}",
                                   str(src), *map(str, SRC), "-o", str(exes[name]), *extra])
        return exes
    vcvars = find_vcvars() if os.name == "nt" else None
    if not vcvars:
        sys.exit("no C++ compiler found (need g++, clang++ or MSVC Build Tools)")
    lines = ["@echo off", f'call "{vcvars}" >nul 2>nul']
    for name, src in TESTS.items():
        obj = Path(tmp) / name
        obj.mkdir()
        files = " ".join(f'"{p}"' for p in [src, *SRC])
        lines.append(f'cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /I"{INC}" '
                     f'/Fo"{obj}\\\\" /Fe"{exes[name]}" {files} || exit /b 1')
    script = Path(tmp) / "build.cmd"
    script.write_text("\r\n".join(lines) + "\r\n")
    subprocess.check_call(["cmd", "/c", str(script)], stdout=subprocess.DEVNULL)
    return exes


def main():
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        exes = build(tmp)

        print("== test_core", flush=True)
        if subprocess.call([str(exes["test_core"])]):
            failed.append("test_core")

        print("== mock selftest", flush=True)
        if subprocess.call([sys.executable, str(MOCK), "--selftest"]):
            failed.append("mock selftest")

        print("== test_integration (mock over TCP)", flush=True)
        mock = subprocess.Popen([sys.executable, str(MOCK), "--tcp", str(MOCK_PORT)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            if subprocess.call([str(exes["test_integration"]), str(MOCK_PORT)]):
                failed.append("test_integration")
        finally:
            mock.terminate()
            out = mock.communicate(timeout=5)[0]
        if "OVERFLOW" in out:
            print(out)
            failed.append("mock reported RX ring overflow")

    print("FAILED: " + ", ".join(failed) if failed else "all host tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
