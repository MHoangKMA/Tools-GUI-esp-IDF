"""Tests of the Xiaozhi Builder logic (no ESP-IDF, no window needed).

    python scripts/tests/test_builder_core.py
"""
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from builder import core  # noqa: E402

VARIANTS = [
    {"board": "linc-robot-v1", "name": "linc-robot-v1", "full_name": "linc-robot-v1", "target": "esp32s3",
     "display_name": "LINC Robot", "build_options": [
         {"key": "tft", "type": "boolean", "default": True},
         {"key": "mode", "type": "select", "default": "a", "choices": [{"label": "A", "value": "a"}]}]},
    {"board": "bread-compact-wifi", "name": "bread-compact-wifi", "full_name": "bread-compact-wifi",
     "target": "esp32s3", "display_name": "Bread Compact"},
]


class BuilderCoreTest(unittest.TestCase):
    def test_filter_requires_every_word(self):
        self.assertEqual(len(core.filter_variants(VARIANTS, "")), 2)
        self.assertEqual([v["name"] for v in core.filter_variants(VARIANTS, "linc esp32s3")], ["linc-robot-v1"])
        self.assertEqual(core.filter_variants(VARIANTS, "linc bread"), [])

    def test_build_argv(self):
        argv = core.build_argv(VARIANTS[0], "vi-VN", "hiesp", {"tft": False})
        self.assertEqual(argv[:5], ["python", "scripts/build.py", "linc-robot-v1", "--name", "linc-robot-v1"])
        self.assertIn("vi-VN", argv)
        self.assertEqual(json.loads(argv[argv.index("--build-options-json") + 1]), {"tft": False})
        self.assertNotIn("--language", core.build_argv(VARIANTS[0]))

    def test_changed_options_only_differences(self):
        self.assertEqual(core.changed_options(VARIANTS[0], {"tft": True, "mode": "a"}), {})
        self.assertEqual(core.changed_options(VARIANTS[0], {"tft": False, "mode": "a", "other": 1}), {"tft": False})

    def test_flash_commands(self):
        commands = core.flash_commands("COM3", 460800, "x.bin", "esp32s3", erase_first=True)
        self.assertEqual(len(commands), 2)
        self.assertIn("erase_flash", commands[0].argv)
        self.assertEqual(commands[1].argv[-3:], ["write_flash", "0x0", "x.bin"])
        self.assertIn("--chip", commands[1].argv)
        self.assertEqual(len(core.flash_commands("COM3", 115200, "x.bin")), 1)

    def test_parse_progress(self):
        self.assertEqual(core.parse_progress("[12/340] Building C object a.o"), (12, 340))
        self.assertIsNone(core.parse_progress("nothing here"))
        self.assertIsNone(core.parse_progress("[1/0] odd"))

    def test_classify_line(self):
        self.assertEqual(core.classify_line("main.cc:3: error: boom"), "error")
        self.assertEqual(core.classify_line("warning: unused variable"), "warning")
        self.assertEqual(core.classify_line("0 errors, 0 warnings"), "info")
        self.assertEqual(core.classify_line("Linking CXX executable"), "info")

    def test_popen_spec_without_idf(self):
        spec = core.popen_spec(core.Command(["echo", "hi"], use_idf=False), None)
        self.assertEqual(spec["args"], ["echo", "hi"])
        self.assertFalse(spec["shell"])

    def test_popen_spec_needs_idf_path(self):
        with self.assertRaises(ValueError):
            core.popen_spec(core.Command(["idf.py", "build"]), None)

    def test_popen_spec_inside_idf(self):
        spec = core.popen_spec(core.Command(["idf.py", "build"]), "/opt/esp-idf")
        text = spec["args"] if isinstance(spec["args"], str) else spec["args"][-1]
        self.assertIn("export", text)
        self.assertIn("idf.py", text)

    def test_settings_round_trip(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "s.json"
            self.assertEqual(core.load_settings(path), {})
            core.save_settings({"board": "x", "baud": "460800"}, path)
            self.assertEqual(core.load_settings(path)["board"], "x")
            path.write_text("not json", encoding="utf-8")
            self.assertEqual(core.load_settings(path), {})

    def test_job_runner_runs_in_order_and_stops_on_failure(self):
        lines, done = [], threading.Event()
        result = {}

        def finish(code, stopped):
            result.update(code=code, stopped=stopped)
            done.set()

        ok = core.Command([sys.executable, "-c", "print('one')"], use_idf=False)
        bad = core.Command([sys.executable, "-c", "import sys; print('two'); sys.exit(3)"], use_idf=False)
        never = core.Command([sys.executable, "-c", "print('three')"], use_idf=False)
        runner = core.JobRunner()
        runner.start([ok, bad, never], None, lines.append, finish)
        self.assertTrue(done.wait(30))
        self.assertEqual(result, {"code": 3, "stopped": False})
        self.assertIn("one", lines)
        self.assertIn("two", lines)
        self.assertNotIn("three", lines)

    def test_job_runner_stop_kills_process(self):
        done = threading.Event()
        result = {}
        slow = core.Command([sys.executable, "-c", "import time; print('go', flush=True); time.sleep(60)"],
                            use_idf=False)
        started = threading.Event()
        runner = core.JobRunner()

        def on_line(line):
            if line == "go":
                started.set()

        runner.start([slow], None, on_line, lambda code, stopped: (result.update(stopped=stopped), done.set()))
        self.assertTrue(started.wait(30))
        runner.stop()
        self.assertTrue(done.wait(30))
        self.assertTrue(result["stopped"])

    def test_real_board_list_contains_linc_robot(self):
        boards = core.load_boards()
        self.assertTrue(any(b["name"] == "linc-robot-v1" for b in boards))


if __name__ == "__main__":
    unittest.main()
