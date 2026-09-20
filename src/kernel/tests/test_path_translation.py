"""Device paths: the bare form is the DEVICE, the separator form is the FILESYSTEM.

kernel_path.c keeps two different meanings for what looks like one path:

    \\Device\\Harddisk0\\Partition5      -> Partition5.img   (raw partition device)
    \\Device\\Harddisk0\\Partition5\\    -> a directory       (filesystem on it)
    \\Device\\CdRom0                    -> cdrom0.dev       (raw CD-ROM device)
    \\Device\\CdRom0\\romdata\\x.pak     -> the game directory

That distinction is load-bearing and was previously untested. Breakdown writes
a FATX superblock straight to the raw partition device (sub_001AE166) after
claiming it from the allocation table in partition0 (sub_001ABB8E), while
Burnout opens the bare CD-ROM device to send it a SCSI MODE SENSE
(sub_00018C9F). Collapsing the two forms breaks one title or the other, and a
proposed "just match the prefix" change to this file would have done exactly
that -- hence this test.

It compiles the real _WIN32 branch (the one both titles ship) and runs it,
rather than asserting on source text, because the bug this guards against is
behavioural. Requires the mingw cross-compiler, wine, and a built
libxbox_kernel.a; skips cleanly when any is missing.
"""

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

_HERE = pathlib.Path(__file__).resolve().parent
_ROOT = _HERE.parents[2]
_HARNESS = _HERE / "path_harness.c"

# Built by any game project that links the toolkit; the Burnout and Breakdown
# trees are the two that exist on this machine.
_LIB_CANDIDATES = [
    _ROOT.parent / "Burnout-Launcher/build-win/xboxrecomp/src/kernel/libxbox_kernel.a",
    _ROOT.parent / "Breakdown-Launcher/build-win/xboxrecomp/src/kernel/libxbox_kernel.a",
]

_CC = "x86_64-w64-mingw32-gcc"
_LIBS = ["-lshlwapi", "-lshell32", "-lole32", "-luuid", "-lws2_32", "-lwinmm"]

# xbox_path_init is called with these two roots, so expectations are literal.
GAME, SAVE = "GAMEDIR", "SAVEDIR"

EXPECTED = {
    # The change under test: the bare device resolves to its own backing
    # object instead of falling through to the unmatched-path passthrough.
    "\\Device\\CdRom0":                        f"{SAVE}\\cdrom0.dev",
    # ...and the filesystem on that disc is untouched by it.
    "\\Device\\CdRom0\\romdata\\x.pak":        f"{GAME}\\romdata\\x.pak",
    # Pre-existing raw devices, pinned so they cannot be collapsed later.
    "\\Device\\Harddisk0\\partition0":         f"{SAVE}\\partition0.bin",
    "\\Device\\Harddisk0\\Partition5":         f"{SAVE}\\Partition5.img",
    "\\Device\\Harddisk0\\Partition5\\foo.txt": f"{SAVE}\\Partition5\\foo.txt",
    # Ordinary rule-table paths, to catch a change that shadows them.
    "D:\\romdata\\x.pak":                      f"{GAME}\\romdata\\x.pak",
    "T:\\save.dat":                            f"{SAVE}\\TitleData\\save.dat",
}


def _find_lib():
    for p in _LIB_CANDIDATES:
        if p.is_file():
            return p
    return None


def _run_harness():
    lib = _find_lib()
    if lib is None:
        raise unittest.SkipTest("no libxbox_kernel.a built; build a game project first")
    if shutil.which(_CC) is None:
        raise unittest.SkipTest(f"{_CC} not installed")
    if shutil.which("wine") is None:
        raise unittest.SkipTest("wine not installed")

    with tempfile.TemporaryDirectory() as td:
        exe = pathlib.Path(td) / "path_harness.exe"
        build = subprocess.run(
            [_CC, "-o", str(exe), str(_HARNESS), str(lib), *_LIBS],
            capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError("harness build failed:\n" + build.stderr[-2000:])
        # Run inside the temp dir: xbox_path_init creates the save directory
        # (and a partition0.bin/cdrom0.dev in it) relative to the working
        # directory, which would otherwise litter the repository root.
        run = subprocess.run(["wine", str(exe)], capture_output=True, text=True,
                             timeout=180, cwd=td)
        out = {}
        for line in run.stdout.splitlines():
            m = re.match(r"^CASE\|(.*)\|(\d)\|(.*)$", line)
            if m:
                out[m.group(1)] = (m.group(2) == "1", m.group(3))
        if not out:
            raise AssertionError(
                "harness produced no CASE lines:\n" + run.stdout[-2000:] + run.stderr[-2000:])
        return out


class PathTranslation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.results = _run_harness()

    def test_every_case_translates(self):
        for path in EXPECTED:
            with self.subTest(path=path):
                self.assertIn(path, self.results, "harness did not report this path")
                ok, _ = self.results[path]
                self.assertTrue(ok, "xbox_translate_path returned FALSE")

    def test_expected_mapping(self):
        for path, want in EXPECTED.items():
            with self.subTest(path=path):
                _, got = self.results[path]
                self.assertEqual(got, want)

    def test_bare_and_separator_forms_differ(self):
        """The whole point: these must not resolve to the same place."""
        for bare, withsep in (
            ("\\Device\\CdRom0", "\\Device\\CdRom0\\romdata\\x.pak"),
            ("\\Device\\Harddisk0\\Partition5", "\\Device\\Harddisk0\\Partition5\\foo.txt"),
        ):
            with self.subTest(bare=bare):
                self.assertNotEqual(self.results[bare][1], self.results[withsep][1])


if __name__ == "__main__":
    unittest.main()
