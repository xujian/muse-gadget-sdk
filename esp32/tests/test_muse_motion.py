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

"""Pick-up wake (muse_motion.c): a board lifted or tilted while the screen
sleeps wakes it; one resting still, a single knock or slow sensor drift
doesn't, and nothing wakes it while the hand that put it to sleep settles."""

from __future__ import annotations

import os
import random
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components" / "muse"
OPTION = "CONFIG_MUSE_PICKUP_WAKE"
FLAT = (0, 0, 1000)     # lying on its back: gravity on z
STEP_MS = 100           # muse_input reads the sensor this often while asleep

HARNESS = r"""
#include <stdio.h>
#include "muse_motion.h"

/* stdin: "r T" resets at T ms; "X Y Z T" is a reading. Prints the time of
 * the first reading that wakes, or -1. */
int main(void)
{
    char op[8];
    while (scanf("%7s", op) == 1) {
        if (op[0] == 'r') {
            unsigned t;
            scanf("%u", &t);
            muse_motion_reset(t);
            continue;
        }
        int x, y, z;
        unsigned t;
        sscanf(op, "%d", &x);
        scanf("%d %d %u", &y, &z, &t);
        int16_t mg[3] = { (int16_t)x, (int16_t)y, (int16_t)z };
        if (muse_motion_update(mg, t)) {
            printf("%u\n", t);
            return 0;
        }
    }
    puts("-1");
    return 0;
}
"""


def noisy(base: tuple[int, int, int], rng: random.Random, amount: int = 20) -> tuple[int, int, int]:
    return tuple(v + rng.randint(-amount, amount) for v in base)  # type: ignore[return-value]


class PickupTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        (out / "harness.c").write_text(HARNESS)
        cls.binary = out / "motion"
        proc = subprocess.run(
            [*cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE),
             str(out / "harness.c"), str(MUSE / "muse_motion.c"), "-o", str(cls.binary)],
            capture_output=True,
            text=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    def first_wake(self, readings: list[tuple[int, int, int]], start_ms: int = 5000) -> int:
        """Reset at start_ms, then one reading every STEP_MS; returns the wake time or -1."""
        lines = [f"r {start_ms}"]
        for i, (x, y, z) in enumerate(readings):
            lines.append(f"{x} {y} {z} {start_ms + i * STEP_MS}")
        proc = subprocess.run([str(self.binary)], input="\n".join(lines) + "\n",
                              capture_output=True, text=True, check=True)
        wake = int(proc.stdout.strip())
        return wake if wake < 0 else wake - start_ms

    def test_resting_noise_never_wakes(self) -> None:
        rng = random.Random(1)
        self.assertEqual(self.first_wake([noisy(FLAT, rng) for _ in range(3000)]), -1)

    def test_single_knock_does_not_wake(self) -> None:
        rng = random.Random(2)
        readings = [noisy(FLAT, rng) for _ in range(100)]
        readings[50] = (400, -300, 1600)
        self.assertEqual(self.first_wake(readings), -1)

    def test_pick_up_wakes_within_two_readings(self) -> None:
        rng = random.Random(3)
        rest = [noisy(FLAT, rng) for _ in range(40)]
        # Lifted and tilted toward the face: gravity moves off z onto y.
        held = [noisy((30, -600, 800), rng, 60) for _ in range(20)]
        self.assertEqual(self.first_wake(rest + held), (len(rest) + 1) * STEP_MS)

    def test_settling_after_sleep_is_ignored(self) -> None:
        rng = random.Random(4)
        # The hand that pressed the button moves it for the first second,
        # then it rests somewhere new: that is its resting position.
        settling = [noisy((200 * (i % 3), -400, 900), rng, 100) for i in range(10)]
        rest = [noisy((0, -400, 900), rng) for _ in range(300)]
        self.assertEqual(self.first_wake(settling + rest), -1)

    def test_slow_drift_does_not_wake(self) -> None:
        rng = random.Random(5)
        # 300 mg of offset drift over ten minutes, far slower than a hand.
        readings = [noisy((i // 20, 0, 1000 - i // 40), rng) for i in range(6000)]
        self.assertEqual(self.first_wake(readings), -1)

    def test_gentle_tilt_wakes(self) -> None:
        rng = random.Random(6)
        rest = [noisy(FLAT, rng) for _ in range(40)]
        # About 15 degrees: sin(15) = 0.26 g onto x.
        tilted = [noisy((260, 0, 966), rng) for _ in range(10)]
        self.assertGreater(self.first_wake(rest + tilted), 0)


class PickupGatingTest(unittest.TestCase):
    def test_built_and_called_only_with_the_option(self) -> None:
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'if\(%s\)\s*\n\s*list\(APPEND srcs "muse_motion\.c"\)' % OPTION)
        inside = False
        uses = 0
        for line in (MUSE / "muse_input.c").read_text().splitlines():
            stripped = line.strip()
            if stripped == f"#if {OPTION}":
                inside = True
            elif stripped == "#endif":
                inside = False
            elif "muse_motion" in line or "check_pickup" in line:
                uses += 1
                self.assertTrue(inside, f"outside #if {OPTION}: {stripped}")
        self.assertGreaterEqual(uses, 3)

    def test_sticks3_offers_the_sensor_and_loads_bosch_config(self) -> None:
        board = (MUSE / "boards" / "board_m5stack_sticks3.c").read_text()
        self.assertIn(".read_accel = read_accel", board)
        self.assertIn('"boards/bmi270_config.c"', (MUSE / "CMakeLists.txt").read_text())
        config = (MUSE / "boards" / "bmi270_config.c").read_text()
        self.assertIn("BSD-3-Clause", config)
        self.assertEqual(config.count("0x"), 8192)


if __name__ == "__main__":
    unittest.main()
