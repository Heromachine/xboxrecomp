"""Execute the actual guest clock conversion with deterministic host clocks."""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class GuestTimestampTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc')
        if not cc:
            raise unittest.SkipTest('C compiler required')
        source = (ROOT / 'src/kernel/kernel_hal.c').read_text()
        match = re.search(r'uint64_t recomp_rdtsc64\(void\)\n\{.*?^\}',
                          source, re.M | re.S)
        if not match:
            raise AssertionError('guest clock implementation not found')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path = pathlib.Path(cls.tmp.name)
        harness = r'''
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef int BOOL;
static uint64_t host_ticks, host_hz;
static void QueryPerformanceCounter(LARGE_INTEGER *p) { p->QuadPart = host_ticks; }
static BOOL QueryPerformanceFrequency(LARGE_INTEGER *p) {
    p->QuadPart = host_hz;
    return host_hz != 0;
}
'''
        harness += match.group(0)
        harness += r'''
int main(void) {
    while (scanf("%" SCNu64 " %" SCNu64, &host_hz, &host_ticks) == 2)
        printf("%" PRIu64 "\n", recomp_rdtsc64());
    return 0;
}
'''
        (path / 'clock.c').write_text(harness)
        cls.binary = path / 'clock'
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path / 'clock.c'), '-o', str(cls.binary)], check=True)

    def convert(self, cases):
        result = subprocess.run([str(self.binary)], text=True, check=True,
                                input=''.join(f'{hz} {ticks}\n' for hz, ticks in cases),
                                capture_output=True)
        return [int(x) for x in result.stdout.splitlines()]

    def test_fixed_rate_across_host_frequencies_and_uptimes(self):
        cases = []
        for hz in [1_000_000, 10_000_000, 733_333_333, 1_000_000_000, 3_200_000_000]:
            for seconds in [0, 1, 86_400, 365 * 86_400]:
                for fraction in [0, 1, hz // 1000, hz // 2, hz - 1]:
                    cases.append((hz, seconds * hz + fraction))
        self.assertEqual(self.convert(cases),
                         [ticks * 733_333_333 // hz for hz, ticks in cases])

    def test_adjacent_ticks_are_monotonic_across_second_boundary(self):
        for hz in [10_000_000, 1_000_000_000, 3_200_000_000]:
            values = self.convert([(hz, hz + i) for i in range(-30, 31)])
            self.assertEqual(values, sorted(values))
            self.assertEqual(values[30], 733_333_333)

    def test_host_frequency_change_does_not_change_guest_elapsed_time(self):
        values = self.convert([(10_000_000, 10_000_000),
                               (3_200_000_000, 3_200_000_000)])
        self.assertEqual(values, [733_333_333, 733_333_333])

    def test_unavailable_host_frequency_returns_zero(self):
        self.assertEqual(self.convert([(0, 12345)]), [0])


if __name__ == '__main__':
    unittest.main()
