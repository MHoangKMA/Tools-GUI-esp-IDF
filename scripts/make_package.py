"""Make a clean zip of the project to move to another PC.

    python scripts/make_package.py                 -> dist/xiaozhi-package.zip
    python scripts/make_package.py --with-components   also pack managed_components (offline build, +700 MB)

On the other PC: unzip anywhere (short path, no spaces is safest), double-click XiaozhiBuilder.bat.
The .bat installs Python / ESP-IDF / toolchain when they are missing (needs Internet the first time).

Left out on purpose, because they are tied to this PC's folder and would break the build there:
build/, sdkconfig, sdkconfig.old, dist/, __pycache__, .cache, .git, .vscode/.
"""
import argparse
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SKIP_DIRS = {"build", "dist", "__pycache__", ".cache", ".git", ".vscode", ".idea", ".pytest_cache"}
SKIP_FILES = {"sdkconfig", "sdkconfig.old", "Espressif-installer.exe"}   # the installer here is a 404 page


def want(path: Path, with_components: bool) -> bool:
    rel = path.relative_to(ROOT)
    for part in rel.parts[:-1]:
        if part in SKIP_DIRS:
            return False
        if part == "managed_components" and not with_components:
            return False
    return rel.name not in SKIP_FILES and not rel.name.endswith((".pyc", ".log"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--with-components", action="store_true", help="include managed_components")
    parser.add_argument("-o", "--output", default=str(ROOT / "dist" / "xiaozhi-package.zip"))
    args = parser.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    files = [p for p in ROOT.rglob("*") if p.is_file() and want(p, args.with_components)]
    missing = [n for n in ("XiaozhiBuilder.bat", "XiaozhiSetup.exe") if not (ROOT / n).is_file()]
    if missing:
        print("WARNING: not found (the package will not install the environment by itself):", ", ".join(missing))
    top = "xiaozhi-esp32-main"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in files:
            archive.write(path, Path(top) / path.relative_to(ROOT))
    print("%d files, %.1f MB -> %s" % (len(files), out.stat().st_size / 1e6, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
