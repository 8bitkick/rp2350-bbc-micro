#!/usr/bin/env python3
"""Host tests; no SDK, downloads or hardware needed. CC defaults to cc."""
from pathlib import Path
import os
import sys
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
with tempfile.TemporaryDirectory(prefix="presto-audio-test-") as tmp:
    tmp = Path(tmp)
    for name in ("pico/stdlib.h", "pico/time.h", "hardware/clocks.h",
                 "hardware/pwm.h", "hardware/sync.h"):
        p = tmp / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('#include "mock_pico.h"\n')
    output = tmp / "backend_test"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I" + str(tmp), "-I" + str(HERE), "-I" + str(ROOT / "src/beebjit"),
        str(HERE / "backend_test.c"), "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)

    output = tmp / "synthesis_test"
    strip = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
        "-std=gnu11", "-O2", "-ffunction-sections", "-fdata-sections",
        "-I" + str(ROOT / "src/beebjit"), str(HERE / "synthesis_test.c"),
        strip, "-lm", "-o", str(output)], check=True)
    subprocess.run([str(output)], check=True)
