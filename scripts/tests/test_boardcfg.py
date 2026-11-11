import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from builder import boardcfg  # noqa: E402


class BoardConfigTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.saved = (boardcfg.BOARDS, boardcfg.STATE_FILE, boardcfg.BACKUP_DIR)
        boardcfg.BOARDS = self.tmp / "boards"
        boardcfg.STATE_FILE = self.tmp / "state.json"
        boardcfg.BACKUP_DIR = self.tmp / "backup"
        folder = boardcfg.BOARDS / "acme" / "demo"
        folder.mkdir(parents=True)
        (folder / "config.h").write_text("#define PIN 1\n")
        (folder / "config.json").write_text("{}")
        (folder / "demo_board.cc").write_text("// board\n")
        self.folder = folder
        self.variant = {"board": "acme/demo", "name": "demo"}

    def tearDown(self):
        boardcfg.BOARDS, boardcfg.STATE_FILE, boardcfg.BACKUP_DIR = self.saved

    def test_files_are_listed_important_first(self):
        names = [p.name for p in boardcfg.config_files(self.variant)]
        self.assertEqual(names[:2], ["config.h", "config.json"])

    def test_original_then_edit_creates_revision(self):
        status = boardcfg.refresh(self.variant)
        self.assertEqual((status["revision"], status["modified"], status["build_state"]), (0, False, "never"))
        (self.folder / "config.h").write_text("#define PIN 2\n")
        status = boardcfg.refresh(self.variant)
        self.assertEqual((status["revision"], status["modified"]), (1, True))
        self.assertEqual(status["differs"], ["config.h"])
        # no change: still revision 1
        self.assertEqual(boardcfg.refresh(self.variant)["revision"], 1)

    def test_build_state_follows_edits(self):
        boardcfg.refresh(self.variant)
        boardcfg.mark_built(self.variant)
        self.assertEqual(boardcfg.refresh(self.variant)["build_state"], "current")
        (self.folder / "config.h").write_text("#define PIN 3\n")
        self.assertEqual(boardcfg.refresh(self.variant)["build_state"], "stale")
        boardcfg.mark_built(self.variant)
        self.assertEqual(boardcfg.refresh(self.variant)["build_state"], "current")

    def test_restore_original(self):
        boardcfg.refresh(self.variant)
        (self.folder / "config.h").write_text("#define PIN 9\n")
        boardcfg.refresh(self.variant)
        restored = boardcfg.restore_original(self.variant)
        self.assertEqual(restored, ["config.h"])
        self.assertEqual((self.folder / "config.h").read_text(), "#define PIN 1\n")
        status = boardcfg.refresh(self.variant)
        self.assertFalse(status["modified"])
        self.assertEqual(status["revision"], 2)  # the restore itself is a revision

    def test_summary_text(self):
        text, kind = boardcfg.summary(boardcfg.refresh(self.variant))
        self.assertIn("never edited", text)
        (self.folder / "config.h").write_text("x")
        text, kind = boardcfg.summary(boardcfg.refresh(self.variant))
        self.assertIn("revision 1", text)
        self.assertEqual(kind, "warn")


if __name__ == "__main__":
    unittest.main()
