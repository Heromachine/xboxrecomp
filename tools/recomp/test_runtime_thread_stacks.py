"""Execute the real guest-stack allocators: game-thread stacks and worker slices.

Regression guard: xbox_AllocThreadStack was a bump allocator over the same
bottom of the stack region the worker-slice table hands out, on the assumption
that a title never uses both. Interrupt delivery takes worker slices in every
title, so the two overlapped: in Breakdown the fourth interrupt thread ran its
service routine on the pak loader thread's stack (both at top 0x0087FFF0),
zeroed a local, and base.pak never finished loading. Every guest stack now
comes from the one table, so no interleaving can overlap two of them.
"""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GAME, WORKER = 512 * 1024, 256 * 1024


def _extract(path, pattern):
    match = re.search(pattern, (ROOT / path).read_text(), re.M | re.S)
    if not match:
        raise AssertionError(f"not found in {path}: {pattern}")
    return match.group(0)


class ThreadStackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        layout_h = "src/kernel/xbox_memory_layout.h"
        pieces = [
            _extract(layout_h, r"^#define XBOX_STACK_SIZE .*?$"),
            _extract(layout_h, r"^#define XBOX_STACK_BASE .*?$"),
            _extract(layout_h, r"^#define XBOX_STACK_TOP .*?$"),
            _extract(layout_h, r"^#define XBOX_WORKER_STACK_SIZE .*?$"),
            _extract(layout_h, r"^#define XBOX_WORKER_STACK_BASE .*?$"),
            _extract(layout_h, r"^#define XBOX_WORKER_STACK_COUNT .*?$"),
            _extract(layout_h, r"^#define XBOX_WORKER_STACK_TOP\(n\) .*?\)\)? *- 16\)"),
            "int xbox_worker_stack_alloc_span(int count);",
            _extract("src/kernel/kernel_thread.c",
                     r"^static volatile LONG g_worker_stack_used\[XBOX_WORKER_STACK_COUNT\];"),
            _extract("src/kernel/kernel_thread.c", r"^int xbox_worker_stack_alloc\(void\)\n\{.*?^\}"),
            _extract("src/kernel/kernel_thread.c",
                     r"^int xbox_worker_stack_alloc_span\(int count\)\n\{.*?^\}"),
            _extract("src/kernel/kernel_thread.c", r"^void xbox_worker_stack_free\(int slot\)\n\{.*?^\}"),
            _extract("src/kernel/xbox_memory_layout.c", r"^uint32_t xbox_AllocThreadStack\(void\)\n\{.*?^\}"),
        ]
        harness = r"""
#include <stdint.h>
#include <stdio.h>
typedef long LONG;
static LONG InterlockedCompareExchange(volatile LONG *d, LONG x, LONG c)
{ LONG o = *d; if (o == c) *d = x; return o; }
static LONG InterlockedExchange(volatile LONG *d, LONG x)
{ LONG o = *d; *d = x; return o; }
""" + "\n".join(pieces) + r"""
int main(void) {
    char op; int arg;
    printf("region %u %u %u\n", (unsigned)XBOX_STACK_BASE, (unsigned)XBOX_STACK_TOP,
           (unsigned)XBOX_WORKER_STACK_COUNT);
    while (scanf(" %c %d", &op, &arg) == 2) {
        if (op == 'g') {
            printf("g %u\n", xbox_AllocThreadStack());
        } else if (op == 'w') {
            int slot = xbox_worker_stack_alloc();
            printf("w %d %u\n", slot, slot < 0 ? 0u : (unsigned)XBOX_WORKER_STACK_TOP(slot));
        } else {
            xbox_worker_stack_free(arg);
            printf("f %d\n", arg);
        }
    }
    return 0;
}
"""
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path = pathlib.Path(cls.tmp.name)
        (path / "stacks.c").write_text(harness)
        cls.binary = path / "stacks"
        subprocess.run([cc, "-std=c11", "-Wall", str(path / "stacks.c"), "-o", str(cls.binary)],
                       check=True)

    def run_ops(self, ops):
        run = subprocess.run([str(self.binary)], text=True, check=True, capture_output=True,
                             input="".join(f"{op} {arg}\n" for op, arg in ops))
        lines = run.stdout.split("\n")
        base, top, count = map(int, lines[0].split()[1:])
        spans, results = [], []
        for line in lines[1:]:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "g":
                t = int(parts[1]); results.append(t)
                if t:
                    spans.append((t + 16 - GAME, t + 16))
            elif parts[0] == "w":
                t = int(parts[2]); results.append(int(parts[1]))
                if t:
                    spans.append((t + 16 - WORKER, t + 16))
        return base, top, count, spans, results

    def assert_disjoint(self, spans, base, top):
        spans = sorted(spans)
        for lo, hi in spans:
            self.assertGreaterEqual(lo, base)
            self.assertLessEqual(hi, top)
        for (lo_a, hi_a), (lo_b, _) in zip(spans, spans[1:]):
            self.assertLessEqual(hi_a, lo_b, f"{lo_a:#x}..{hi_a:#x} overlaps {lo_b:#x}")

    def test_breakdown_thread_and_interrupt_order_never_overlaps(self):
        # Two game threads, three interrupt threads, four game threads, one more interrupt.
        ops = [("g", 0)] * 2 + [("w", 0)] * 3 + [("g", 0)] * 4 + [("w", 0)]
        base, top, _, spans, results = self.run_ops(ops)
        self.assertEqual(len(spans), 10)
        self.assert_disjoint(spans, base, top)
        # The first game threads keep the addresses they always had.
        self.assertEqual(results[:2], [0x007FFFF0, 0x0087FFF0])

    def test_any_interleaving_never_overlaps(self):
        for pattern in ("wgwgwgwg", "wwwwgggg", "gwwgwgww", "ggggwwww"):
            base, top, _, spans, _ = self.run_ops([(c, 0) for c in pattern])
            self.assert_disjoint(spans, base, top)

    def test_main_thread_keeps_the_top_of_the_region(self):
        base, top, count, spans, _ = self.run_ops([("w", 0)] * 64)
        self.assertLessEqual(max(hi for _, hi in spans), top + 16 - 2 * 1024 * 1024)

    def test_game_stack_needs_two_adjacent_free_slices(self):
        count = self.run_ops([])[2]
        top_of = lambda slot: 0x00780000 + WORKER * (slot + 1) - 16
        # Fill all but the last two slices, then free slices 1 and 3: two free
        # slices, but not adjacent. The game stack must take the free pair at the end.
        ops = [("w", 0)] * (count - 2) + [("f", 1), ("f", 3), ("g", 0)]
        # Then free slice 2 as well: 1-2-3 is a free run, and 1-2 is the first fit.
        ops += [("f", 2), ("g", 0)]
        base, top, _, spans, results = self.run_ops(ops)
        self.assertEqual(results[-2], top_of(count - 1))
        self.assertEqual(results[-1], top_of(2))

    def test_exhaustion_is_reported(self):
        base, top, count, spans, results = self.run_ops([("w", 0)] * 40 + [("g", 0)])
        self.assertIn(-1, results)
        self.assertEqual(results[-1], 0)



if __name__ == "__main__":
    unittest.main()
