#!/usr/bin/env python3
"""Check setup flags offline, with fake tools and already-present dependencies."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="presto-setup-test-") as td:
    root = Path(td)
    (root / "scripts").mkdir()
    shutil.copyfile(ROOT / "scripts/setup.sh", root / "scripts/setup.sh")
    (root / "deps/pico-sdk/.git").mkdir(parents=True)
    tc = root / "deps/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin"
    tc.mkdir(parents=True)
    compiler = tc / "arm-none-eabi-gcc"
    compiler.write_text("#!/bin/sh\nexit 0\n")
    compiler.chmod(0o755)
    for name in ("discs/elite-disc-flicker-free-sth.ssd", "discs/bs-wobble-colours.ssd",
                 "discs/bs-bar-beeb.ssd", "roms/general/gxr.rom"):
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.touch()
    bin_dir = root / "bin"
    bin_dir.mkdir()
    def tool(name, body):
        p = bin_dir / name
        p.write_text("#!/bin/sh\n" + body)
        p.chmod(0o755)
    tool("uname", 'case "$1" in -s) echo Linux;; -m) echo x86_64;; *) echo Linux x86_64;; esac\n')
    tool("cmake", 'if [ "$1" = --version ]; then echo "cmake version 3.30.0"; else printf "%s\\n" "$*" >> "$SETUP_TEST_LOG"; fi\n')
    tool("ninja", "exit 0\n")
    for name in ("git", "curl", "tar"):
        tool(name, 'echo "unexpected dependency fetch" >&2; exit 99\n')
    log = root / "commands.log"
    env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ["PATH"],
               SETUP_TEST_LOG=str(log))
    def run(*args):
        return subprocess.run(["sh", str(root / "scripts/setup.sh"), *args],
                              env=env, capture_output=True, text=True)
    result = run("--help")
    assert result.returncode == 0 and "--audio" in result.stdout
    assert run("--not-an-option").returncode != 0
    result = run("--audio", "--gxr", "--demos")
    assert result.returncode == 0, result.stderr
    assert "-DPRESTO_BEEBJIT_AUDIO=ON" in log.read_text()
    log.write_text("")
    result = run()
    assert result.returncode == 0, result.stderr
    assert "-DPRESTO_BEEBJIT_AUDIO=OFF" in log.read_text()
    log.write_text("")
    result = run("--audio", "--no-build")
    assert result.returncode == 0 and not log.read_text()
    assert "-DPRESTO_BEEBJIT_AUDIO=ON" in result.stdout
    print("PASS: setup help, unknown option, combined flags, ON/OFF and no-build")
