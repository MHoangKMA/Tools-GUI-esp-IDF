"""Hardware configuration of a board: find its files, open them in an editor, track who changed what.

The repository is not necessarily a git checkout, so the Builder keeps its own record in
~/.xiaozhi_builder_boards.json and a copy of the original files in ~/.xiaozhi_builder_backup/.

Revision 0 is "original" (the files as the Builder first saw them). Every time the files differ from the
last recorded state a new revision is added (time + files changed). A successful build remembers which
revision it used, so the window can say "edited after the last build".
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[2]
BOARDS = ROOT / "main" / "boards"
STATE_FILE = Path.home() / ".xiaozhi_builder_boards.json"
BACKUP_DIR = Path.home() / ".xiaozhi_builder_backup"
TEXT_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".json", ".md", ".txt", ".cmake", ".yml", ".yaml", ".csv", ".html"}
MAX_FILES = 80
# what each file is for (first match wins), shown in the "Files" menu
LABELS = (("config.h", "Pins and hardware constants (config.h)"),
          ("config.json", "Variants / sdkconfig of the board (config.json)"),
          ("README.md", "Board notes (README.md)"))


def board_dir(variant: dict) -> Optional[Path]:
    path = BOARDS / variant.get("board", "")
    return path if variant.get("board") and path.is_dir() else None


def board_key(variant: dict) -> str:
    return variant.get("board", "").replace("\\", "/")


def config_files(variant: dict) -> list[Path]:
    """Editable text files of the board folder, the important ones first."""
    folder = board_dir(variant)
    if not folder:
        return []
    found = [p for p in folder.rglob("*") if p.is_file() and p.suffix.lower() in TEXT_SUFFIXES
             and len(p.relative_to(folder).parts) <= 3]
    first = ["config.h", "config.json"]
    found.sort(key=lambda p: (0 if p.name in first[:1] else 1 if p.name in first[1:] else
                              2 if p.suffix in (".cc", ".cpp") and "board" in p.name else 3, str(p)))
    return found[:MAX_FILES]


def describe(path: Path) -> str:
    for name, label in LABELS:
        if path.name == name:
            return label
    if path.suffix in (".cc", ".cpp") and "board" in path.name:
        return "Board initialisation code (%s)" % path.name
    return path.name


def _hash(path: Path) -> str:
    try:
        return hashlib.sha1(path.read_bytes()).hexdigest()
    except OSError:
        return ""


def snapshot(variant: dict) -> dict[str, str]:
    folder = board_dir(variant)
    return {p.relative_to(folder).as_posix(): _hash(p) for p in config_files(variant)} if folder else {}


def signature(snap: dict[str, str]) -> str:
    return hashlib.sha1(json.dumps(snap, sort_keys=True).encode()).hexdigest()[:12]


# ----------------------------------------------------------------------------- stored state
def _load() -> dict:
    try:
        data = json.loads(STATE_FILE.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _save(data: dict) -> None:
    try:
        STATE_FILE.write_text(json.dumps(data, indent=1), encoding="utf-8")
    except OSError:
        pass


def _backup(variant: dict) -> None:
    folder = board_dir(variant)
    target = BACKUP_DIR / board_key(variant)
    if not folder or target.exists():
        return
    try:
        for path in config_files(variant):
            destination = target / path.relative_to(folder)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
    except OSError:
        pass


def refresh(variant: dict) -> dict:
    """Record a new revision when the files changed since last time; return the current status."""
    key = board_key(variant)
    data = _load()
    now = snapshot(variant)
    entry = data.get(key)
    if entry is None:                      # first time this board is seen: that state is the original
        _backup(variant)
        entry = data[key] = {"baseline": now, "current": now, "revisions": [], "built_rev": None, "warned": False}
        _save(data)
    elif entry["current"] != now:
        before = entry["current"]
        changed = sorted(f for f in set(before) | set(now) if before.get(f) != now.get(f))
        entry["revisions"].append({"n": len(entry["revisions"]) + 1, "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                                   "changed": changed})
        entry["current"] = now
        _save(data)
    revisions = entry["revisions"]
    revision = revisions[-1]["n"] if revisions else 0
    base = entry["baseline"]
    differs = sorted(f for f in set(base) | set(now) if base.get(f) != now.get(f))
    built = entry.get("built_rev")
    if built is None:
        build_state = "never"
    else:
        build_state = "current" if built == revision else "stale"
    return {"revision": revision, "modified": bool(differs), "differs": differs,
            "last_time": revisions[-1]["time"] if revisions else "", "revisions": revisions,
            "built_rev": built, "build_state": build_state, "warned": entry.get("warned", False),
            "folder": board_dir(variant)}


def mark_built(variant: dict) -> None:
    """Call after a successful build: it used the current files."""
    status = refresh(variant)
    data = _load()
    if board_key(variant) in data:
        data[board_key(variant)]["built_rev"] = status["revision"]
        _save(data)


def mark_warned(variant: dict) -> None:
    data = _load()
    if board_key(variant) in data:
        data[board_key(variant)]["warned"] = True
        _save(data)


def restore_original(variant: dict) -> list[str]:
    """Copy the saved original files back. Returns the files that were restored."""
    folder = board_dir(variant)
    source = BACKUP_DIR / board_key(variant)
    restored: list[str] = []
    if not folder or not source.is_dir():
        raise FileNotFoundError("no backup of the original files was found")
    for path in source.rglob("*"):
        if path.is_file():
            destination = folder / path.relative_to(source)
            if _hash(destination) != _hash(path):
                shutil.copy2(path, destination)
                restored.append(path.relative_to(source).as_posix())
    refresh(variant)
    return restored


def summary(status: dict) -> tuple[str, str]:
    """(text, kind) for the status line; kind is 'ok' / 'warn' / 'idle'."""
    if not status["modified"] and status["revision"] == 0:
        head = "Original configuration: never edited"
    elif not status["modified"]:
        head = "Back to the original configuration (after %d edit(s))" % status["revision"]
    else:
        head = "Edited: revision %d (%s), %s changed from the original" % (
            status["revision"], status["last_time"], ", ".join(status["differs"][:4]) +
            (" ..." if len(status["differs"]) > 4 else ""))
    tail = {"never": "Not built yet with this Builder.",
            "current": "Last build used exactly this configuration.",
            "stale": "Edited after the last build (it used revision %s): build again." % status["built_rev"]
            }[status["build_state"]]
    kind = "warn" if (status["modified"] or status["build_state"] == "stale") else "ok"
    return head + "\n" + tail, kind


# ----------------------------------------------------------------------------- editors
def find_vscode() -> Optional[str]:
    found = shutil.which("code")
    if found:
        return found
    candidates = [Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "Microsoft VS Code" / "bin" / "code.cmd",
                  Path(os.environ.get("ProgramFiles", "")) / "Microsoft VS Code" / "bin" / "code.cmd",
                  Path("/usr/bin/code"), Path("/usr/local/bin/code"),
                  Path("/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code")]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    return None


def open_files(paths: list[Path], folder: Optional[Path] = None) -> str:
    """Open files in VS Code (folder + files) or the system editor. Returns the name of the editor used."""
    code = find_vscode()
    if code:
        argv = [code, "--reuse-window"] + ([str(folder)] if folder else []) + [str(p) for p in paths]
        subprocess.Popen(argv, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        return "VS Code"
    if os.name == "nt":
        for path in paths[:5]:
            try:
                os.startfile(str(path))  # noqa: S606 (opens the file in the associated editor)
            except OSError:
                subprocess.Popen(["notepad.exe", str(path)])
        return "the default editor"
    subprocess.Popen(["xdg-open", str(paths[0])])
    return "the default editor"


def open_folder(folder: Path) -> None:
    if os.name == "nt":
        os.startfile(str(folder))  # noqa: S606
    else:
        subprocess.Popen(["open" if os.uname().sysname == "Darwin" else "xdg-open", str(folder)])
