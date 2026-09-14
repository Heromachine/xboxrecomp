"""Execute the real NtQueryDirectoryFile search-pattern conversion out of bounds.

Run: py -3 -m pytest tools/kernel_audit/test_dir_pattern_bounds.py

FileName->Length in NtQueryDirectoryFile is a 16-bit byte count the GUEST
chooses. The Win32 backend used to convert the pattern with a bounded
MultiByteToWideChar and then terminate it with

    pattern_wide[FileName->Length] = L'\\0';

into a WCHAR[MAX_PATH]. The conversion was bounded; that write was not, so a
Length of 0xFFFF stored a zero WCHAR 130 KB past a stack array. Breakdown hit
it for real (host crash at kernel_file.c:604, HeroLab task 17f002cc) because
the ordinal-207 bridge read its arguments one slot short and passed the
FILE_INFORMATION_CLASS constant where the ANSI_STRING pointer belonged -- but
the write was reachable from any title that built a long or malformed
descriptor, so the bridge fix alone is not the guard.

This compiles the real dir_pattern_from_ansi out of src/kernel/kernel_file.c,
puts its output buffer between canary pages of a larger array, and feeds it the
descriptors that used to overflow. The canaries are what makes it a bounds
test: a clamp that quietly shortened the pattern would also pass an
"it did not crash" check, so the statuses are asserted too.
"""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

MAX_PATH = 260


def _extract(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise AssertionError(f"not found in kernel_file.c: {pattern}")
    return match.group(0)


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef long           NTSTATUS;
typedef unsigned short USHORT;
typedef char *         PCHAR;

#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_PARAMETER     ((NTSTATUS)0xC000000DL)
#define STATUS_OBJECT_NAME_INVALID   ((NTSTATUS)0xC0000033L)
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#define MAX_PATH 260

typedef struct _XBOX_ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR  Buffer;
} XBOX_ANSI_STRING;

/*__DIR_PATTERN_FUNC__*/

/* The pattern buffer sits in the middle of a canaried arena. Anything the
 * conversion writes outside [PAD, PAD + MAX_PATH) shows up as a changed
 * canary byte, whether it is one WCHAR past the end or 0xFFFF of them. */
#define PAD 4096
static unsigned char arena[PAD + MAX_PATH + PAD];

static int failures = 0;

static void check(const char *name, USHORT length, USHORT maximum_length,
                  int buffer_bytes, char fill,
                  NTSTATUS want_status, const char *want_pattern)
{
    XBOX_ANSI_STRING fn;
    static char guest[0x20000];
    NTSTATUS got;
    char *out = (char *)(arena + PAD);
    size_t i;

    memset(arena, 0xCC, sizeof(arena));
    memset(guest, fill, sizeof(guest));
    if (buffer_bytes >= 0 && (size_t)buffer_bytes < sizeof(guest))
        guest[buffer_bytes] = '\0';

    fn.Length        = length;
    fn.MaximumLength = maximum_length;
    fn.Buffer        = guest;

    got = dir_pattern_from_ansi(&fn, out, MAX_PATH);

    for (i = 0; i < PAD; i++) {
        if (arena[i] != 0xCC) {
            printf("FAIL %s: wrote %zu bytes BELOW the buffer\n", name, PAD - i);
            failures++;
            return;
        }
    }
    for (i = 0; i < PAD; i++) {
        if (arena[PAD + MAX_PATH + i] != 0xCC) {
            printf("FAIL %s: wrote %zu bytes PAST the end of a %d-byte buffer\n",
                   name, i + 1, MAX_PATH);
            failures++;
            return;
        }
    }
    if (got != want_status) {
        printf("FAIL %s: status 0x%08lX, want 0x%08lX\n",
               name, (unsigned long)got, (unsigned long)want_status);
        failures++;
        return;
    }
    if (want_pattern && strcmp(out, want_pattern) != 0) {
        printf("FAIL %s: pattern \"%s\", want \"%s\"\n", name, out, want_pattern);
        failures++;
        return;
    }
    printf("ok   %s\n", name);
}

int main(void)
{
    XBOX_ANSI_STRING fn;
    char *out = (char *)(arena + PAD);

    /* The shape Breakdown actually passes: "*.*" with Length == 3. */
    check("normal_pattern", 3, 3, 3, '*', STATUS_SUCCESS, "***");

    /* Exactly MAX_PATH bytes leaves no room for the terminator. The old code
     * wrote pattern_wide[260] -- one element past a WCHAR[260]. */
    check("length_is_MAX_PATH", MAX_PATH, MAX_PATH, MAX_PATH, 'A',
          STATUS_OBJECT_NAME_INVALID, "");

    /* One under is the largest pattern that legitimately fits: MAX_PATH - 1
     * bytes plus the terminator is exactly MAX_PATH. Converting that to wide
     * characters also lands exactly on WCHAR[MAX_PATH], so this is the real
     * boundary on both sides and must keep working, not be clamped away. */
    check("length_is_MAX_PATH_minus_1", MAX_PATH - 1, MAX_PATH - 1,
          MAX_PATH - 1, 'A', STATUS_SUCCESS, NULL);

    /* The observed crash: a Length read out of a non-descriptor. 0xFFFF of
     * these is a 128 KB stack smear in the old code.
     * Note what proves this one: a write that far out clears the canaries
     * entirely and lands in unrelated memory, so the asserted STATUS is the
     * guard here, not the canary. The canary is what catches the near case
     * (length_is_MAX_PATH, one element past), which is the one that is hard
     * to see by reading. Both shapes are covered, by different assertions. */
    check("length_is_0xFFFF", 0xFFFF, 0xFFFF, 0x1000, 'A',
          STATUS_OBJECT_NAME_INVALID, "");

    /* Length larger than the buffer the descriptor itself declares: the guest
     * struct is inconsistent, so reading Length bytes runs off its allocation
     * even when Length is small enough for the host buffer. */
    check("length_exceeds_maximum_length", 64, 16, 64, 'A',
          STATUS_INVALID_PARAMETER, "");

    /* MaximumLength == 0 means the guest did not fill it in; Length still
     * governs, and still has to fit. */
    check("zero_maximum_length_is_not_a_violation", 4, 0, 4, 'B',
          STATUS_SUCCESS, "BBBB");

    /* No pattern at all -> empty result, caller substitutes "*". */
    check("zero_length", 0, 16, 0, 'A', STATUS_SUCCESS, "");

    memset(arena, 0xCC, sizeof(arena));
    fn.Length = 0xFFFF; fn.MaximumLength = 0xFFFF; fn.Buffer = NULL;
    if (dir_pattern_from_ansi(&fn, out, MAX_PATH) != STATUS_SUCCESS ||
        out[0] != '\0') {
        printf("FAIL null_buffer\n"); failures++;
    } else {
        printf("ok   null_buffer\n");
    }

    memset(arena, 0xCC, sizeof(arena));
    if (dir_pattern_from_ansi(NULL, out, MAX_PATH) != STATUS_SUCCESS ||
        out[0] != '\0') {
        printf("FAIL null_descriptor\n"); failures++;
    } else {
        printf("ok   null_descriptor\n");
    }

    printf(failures ? "FAILURES %d\n" : "all bounds checks passed\n", failures);
    return failures ? 1 : 0;
}
"""


class DirPatternBoundsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        source = (ROOT / "src/kernel/kernel_file.c").read_text(
            encoding="utf-8", errors="replace")
        func = _extract(
            source,
            r"^static NTSTATUS dir_pattern_from_ansi\(.*?^\}")
        cls.tmp = tempfile.TemporaryDirectory()
        tmp = pathlib.Path(cls.tmp.name)
        (tmp / "h.c").write_text(HARNESS.replace("/*__DIR_PATTERN_FUNC__*/", func))
        subprocess.run(
            [cc, "-std=c99", "-O1", "-Wall", "-Werror",
             str(tmp / "h.c"), "-o", str(tmp / "h")],
            check=True, capture_output=True)
        cls.result = subprocess.run([str(tmp / "h")], capture_output=True,
                                    text=True)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "tmp"):
            cls.tmp.cleanup()

    def test_no_write_lands_outside_the_pattern_buffer(self):
        self.assertEqual(
            self.result.returncode, 0,
            "guest-controlled Length escaped the host buffer:\n"
            + self.result.stdout + self.result.stderr)
        self.assertIn("all bounds checks passed", self.result.stdout)

    def test_nothing_indexes_a_host_buffer_by_a_guest_length(self):
        """The literal shape of the bug, guarded in the source itself.

        The bounds harness above only proves the helper is safe. This makes
        sure the call sites did not keep a copy of the old pattern: no host
        array may be subscripted by an ANSI_STRING Length field.
        """
        source = (ROOT / "src/kernel/kernel_file.c").read_text(
            encoding="utf-8", errors="replace")
        code = "\n".join(
            line for line in source.splitlines()
            if not line.lstrip().startswith("*")
            and not line.lstrip().startswith("/*")
            and not line.lstrip().startswith("//"))
        offenders = re.findall(r"\w+\s*\[\s*\w+->Length\s*\]", code)
        self.assertEqual(
            offenders, [],
            "a host buffer is indexed by a guest-supplied Length: %s" % offenders)


class DotEntryTest(unittest.TestCase):
    """NtQueryDirectoryFile must not report "." or ".." to the guest.

    FATX keeps no dot entries in a subdirectory, so the Xbox kernel never
    returns them; both host backends do (FindFirstFileW and readdir). The
    evidence that this matters is Breakdown's own recursive directory delete
    at 0x001AE48A, which recurses into every entry that has the directory
    attribute and filters no names at all -- a kernel that returned "." would
    send retail code into unbounded recursion on real hardware.

    This is a source-shape guard rather than an execution test: the Win32 half
    cannot run here, and the failure it protects against is a backend rewrite
    dropping the skip, not the predicate being subtly wrong.
    """

    def setUp(self):
        self.source = (ROOT / "src/kernel/kernel_file.c").read_text(
            encoding="utf-8", errors="replace")

    def test_both_backends_skip_dot_entries(self):
        self.assertIn(
            "dir_entry_is_dot_w(ctx->find_data.cFileName)", self.source,
            "the Win32 enumeration no longer skips FindFirstFileW's "
            '"." and ".." entries')
        self.assertIn(
            "dir_entry_is_dot(de->d_name)", self.source,
            'the POSIX enumeration no longer skips readdir\'s "." and ".."')

    def test_the_skip_covers_the_first_entry_and_every_later_one(self):
        """A skip on FindNextFileW alone still leaks a leading dot entry.

        FindFirstFileW almost always returns "." first, so guarding only the
        continuation path passes a casual read and still hands the guest the
        one entry that causes the recursion.
        """
        win32 = self.source.split("#if defined(_WIN32)", 1)[1]
        win32 = win32.split("#else /* !_WIN32 */", 1)[0]
        self.assertEqual(
            win32.count("dir_entry_is_dot_w("), 3,
            "expected the dot predicate in the Win32 backend three times: its "
            "definition, the FindFirstFileW skip and the FindNextFileW skip")


if __name__ == "__main__":
    unittest.main(verbosity=2)
