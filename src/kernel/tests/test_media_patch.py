"""XBOXRECOMP_MEDIA_PATCH must relax AllowedMediaTypes, and must be honourable.

A retail XBE's certificate says it may run only from a retail DVD
(AllowedMediaTypes = 0x00000002, DVD_X2). Titles check it -- Burnout compares
[[0x10118]+0x9C] & 0xFFFFFF against 2 and, on a match, interrogates the DVD
drive over SCSI before it will start. There is no drive here, so the loader
relaxes the field the way a modchip BIOS does, which is why that check does not
run under a modded console or under xemu either.

Two behaviours are pinned:
  * default            -> the field is relaxed to 0x00FFFFFF
  * MEDIA_PATCH=0      -> the field is left exactly as authored

The second matters as much as the first. It selects the faithful-to-retail path
where the title runs its own check and kernel_file.c answers the MODE SENSE, so
a regression that quietly ignored the switch would strand that path untested
while looking fine.

This drives the real loader through a game build rather than asserting on
source text: it runs the recompiled Burnout executable far enough to print its
memory-layout banner, which reports what the field became.
"""

import os
import pathlib
import re
import shutil
import subprocess
import unittest

# parents: [0] tests, [1] kernel, [2] src, [3] repo root
_ROOT = pathlib.Path(__file__).resolve().parents[3]
_EXE = _ROOT.parent / "Burnout-Launcher/build-win/burnout_recomp.exe"
_CWD = _ROOT.parent / "Burnout-Launcher"

# "  Media types: 0x00000002 -> 0x00FFFFFF at Xbox VA 0x00010214 ..."
_PATCHED = re.compile(r"Media types:\s*(0x[0-9A-Fa-f]+)\s*->\s*(0x[0-9A-Fa-f]+)")
_LEFT = re.compile(r"Media types:\s*left as authored")


def _run(env_overrides):
    if not _EXE.is_file():
        raise unittest.SkipTest(f"{_EXE} not built")
    if shutil.which("wine") is None:
        raise unittest.SkipTest("wine not installed")

    env = dict(os.environ)
    env.update(env_overrides)
    try:
        # The banner is printed during init; the title then runs indefinitely,
        # so it is killed once there has been ample time to emit it.
        proc = subprocess.run(["wine", str(_EXE)], cwd=str(_CWD), env=env,
                              capture_output=True, text=True, timeout=60)
        out = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as exc:
        out = (exc.stdout or "") + (exc.stderr or "")
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
    return out


class MediaPatch(unittest.TestCase):
    def test_default_relaxes_the_field(self):
        out = _run({})
        m = _PATCHED.search(out)
        self.assertIsNotNone(m, f"no 'Media types: X -> Y' line:\n{out[-3000:]}")
        was, now = int(m.group(1), 16), int(m.group(2), 16)
        self.assertEqual(was & 0xFFFFFF, 0x2, "Burnout's XBE should be authored DVD_X2")
        self.assertEqual(now, 0x00FFFFFF)
        # The whole point: the title's own check must no longer match.
        self.assertNotEqual(now & 0xFFFFFF, 0x2)

    def test_switch_off_leaves_it_alone(self):
        out = _run({"XBOXRECOMP_MEDIA_PATCH": "0"})
        self.assertIsNotNone(_LEFT.search(out),
                             f"switch ignored:\n{out[-3000:]}")
        self.assertIsNone(_PATCHED.search(out), "field was patched despite the switch")

    def test_switch_off_still_reaches_the_device(self):
        """The faithful path must actually work, not merely skip the patch."""
        out = _run({"XBOXRECOMP_MEDIA_PATCH": "0"})
        self.assertIn("cdrom0.dev", out,
                      "with the check live, the title should open the raw CD-ROM device")


if __name__ == "__main__":
    unittest.main()
