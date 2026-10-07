from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CalendarModelTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler not installed")
    def test_dates_and_day_assignment(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "calendar-model-test")
            subprocess.run([
                "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-fno-sanitize-recover=all",
                "-I", str(ROOT / "include"), str(ROOT / "src/calendar_model.cpp"),
                str(ROOT / "tests/calendar_model_test.cpp"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)
