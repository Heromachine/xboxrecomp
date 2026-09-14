"""
Self-check for lifting a compiled switch whose table is followed by look-alike
data.

Run: py -3 tools/recomp/test_lifter_switch_table.py

A switch lifts to real gotos only for table entries inside the function.
Entries used to be read until a word was not a code address, and one outside
the function rejected the whole switch. But MSVC parks the byte index table of
a two-level switch right after the pointer table, and index bytes 00 00 03 00
read as 0x00030000, a perfectly good .text address. Breakdown's sub_000411D0
lost its second switch that way: every case went through RECOMP_ITAIL, which
found no function at the in-function case 0x00041276 and returned 0.

The table addresses and values below match that function; they are addresses,
not code, and the image around them is filler.
"""

import os
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.disasm import Operand  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402

TEXT_VA = 0x00011000
TEXT_RAW = 0x00001000
TEXT_SIZE = 0x00100000          # covers 0x00030000 and 0x00041000 alike

FUNC_START, FUNC_END = 0x000411D0, 0x0004127D

TABLE1 = 0x00041280             # 3 cases, then a 7-byte index table
TABLE1_CASES = [0x00041212, 0x00041219, 0x0004127B]
INDEX1 = bytes([0x00, 0x02, 0x01, 0x00, 0x02, 0x00, 0x00])
TABLE2 = 0x00041294             # 4 cases, then index bytes 00 00 03 00 ...
TABLE2_CASES = [0x0004127B, 0x00041260, 0x00041259, 0x00041276]
INDEX2 = bytes([0x00, 0x00, 0x03, 0x00, 0x01, 0x03, 0x02, 0x02, 0x02, 0x02, 0x02])
OUTSIDE = 0x00041500            # a table that starts outside the function
INSIDE_THEN_DATA = 0x00041600   # in-function cases, then a word below .text

_INSTALL_STATE = ("_SECTIONS", "SECTIONS", "_configured_from", "TEXT_VA_START",
                  "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
                  "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR",
                  "ENTRY_POINT")


def _put(buf, va, blob):
    off = va - TEXT_VA + TEXT_RAW
    buf[off:off + len(blob)] = blob


def _words(values):
    return b"".join(struct.pack("<I", v) for v in values)


def _lifter():
    buf = bytearray(TEXT_RAW + TEXT_SIZE)
    _put(buf, TABLE1, _words(TABLE1_CASES) + INDEX1)
    _put(buf, TABLE2, _words(TABLE2_CASES) + INDEX2)
    _put(buf, OUTSIDE, _words([0x00030000, FUNC_START + 0x10]))
    _put(buf, INSIDE_THEN_DATA, _words([0x00041212, 0x00041219, 0x00010000]))
    lifter = Lifter(xbe_data=bytes(buf))
    lifter.func_start, lifter.func_end = FUNC_START, FUNC_END
    lifter.jump_table_targets = {}
    return lifter


def _op(table):
    """`jmp dword ptr [edx*4 + table]`"""
    return [Operand(type="mem", mem_index="edx", mem_scale=4, mem_disp=table,
                    mem_size=4)]


class _Layout:
    """Install a one-section layout, restoring the module's own afterwards."""

    def __enter__(self):
        self.saved = {k: getattr(config, k) for k in _INSTALL_STATE}
        config._install(
            [config.Section(".text", TEXT_VA, TEXT_SIZE, TEXT_RAW, TEXT_SIZE, True)],
            entry_point=TEXT_VA, kernel_thunk_addr=0, origin="switch-table-test")

    def __exit__(self, *exc):
        for k, v in self.saved.items():
            setattr(config, k, v)


def test_index_bytes_after_the_table_do_not_reject_the_switch():
    with _Layout():
        targets = _lifter()._analyze_switch_table(_op(TABLE2))
    assert targets == TABLE2_CASES, [hex(t) for t in targets]


def test_single_level_table_is_unchanged():
    with _Layout():
        targets = _lifter()._analyze_switch_table(_op(TABLE1))
    assert targets == TABLE1_CASES, [hex(t) for t in targets]


def test_table_that_starts_outside_the_function_is_not_a_switch():
    with _Layout():
        assert _lifter()._analyze_switch_table(_op(OUTSIDE)) == []


def test_table_ending_at_a_non_code_word_keeps_every_case():
    with _Layout():
        targets = _lifter()._analyze_switch_table(_op(INSIDE_THEN_DATA))
    assert targets == [0x00041212, 0x00041219], [hex(t) for t in targets]


def test_lifted_switch_jumps_to_each_case_and_keeps_its_fallback():
    with _Layout():
        lifter = _lifter()
        insn = SimpleNamespace(jump_target=None, address=0x00041252)
        lines = lifter._lift_jmp(insn, _op(TABLE2))
    text = "\n".join(lines)
    for case in TABLE2_CASES:
        assert f"goto loc_{case:08X};" in text, text
    assert "RECOMP_ITAIL(_jt)" in text, text
    assert "indirect tail jmp" not in text, text


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}: {exc}")
    print("switch tables: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
