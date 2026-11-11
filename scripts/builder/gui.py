"""Tkinter front end of the Xiaozhi Builder (pick a board, build, upload, erase, verify, serial monitor)."""
from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from builder import boardcfg, core

DEFAULT_LABEL = "(default)"
AUTO_LABEL = "(auto)"
WAKE_WORDS = [AUTO_LABEL, "disabled", "hiesp", "nihaoxiaozhi"]
FLASH_BAUDS = ["115200", "230400", "460800", "921600", "1500000", "2000000"]
MONITOR_BAUDS = ["9600", "74880", "115200", "230400", "460800", "921600", "1500000"]
MONITOR_BAUD = 115200
INSTALLER = core.ROOT / "Espressif-installer.exe"
UPLOAD_LABELS = ("Upload", "Build + Upload", "Upload file")


BG = "#f3f5f9"
CARD = "#ffffff"
INK = "#1f2937"
MUTED = "#6b7280"
HEADER = "#14213d"
ACCENTS = {  # button style -> (normal, hover)
    "Build": ("#2563eb", "#1d4ed8"),
    "Upload": ("#16a34a", "#15803d"),
    "Erase": ("#dc2626", "#b91c1c"),
    "Monitor": ("#7c3aed", "#6d28d9"),
    "Tool": ("#475569", "#334155"),
    "Stop": ("#ea580c", "#c2410c"),
}
BANNER = {  # kind -> (background, text colour)
    "idle": ("#e5e9f0", "#374151"), "run": ("#dbeafe", "#1e40af"),
    "ok": ("#dcfce7", "#166534"), "fail": ("#fee2e2", "#991b1b"), "warn": ("#fef3c7", "#92400e"),
}
CRASH_LOG = Path.home() / ".xiaozhi_builder_error.log"


def apply_theme(root: tk.Tk) -> None:
    """Flat, light look: white cards on a pale background, coloured action buttons."""
    root.configure(bg=BG)
    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass
    style.configure(".", background=BG, foreground=INK, font=("Segoe UI", 9))
    style.configure("TFrame", background=BG)
    style.configure("Card.TFrame", background=CARD)
    style.configure("TLabel", background=BG, foreground=INK)
    style.configure("Muted.TLabel", foreground=MUTED)
    style.configure("TLabelframe", background=BG, bordercolor="#d5dae3", relief="solid")
    style.configure("TLabelframe.Label", background=BG, foreground=HEADER, font=("Segoe UI", 9, "bold"))
    style.configure("TCheckbutton", background=BG)
    style.configure("TButton", padding=(10, 5), background="#e5e9f0", bordercolor="#cbd2dc", relief="flat")
    style.map("TButton", background=[("active", "#d5dbe5"), ("disabled", "#eceff4")],
              foreground=[("disabled", "#9ca3af")])
    for name, (normal, hover) in ACCENTS.items():
        style.configure(name + ".TButton", background=normal, foreground="#ffffff", padding=(14, 8),
                        font=("Segoe UI", 9, "bold"), borderwidth=0, focusthickness=0)
        style.map(name + ".TButton", background=[("disabled", "#cfd5df"), ("active", hover)],
                  foreground=[("disabled", "#8b93a1")])
    style.configure("Treeview", background=CARD, fieldbackground=CARD, rowheight=24, bordercolor="#d5dae3")
    style.configure("Treeview.Heading", background="#e5e9f0", font=("Segoe UI", 9, "bold"), relief="flat")
    style.map("Treeview", background=[("selected", "#16a34a")], foreground=[("selected", "#ffffff")])
    style.configure("TNotebook", background=BG, borderwidth=0)
    style.configure("TNotebook.Tab", padding=(16, 6), background="#e5e9f0")
    style.map("TNotebook.Tab", background=[("selected", CARD)])
    style.configure("Horizontal.TProgressbar", troughcolor="#e5e9f0", background="#2563eb", thickness=8,
                    bordercolor=BG, lightcolor="#2563eb", darkcolor="#2563eb")
    style.configure("TEntry", padding=4, fieldbackground=CARD)
    style.configure("TCombobox", padding=4, fieldbackground=CARD)
    root.option_add("*TCombobox*Listbox.font", ("Segoe UI", 9))


class App:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("Xiaozhi Builder")
        root.geometry("1180x900")
        root.minsize(980, 700)
        apply_theme(root)
        root.report_callback_exception = self._on_ui_error

        self.settings = core.load_settings()
        self.runner = core.JobRunner()
        self.monitor = core.MonitorSession()
        self.events: queue.Queue = queue.Queue()
        self.variants: list[dict] = []
        self.shown: list[dict] = []
        self.languages = ["zh-CN", "en-US", "vi-VN"]
        self.option_vars: dict[str, tuple[str, tk.Variable, dict]] = {}
        self.port_map: dict[str, str] = {}   # text shown in the port box -> device name
        self.resume_monitor = False
        self.recent: list[str] = []   # output lines of the current command (to recognise a broken ESP-IDF)
        self.busy = False
        self.first_error: str | None = None
        self.picked = ''

        self._build_ui()
        self._init_idf()
        threading.Thread(target=self._load_catalog, daemon=True).start()
        self.refresh_ports()
        root.bind("<FocusIn>", self._on_focus)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(60, self._pump)

    # ------------------------------------------------------------------ layout
    def _build_ui(self) -> None:
        menu = tk.Menu(self.root)
        file_menu = tk.Menu(menu, tearoff=False)
        file_menu.add_command(label="Upload a .bin file...", command=self.on_flash_file)
        file_menu.add_command(label="Open the build folder", command=self.on_open_build)
        file_menu.add_command(label="Save the monitor log...", command=self.on_save_monitor)
        file_menu.add_separator()
        file_menu.add_command(label="Quit", command=self.close)
        tools_menu = tk.Menu(menu, tearoff=False)
        tools_menu.add_command(label="Menuconfig (new window)", command=self.on_menuconfig)
        tools_menu.add_command(label="Clean the build (fullclean)", command=self.on_clean)
        tools_menu.add_command(label="Firmware size (idf.py size)", command=self.on_size)
        tools_menu.add_separator()
        tools_menu.add_command(label="Chip / flash / MAC info", command=self.on_chip_info)
        tools_menu.add_command(label="Reset the board (monitor must run)", command=self.on_reset_board)
        tools_menu.add_command(label="Enter download mode (monitor must run)", command=self.on_bootloader)
        tools_menu.add_separator()
        tools_menu.add_command(label="Compare flash with the built image", command=self.on_compare_flash)
        tools_menu.add_separator()
        tools_menu.add_command(label="Repair ESP-IDF Python environment", command=self.on_repair_idf)
        tools_menu.add_command(label="Open an ESP-IDF terminal", command=self.on_idf_terminal)
        tools_menu.add_command(label="Install ESP-IDF (Espressif installer)", command=self.on_install_idf)
        tools_menu.add_command(label="Reload the board list", command=self.reload_catalog)
        menu.add_cascade(label="File", menu=file_menu)
        menu.add_cascade(label="Tools", menu=tools_menu)
        self.root.config(menu=menu)

        header = tk.Frame(self.root, bg=HEADER)
        header.pack(fill="x")
        tk.Label(header, text="Xiaozhi Builder", bg=HEADER, fg="#ffffff",
                 font=("Segoe UI", 17, "bold")).pack(side="left", padx=(18, 8), pady=12)
        tk.Label(header, text="Build  •  Upload  •  Erase  •  Monitor", bg=HEADER, fg="#9fb3d1",
                 font=("Segoe UI", 9)).pack(side="left")
        self.chip_badge = tk.Label(header, text="NO BOARD", bg="#233a66", fg="#ffffff", padx=12, pady=4,
                                   font=("Segoe UI", 9, "bold"))
        self.chip_badge.pack(side="right", padx=18)
        self.idf_chip = tk.Label(header, text="ESP-IDF ?", bg="#233a66", fg="#ffffff", padx=12, pady=4,
                                 font=("Segoe UI", 9, "bold"))
        self.idf_chip.pack(side="right")

        # The settings/buttons (top) and the log (bottom) sit in a vertical paned window:
        # drag the bar between them, or use the "Enlarge log" button, to see more of the log.
        container = ttk.Frame(self.root, padding=(14, 10))
        container.pack(fill="both", expand=True)
        self.paned = ttk.PanedWindow(container, orient="vertical")
        self.paned.pack(fill="both", expand=True)
        outer = ttk.Frame(self.paned)
        self.top_pane = outer
        self.paned.add(outer, weight=0)
        bottom = ttk.Frame(self.paned)
        self.paned.add(bottom, weight=1)
        self.log_enlarged = False

        # ESP-IDF row (slim)
        idf = ttk.Frame(outer)
        idf.pack(fill="x")
        ttk.Label(idf, text="ESP-IDF folder", style="Muted.TLabel").pack(side="left")
        self.idf_var = tk.StringVar()
        ttk.Entry(idf, textvariable=self.idf_var).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Button(idf, text="Browse...", command=self.on_browse_idf).pack(side="left")
        ttk.Button(idf, text="Detect", command=self._init_idf).pack(side="left", padx=4)
        ttk.Button(idf, text="Set up environment", command=self.on_install_idf).pack(side="left")
        self.idf_info = ttk.Label(idf, text="", style="Muted.TLabel")
        self.idf_var.trace_add("write", lambda *_: self._update_idf_info())

        columns = ttk.Frame(outer)
        columns.pack(fill="x", pady=(10, 0))
        columns.columnconfigure(0, weight=5, uniform="c")
        columns.columnconfigure(1, weight=4, uniform="c")

        # ---- (1) board
        board = ttk.LabelFrame(columns, text="  ①  Choose the board  ", padding=8)
        board.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        self.search_var = tk.StringVar()
        search = ttk.Entry(board, textvariable=self.search_var)
        search.pack(fill="x")
        self.search_var.trace_add("write", lambda *_: self._fill_boards())
        self.pick_card = tk.Label(board, text="No board selected yet: pick one from the list", anchor="w",
                                  bg="#fef3c7", fg="#92400e", padx=10, pady=6, font=("Segoe UI", 10, "bold"))
        self.pick_card.pack(side="bottom", fill="x", pady=(6, 0))
        self.count_label = ttk.Label(board, text="loading the board list...", style="Muted.TLabel")
        self.count_label.pack(side="bottom", anchor="w")
        self.tree = ttk.Treeview(board, columns=("board", "chip"), show="headings", selectmode="browse", height=8)
        self.tree.heading("board", text="Board  (type above to filter)")
        self.tree.heading("chip", text="Chip")
        self.tree.column("board", width=380)
        self.tree.column("chip", width=80, anchor="center")
        scroll = ttk.Scrollbar(board, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True, pady=(6, 0))
        scroll.pack(side="left", fill="y", pady=(6, 0))
        self.tree.bind("<<TreeviewSelect>>", lambda _e: self._on_select())

        # ---- (2) options and (3) connection
        right = ttk.Frame(columns)
        right.grid(row=0, column=1, sticky="nsew")
        self.options_frame = ttk.LabelFrame(right, text="  ②  Build options  ", padding=8)
        self.options_frame.pack(fill="x")
        self.language_var = tk.StringVar(value=self.settings.get("language", DEFAULT_LABEL))
        self.wake_var = tk.StringVar(value=self.settings.get("wake_word", AUTO_LABEL))
        self._fixed_options()

        flash = ttk.LabelFrame(right, text="  ③  Connection and upload  ", padding=8)
        flash.pack(fill="x", pady=(8, 0))
        self.port_var = tk.StringVar(value=self.settings.get("port", ""))
        row = ttk.Frame(flash)
        row.pack(fill="x")
        ttk.Label(row, text="COM port", width=12).pack(side="left")
        self.port_box = ttk.Combobox(row, textvariable=self.port_var, width=30)
        self.port_box.pack(side="left", padx=4, fill="x", expand=True)
        ttk.Button(row, text="Refresh", command=self.refresh_ports).pack(side="left")
        row = ttk.Frame(flash)
        row.pack(fill="x", pady=(6, 0))
        ttk.Label(row, text="Upload speed", width=12).pack(side="left")
        self.baud_var = tk.StringVar(value=str(self.settings.get("baud", "460800")))
        ttk.Combobox(row, textvariable=self.baud_var, values=FLASH_BAUDS, width=9).pack(side="left", padx=4)
        ttk.Label(row, text="Offset").pack(side="left", padx=(10, 0))
        self.offset_var = tk.StringVar(value=self.settings.get("offset", "0x0"))
        ttk.Entry(row, textvariable=self.offset_var, width=9).pack(side="left", padx=4)
        row = ttk.Frame(flash)
        row.pack(fill="x", pady=(6, 0))
        self.erase_var = tk.BooleanVar(value=bool(self.settings.get("erase", False)))
        ttk.Checkbutton(row, text="Erase all flash before upload", variable=self.erase_var).pack(side="left")
        self.auto_monitor_var = tk.BooleanVar(value=bool(self.settings.get("auto_monitor", True)))
        ttk.Checkbutton(row, text="Open monitor after upload", variable=self.auto_monitor_var).pack(side="left", padx=10)

        # ---- hardware configuration of the chosen board (pins etc.)
        cfg = ttk.LabelFrame(outer, text="  Board hardware config  (pins, peripherals: edit, save, then Build)  ",
                             padding=8)
        cfg.pack(fill="x", pady=(10, 0))
        self.cfg_label = tk.Label(cfg, text="Select a board to see which configuration it uses.", anchor="w",
                                  justify="left", bg=BANNER["idle"][0], fg=BANNER["idle"][1], padx=10, pady=6,
                                  font=("Segoe UI", 9, "bold"))
        self.cfg_label.pack(side="left", fill="x", expand=True)
        cfg_buttons = ttk.Frame(cfg)
        cfg_buttons.pack(side="right", padx=(10, 0))
        self.cfg_buttons = {
            "edit": ttk.Button(cfg_buttons, text="✎ Edit config", command=self.on_edit_config, style="Monitor.TButton"),
            "files": ttk.Menubutton(cfg_buttons, text="Files ▾"),
            "folder": ttk.Button(cfg_buttons, text="Folder", command=self.on_open_config_folder),
            "history": ttk.Button(cfg_buttons, text="History", command=self.on_config_history),
            "restore": ttk.Button(cfg_buttons, text="Restore original", command=self.on_restore_config)}
        for button in self.cfg_buttons.values():
            button.pack(side="left", padx=2)
        self.files_menu = tk.Menu(self.cfg_buttons["files"], tearoff=False)
        self.cfg_buttons["files"].config(menu=self.files_menu)
        self.cfg_status: dict | None = None

        # ---- actions
        actions = ttk.Frame(outer)
        actions.pack(fill="x", pady=(12, 0))
        self.buttons: dict[str, ttk.Button] = {}
        for key, label, handler, kind in (
                ("build", "✓  Build", self.on_build, "Build"),
                ("verify", "Verify (no port)", self.on_verify, "Build"),
                ("flash", "→  Upload", self.on_flash, "Upload"),
                ("both", "Build + Upload", self.on_build_flash, "Upload"),
                ("erase", "Erase flash", self.on_erase, "Erase"),
                ("monitor", "Serial Monitor", self.on_monitor, "Monitor"),
                ("info", "Chip info", self.on_chip_info, "Tool")):
            button = ttk.Button(actions, text=label, command=handler, style=kind + ".TButton")
            button.pack(side="left", padx=3)
            self.buttons[key] = button
        self.stop_button = ttk.Button(actions, text="■  Stop", command=self.on_stop, state="disabled",
                                      style="Stop.TButton")
        self.stop_button.pack(side="right", padx=3)
        self.hint = ttk.Label(outer, text="", style="Muted.TLabel")
        self.hint.pack(fill="x", pady=(4, 0))

        # ---- result banner: always says what happened
        self.banner = tk.Frame(outer, bg=BANNER["idle"][0])
        self.banner.pack(fill="x", pady=(8, 0))
        self.banner_label = tk.Label(self.banner, text="Ready", anchor="w", padx=12, pady=7,
                                     font=("Segoe UI", 11, "bold"), bg=BANNER["idle"][0], fg=BANNER["idle"][1])
        self.banner_label.pack(side="left", fill="x", expand=True)
        self.jump_button = ttk.Button(self.banner, text="Jump to first error", command=self.on_jump_error)
        self.copy_button = ttk.Button(self.banner, text="Copy errors", command=lambda: self._copy(self.log, True))

        self.progress = ttk.Progressbar(outer, mode="determinate", maximum=100)
        self.progress.pack(fill="x", pady=(6, 0))
        log_bar = ttk.Frame(bottom)
        log_bar.pack(fill="x", pady=(6, 0))
        self.enlarge_button = ttk.Button(log_bar, text="▲  Enlarge log", command=self.on_toggle_log_size)
        self.enlarge_button.pack(side="right")
        ttk.Label(log_bar, text="Tip: drag the bar above to resize the log", style="Muted.TLabel").pack(side="left")
        self.notebook = ttk.Notebook(bottom)
        self.notebook.pack(fill="both", expand=True, pady=(3, 0))

        out_tab = ttk.Frame(self.notebook, padding=2)
        self.notebook.add(out_tab, text="  Output  ")
        out_bar = ttk.Frame(out_tab)
        out_bar.pack(fill="x", pady=(0, 3))
        ttk.Button(out_bar, text="Clear", command=lambda: self._clear(self.log)).pack(side="right")
        ttk.Button(out_bar, text="Copy errors", command=lambda: self._copy(self.log, errors_only=True)).pack(
            side="right", padx=4)
        ttk.Button(out_bar, text="Copy log", command=lambda: self._copy(self.log)).pack(side="right")
        self.log = self._make_text(out_tab)

        mon_tab = ttk.Frame(self.notebook, padding=2)
        self.notebook.add(mon_tab, text="  Serial Monitor  ")
        mon_bar = ttk.Frame(mon_tab)
        mon_bar.pack(fill="x", pady=(0, 3))
        self.mon_button = ttk.Button(mon_bar, text="Connect", command=self.on_monitor_toggle)
        self.mon_button.pack(side="left")
        ttk.Label(mon_bar, text="Baud").pack(side="left", padx=(8, 2))
        self.mon_baud_var = tk.StringVar(value=str(self.settings.get("monitor_baud", MONITOR_BAUD)))
        ttk.Combobox(mon_bar, textvariable=self.mon_baud_var, values=MONITOR_BAUDS, width=9).pack(side="left")
        ttk.Button(mon_bar, text="Reset board", command=self.on_reset_board).pack(side="left", padx=(8, 0))
        self.autoscroll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(mon_bar, text="Autoscroll", variable=self.autoscroll_var).pack(side="left", padx=8)
        self.timestamp_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(mon_bar, text="Timestamp", variable=self.timestamp_var).pack(side="left")
        ttk.Button(mon_bar, text="Clear", command=lambda: self._clear(self.mon_text)).pack(side="right")
        ttk.Button(mon_bar, text="Save...", command=self.on_save_monitor).pack(side="right", padx=4)
        ttk.Button(mon_bar, text="Copy", command=lambda: self._copy(self.mon_text)).pack(side="right")
        send = ttk.Frame(mon_tab)
        send.pack(fill="x", pady=(3, 0), side="bottom")
        self.send_var = tk.StringVar()
        entry = ttk.Entry(send, textvariable=self.send_var)
        entry.pack(side="left", fill="x", expand=True)
        entry.bind("<Return>", lambda _e: self.on_send())
        ttk.Button(send, text="Send", command=self.on_send).pack(side="left", padx=(4, 0))
        self.mon_text = self._make_text(mon_tab)

        self.status = ttk.Label(container, text="", anchor="w", style="Muted.TLabel")
        self.status.pack(fill="x", pady=(4, 0), side="bottom", before=self.paned)
        self._refresh_buttons()

    def on_toggle_log_size(self) -> None:
        """Hide the settings pane so the log fills the window, or bring it back."""
        if self.log_enlarged:
            self.paned.insert(0, self.top_pane, weight=0)
            self.enlarge_button.configure(text="▲  Enlarge log")
        else:
            self.paned.forget(self.top_pane)
            self.enlarge_button.configure(text="▼  Show settings")
        self.log_enlarged = not self.log_enlarged

    # ---- board hardware configuration
    def _refresh_config(self) -> None:
        """Show which configuration the chosen board uses: original, or edited (which revision), built or not."""
        variant = self.selected_variant()
        have = bool(variant and boardcfg.board_dir(variant))
        for button in self.cfg_buttons.values():
            button.config(state="normal" if have else "disabled")
        self.files_menu.delete(0, "end")
        if not variant or not have:
            self.cfg_status = None
            self.cfg_label.config(text="Select a board to see which configuration it uses." if not variant else
                                  "This board has no folder under main/boards.",
                                  bg=BANNER["idle"][0], fg=BANNER["idle"][1])
            return
        try:
            self.cfg_status = boardcfg.refresh(variant)
            text, kind = boardcfg.summary(self.cfg_status)
            folder = boardcfg.board_dir(variant)
            for path in boardcfg.config_files(variant):
                self.files_menu.add_command(label=boardcfg.describe(path),
                                            command=lambda p=path: self._open_config([p]))
            text = "Folder: main/boards/%s\n%s" % (folder.relative_to(boardcfg.BOARDS).as_posix(), text)
        except OSError as error:
            text, kind = "Cannot read the board configuration: %s" % error, "warn"
        bg, fg = BANNER[kind]
        self.cfg_label.config(text=text, bg=bg, fg=fg)

    def _open_config(self, paths: list[Path]) -> None:
        variant = self.selected_variant()
        if not variant or not paths:
            return
        try:
            editor = boardcfg.open_files(paths, boardcfg.board_dir(variant))
        except OSError as error:
            messagebox.showerror("Editor", "Cannot open the editor: %s" % error)
            return
        self.status.config(text="Opened %d file(s) in %s. Save them, then press Build: the change is recorded as "
                           "a new revision and used by the build." % (len(paths), editor))

    def on_edit_config(self) -> None:
        variant = self.selected_variant()
        if not variant:
            messagebox.showinfo("Config", "Select a board first.")
            return
        files = boardcfg.config_files(variant)
        if not files:
            messagebox.showinfo("Config", "No configuration files found for this board.")
            return
        status = self.cfg_status or boardcfg.refresh(variant)
        if not status["warned"]:
            if not messagebox.askokcancel(
                    "Edit the board configuration",
                    "These files define the pins and hardware of '%s' for every build.\n\n"
                    "The Builder keeps a copy of the original files (button 'Restore original') and records "
                    "each edit as a revision.\n\nChanging the pins of an existing board for different hardware "
                    "also changes what the board reports to the OTA server: keep a copy of the board if the "
                    "firmware is shared." % (variant.get("display_name") or variant["name"])):
                return
            boardcfg.mark_warned(variant)
        # config.h (pins), config.json (variants) and the board code, the usual three
        self._open_config(files[:3] if len(files) >= 3 else files)

    def on_open_config_folder(self) -> None:
        variant = self.selected_variant()
        folder = boardcfg.board_dir(variant) if variant else None
        if folder:
            boardcfg.open_folder(folder)

    def on_config_history(self) -> None:
        variant = self.selected_variant()
        if not variant:
            return
        status = boardcfg.refresh(variant)
        window = tk.Toplevel(self.root)
        window.title("Configuration history: %s" % (variant.get("display_name") or variant["name"]))
        window.geometry("640x320")
        window.configure(bg=BG)
        tree = ttk.Treeview(window, columns=("rev", "time", "files", "build"), show="headings")
        for column, text, width in (("rev", "Revision", 70), ("time", "When", 140), ("files", "Files changed", 320),
                                    ("build", "Built", 70)):
            tree.heading(column, text=text)
            tree.column(column, width=width, anchor="w")
        tree.insert("", "end", values=("0", "(first seen)", "original files", "✓" if status["built_rev"] == 0 else ""))
        for item in status["revisions"]:
            tree.insert("", "end", values=(item["n"], item["time"], ", ".join(item["changed"]),
                                           "✓" if status["built_rev"] == item["n"] else ""))
        tree.pack(fill="both", expand=True, padx=10, pady=10)
        ttk.Label(window, text="'Built ✓' marks the revision used by the last successful build.",
                  style="Muted.TLabel").pack(anchor="w", padx=10, pady=(0, 8))

    def on_restore_config(self) -> None:
        variant = self.selected_variant()
        if not variant:
            return
        if not messagebox.askyesno("Restore original", "Put the original configuration files back? Your edits "
                                   "to this board's files are overwritten (the History keeps a record)."):
            return
        try:
            restored = boardcfg.restore_original(variant)
        except (OSError, FileNotFoundError) as error:
            messagebox.showerror("Restore", str(error))
            return
        self._refresh_config()
        self.status.config(text="Restored: " + (", ".join(restored) if restored else "nothing to restore"))

    # ---- result banner / highlight helpers
    def set_banner(self, kind: str, text: str, with_buttons: bool = False) -> None:
        bg, fg = BANNER[kind]
        self.banner.config(bg=bg)
        self.banner_label.config(text=text, bg=bg, fg=fg)
        for button in (self.jump_button, self.copy_button):
            button.pack_forget()
        if with_buttons:
            self.copy_button.pack(side="right", padx=(0, 8), pady=4)
            self.jump_button.pack(side="right", padx=4, pady=4)

    def on_jump_error(self) -> None:
        self.notebook.select(0)
        if self.first_error:
            self.log.see(self.first_error)
            self.log.tag_remove("focus", "1.0", "end")
            self.log.tag_add("focus", self.first_error, self.first_error + " lineend")

    def _refresh_buttons(self) -> None:
        """Buttons that need a board stay grey until one is chosen; everything is off while a job runs."""
        have_board = self.selected_variant() is not None if hasattr(self, "tree") else False
        for key, button in self.buttons.items():
            if key == "monitor":
                button.config(state="normal")
            elif self.busy:
                button.config(state="disabled")
            elif key in ("build", "verify", "both") and not have_board:
                button.config(state="disabled")
            else:
                button.config(state="normal")
        self.stop_button.config(state="normal" if self.busy else "disabled")
        self.hint.config(text="" if have_board or self.busy else
                         "Pick a board in step ① to enable Build / Verify / Upload.")

    def _on_focus(self, event) -> None:
        """Back from the editor: look at the board files again (cheap, throttled)."""
        now = time.monotonic()
        if event.widget is self.root and now - getattr(self, "_focus_at", 0) > 1.0 and not self.busy:
            self._focus_at = now
            self.root.after(200, self._refresh_config)

    def _on_ui_error(self, exc_type, exc, tb) -> None:
        """Any error inside a button handler: log it, tell the user, keep the window alive."""
        import traceback
        text = "".join(traceback.format_exception(exc_type, exc, tb))
        try:
            with CRASH_LOG.open("a", encoding="utf-8") as handle:
                handle.write(time.strftime("%Y-%m-%d %H:%M:%S\n") + text + "\n")
        except OSError:
            pass
        try:
            self._set_busy(False)
            self._append_text(self.log, "[ERROR] unexpected: %s" % exc, "error")
            messagebox.showerror("Unexpected error", "%s\n\nDetails: %s" % (exc, CRASH_LOG))
        except Exception:
            pass

    def _make_text(self, parent) -> tk.Text:
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)
        text = tk.Text(frame, height=12, wrap="none", state="disabled", font=("Consolas", 10), bg="#0f172a",
                       fg="#e2e8f0", insertbackground="#e2e8f0", relief="flat", padx=6, pady=4)
        scroll = ttk.Scrollbar(frame, orient="vertical", command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        text.pack(side="left", fill="both", expand=True)
        scroll.pack(side="left", fill="y")
        text.tag_configure("error", foreground="#fecaca", background="#450a0a")
        text.tag_configure("warning", foreground="#fde68a", background="#3b2f05")
        text.tag_configure("success", foreground="#86efac", font=("Consolas", 10, "bold"))
        text.tag_configure("stage", foreground="#7dd3fc")
        text.tag_configure("ok_banner", foreground="#052e16", background="#4ade80", font=("Consolas", 10, "bold"))
        text.tag_configure("fail_banner", foreground="#ffffff", background="#dc2626", font=("Consolas", 10, "bold"))
        text.tag_configure("focus", background="#facc15", foreground="#000000")
        text.tag_configure("command", foreground="#60a5fa", font=("Consolas", 10, "bold"))
        text.tag_configure("note", foreground="#c4b5fd")
        return text

    def _copy(self, widget: tk.Text, errors_only: bool = False) -> None:
        """Put the log (or only its error/warning lines, with the command above them) on the clipboard."""
        content = widget.get("1.0", "end-1c")
        if errors_only:
            lines = content.splitlines()
            keep = [l for l in lines if core.classify_line(l) != "info" or l.startswith("$ ")]
            content = "\n".join(keep) if len(keep) > 0 else content
        if not content.strip():
            self.status.config(text="Nothing to copy")
            return
        try:
            self.root.clipboard_clear()
            self.root.clipboard_append(content)
            self.root.update()  # keep the text after the window closes
            self.status.config(text="Copied %d lines to the clipboard" % (content.count("\n") + 1))
        except tk.TclError as error:
            messagebox.showerror("Copy", str(error))

    @staticmethod
    def _clear(text: tk.Text) -> None:
        text.config(state="normal")
        text.delete("1.0", "end")
        text.config(state="disabled")

    def _fixed_options(self) -> None:
        """Language and wake word exist for every board; the board's own options follow them."""
        for child in self.options_frame.winfo_children():
            child.destroy()
        self.option_vars.clear()
        row = ttk.Frame(self.options_frame)
        row.pack(fill="x")
        ttk.Label(row, text="Language", width=14).pack(side="left")
        ttk.Combobox(row, textvariable=self.language_var, values=[DEFAULT_LABEL] + self.languages,
                     width=18).pack(side="left")
        row = ttk.Frame(self.options_frame)
        row.pack(fill="x", pady=(4, 0))
        ttk.Label(row, text="Wake word", width=14).pack(side="left")
        self.wake_box = ttk.Combobox(row, textvariable=self.wake_var, values=WAKE_WORDS, width=18)
        self.wake_box.pack(side="left")
        self.board_options = ttk.Frame(self.options_frame)
        self.board_options.pack(fill="x", pady=(4, 0))
        if not hasattr(self, "fast_var"):
            self.fast_var = tk.BooleanVar(value=bool(self.settings.get("fast_build", True)))
        ttk.Checkbutton(self.options_frame, text="Fast build (ccache: reuse compiled files)",
                        variable=self.fast_var).pack(anchor="w", pady=(6, 0))

    # ------------------------------------------------------------------ ESP-IDF
    def _init_idf(self) -> None:
        saved = self.settings.get("idf_path", "")
        if core.is_idf_dir(saved):
            self.idf_var.set(saved)
            return
        found = core.find_idf_candidates()
        if found:
            self.idf_var.set(str(found[0]))
        self._update_idf_info()

    def _update_idf_info(self) -> None:
        path = self.idf_var.get().strip()
        if core.is_idf_dir(path):
            self.idf_chip.config(text="ESP-IDF ✓ " + core.idf_version(path), bg="#15803d")
        else:
            self.idf_chip.config(text="ESP-IDF ✗ not set up", bg="#b91c1c")

    def on_browse_idf(self) -> None:
        chosen = filedialog.askdirectory(title="Select the ESP-IDF folder (it contains export.bat / export.sh)")
        if chosen:
            self.idf_var.set(chosen)

    def _need_idf(self) -> bool:
        if core.is_idf_dir(self.idf_var.get().strip()):
            return True
        messagebox.showwarning("ESP-IDF", "Select the ESP-IDF folder first (the one with export.bat / export.sh).\n"
                               "The project needs ESP-IDF 6.0.1 or newer. Tools > Install ESP-IDF starts the "
                               "installer that ships with this project.")
        return False

    def on_install_idf(self) -> None:
        """Run the environment setup (checks Git, ESP-IDF, toolchain, Python packages; installs what is missing)."""
        exe = core.ROOT / "XiaozhiSetup.exe"
        argv = [str(exe)] if exe.is_file() else [sys.executable, str(core.ROOT / "scripts" / "setup_env.py")]
        try:
            subprocess.Popen(argv, cwd=str(core.ROOT))
            messagebox.showinfo("Environment", "The setup window opened. When it says Done, click Detect.")
        except OSError as error:
            messagebox.showerror("Environment", "Cannot start the setup: %s" % error)

    def on_idf_terminal(self) -> None:
        if not self._need_idf():
            return
        argv = core.external_terminal_argv(self.idf_var.get().strip(), ["cd", "."])
        if argv is None:
            messagebox.showinfo("Terminal", "No terminal program found.")
            return
        subprocess.Popen(argv, cwd=str(core.ROOT))

    # ------------------------------------------------------------------ boards
    def _load_catalog(self) -> None:
        try:
            boards = core.load_boards()
            languages = core.load_languages()
            self.events.put(("catalog", boards, languages, ""))
        except Exception as error:  # shown in the log, the window stays usable
            self.events.put(("catalog", [], [], str(error)))

    def reload_catalog(self) -> None:
        self.count_label.config(text="loading the board list...")
        threading.Thread(target=self._load_catalog, daemon=True).start()

    def _fill_boards(self) -> None:
        self.shown = core.filter_variants(self.variants, self.search_var.get())
        self.tree.delete(*self.tree.get_children())
        for index, variant in enumerate(self.shown):
            label = variant.get("display_name") or variant["name"]
            self.tree.insert("", "end", iid=str(index), values=(label, variant.get("target", "")))
        self.count_label.config(text="%d of %d boards" % (len(self.shown), len(self.variants)))
        wanted = self.picked or self.settings.get("board", "")
        for index, variant in enumerate(self.shown):
            if variant["full_name"] == wanted:
                self._quiet = True   # re-selecting the same board must not rebuild its option widgets
                try:
                    self.tree.selection_set(str(index))
                finally:
                    self.root.after_idle(lambda: setattr(self, "_quiet", False))
                self.tree.see(str(index))
                break
        if not self.picked and wanted:  # board remembered from the last session
            self._on_select()
        else:
            self._mark_picked(self.selected_variant())

    def selected_variant(self) -> dict | None:
        """The chosen board (it stays chosen while the filter hides it from the list)."""
        selection = self.tree.selection()
        if selection:
            return self.shown[int(selection[0])]
        for variant in self.variants:
            if self.picked and variant["full_name"] == self.picked:
                return variant
        return None

    def _mark_picked(self, variant: dict | None) -> None:
        """Make the choice obvious: tick in the list, green card, chip badge, enabled buttons."""
        for iid in self.tree.get_children():
            shown = self.shown[int(iid)]
            label = shown.get("display_name") or shown["name"]
            tick = "✓  " if variant and shown["full_name"] == variant["full_name"] else ""
            self.tree.item(iid, values=(tick + label, shown.get("target", "")))
        if variant:
            name = variant.get("display_name") or variant["name"]
            self.pick_card.config(text="✓  Selected: %s   [%s]" % (name, variant.get("target", "?").upper()),
                                  bg="#dcfce7", fg="#166534")
            self.chip_badge.config(text=variant.get("target", "?").upper(), bg="#15803d")
            self.picked = variant["full_name"]
        else:
            self.pick_card.config(text="No board selected yet: pick one from the list", bg="#fef3c7", fg="#92400e")
            self.chip_badge.config(text="NO BOARD", bg="#233a66")
        self._refresh_buttons()
        self._refresh_config()

    def _on_select(self) -> None:
        if getattr(self, "_quiet", False):
            return
        variant = self.selected_variant()
        for child in self.board_options.winfo_children():
            child.destroy()
        self.option_vars.clear()
        self._mark_picked(variant)
        if not variant:
            return
        self.wake_box.config(state="normal" if variant.get("wake_word_supported", True) else "disabled")
        saved = self.settings.get("options", {}).get(variant["full_name"], {})
        for option in variant.get("build_options", []):
            key = option["key"]
            row = ttk.Frame(self.board_options)
            row.pack(fill="x", pady=2)
            if option["type"] == "boolean":
                var = tk.BooleanVar(value=bool(saved.get(key, option.get("default", False))))
                ttk.Checkbutton(row, text=key.replace("_", " "), variable=var).pack(side="left")
                self.option_vars[key] = ("boolean", var, option)
            else:
                labels = {c["label"]: c["value"] for c in option.get("choices", [])}
                current = saved.get(key, option.get("default"))
                label = next((l for l, v in labels.items() if v == current), next(iter(labels), ""))
                var = tk.StringVar(value=label)
                ttk.Label(row, text=key.replace("_", " "), width=16).pack(side="left")
                ttk.Combobox(row, textvariable=var, values=list(labels), width=18, state="readonly").pack(side="left")
                self.option_vars[key] = ("select", var, dict(option, labels=labels))
        self.status.config(text="%s  (%s)" % (variant.get("display_name") or variant["name"], variant.get("target", "")))


    def _option_values(self) -> dict:
        values: dict = {}
        for key, (kind, var, option) in self.option_vars.items():
            values[key] = bool(var.get()) if kind == "boolean" else option["labels"].get(var.get(), option.get("default"))
        return values

    # ------------------------------------------------------------------ ports
    def refresh_ports(self) -> None:
        threading.Thread(target=self._list_ports, daemon=True).start()

    def _list_ports(self) -> None:
        ports: list[dict] = []
        try:
            from serial.tools import list_ports  # fast path when pyserial is installed here
            ports = [{"device": p.device, "description": p.description} for p in sorted(list_ports.comports())]
        except Exception:
            idf = self.idf_var.get().strip()
            if core.is_idf_dir(idf):
                try:
                    kwargs = dict(capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
                    if core.is_windows():
                        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
                    result = subprocess.run(**core.popen_spec(core.ports_command(), idf), **kwargs)
                    for line in reversed(result.stdout.splitlines()):
                        if line.startswith("["):
                            ports = json.loads(line)
                            break
                except (OSError, ValueError, subprocess.SubprocessError):
                    pass
        self.events.put(("ports", ports))

    def current_port(self) -> str:
        text = self.port_var.get().strip()
        return self.port_map.get(text, text.split(" - ")[0].strip())

    def _need_port(self, title: str) -> str:
        port = self.current_port()
        if not port:
            messagebox.showwarning(title, "Choose the serial port of the board first (plug it in, then Refresh).")
        return port

    # ------------------------------------------------------------------ jobs
    def _baud(self) -> int:
        try:
            return int(self.baud_var.get())
        except ValueError:
            return 460800

    def _chip(self) -> str:
        variant = self.selected_variant()
        return variant.get("target", "") if variant else ""

    def _collect_build(self, variant: dict) -> core.Command:
        try:
            if core.clean_stale_build():
                self._append("[builder] build/ came from another folder or PC: removed it, building from scratch")
        except Exception:  # never stop a build because of the clean-up
            pass
        language = self.language_var.get().strip()
        wake = self.wake_var.get().strip()
        argv = core.build_argv(
            variant,
            "" if language in ("", DEFAULT_LABEL) else language,
            "" if wake in ("", AUTO_LABEL) or not variant.get("wake_word_supported", True) else wake,
            core.changed_options(variant, self._option_values()))
        env = {}
        if self.fast_var.get():  # ccache: unchanged files are not recompiled (big win on every rebuild)
            env.update(IDF_CCACHE_ENABLE="1", CCACHE_MAXSIZE="10G", CCACHE_SLOPPINESS="time_macros")
        return core.Command(argv, title="Build", env=env)

    def _image(self, image: Path | None = None) -> Path | None:
        image = Path(image or core.MERGED_IMAGE)
        if not image.is_file():
            messagebox.showwarning("Firmware", "%s does not exist yet. Build first." % image)
            return None
        return image

    def _offset(self) -> str | None:
        offset = core.parse_offset(self.offset_var.get())
        if offset is None:
            messagebox.showwarning("Upload", "The offset must be a number such as 0x0 or 0x10000.")
        return offset

    def _flash_commands(self, image: Path | None = None, need_image: bool = True) -> list[core.Command] | None:
        port = self._need_port("Upload")
        offset = self._offset()
        if not port or offset is None:
            return None
        if need_image:
            image = self._image(image)
            if image is None:
                return None
        return core.flash_commands(port, self._baud(), image or core.MERGED_IMAGE, self._chip(),
                                   self.erase_var.get(), offset)

    def _run(self, commands: list[core.Command], label: str, retry: bool = False) -> None:
        if self.runner.running:
            return
        if not retry:
            self._auto_cleaned = False
        self._last_run = (commands, label)
        uses_port = any("esptool" in " ".join(c.argv) for c in commands)
        if uses_port and self.monitor.running:  # esptool needs the port: leave the monitor, come back afterwards
            self.monitor.stop()
            self._set_monitor_state(False)
            self.resume_monitor = True
        self._save_settings()
        self._set_busy(True, label)
        self.notebook.select(0)
        self.job_variant = self.selected_variant()
        if label.startswith(("Build", "Verify")) and self.job_variant:
            self._refresh_config()
            if self.cfg_status:
                first = boardcfg.summary(self.cfg_status)[0].replace("\n", "  ")
                self._append_text(self.log, "[config] %s: %s" % (self.job_variant["name"], first), "stage")
        self.progress.config(mode="determinate", value=0)
        self.job_start = time.monotonic()
        self.job_label = label
        self._tick()
        self.runner.start(commands, self.idf_var.get().strip() or None,
                          lambda line: self.events.put(("line", line)),
                          lambda code, stopped: self.events.put(("done", code, stopped, label)))

    def on_build(self) -> None:
        variant = self.selected_variant()
        if not variant:
            messagebox.showinfo("Build", "Select a board first.")
        elif self._need_idf():
            self._run([self._collect_build(variant)], "Build")

    def on_verify(self) -> None:
        """Arduino-style Verify: compile the project and report its size. No serial port needed."""
        variant = self.selected_variant()
        if not variant:
            messagebox.showinfo("Verify", "Select a board first.")
        elif self._need_idf():
            self._run([self._collect_build(variant), core.size_command()], "Verify")

    def on_repair_idf(self) -> None:
        if not self._need_idf():
            return
        if messagebox.askyesno("Repair ESP-IDF", "Reinstall the Python packages of the ESP-IDF environment?\n"
                               "Needs an Internet connection and can take a few minutes."):
            self._run([core.repair_command(self.idf_var.get().strip(), self.recent)], "Repair ESP-IDF")

    def on_compare_flash(self) -> None:
        """Compare what is on the board with the built image."""
        if not self._need_idf():
            return
        port, offset = self._need_port("Verify"), self._offset()
        image = self._image()
        if port and offset is not None and image:
            self._run([core.verify_command(port, self._baud(), image, self._chip(), offset)], "Verify flash")

    def on_flash(self) -> None:
        if self._need_idf():
            commands = self._flash_commands()
            if commands:
                self._run(commands, "Upload")

    def on_build_flash(self) -> None:
        variant = self.selected_variant()
        if not variant:
            messagebox.showinfo("Build + Upload", "Select a board first.")
            return
        if not self._need_idf():
            return
        flash = self._flash_commands(need_image=False)
        if flash:
            self._run([self._collect_build(variant)] + flash, "Build + Upload")

    def on_erase(self) -> None:
        if not self._need_idf():
            return
        port = self._need_port("Erase")
        if port and messagebox.askyesno("Erase flash", "Erase the WHOLE flash of the board on %s?\n"
                                        "Firmware, settings and Wi-Fi data are lost." % port):
            self._run([core.erase_command(port, self._baud(), self._chip())], "Erase flash")

    def on_chip_info(self) -> None:
        if not self._need_idf():
            return
        port = self._need_port("Chip info")
        if port:
            self._run(core.chip_info_commands(port, self._baud(), self._chip()), "Chip info")

    def on_flash_file(self) -> None:
        if not self._need_idf():
            return
        path = filedialog.askopenfilename(title="Firmware image (merged-binary.bin)",
                                          filetypes=[("Firmware", "*.bin"), ("All files", "*.*")])
        if path:
            commands = self._flash_commands(Path(path))
            if commands:
                self._run(commands, "Upload file")

    def on_size(self) -> None:
        if self._need_idf():
            self._run([core.size_command()], "Size")

    def on_clean(self) -> None:
        if self._need_idf() and messagebox.askyesno("Clean", "Delete the build folder? The next build starts from zero."):
            self._run([core.Command(["idf.py", "fullclean"], title="Clean")], "Clean")

    def on_menuconfig(self) -> None:
        if not self._need_idf():
            return
        argv = core.external_terminal_argv(self.idf_var.get().strip(), ["idf.py", "menuconfig"])
        if argv is None:
            messagebox.showinfo("Menuconfig", "No terminal program found. Run 'idf.py menuconfig' in the ESP-IDF "
                                "terminal, in the project folder.")
            return
        subprocess.Popen(argv, cwd=str(core.ROOT))

    def on_open_build(self) -> None:
        target = core.ROOT / "build"
        target = target if target.is_dir() else core.ROOT
        if core.is_windows():
            os.startfile(str(target))  # noqa: S606 (opens a folder in Explorer)
        else:
            subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", str(target)])

    def on_stop(self) -> None:
        self.runner.stop()
        self.status.config(text="Stopping...")

    # ------------------------------------------------------------------ serial monitor
    def on_monitor(self) -> None:
        self.notebook.select(1)
        if not self.monitor.running:
            self.on_monitor_toggle()

    def on_monitor_toggle(self) -> None:
        if self.monitor.running:
            self.monitor.stop()
            self._set_monitor_state(False)
            self._monitor_note("--- disconnected ---")
            return
        if self.runner.running:
            messagebox.showinfo("Monitor", "Wait for the running job to finish (it uses the serial port).")
            return
        if not self._need_idf():
            return
        port = self._need_port("Monitor")
        if not port:
            return
        try:
            baud = int(self.mon_baud_var.get())
        except ValueError:
            baud = MONITOR_BAUD
        try:
            self.monitor.start(port, baud, self.idf_var.get().strip(),
                               lambda line: self.events.put(("mline", line)),
                               lambda code: self.events.put(("mexit", code)))
        except (OSError, ValueError) as error:
            messagebox.showerror("Monitor", str(error))
            return
        self._set_monitor_state(True)

    def _set_monitor_state(self, connected: bool) -> None:
        self.mon_button.config(text="Disconnect" if connected else "Connect")

    def _monitor_note(self, text: str) -> None:
        self._append_text(self.mon_text, text, "note")

    def on_send(self) -> None:
        text = self.send_var.get()
        if not self.monitor.running:
            messagebox.showinfo("Monitor", "Connect the monitor first.")
            return
        self.monitor.send(text)
        self._append_text(self.mon_text, "> " + text, "command")
        self.send_var.set("")

    def on_reset_board(self) -> None:
        if self.monitor.running:
            self.monitor.reset()
            self._monitor_note("--- reset ---")
        else:
            messagebox.showinfo("Reset", "Connect the Serial Monitor first, then reset (or press Upload: it resets "
                                "the board when it finishes).")

    def on_bootloader(self) -> None:
        if self.monitor.running:
            self.monitor.bootloader()
            self._monitor_note("--- download mode requested: disconnect before uploading ---")
        else:
            messagebox.showinfo("Download mode", "Connect the Serial Monitor first.")

    def on_save_monitor(self) -> None:
        path = filedialog.asksaveasfilename(title="Save the monitor log", defaultextension=".log",
                                            filetypes=[("Log", "*.log *.txt"), ("All files", "*.*")])
        if path:
            try:
                Path(path).write_text(self.mon_text.get("1.0", "end-1c"), encoding="utf-8")
            except OSError as error:
                messagebox.showerror("Save", str(error))

    # ------------------------------------------------------------------ plumbing
    def _elapsed(self) -> str:
        seconds = int(time.monotonic() - getattr(self, "job_start", time.monotonic()))
        return "%d:%02d" % divmod(seconds, 60)

    def _tick(self) -> None:
        """Show the running time of the current job."""
        if self.busy:
            self.set_banner("run", "⏳  %s running...  %s" % (self.job_label, self._elapsed()))
            self.root.after(1000, self._tick)

    def _set_busy(self, busy: bool, label: str = "") -> None:
        self.busy = busy
        self._refresh_buttons()
        if busy:
            self.first_error = None
            self.log.tag_remove("focus", "1.0", "end")
            self.set_banner("run", "⏳  %s running..." % label)

    def _append_text(self, widget: tk.Text, line: str, tag: str, scroll: bool = True) -> None:
        widget.config(state="normal")
        widget.insert("end", line + "\n", tag)
        # keep the text from growing without limit
        if int(widget.index("end-1c").split(".")[0]) > 6000:
            widget.delete("1.0", "1500.0")
        widget.config(state="disabled")
        if scroll:
            widget.see("end")

    def _append(self, line: str) -> None:
        tag = "command" if line.startswith("$ ") else core.classify_line(line)
        if line.startswith("$ "):
            self.recent.clear()
        self.recent.append(line)
        del self.recent[:-200]
        self._append_text(self.log, line, tag)
        if tag == "error" and self.first_error is None and not line.startswith("$ "):
            self.first_error = self.log.index("end-2l linestart")
        progress = core.parse_progress(line)
        if progress:
            self.progress.config(mode="determinate", value=100.0 * progress[0] / progress[1])
        elif line.startswith("Writing at") and "%" in line:  # esptool: Writing at 0x... [====] 42.0% ...
            try:
                self.progress.config(mode="determinate", value=float(line.split("(")[1].split("%")[0]))
            except (IndexError, ValueError):
                pass

    def _append_monitor(self, line: str) -> None:
        line = core.strip_ansi(line)
        if self.timestamp_var.get():
            line = time.strftime("[%H:%M:%S] ") + line
        text = line.split("] ", 1)[1] if line.startswith("[") and "] " in line[:12] else line
        self._append_text(self.mon_text, line, core.classify_log(text), self.autoscroll_var.get())

    def _offer_repair(self) -> None:
        if messagebox.askyesno("ESP-IDF environment is broken",
                               "The ESP-IDF Python environment is incomplete (packages such as esptool or "
                               "esp_idf_monitor are missing).\n\nReinstall them now? (Internet needed, a few minutes)"):
            self._run([core.repair_command(self.idf_var.get().strip(), self.recent)], "Repair ESP-IDF")

    def _after_job(self, code: int, label: str) -> None:
        if (code != 0 and label.startswith("Build") and not getattr(self, "_auto_cleaned", False)
                and core.looks_like_bad_build_dir(self.recent) and getattr(self, "_last_run", None)):
            # the build/ folder cannot be reused (copied from another PC, half written...): wipe it, retry once
            self._auto_cleaned = True
            core.force_clean_build()
            self._append("[builder] build/ is unusable: deleted it and building again from scratch")
            commands, run_label = self._last_run
            self.root.after(300, lambda: self._run(commands, run_label, retry=True))
            return
        if code != 0 and label != "Repair ESP-IDF" and core.looks_like_broken_env(self.recent):
            self.root.after(100, self._offer_repair)
        if (self.resume_monitor or (code == 0 and label in UPLOAD_LABELS and self.auto_monitor_var.get())) \
                and not self.monitor.running:
            if code == 0 or self.resume_monitor:
                self.resume_monitor = False
                self.on_monitor()
        self.resume_monitor = False

    def _pump(self) -> None:
        for _ in range(300):
            try:
                event = self.events.get_nowait()
            except queue.Empty:
                break
            kind = event[0]
            if kind == "line":
                self._append(event[1])
            elif kind == "mline":
                self._append_monitor(event[1])
            elif kind == "mexit":
                self._set_monitor_state(False)
                if event[1]:
                    self._monitor_note("--- monitor stopped (exit code %d): is the port in use or unplugged? ---"
                                       % event[1])
            elif kind == "done":
                _, code, stopped, label = event
                self._set_busy(False)
                took = self._elapsed()
                if stopped:
                    self.set_banner("warn", "■  %s stopped after %s" % (label, took))
                    self._append_text(self.log, "══════ ■ %s stopped (%s) ══════" % (label, took), "warning")
                elif code == 0:
                    extra = ""
                    if label.startswith(("Build", "Verify")) and core.MERGED_IMAGE.is_file():
                        extra = "   →  %s (%.1f MB)" % (core.MERGED_IMAGE.name, core.MERGED_IMAGE.stat().st_size / 1048576)
                    self.set_banner("ok", "✓  %s finished OK in %s%s" % (label, took, extra))
                    self._append_text(self.log, "══════ ✓ %s OK (%s) ══════" % (label, took), "ok_banner")
                    self.progress.config(value=100)
                    if label.startswith(("Build", "Verify")) and getattr(self, "job_variant", None):
                        boardcfg.mark_built(self.job_variant)
                        self._refresh_config()
                else:
                    hint = core.explain_error(self.recent)
                    first = ""
                    if self.first_error:
                        first = self.log.get(self.first_error, self.first_error + " lineend").strip()[:110]
                    self.set_banner("fail", "✗  %s FAILED (exit %d)%s%s" % (
                        label, code, "   " + first if first else "", "\n💡 " + hint if hint else ""),
                        with_buttons=True)
                    self._append_text(self.log, "══════ ✗ %s FAILED (exit %d, %s) ══════" % (label, code, took),
                                      "fail_banner")
                    self.progress.config(value=0)
                self.status.config(text="")
                if not stopped:
                    self._after_job(code, label)
                else:
                    self.resume_monitor = False
            elif kind == "catalog":
                _, boards, languages, error = event
                if error:
                    self._append("[ERROR] " + error)
                    self.count_label.config(text="board list not available")
                else:
                    self.variants = boards
                    if languages:
                        self.languages = languages
                    self._fixed_options()
                    self._fill_boards()
            elif kind == "ports":
                ports = event[1]
                self.port_map = {}
                texts = []
                for port in ports:
                    text = port["device"] + (" - " + port["description"] if port.get("description") else "")
                    self.port_map[text] = port["device"]
                    texts.append(text)
                self.port_box.config(values=texts)
                current = self.current_port()
                if texts and current not in self.port_map.values():
                    self.port_var.set(texts[0])
                elif current in self.port_map.values():
                    self.port_var.set(next(t for t, d in self.port_map.items() if d == current))
        self.root.after(60, self._pump)

    def _save_settings(self) -> None:
        variant = self.selected_variant()
        self.settings.update(
            idf_path=self.idf_var.get().strip(), language=self.language_var.get(), wake_word=self.wake_var.get(),
            port=self.current_port(), baud=self.baud_var.get(), erase=bool(self.erase_var.get()),
            offset=self.offset_var.get(), auto_monitor=bool(self.auto_monitor_var.get()),
            monitor_baud=self.mon_baud_var.get(), fast_build=bool(self.fast_var.get()))
        if variant:
            self.settings["board"] = variant["full_name"]
            options = self.settings.setdefault("options", {})
            options[variant["full_name"]] = self._option_values()
        core.save_settings(self.settings)

    def close(self) -> None:
        if self.runner.running:
            if not messagebox.askyesno("Quit", "A job is still running. Stop it and quit?"):
                return
            self.runner.stop()
        self.monitor.stop()
        self._save_settings()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    try:
        App(root)
    except Exception as error:  # startup failure: show it instead of closing silently
        import traceback
        try:
            CRASH_LOG.write_text(traceback.format_exc(), encoding="utf-8")
        except OSError:
            pass
        messagebox.showerror("Xiaozhi Builder cannot start", "%s\n\nDetails: %s" % (error, CRASH_LOG))
        root.destroy()
        return
    root.mainloop()
