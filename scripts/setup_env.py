#!/usr/bin/env python3
"""Xiaozhi environment setup: check everything the Builder needs and install what is missing.

    setup_env.py --check          exit code 0 when the environment is complete (prints what is missing)
    setup_env.py                  small window: install what is missing (used by XiaozhiBuilder.bat / XiaozhiSetup.exe)
    setup_env.py --silent         same without a window (log on stdout)

Needs only the Python standard library. Steps: Git -> ESP-IDF -> toolchain -> ESP-IDF Python packages.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from builder import core  # noqa: E402  (stdlib only)

IDF_TAG = "v6.0.1"
IDF_URL = "https://github.com/espressif/esp-idf.git"
TOOL_DIRS = ("xtensa-esp-elf", "cmake", "ninja")          # esp32 / esp32s3 builds
PY_MODULES = {                                              # import name -> pip name
    "esptool": "esptool", "serial": "pyserial", "cryptography.x509": "cryptography",
    "idf_component_manager": "idf-component-manager", "esp_idf_monitor": "esp-idf-monitor",
    "esp_idf_size": "esp-idf-size", "kconfiglib": "esp-idf-kconfig",
}
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


# ----------------------------------------------------------------------------- discovery
def find_git() -> str | None:
    found = shutil.which("git")
    if found:
        return found
    for candidate in (r"C:\Program Files\Git\cmd\git.exe", r"C:\Program Files (x86)\Git\cmd\git.exe"):
        if Path(candidate).is_file():
            return candidate
    return None


def find_idf() -> Path | None:
    found = core.find_idf_candidates()
    return found[0] if found else None


def tools_dir(idf: Path | None) -> Path:
    return (core.idf_tools_path(idf) if idf else None) or Path.home() / ".espressif"


def env_python(tools: Path) -> Path | None:
    for folder in sorted((tools / "python_env").glob("idf*_env"), reverse=True):
        exe = folder / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        if exe.is_file():
            return exe
    return None


def bad_modules(python: Path) -> list[str]:
    """Modules of the ESP-IDF Python env that do not import (missing or half installed)."""
    code = ("import importlib,json;bad=[]\n"
            "for m in %r:\n"
            "    try: importlib.import_module(m)\n"
            "    except Exception: bad.append(m)\n"
            "print(json.dumps(bad))" % list(PY_MODULES))
    try:
        result = subprocess.run([str(python), "-c", code], capture_output=True, text=True, timeout=120,
                                creationflags=NO_WINDOW)
        return json.loads(result.stdout.strip().splitlines()[-1])
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        return list(PY_MODULES)


def check() -> list[tuple[str, str]]:
    """(key, human text) of everything that is missing, in installation order."""
    issues: list[tuple[str, str]] = []
    idf = find_idf()
    if not find_git() and not idf:
        issues.append(("git", "Git is not installed"))
    if not idf:
        issues.append(("idf", "ESP-IDF %s is not installed" % IDF_TAG))
        return issues + [("tools", "Toolchain (compiler, CMake, Ninja) is not installed"),
                         ("pyenv", "ESP-IDF Python environment is not installed")]
    tools = tools_dir(idf)
    missing = [d for d in TOOL_DIRS if not any((tools / "tools" / d).glob("*"))]
    if missing:
        issues.append(("tools", "Toolchain parts missing: " + ", ".join(missing)))
    python = env_python(tools)
    if not python:
        issues.append(("pyenv", "ESP-IDF Python environment is not installed"))
    else:
        bad = bad_modules(python)
        if bad:
            issues.append(("pyenv", "Python packages missing or broken: " + ", ".join(bad)))
    return issues


# ----------------------------------------------------------------------------- installation
def run(argv: list[str], log, env: dict | None = None, cwd: Path | None = None) -> int:
    log("$ " + " ".join(argv))
    try:
        process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   encoding="utf-8", errors="replace", cwd=str(cwd) if cwd else None,
                                   env=dict(os.environ, **(env or {})), creationflags=NO_WINDOW)
    except OSError as error:
        log("[ERROR] %s" % error)
        return 1
    for line in process.stdout:
        log(line.rstrip())
    return process.wait()


def install(issues: list[tuple[str, str]], log, targets: str = "esp32s3") -> bool:
    keys = {key for key, _ in issues}
    if "git" in keys:
        log("== Installing Git (winget)")
        if shutil.which("winget"):
            run(["winget", "install", "--id", "Git.Git", "-e", "--silent", "--accept-package-agreements",
                 "--accept-source-agreements"], log)
        if not find_git():
            log("[ERROR] Git could not be installed. Install it from https://git-scm.com/download/win and retry.")
            return False
    if "idf" in keys:
        destination = Path.home() / "esp" / ("esp-idf-" + IDF_TAG)
        log("== Downloading ESP-IDF %s to %s (about 1-2 GB, a few minutes)" % (IDF_TAG, destination))
        destination.parent.mkdir(parents=True, exist_ok=True)
        git = find_git() or "git"
        if run([git, "clone", "-b", IDF_TAG, "--depth", "1", "--recursive", "--shallow-submodules",
                IDF_URL, str(destination)], log):
            log("[ERROR] ESP-IDF download failed (Internet? an incomplete folder was left at %s)" % destination)
            return False
    idf = find_idf() or Path.home() / "esp" / ("esp-idf-" + IDF_TAG)
    env = {"IDF_PATH": str(idf), "IDF_TOOLS_PATH": str(tools_dir(idf)), "MSYSTEM": ""}
    env = {k: v for k, v in env.items() if v}
    script = str(idf / "tools" / "idf_tools.py")
    if "tools" in keys or "idf" in keys:
        log("== Installing the toolchain for %s (compiler, CMake, Ninja...)" % targets)
        if run([sys.executable, script, "install", "--targets", targets], log, env):
            log("[ERROR] toolchain installation failed")
            return False
    if "pyenv" in keys or "idf" in keys:
        log("== Installing the ESP-IDF Python packages")
        if run([sys.executable, script, "install-python-env"], log, env):
            log("[ERROR] Python environment installation failed")
            return False
        python = env_python(tools_dir(idf))
        bad = bad_modules(python) if python else []
        if bad:  # a package installed halfway: reinstall it without trusting the old files
            names = sorted({PY_MODULES[m] for m in bad})
            log("== Repairing: " + ", ".join(names))
            run([str(python), "-m", "pip", "install", "--ignore-installed"] + names, log, env)
    left = check()
    for _, text in left:
        log("[ERROR] still missing: " + text)
    log("== Environment ready" if not left else "== Environment is NOT complete")
    return not left


# ----------------------------------------------------------------------------- front ends
def run_silent() -> int:
    issues = check()
    if not issues:
        print("Environment OK")
        return 0
    return 0 if install(issues, lambda line: print(line, flush=True)) else 1


def run_window() -> int:
    import queue
    import threading
    import tkinter as tk
    from tkinter import ttk

    issues = check()
    if not issues:
        return 0
    root = tk.Tk()
    root.title("Xiaozhi Builder - environment setup")
    root.geometry("720x420")
    root.configure(bg="#f3f5f9")
    tk.Label(root, text="Setting up the build environment", font=("Segoe UI", 14, "bold"), bg="#f3f5f9",
             fg="#14213d").pack(anchor="w", padx=14, pady=(12, 0))
    tk.Label(root, text="Missing: " + "; ".join(text for _, text in issues), bg="#f3f5f9", fg="#6b7280",
             wraplength=690, justify="left").pack(anchor="w", padx=14)
    bar = ttk.Progressbar(root, mode="indeterminate")
    bar.pack(fill="x", padx=14, pady=8)
    bar.start(12)
    text = tk.Text(root, bg="#0f172a", fg="#e2e8f0", font=("Consolas", 9), relief="flat", state="disabled")
    text.pack(fill="both", expand=True, padx=14, pady=(0, 6))
    status = tk.Label(root, text="Working... please keep this window open", bg="#f3f5f9", fg="#1f2937",
                      font=("Segoe UI", 10, "bold"))
    status.pack(anchor="w", padx=14, pady=(0, 10))
    lines: queue.Queue = queue.Queue()
    result = {"ok": None}

    def work() -> None:
        try:
            result["ok"] = install(issues, lines.put)
        except Exception as error:  # show it, never close silently
            lines.put("[ERROR] %s" % error)
            result["ok"] = False

    def pump() -> None:
        while True:
            try:
                line = lines.get_nowait()
            except queue.Empty:
                break
            text.config(state="normal")
            text.insert("end", line + "\n")
            text.see("end")
            text.config(state="disabled")
        if result["ok"] is None:
            root.after(100, pump)
            return
        bar.stop()
        bar.config(mode="determinate", value=100 if result["ok"] else 0)
        status.config(text="Done: the environment is ready." if result["ok"] else
                      "Failed: see the log above (Internet connection?). Close this window and run again.",
                      fg="#15803d" if result["ok"] else "#b91c1c")
        if result["ok"]:
            root.after(1500, root.destroy)

    threading.Thread(target=work, daemon=True).start()
    root.after(100, pump)
    root.mainloop()
    return 0 if result["ok"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="only report what is missing (exit 1 if anything)")
    parser.add_argument("--silent", action="store_true", help="install without a window")
    args = parser.parse_args()
    if args.check:
        issues = check()
        for _, text in issues:
            print("MISSING: " + text)
        print("Environment OK" if not issues else "")
        return 1 if issues else 0
    return run_silent() if args.silent else run_window()


if __name__ == "__main__":
    sys.exit(main())
