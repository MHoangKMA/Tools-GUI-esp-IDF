"""Remove build/ and sdkconfig when they were created in another folder or on another PC.

Run by XiaozhiBuilder.bat before the window opens, so a project copied between machines always
configures from scratch (CMake refuses a cache created elsewhere, and the compiler's spec files
point at the old path). Never fails: a problem here must not stop the Builder from opening.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

try:
    from builder import core

    if core.clean_stale_build(ROOT):
        print("build/ came from another folder or PC: removed it, the next build starts from scratch.")
except Exception as error:  # noqa: BLE001
    print("clean_stale skipped:", error)

# Long project paths break ninja on Windows ("CreateProcess: The parameter is incorrect")
if sys.platform == "win32" and len(str(ROOT)) > 40:
    print("WARNING: the project path is long (%d characters): %s" % (len(str(ROOT)), ROOT))
    print("         If the build stops with 'CreateProcess' / 'parameter is incorrect', move the folder to C:/xz")
