#!/usr/bin/env python3
"""Xiaozhi Builder: choose a board, build and flash the firmware from a window.

    python scripts/builder_gui.py

Needs Python with tkinter (the python.org installers include it) and an installed ESP-IDF
(6.0.1 or newer). See docs/builder.md.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from builder.gui import main  # noqa: E402

if __name__ == "__main__":
    main()
