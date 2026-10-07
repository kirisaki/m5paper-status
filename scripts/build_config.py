"""Generate private C++ configuration before PlatformIO scans dependencies."""

import json
import hashlib
import os
from pathlib import Path
import re


class ConfigError(ValueError):
    pass


def _object(value, allowed, label):
    if not isinstance(value, dict):
        raise ConfigError(f"{label} must be an object")
    if set(value) - allowed:
        raise ConfigError(f"{label} contains unknown keys")


def _string(value, label):
    if not isinstance(value, str) or "\0" in value:
        raise ConfigError(f"{label} must be a string without NUL characters")
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        raise ConfigError(f"{label} must contain valid Unicode") from None
    return value


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ConfigError("configuration contains duplicate keys")
        result[key] = value
    return result


def cpp_string(value):
    # Fixed-width octal escapes preserve UTF-8 and cannot inject C++ or shell code.
    return '"' + "".join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def render_header(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except OSError:
        raise ConfigError("cannot read configuration; copy config.example.json to config.local.json or set M5PAPER_CONFIG") from None
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise ConfigError("configuration must be valid UTF-8 JSON") from None

    _object(data, {"hostname", "fonts", "wifi", "api_tokens", "calendar", "usage"}, "configuration")
    hostname = _string(data.get("hostname", "paper"), "hostname")
    if not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", hostname):
        raise ConfigError("hostname must be 1-63 lowercase ASCII letters, digits or hyphens, without a leading/trailing hyphen; omit .local")
    fonts = data.get("fonts", {})
    _object(fonts, {"REGULAR", "BOLD"}, "fonts")
    font_entries = []
    for role, default in (("REGULAR", "MPLUS1p-Medium.ttf"), ("BOLD", "MPLUS1p-ExtraBold.ttf")):
        name = _string(fonts.get(role, default), "fonts." + role)
        if name and (len(name.encode("utf-8")) > 128 or name.startswith(".")
                     or any(ord(c) < 32 or c in '/\\:' or ord(c) == 127 for c in name)
                     or not name.lower().endswith(".ttf")):
            raise ConfigError("fonts." + role + " must be a TTF filename without directories, or empty for the built-in font")
        font_entries.append(f"constexpr char kFont{role.title()}[] = {cpp_string(name)};")
    wifi = data.get("wifi", {})
    _object(wifi, {"ssid", "password"}, "wifi")
    ssid = _string(wifi.get("ssid", ""), "wifi.ssid")
    password = _string(wifi.get("password", ""), "wifi.password")
    if len(ssid.encode("utf-8")) > 32:
        raise ConfigError("wifi.ssid must be at most 32 UTF-8 bytes")
    if len(password.encode("utf-8")) > 64:
        raise ConfigError("wifi.password must be at most 64 UTF-8 bytes")

    tokens = data.get("api_tokens", {})
    if not isinstance(tokens, dict):
        raise ConfigError("api_tokens must be an object of string values")
    entries = []
    for name, value in sorted(tokens.items()):
        _string(name, "API token name")
        _string(value, "API token value")
        entries.append(f"  {{{cpp_string(name)}, {cpp_string(value)}}},")

    calendar = data.get("calendar", {})
    _object(calendar, {"id", "service_account_file", "refresh_seconds"}, "calendar")
    calendar_id = _string(calendar.get("id", ""), "calendar.id")
    account_file = _string(calendar.get("service_account_file", ""), "calendar.service_account_file")
    refresh = calendar.get("refresh_seconds", 300)
    if type(refresh) is not int or not 60 <= refresh <= 86400:
        raise ConfigError("calendar.refresh_seconds must be an integer from 60 to 86400")
    if len(calendar_id.encode("utf-8")) > 512 or any(ord(c) < 32 for c in calendar_id):
        raise ConfigError("calendar.id must be at most 512 bytes without control characters")
    if bool(calendar_id) != bool(account_file):
        raise ConfigError("set both calendar.id and calendar.service_account_file, or leave both empty")
    email = private_key = ""
    if account_file:
        if calendar_id == "primary":
            raise ConfigError("calendar.id must be the shared calendar ID, not primary")
        credential_path = Path(account_file).expanduser()
        if not credential_path.is_absolute():
            credential_path = path.parent / credential_path
        try:
            account = json.loads(credential_path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
        except (OSError, ValueError, UnicodeError):
            raise ConfigError("cannot read service account JSON key") from None
        if not isinstance(account, dict) or account.get("type") != "service_account":
            raise ConfigError("calendar credentials must be a service_account JSON key")
        email = _string(account.get("client_email"), "service account client_email")
        private_key = _string(account.get("private_key"), "service account private_key")
        if not re.fullmatch(r"[a-zA-Z0-9._-]+@[a-zA-Z0-9.-]+\.gserviceaccount\.com", email):
            raise ConfigError("invalid service account client_email")
        if not private_key.startswith("-----BEGIN PRIVATE KEY-----\n") or not private_key.rstrip().endswith("-----END PRIVATE KEY-----") or len(private_key) > 8192:
            raise ConfigError("service account private_key must be a PEM private key")

    usage = data.get("usage", {})
    _object(usage, {"codex_credentials_file", "claude_credentials_file", "refresh_seconds"}, "usage")
    usage_refresh = usage.get("refresh_seconds", 300)
    if type(usage_refresh) is not int or not 60 <= usage_refresh <= 3600:
        raise ConfigError("usage.refresh_seconds must be an integer from 60 to 3600")
    usage_entries = []
    for provider in ("codex", "claude"):
        selected = _string(usage.get(provider + "_credentials_file", ""), "usage credentials path")
        refresh_token = account_id = seed = ""
        if selected:
            credentials_path = Path(selected).expanduser()
            if not credentials_path.is_absolute():
                credentials_path = path.parent / credentials_path
            try:
                credentials = json.loads(credentials_path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
            except (OSError, ValueError, UnicodeError):
                raise ConfigError("cannot read dedicated usage credentials JSON") from None
            if not isinstance(credentials, dict):
                raise ConfigError("usage credentials must be an object")
            if provider == "claude":
                credentials = credentials.get("claudeAiOauth", {})
                if not isinstance(credentials, dict):
                    raise ConfigError("missing claudeAiOauth credentials object")
            refresh_token = _string(credentials.get("refresh_token" if provider == "codex" else "refreshToken"), "usage refresh token")
            if not refresh_token or len(refresh_token) > 4096 or not re.fullmatch(r"[!-~]+", refresh_token):
                raise ConfigError("usage refresh token must be 1-4096 printable ASCII characters without spaces")
            if provider == "codex":
                account_id = _string(credentials.get("account_id"), "Codex account ID")
                if not re.fullmatch(r"[a-zA-Z0-9_-]{1,128}", account_id):
                    raise ConfigError("invalid Codex account ID")
            seed = hashlib.sha256((refresh_token + "\0" + account_id).encode()).hexdigest()
        usage_entries.append("  {" + ", ".join(cpp_string(v) for v in (refresh_token, account_id, seed)) + "},")

    return "\n".join([
        "// Generated from local configuration. Do not commit or edit.",
        "#pragma once",
        "namespace config {",
        f"constexpr char kHostname[] = {cpp_string(hostname)};",
        *font_entries,
        f"constexpr char kWifiSsid[] = {cpp_string(ssid)};",
        f"constexpr char kWifiPassword[] = {cpp_string(password)};",
        f"constexpr char kCalendarId[] = {cpp_string(calendar_id)};",
        f"constexpr char kCalendarEmail[] = {cpp_string(email)};",
        f"constexpr char kCalendarPrivateKey[] = {cpp_string(private_key)};",
        f"constexpr unsigned kCalendarRefreshSeconds = {refresh};",
        f"constexpr unsigned kUsageRefreshSeconds = {usage_refresh};",
        "struct UsageCredentials { const char* refreshToken; const char* accountId; const char* seed; };",
        "constexpr UsageCredentials kUsageCredentials[] = {",
        *usage_entries,
        "};",
        "struct ApiToken { const char* name; const char* value; };",
        "constexpr ApiToken kApiTokens[] = {",
        *entries,
        "  {nullptr, nullptr},",
        "};",
        "}  // namespace config",
        "",
    ])


def configure(env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    selected = os.environ.get("M5PAPER_CONFIG", "config.local.json")
    if not selected:
        raise ConfigError("M5PAPER_CONFIG must not be empty")
    path = Path(selected).expanduser()
    if not path.is_absolute():
        path = project_dir / path
    contents = render_header(path)
    generated_dir = Path(env.subst("$BUILD_DIR")) / "generated"
    generated_dir.mkdir(parents=True, exist_ok=True)
    header = generated_dir / "build_config.h"
    if not header.exists() or header.read_text(encoding="utf-8") != contents:
        header.write_text(contents, encoding="utf-8")
    env.Prepend(CPPPATH=[str(generated_dir)])


if "Import" in globals():
    Import("env")
    try:
        configure(env)
    except ConfigError as error:
        # Never include configuration values or JSON source lines in diagnostics.
        print(f"Build configuration error: {error}")
        env.Exit(1)
