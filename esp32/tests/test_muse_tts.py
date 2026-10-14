# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Spoken replies (muse_tts.c): the request ElevenLabs gets is well-formed
whatever the reply says, replies stay text without an API key, and the whole
feature is built only under CONFIG_MUSE_TTS_ELEVENLABS."""

from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
OPTION = "CONFIG_MUSE_TTS_ELEVENLABS"

HARNESS = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "muse_tts.h"
%s
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "enabled")) {
        puts(muse_tts_enabled() ? "yes" : "no");
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "url")) {
        char url[160];
        muse_tts_url(argv[2], url, sizeof(url));
        puts(url);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "body")) {
        char *body = muse_tts_body(argv[2], argv[3]);
        if (!body) {
            return 1;
        }
        fputs(body, stdout);
        cJSON_free(body);
        return 0;
    }
    return 2;
}
"""


class TtsRequestTest(unittest.TestCase):
    binaries: dict[str, Path]

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        if not (JSON / "cJSON.c").exists():
            raise unittest.SkipTest("cJSON not found: run one idf.py build, or set CJSON_SOURCE_DIR")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (MUSE / "muse_tts.c").read_text()
        request = source[source.index("/* ---- Request ---- */"):source.index("/* ---- Fetch ---- */")]
        (out / "harness.c").write_text(HARNESS % request)
        cls.binaries = {}
        for name, key in (("keyed", "sk_test"), ("unkeyed", "")):
            cls.binaries[name] = out / name
            proc = subprocess.run(
                [
                    *cc,
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f'-D{OPTION}_API_KEY="{key}"',
                    "-I",
                    str(JSON),
                    "-I",
                    str(MUSE),
                    str(out / "harness.c"),
                    str(JSON / "cJSON.c"),
                    "-o",
                    str(cls.binaries[name]),
                ],
                capture_output=True,
                text=True,
            )
            if proc.returncode:
                raise AssertionError(proc.stdout + proc.stderr)

    def run_harness(self, *args: str, binary: str = "keyed") -> str:
        proc = subprocess.run([str(self.binaries[binary]), *args], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return proc.stdout

    def test_body_round_trips_any_reply_text(self) -> None:
        for text in (
            "Hello there.",
            'She said "hi",\nthen left.\tBack\\slash',
            "你好，今天天气很好。",
            "Mixed 中文 and English, with an emoji 🙂",
        ):
            with self.subTest(text=text):
                body = json.loads(self.run_harness("body", text, "eleven_flash_v2_5"))
                self.assertEqual(body, {"text": text, "model_id": "eleven_flash_v2_5"})

    def test_url_names_the_voice_and_a_small_mp3_format(self) -> None:
        url = self.run_harness("url", "VoiceId123").strip()
        self.assertEqual(
            url,
            "https://api.elevenlabs.io/v1/text-to-speech/VoiceId123/stream?output_format=mp3_22050_32",
        )

    def test_replies_stay_text_without_an_api_key(self) -> None:
        self.assertEqual(self.run_harness("enabled", binary="keyed").strip(), "yes")
        self.assertEqual(self.run_harness("enabled", binary="unkeyed").strip(), "no")


class TtsGatingTest(unittest.TestCase):
    def test_option_is_off_and_keyless_by_default(self) -> None:
        kconfig = (MUSE / "Kconfig").read_text()
        option = kconfig[kconfig.index("config MUSE_TTS_ELEVENLABS\n"):kconfig.index("config MUSE_TTS_ELEVENLABS_API_KEY")]
        self.assertIn("depends on MUSE_HATCH", option)
        self.assertIn("default n", option)
        key = kconfig[kconfig.index("config MUSE_TTS_ELEVENLABS_API_KEY"):kconfig.index("config MUSE_TTS_ELEVENLABS_VOICE_ID")]
        self.assertIn('default ""', key)

    def test_module_is_built_only_with_the_option(self) -> None:
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'if\(%s\)\s*\n\s*list\(APPEND srcs "muse_tts\.c"\)\s*\n\s*endif\(\)' % OPTION)
        self.assertEqual(cmake.count("muse_tts.c"), 1)

    def test_session_uses_it_only_under_the_option(self) -> None:
        """Every muse_tts name in the session file sits inside an #if for the option."""
        inside = False
        uses = 0
        for line in (MUSE / "muse_chat_session.cpp").read_text().splitlines():
            stripped = line.strip()
            if stripped == f"#if {OPTION}":
                self.assertFalse(inside, "nested #if")
                inside = True
            elif stripped == "#endif" and inside:
                inside = False
            elif re.search(r"\b(muse_tts_\w+|MUSE_TTS_\w+|tts_pump)\b", line) and not stripped.startswith(("*", "/*")):
                uses += 1
                self.assertTrue(inside, f"outside #if {OPTION}: {stripped}")
        self.assertFalse(inside)
        self.assertGreaterEqual(uses, 4)   # start, read through tts_pump, state, cancel

    def test_no_tracked_config_carries_a_key_or_turns_it_on(self) -> None:
        for path in [ROOT / "sdkconfig.defaults", *sorted((ROOT / "devices").glob("sdkconfig.*"))]:
            with self.subTest(path=path.name):
                self.assertNotIn("MUSE_TTS_ELEVENLABS", path.read_text())


if __name__ == "__main__":
    unittest.main()
