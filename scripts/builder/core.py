"""Logic of the Xiaozhi Builder. No GUI code here, so it can be unit-tested.

The builder does not reimplement anything: it runs the repository's own `scripts/build.py`
(board selection, language, wake word, sdkconfig), then esptool / idf.py for flashing, all inside
the ESP-IDF environment that `export.bat` / `export.sh` sets up.
"""
from __future__ import annotations

import json
import os
import queue
import re
import shlex
import subprocess
import sys
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

ROOT = Path(__file__).resolve().parents[2]  # repository root (scripts/builder/core.py)
SERIAL_TOOLS = Path(__file__).resolve().parent / "serial_tools.py"
SETTINGS_FILE = Path.home() / ".xiaozhi_builder.json"
MERGED_IMAGE = ROOT / "build" / "merged-binary.bin"


def is_windows() -> bool:
    return os.name == "nt"


# ----------------------------------------------------------------------------- ESP-IDF
def idf_export_script(idf_path: os.PathLike) -> Path:
    return Path(idf_path) / ("export.bat" if is_windows() else "export.sh")


def is_idf_dir(path: os.PathLike) -> bool:
    return bool(path) and idf_export_script(path).is_file() and (Path(path) / "tools" / "idf.py").is_file()


def tools_candidates() -> list[Path]:
    """Folders that may hold the ESP-IDF tools (toolchain, python_env): where export.bat looks."""
    roots: list[Path] = []
    env = os.environ.get("IDF_TOOLS_PATH")
    if env:
        roots.append(Path(env))
    roots.append(Path.home() / ".espressif")
    if is_windows():
        roots += [Path("C:/Espressif"), Path("D:/Espressif"), Path.home() / "Espressif"]
    return [r for r in roots if r.is_dir()]


def idf_tools_path(idf_path: os.PathLike) -> Optional[Path]:
    """IDF_TOOLS_PATH to use for this ESP-IDF: the one with python_env (export.bat defaults to ~/.espressif)."""
    wanted = str(Path(idf_path)).lower()
    candidates = tools_candidates()
    for tools in candidates:  # prefer the folder whose idf-env.json lists this ESP-IDF
        try:
            data = json.loads((tools / "idf-env.json").read_text(encoding="utf-8"))
            if any(str(Path(i.get("path", ""))).lower() == wanted for i in data.get("idfInstalled", {}).values()):
                return tools
        except (OSError, ValueError, AttributeError):
            pass
    for tools in candidates:
        if (tools / "python_env").is_dir():
            return tools
    return None


def find_idf_candidates() -> list[Path]:
    """ESP-IDF folders found on this machine, newest first (IDF_PATH always first)."""
    found: list[Path] = []
    env = os.environ.get("IDF_PATH")
    if env:
        found.append(Path(env))
    home = Path.home()
    roots: list[Path] = [home / "esp", Path("/opt")]
    if is_windows():
        roots += [Path("C:/Espressif/frameworks"), Path("D:/Espressif/frameworks"), home / "Espressif" / "frameworks"]
    for root in roots:
        if root.is_dir():
            found += sorted(root.glob("esp-idf*"), reverse=True)
    # installs recorded by the Espressif installer / VS Code extension (idf-env.json)
    for tools in tools_candidates():
        try:
            data = json.loads((tools / "idf-env.json").read_text(encoding="utf-8"))
            for item in data.get("idfInstalled", {}).values():
                found.append(Path(item.get("path", "")))
        except (OSError, ValueError, AttributeError):
            pass
    unique: list[Path] = []
    for path in found:
        if path not in unique and is_idf_dir(path):
            unique.append(path)
    return unique


def idf_version(idf_path: os.PathLike) -> str:
    version_file = Path(idf_path) / "version.txt"
    try:
        return version_file.read_text(encoding="utf-8").strip().splitlines()[0]
    except (OSError, IndexError):
        return Path(idf_path).name  # a git clone has no version.txt: the folder name (esp-idf-v6.0.1)


# ----------------------------------------------------------------------------- commands
@dataclass
class Command:
    argv: list[str]
    use_idf: bool = True        # run inside the ESP-IDF environment
    cwd: Path = ROOT
    title: str = ""
    env: dict = field(default_factory=dict)   # extra environment variables

    def display(self) -> str:
        return " ".join(shlex.quote(a) if not is_windows() else subprocess.list2cmdline([a]) for a in self.argv)


def popen_spec(command: Command, idf_path: Optional[os.PathLike]) -> dict:
    """Arguments for subprocess.Popen that run `command` (inside ESP-IDF when asked to)."""
    env = dict(os.environ, PYTHONUNBUFFERED="1", PYTHONIOENCODING="utf-8", **command.env)
    spec = dict(cwd=str(command.cwd), env=env)
    if not command.use_idf:
        return dict(spec, args=command.argv, shell=False)
    if not idf_path:
        raise ValueError("ESP-IDF folder is not set")
    export = idf_export_script(idf_path)
    tools = idf_tools_path(idf_path)
    if tools and not os.environ.get("IDF_TOOLS_PATH"):
        env["IDF_TOOLS_PATH"] = str(tools)
    # export.bat refuses to run from MSYS/Git Bash; the build must look like a plain cmd.exe
    env.pop("MSYSTEM", None)
    if is_windows():
        # ~300 -I<long path> arguments overflow Windows' 32767-character command line (ninja: "CreateProcess:
        # The parameter is incorrect"): make CMake/ninja pass them through response files (@file) instead
        env.setdefault("CMAKE_NINJA_FORCE_RESPONSE_FILE", "1")
        # shell=True makes Python run:  cmd.exe /c "<line>"  which keeps the nested quotes intact
        line = 'call "%s" >nul && %s' % (export, subprocess.list2cmdline(command.argv))
        return dict(spec, args=line, shell=True)
    line = "source %s >/dev/null && %s" % (shlex.quote(str(export)), shlex.join(command.argv))
    return dict(spec, args=["bash", "-lc", line], shell=False)


def external_terminal_argv(idf_path: os.PathLike, argv: list[str]) -> Optional[list[str]]:
    """Command that opens a NEW terminal window (for interactive tools such as menuconfig)."""
    export = idf_export_script(idf_path)
    tools = idf_tools_path(idf_path)
    if is_windows():
        prefix = 'set "IDF_TOOLS_PATH=%s" && ' % tools if tools and not os.environ.get("IDF_TOOLS_PATH") else ""
        line = '%scall "%s" && %s' % (prefix, export, subprocess.list2cmdline(argv))
        return ["cmd.exe", "/c", "start", "ESP-IDF", "cmd.exe", "/k", line]
    inner = "source %s && %s; exec bash" % (shlex.quote(str(export)), shlex.join(argv))
    for terminal in ("x-terminal-emulator", "gnome-terminal", "konsole", "xterm"):
        from shutil import which
        if which(terminal):
            return [terminal, "-e", "bash", "-lc", inner] if terminal != "gnome-terminal" else \
                [terminal, "--", "bash", "-lc", inner]
    return None


def stale_build_dir(root: os.PathLike = ROOT) -> bool:
    """True when ./build was configured in another folder (project copied from another PC/folder).

    CMake refuses such a cache, and compiler spec files point at the old path."""
    cache = Path(root) / "build" / "CMakeCache.txt"
    try:
        text = cache.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    here = os.path.normcase(os.path.abspath(str(root))).replace("\\", "/").rstrip("/")
    for line in text.splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:"):
            there = os.path.normcase(line.split("=", 1)[1].strip()).replace("\\", "/").rstrip("/")
            return there != here
    return False


_BAD_BUILD_DIR = ("is different than the directory", "does not match the source", "cannot read spec file",
                  "Could not load cache", "loading 'build.ninja'", "CMakeCache.txt is corrupt")


def looks_like_bad_build_dir(lines) -> bool:
    """Output of a build folder that cannot be reused (made elsewhere, half written, ...)."""
    return any(marker in line for line in lines for marker in _BAD_BUILD_DIR)


def force_clean_build(root: os.PathLike = ROOT) -> None:
    """Delete build/ and the generated sdkconfig files; the next build configures everything again."""
    import shutil
    shutil.rmtree(Path(root) / "build", ignore_errors=True)
    for name in ("sdkconfig", "sdkconfig.old"):
        try:
            (Path(root) / name).unlink()
        except OSError:
            pass


def clean_stale_build(root: os.PathLike = ROOT) -> bool:
    """Remove a build folder (and sdkconfig) that belong to another location. True if cleaned."""
    if not stale_build_dir(root):
        return False
    import shutil
    shutil.rmtree(Path(root) / "build", ignore_errors=True)
    for name in ("sdkconfig", "sdkconfig.old"):
        try:
            (Path(root) / name).unlink()
        except OSError:
            pass
    return True


def build_argv(variant: dict, language: str = "", wake_word: str = "", options: Optional[dict] = None) -> list[str]:
    """`python scripts/build.py <board> --name <variant> [...]`, exactly what a developer would type."""
    argv = ["python", "scripts/build.py", variant["board"], "--name", variant["name"]]
    if language:
        argv += ["--language", language]
    if wake_word:
        argv += ["--wake-word", wake_word]
    if options:
        argv += ["--build-options-json", json.dumps(options, separators=(",", ":"))]
    return argv


def flash_commands(port: str, baud: int, image: os.PathLike, chip: str = "", erase_first: bool = False,
                   offset: str = "0x0") -> list[Command]:
    """esptool commands that write a merged firmware image (at offset 0 unless told otherwise)."""
    base = ["python", "-m", "esptool"]
    if chip:
        base += ["--chip", chip]
    base += ["-p", port, "-b", str(baud)]
    commands: list[Command] = []
    if erase_first:
        commands.append(Command(base + ["erase_flash"], title="Erase flash"))
    commands.append(Command(base + ["write_flash", offset, str(image)], title="Flash"))
    return commands


def _esptool(port: str, baud: int, chip: str = "") -> list[str]:
    base = ["python", "-m", "esptool"]
    if chip:
        base += ["--chip", chip]
    return base + ["-p", port, "-b", str(baud)]


def erase_command(port: str, baud: int, chip: str = "") -> Command:
    return Command(_esptool(port, baud, chip) + ["erase_flash"], title="Erase flash")


def verify_command(port: str, baud: int, image: os.PathLike, chip: str = "", offset: str = "0x0") -> Command:
    """Compare the flash content with the image (writes nothing)."""
    return Command(_esptool(port, baud, chip) + ["verify_flash", offset, str(image)], title="Verify flash")


def chip_info_commands(port: str, baud: int, chip: str = "") -> list[Command]:
    base = _esptool(port, baud, chip)
    return [Command(base + ["chip_id"], title="Chip ID"),
            Command(base + ["flash_id"], title="Flash ID"),
            Command(base + ["read_mac"], title="MAC")]


_remembered_python: Optional[str] = None   # last Python version a build log asked for


def wanted_python_version(lines) -> Optional[str]:
    global _remembered_python
    """'3.11' when a log says the venv 'idf6.1_py3.11_env' is missing (the build wants that Python)."""
    for line in lines or ():
        m = re.search(r"python_env[\\/]+idf[\d.]+_py(\d+\.\d+)_env", line)
        if m:
            _remembered_python = m.group(1)
            return _remembered_python
    return _remembered_python


def _python_version(exe: str) -> Optional[str]:
    try:
        out = subprocess.run([exe, "-c", "import sys;print('%d.%d' % sys.version_info[:2])"],
                             capture_output=True, text=True, timeout=20).stdout.strip()
        return out or None
    except (OSError, subprocess.SubprocessError):
        return None


def find_python(version: str) -> Optional[str]:
    """A Python interpreter of exactly `version` (py launcher, Espressif's own Python, PATH)."""
    candidates: list[str] = []
    if is_windows():
        try:
            out = subprocess.run(["py", "-" + version, "-c", "import sys;print(sys.executable)"],
                                 capture_output=True, text=True, timeout=20).stdout.strip()
            if out:
                candidates.append(out)
        except (OSError, subprocess.SubprocessError):
            pass
    roots = list(tools_candidates()) + [Path("C:/Espressif"), Path("D:/Espressif")]
    for root in roots:
        for sub in ("tools/python/python.exe", "tools/python/bin/python3"):
            exe = Path(root) / sub
            if exe.is_file():
                candidates.append(str(exe))
    for exe in candidates:
        if _python_version(exe) == version:
            return exe
    return None


def repair_command(idf_path: os.PathLike, lines=()) -> Command:
    """Reinstall the Python packages of the ESP-IDF python_env (what install.bat does for that step).

    The venv name contains the Python version, so it is created with the Python the build expects
    (taken from the failed build's log) rather than whatever `python` is first on PATH."""
    env = {"IDF_PATH": str(idf_path)}
    tools = idf_tools_path(idf_path)
    if tools:
        env["IDF_TOOLS_PATH"] = str(tools)
    python = "python"
    wanted = wanted_python_version(lines)
    if wanted:
        python = find_python(wanted) or python
    return Command([python, str(Path(idf_path) / "tools" / "idf_tools.py"), "install-python-env"],
                   use_idf=False, title="Repair ESP-IDF Python env", env=env)


# Output of a broken ESP-IDF Python environment (extension installed halfway, new Python, ...)
_BROKEN_ENV = ("check-python-dependencies", "No module named 'esp_idf_monitor'", "Activation script failed",
               "python dependencies ... FAILED", "No module named 'rich_click'", "No module named 'click'",
               "Python virtual environment", "python_env", "Please run the install script",
               "was not spawned within an ESP-IDF shell")


def looks_like_broken_env(lines) -> bool:
    return any(marker in line for line in lines for marker in _BROKEN_ENV)


def size_command() -> Command:
    return Command(["idf.py", "size"], title="Size")


def monitor_command(port: str, baud: int) -> Command:
    return Command(["python", str(SERIAL_TOOLS), "monitor", port, str(baud)], title="Monitor")


def ports_command() -> Command:
    return Command(["python", str(SERIAL_TOOLS), "ports"], title="Ports")


_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    return _ANSI.sub("", text)


def classify_log(line: str) -> str:
    """Colour class for an ESP-IDF runtime log line such as 'E (123) tag: ...'."""
    if line.startswith("E (") or "Guru Meditation" in line or "abort()" in line or "Backtrace:" in line:
        return "error"
    if line.startswith("W ("):
        return "warning"
    return "info"


def parse_offset(text: str) -> Optional[str]:
    """'0x10000' / '65536' -> '0x10000'; None when it is not a number."""
    try:
        value = int(text.strip(), 0)
    except ValueError:
        return None
    return hex(value) if value >= 0 else None


# ----------------------------------------------------------------------------- boards
def load_boards(python: str = sys.executable) -> list[dict]:
    """Variants reported by `scripts/build.py --list-boards --json` (works without ESP-IDF)."""
    result = subprocess.run([python, "scripts/build.py", "--list-boards", "--json"], cwd=str(ROOT),
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise RuntimeError("build.py --list-boards failed:\n" + (result.stderr or result.stdout)[-800:])
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("build.py printed something that is not JSON: %s" % error)


def load_languages(python: str = sys.executable) -> list[str]:
    result = subprocess.run([python, "scripts/build.py", "--list-languages", "--json"], cwd=str(ROOT),
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    try:
        return list(json.loads(result.stdout))
    except (json.JSONDecodeError, TypeError):
        return ["zh-CN", "en-US", "vi-VN"]


def filter_variants(variants: list[dict], text: str) -> list[dict]:
    """Every word of `text` must appear in the name, board, chip or display name."""
    words = text.lower().split()
    if not words:
        return list(variants)
    out = []
    for variant in variants:
        haystack = " ".join(str(variant.get(k, "")) for k in ("display_name", "board", "name", "target")).lower()
        if all(word in haystack for word in words):
            out.append(variant)
    return out


def default_options(variant: dict) -> dict:
    return {o["key"]: o.get("default") for o in variant.get("build_options", [])}


def changed_options(variant: dict, values: dict) -> dict:
    """Only the options the user changed: the rest keep the board's own defaults."""
    defaults = default_options(variant)
    return {k: v for k, v in values.items() if k in defaults and v != defaults[k]}


# ----------------------------------------------------------------------------- output
_PROGRESS = re.compile(r"^\[(\d+)/(\d+)\]")
_ERROR = re.compile(r"\berror\b|\bfailed\b|traceback|fatal|\[ERROR\]", re.IGNORECASE)
_WARNING = re.compile(r"\bwarning\b|\[WARN\]", re.IGNORECASE)


def parse_progress(line: str) -> Optional[tuple[int, int]]:
    """(done, total) from a ninja line such as `[123/456] Building C object ...`."""
    match = _PROGRESS.match(line)
    if not match:
        return None
    done, total = int(match.group(1)), int(match.group(2))
    return (done, total) if total > 0 else None


_SUCCESS = re.compile(
    r"Project build complete|Hash of data verified|Chip erase completed|Erase completed successfully|"
    r"Wrote \d+ bytes|Merged binary .* created|^Generated .*\.bin|Leaving\.\.\.|Environment ready|"
    r"^\[\d+/\d+\] Linking CXX executable", re.IGNORECASE)
_STAGE = re.compile(r"^(Executing action|Running cmake|Configuring target|Writing at|Connecting|Detecting chip|"
                    r"Chip type|Features|MAC|Uploading stub|Changing baud|Erasing flash|Verifying|==)", re.IGNORECASE)


def classify_line(line: str) -> str:
    """'error', 'warning', 'success', 'stage' or 'info' (used to colour the log)."""
    if re.search(r"(0|no) errors?\b", line, re.IGNORECASE):
        return "info"
    if _ERROR.search(line):
        return "error"
    if _WARNING.search(line):
        return "warning"
    if _SUCCESS.search(line):
        return "success"
    if _STAGE.match(line):
        return "stage"
    return "info"


def explain_error(lines) -> str:
    """One-sentence hint for the most common failures (shown in the result banner)."""
    text = "\n".join(lines)
    hints = (
        ("CreateProcess|command line is too long|filename or extension is too long", "Windows command/path too long: "
         "move the project to a SHORT folder (for example C:/xz), delete build/ and build again."),
        ("could not open port|PermissionError|Access is denied|Failed to connect", "Serial port busy or board not "
         "in download mode: close the monitor/other program, or hold BOOT while pressing RESET."),
        ("No such file or directory|cannot find|does not exist", "A file or tool is missing: check the path in the log."),
        ("check-python-dependencies|No module named|not installed", "The ESP-IDF Python packages are broken: "
         "run XiaozhiSetup / Tools > Repair ESP-IDF."),
        ("not in the requirements list", "A component misses a dependency in its CMakeLists.txt (see the yellow hint)."),
        ("Connection|timed out|Could not resolve|SSL", "Network problem while downloading components: check the "
         "Internet connection and retry."),
        ("region .* overflowed|will not fit", "The firmware does not fit the partition: pick another partition table."),
    )
    for pattern, hint in hints:
        if re.search(pattern, text, re.IGNORECASE):
            return hint
    return ""


# ----------------------------------------------------------------------------- settings
def load_settings(path: Path = SETTINGS_FILE) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_settings(settings: dict, path: Path = SETTINGS_FILE) -> None:
    try:
        path.write_text(json.dumps(settings, indent=2, ensure_ascii=False), encoding="utf-8")
    except OSError:
        pass  # a read-only home folder must not stop the builder


# ----------------------------------------------------------------------------- running jobs
class JobRunner:
    """Runs commands one after the other in a thread and reports their output line by line."""

    def __init__(self) -> None:
        self._thread: Optional[threading.Thread] = None
        self._process: Optional[subprocess.Popen] = None
        self._stop = threading.Event()
        self._lock = threading.Lock()

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self, commands: list[Command], idf_path: Optional[os.PathLike],
              on_line: Callable[[str], None], on_finish: Callable[[int, bool], None]) -> None:
        if self.running:
            raise RuntimeError("a job is already running")
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, args=(commands, idf_path, on_line, on_finish), daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        with self._lock:
            process = self._process
        if process and process.poll() is None:
            _kill_tree(process)

    def _run(self, commands, idf_path, on_line, on_finish) -> None:
        code = 0
        for command in commands:
            if self._stop.is_set():
                break
            on_line("$ " + command.display())
            try:
                spec = popen_spec(command, idf_path)
                kwargs = dict(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                              text=True, encoding="utf-8", errors="replace", bufsize=1, **spec)
                if is_windows():
                    kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
                else:
                    kwargs["start_new_session"] = True
                process = subprocess.Popen(**kwargs)
            except Exception as error:  # never let the worker thread die silently
                on_line("[ERROR] cannot start: %s" % error)
                code = 1
                break
            with self._lock:
                self._process = process
            with process.stdout:
                for line in process.stdout:
                    on_line(line.rstrip("\r\n"))
            code = process.wait()
            with self._lock:
                self._process = None
            if code != 0 or self._stop.is_set():
                break
        on_finish(code, self._stop.is_set())


def _kill_tree(process: subprocess.Popen) -> None:
    try:
        if is_windows():
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)], capture_output=True)
        else:
            import signal
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
    except (OSError, ProcessLookupError):
        pass


class MonitorSession:
    """Interactive serial monitor: runs serial_tools.py monitor inside ESP-IDF and keeps its stdin for commands."""

    def __init__(self) -> None:
        self._process: Optional[subprocess.Popen] = None

    @property
    def running(self) -> bool:
        return self._process is not None and self._process.poll() is None

    def start(self, port: str, baud: int, idf_path, on_line: Callable[[str], None],
              on_exit: Callable[[int], None]) -> None:
        if self.running:
            return
        spec = popen_spec(monitor_command(port, baud), idf_path)
        kwargs = dict(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                      text=True, encoding="utf-8", errors="replace", bufsize=1, **spec)
        if is_windows():
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_NO_WINDOW
        else:
            kwargs["start_new_session"] = True
        process = subprocess.Popen(**kwargs)
        self._process = process

        def pump() -> None:
            with process.stdout:
                for line in process.stdout:
                    on_line(line.rstrip("\r\n"))
            code = process.wait()
            if self._process is process:
                self._process = None
            on_exit(code)

        threading.Thread(target=pump, daemon=True).start()

    def _write(self, text: str) -> None:
        process = self._process
        if process and process.poll() is None and process.stdin:
            try:
                process.stdin.write(text.replace("\r", " ").replace("\n", " ") + "\n")
                process.stdin.flush()
            except OSError:
                pass

    def send(self, text: str) -> None:
        self._write("T" + text)

    def reset(self) -> None:
        self._write("R")

    def bootloader(self) -> None:
        self._write("B")

    def stop(self) -> None:
        process = self._process
        self._process = None
        if process and process.poll() is None:
            self._write("Q")
            _kill_tree(process)
