from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MessageTextTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler not installed")
    def test_layout_and_utf8(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = str(Path(temp) / "message-text-test")
            subprocess.run([
                "c++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=undefined", "-fno-sanitize-recover=all",
                "-I", str(ROOT / "include"),
                str(ROOT / "src/message_text.cpp"),
                str(ROOT / "tests/message_text_test.cpp"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)
