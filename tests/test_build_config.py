import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "scripts" / "build_config.py"
spec = importlib.util.spec_from_file_location("build_config", MODULE_PATH)
build_config = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_config)


class FakeEnvironment:
    def __init__(self, project):
        self.paths = {"$PROJECT_DIR": str(project), "$BUILD_DIR": str(project / "build")}

    def subst(self, key):
        return self.paths[key]

    def Prepend(self, **kwargs):
        self.cpppath = kwargs["CPPPATH"]


class BuildConfigTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = self.root / "config.local.json"

    def write_config(self, data):
        self.config.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler not installed")
    def test_cpp_round_trip_special_characters_and_utf8(self):
        values = ['日本語 "SSID"', 'quote" slash\\ newline\n $() `x`', 'token\t"\\☀']
        values.append("paper-test")
        values.extend(['日本語 "本文".ttf', 'Custom-Heavy.ttf'])
        self.write_config({"hostname": values[3],
                           "fonts": {"REGULAR": values[4], "BOLD": values[5]},
                           "wifi": {"ssid": values[0], "password": values[1]},
                           "api_tokens": {"test": values[2]}})
        (self.root / "build_config.h").write_text(build_config.render_header(self.config))
        source = self.root / "check.cpp"
        source.write_text('#include "config.h"\n#include <cstdio>\n'
                          'int main() { puts(config::kWifiSsid); puts(config::kWifiPassword);'
                          ' puts(config::apiToken("test"));'
                          ' puts(config::kHostname);'
                          ' puts(config::kFontRegular); puts(config::kFontBold);'
                          ' return config::apiToken("missing")[0] != 0; }\n')
        binary = self.root / "check"
        subprocess.run(["c++", "-std=c++11", "-I", str(self.root), "-I",
                        str(MODULE_PATH.parents[1] / "include"), str(source),
                        "-o", str(binary)], check=True, capture_output=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True)
        self.assertEqual(result.stdout, ("\n".join(values) + "\n").encode("utf-8"))

    def test_invalid_json_does_not_echo_secret(self):
        self.config.write_text('{"wifi": "SYNTHETIC_SECRET", broken}')
        with self.assertRaises(build_config.ConfigError) as error:
            build_config.render_header(self.config)
        self.assertNotIn("SYNTHETIC_SECRET", str(error.exception))

    def test_invalid_values_rejected(self):
        for data in [{"wifi": {"ssid": "あ" * 11}},
                     {"wifi": {"password": "x" * 65}},
                     {"wifi": {"ssid": "a\0b"}},
                     {"wifi": {"password": 123}},
                     {"api_tokens": {"test": False}},
                     {"wifi": {"pasword": "typo"}}]:
            with self.subTest(data=data):
                self.write_config(data)
                with self.assertRaises(build_config.ConfigError):
                    build_config.render_header(self.config)

    def test_duplicate_keys_rejected(self):
        self.config.write_text('{"wifi": {}, "wifi": {}}')
        with self.assertRaises(build_config.ConfigError):
            build_config.render_header(self.config)

    def test_invalid_hostnames_rejected(self):
        for hostname in ["", "paper.local", "-paper", "paper-", "paper_name",
                         "Paper", "紙", "x" * 64, 123]:
            with self.subTest(hostname=hostname):
                self.write_config({"hostname": hostname})
                with self.assertRaises(build_config.ConfigError):
                    build_config.render_header(self.config)

    def test_missing_file_rejected(self):
        with self.assertRaises(build_config.ConfigError):
            build_config.render_header(self.config)

    def test_font_filenames(self):
        for name in ('/fonts/test.ttf', '../test.ttf', 'fonts/test.ttf', 'bad\\font.ttf',
                     'bad\nfont.ttf', 'test.otf', 'x' * 129 + '.ttf', 123):
            with self.subTest(name=name):
                self.write_config({'fonts': {'REGULAR': name}})
                with self.assertRaises(build_config.ConfigError):
                    build_config.render_header(self.config)
        self.write_config({'fonts': {'REGULAR': '', 'BOLD': 'Other-Heavy.TTF'}})
        header = build_config.render_header(self.config)
        self.assertIn('constexpr char kFontRegular[] = "";', header)
        self.assertIn(build_config.cpp_string('Other-Heavy.TTF'), header)

    def test_config_selection_and_updates(self):
        from unittest.mock import patch

        env = FakeEnvironment(self.root)
        self.write_config({"wifi": {"ssid": "first"}})
        with patch.dict(build_config.os.environ, {}, clear=True):
            build_config.configure(env)
            header = self.root / "build" / "generated" / "build_config.h"
            original = header.read_text()
            timestamp = header.stat().st_mtime_ns
            build_config.configure(env)
            self.assertEqual(header.stat().st_mtime_ns, timestamp)
            self.write_config({"wifi": {"ssid": "second"}})
            build_config.configure(env)
            self.assertNotEqual(header.read_text(), original)
            alternate = self.root / "other.local.json"
            alternate.write_text('{"wifi": {"ssid": "alternate"}}')
            for path in [alternate.name, str(alternate)]:
                build_config.os.environ["M5PAPER_CONFIG"] = path
                build_config.configure(env)
                self.assertEqual(header.read_text(), build_config.render_header(alternate))

    def test_calendar_credentials_relative_to_config(self):
        key = "-----BEGIN PRIVATE KEY-----\nSYNTHETIC\n-----END PRIVATE KEY-----\n"
        account = {"type": "service_account", "client_email": "paper@example.iam.gserviceaccount.com", "private_key": key}
        folder = self.root / "credentials"
        folder.mkdir()
        (folder / "key.json").write_text(json.dumps(account))
        self.write_config({"calendar": {"id": "test@example.com", "service_account_file": "credentials/key.json", "refresh_seconds": 600}})
        header = build_config.render_header(self.config)
        self.assertIn(build_config.cpp_string(key), header)
        self.assertIn("kCalendarRefreshSeconds = 600", header)

    def test_calendar_invalid_settings(self):
        for calendar in [{"id": "test"}, {"service_account_file": "key.json"},
                         {"id": "primary", "service_account_file": "key.json"},
                         {"refresh_seconds": True}, {"refresh_seconds": 59},
                         {"refresh_seconds": 86401}, {"id": "x\ny"},
                         {"id": "test", "service_account_file": "missing.json"}]:
            self.write_config({"calendar": calendar})
            with self.assertRaises(build_config.ConfigError):
                build_config.render_header(self.config)

    def test_calendar_bad_key_does_not_echo_secret(self):
        (self.root / "key.json").write_text('{"private_key": "SYNTHETIC_SECRET", broken}')
        self.write_config({"calendar": {"id": "test", "service_account_file": "key.json"}})
        with self.assertRaises(build_config.ConfigError) as error:
            build_config.render_header(self.config)
        self.assertNotIn("SYNTHETIC_SECRET", str(error.exception))

    def test_usage_credentials_and_seed(self):
        codex = self.root / "codex.json"
        claude = self.root / "claude.json"
        codex.write_text(json.dumps({"refresh_token": "synthetic-refresh", "account_id": "test-account"}))
        claude.write_text(json.dumps({"claudeAiOauth": {"refreshToken": "synthetic-claude"}}))
        self.write_config({"usage": {"codex_credentials_file": "codex.json", "claude_credentials_file": "claude.json"}})
        original = build_config.render_header(self.config)
        self.assertIn(build_config.cpp_string("synthetic-refresh"), original)
        self.assertIn(build_config.cpp_string("synthetic-claude"), original)
        # A new bootstrap token changes the seed; ordinary rebuilds don't.
        self.assertEqual(original, build_config.render_header(self.config))
        codex.write_text(json.dumps({"refresh_token": "new-refresh", "account_id": "test-account"}))
        self.assertNotEqual(original, build_config.render_header(self.config))

    def test_usage_rejects_incomplete_and_header_injection(self):
        for credentials in [{}, {"refresh_token": "x\r\nBad: header", "account_id": "test"},
                            {"refresh_token": "abc", "account_id": "x\r\n"}]:
            (self.root / "key.json").write_text(json.dumps(credentials))
            self.write_config({"usage": {"codex_credentials_file": "key.json"}})
            with self.assertRaises(build_config.ConfigError):
                build_config.render_header(self.config)


if __name__ == "__main__":
    unittest.main()
