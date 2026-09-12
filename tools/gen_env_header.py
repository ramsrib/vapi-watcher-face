#!/usr/bin/env python3
"""Generate a C header of ENV_* string macros from a .env file.

Usage:
    gen_env_header.py <path-to-.env> <path-to-output-header>

Only a fixed allowlist of keys is emitted, each as:
    #define ENV_<KEY> "<value>"

A missing .env, an unknown key, or an empty value simply omits that macro (so
settings.h falls back to the menuconfig CONFIG_* value). The header is always
(re)written when its contents change, clearing stale values.

This is intentionally done in Python rather than in CMake: it sidesteps CMake's
list-semicolon splitting and command-line quote-escaping, so values with spaces,
semicolons, quotes, or backslashes (e.g. WiFi passwords) survive intact.
"""

import os
import sys

# Keys recognized in .env, in the order they appear in the generated header.
# Keys the firmware cannot run without. Missing ones are only a warning, not an
# error: menuconfig (CONFIG_VAPI_*) is a legitimate alternative to .env, and a
# CI or smoke build has no credentials at all. The point is to surface it at
# build time rather than after a flash, on the serial log.
REQUIRED = ["WIFI_SSID", "VAPI_API_KEY", "VAPI_ASSISTANT_ID"]

KEYS = [
    "WIFI_SSID",
    "WIFI_PASSWORD",
    "VAPI_API_URL",
    "VAPI_API_KEY",
    "VAPI_ASSISTANT_ID",
    "VAPI_FIRST_MESSAGE",
    "VAPI_MAX_DURATION_SECONDS",
]


def parse_env(path):
    """Return {key: value} for recognized, non-empty keys in the .env file."""
    values = {}
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[len("export "):].lstrip()
            if "=" not in line:
                continue
            key, val = line.split("=", 1)
            key = key.strip()
            val = val.strip()
            # A quoted value ends at its closing quote; anything after it (e.g. an
            # inline "# comment") is discarded. An unquoted value is cut at the
            # first '#'. Getting this wrong silently ships the comment as part of
            # the value, which is exactly how a stray comment once ended up in an
            # HTTP header.
            if val[:1] in ("'", '"'):
                quote = val[0]
                end = val.find(quote, 1)
                val = val[1:end] if end != -1 else val[1:]
            else:
                hash_at = val.find("#")
                if hash_at != -1:
                    val = val[:hash_at]
                val = val.rstrip()
            if key in KEYS and val != "":
                values[key] = val
    return values


def c_escape(s):
    """Escape a string for use inside a C double-quoted string literal."""
    out = []
    for ch in s:
        if ch in ("\\", '"'):
            out.append("\\" + ch)
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return "".join(out)


def render(values):
    lines = [
        "/* AUTO-GENERATED from .env by tools/gen_env_header.py.",
        "   Do not edit. Do not commit (this file is gitignored). */",
        "#pragma once",
        "",
    ]
    emitted = False
    for key in KEYS:
        if key in values:
            lines.append('#define ENV_{} "{}"'.format(key, c_escape(values[key])))
            emitted = True
    if not emitted:
        lines.append("/* (no .env overrides in effect) */")
    lines.append("")
    return "\n".join(lines)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_env_header.py <.env> <output.h>\n")
        return 2
    env_path, out_path = argv[1], argv[2]

    values = parse_env(env_path) if os.path.isfile(env_path) else {}

    if os.path.isfile(env_path):
        missing = [k for k in REQUIRED if not values.get(k)]
        if missing:
            sys.stderr.write(
                "\n"
                "  ****************************************************************\n"
                "  *  {} is missing required values:\n".format(env_path)
                + "".join("  *    {}\n".format(k) for k in missing)
                + "  *\n"
                  "  *  The firmware will build, but calls will fail at runtime.\n"
                  "  *  Edit {} (or set them via `idf.py menuconfig`).\n".format(env_path)
                + "  ****************************************************************\n\n")
    elif not os.path.isfile(env_path):
        sys.stderr.write(
            "\n  note: no {} found — using menuconfig values. "
            "`make setup` creates one from .env.example.\n\n".format(env_path))

    content = render(values)

    # Only rewrite when changed, so we don't force an unnecessary rebuild.
    old = None
    if os.path.isfile(out_path):
        with open(out_path, "r", encoding="utf-8") as fh:
            old = fh.read()
    if old != content:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write(content)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
