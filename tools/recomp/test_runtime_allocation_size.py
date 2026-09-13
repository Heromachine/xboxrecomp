"""Execute the real contiguous allocator and MmQueryAllocationSize's size lookup.

Regression guard: MmQueryAllocationSize answered with VirtualQuery's RegionSize,
which for the contiguous window is the distance to the end of the whole
mapping. Breakdown's audio allocator zeroes that many bytes after allocating,
so each new block wiped every block above it. The size the runtime reports
must stay inside the block it describes.
"""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGE = 4096

# (size, alignment) of Breakdown's contiguous allocations during audio init,
# in order, as logged by [CONTIG].
BREAKDOWN_SEQUENCE = [
    (8192, 4096), (24576, 4096), (16, 4096), (32768, 32768), (16416, 16384),
    (16384, 16384), (65536, 16384), (8192, 16384), (8192, 16384), (16, 16384),
    (48, 16384), (4096, 16384), (49152, 16384), (4192, 16384), (77824, 16384),
    (152, 16384),
]


def _extract(source, pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise AssertionError(f"not found in xbox_memory_layout.c: {pattern}")
    return match.group(0)


class AllocationSizeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("C compiler required")
        source = (ROOT / "src/kernel/xbox_memory_layout.c").read_text()
        pieces = [
            _extract(source, r"^#define XBOX_HEAP_MAX_BLOCKS .*?g_heap_block_count = 0;"),
            _extract(source, r"^#define XBOX_CONTIG_FLOOR .*?^#define XBOX_CONTIG_TOP .*?$"),
            _extract(source, r"^static uint32_t g_contig_next = .*?g_contig_block_count = 0;"),
            _extract(source, r"^uint32_t xbox_ContigAlloc\(uint32_t size, uint32_t alignment\)\n\{.*?^\}"),
            _extract(source, r"^uint32_t xbox_AllocationSize\(uint32_t xbox_va\)\n\{.*?^\}"),
        ]
        harness = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE (64u * 1024u * 1024u)
static ptrdiff_t g_memory_offset;
static void *g_contig_memory;
static uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{ (void)size; (void)alignment; return 0; }
""" + "\n".join(pieces) + r"""
int main(void) {
    unsigned size, align;
    g_contig_memory = malloc(XBOX_CONTIG_SIZE);
    if (!g_contig_memory) return 2;
    g_memory_offset = (ptrdiff_t)((uintptr_t)g_contig_memory - XBOX_CONTIG_BASE);
    /* One heap block, to check the exact-size branch. */
    g_heap_blocks[0].addr = 0x00F80000u; g_heap_blocks[0].size = 20; g_heap_blocks[0].free = 0;
    g_heap_block_count = 1;
    while (scanf("%u %u", &size, &align) == 2) {
        uint32_t va = xbox_ContigAlloc(size, align);
        printf("%u %u\n", va, xbox_AllocationSize(va));
    }
    printf("heap %u unknown %u\n", xbox_AllocationSize(0x00F80000u),
           xbox_AllocationSize(0x80001234u));
    g_heap_blocks[0].free = 1;
    printf("freed %u\n", xbox_AllocationSize(0x00F80000u));
    return 0;
}
"""
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path = pathlib.Path(cls.tmp.name)
        (path / "alloc.c").write_text(harness)
        cls.binary = path / "alloc"
        subprocess.run([cc, "-std=c11", "-Wall", "-Werror", str(path / "alloc.c"),
                        "-o", str(cls.binary)], check=True)
        run = subprocess.run([str(cls.binary)], text=True, check=True, capture_output=True,
                             input="".join(f"{s} {a}\n" for s, a in BREAKDOWN_SEQUENCE))
        cls.lines = run.stdout.splitlines()

    def test_contiguous_sizes_are_whole_pages(self):
        for (req, _), line in zip(BREAKDOWN_SEQUENCE, self.lines):
            va, size = map(int, line.split())
            self.assertNotEqual(va, 0)
            self.assertEqual(size, (max(req, 16) + PAGE - 1) // PAGE * PAGE, line)

    def test_reported_sizes_never_overlap_another_block(self):
        spans = []
        for line in self.lines[:len(BREAKDOWN_SEQUENCE)]:
            va, size = map(int, line.split())
            spans.append((va, va + size))
        spans.sort()
        for (lo_a, hi_a), (lo_b, _) in zip(spans, spans[1:]):
            self.assertLessEqual(hi_a, lo_b, f"{lo_a:#x}..{hi_a:#x} runs into {lo_b:#x}")

    def test_heap_blocks_and_unknown_addresses(self):
        self.assertEqual(self.lines[len(BREAKDOWN_SEQUENCE)], "heap 20 unknown 0")
        self.assertEqual(self.lines[len(BREAKDOWN_SEQUENCE) + 1], "freed 0")


if __name__ == "__main__":
    unittest.main()
