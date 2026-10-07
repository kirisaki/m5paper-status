from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HistoryUiTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler not installed")
    def test_gestures_navigation_live_updates_and_paging(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "history-ui-test")
            subprocess.run([
                "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-fno-sanitize-recover=all",
                "-I", str(ROOT / "include"),
                str(ROOT / "src/history_ui.cpp"),
                str(ROOT / "src/message_text.cpp"),
                str(ROOT / "tests/history_ui_test.cpp"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)
