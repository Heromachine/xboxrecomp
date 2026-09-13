"""
Self-check for recovering functions whose address is only ever taken.

Run: py -3 tools/disasm/test_addr_taken.py

A function installed by storing its address -- `mov [eax+20h], 1B900h`, or a
vtable slot in .rdata -- and later reached only through an indirect call is
invisible to every pass that follows calls and jumps. Breakdown lost its whole
front end to one: after the opening movie the game switched its update
callback to 0x0001B900, the recompiler had no function there, and every frame
logged "[ICALL] Failed to resolve VA" and returned 0.

_pass_imm_ref_targets promotes immediates to function starts, but only where
nothing already covers the target and the bytes reach a ret.
_pass_data_ptr_targets turns pointers held in data sections into alias entries.
These cases pin down which addresses each accepts and which it refuses.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import Function, FunctionDetector  # noqa: E402


class _Section:
    def __init__(self, name, va, data):
        self.name = name
        self.virtual_addr = va
        self.virtual_size = len(data)
        self.raw_size = len(data)
        self.executable = True   # XBEs mark .rdata executable too
        self.data = data


class _Image:
    """Minimal duck-typed BinaryImage."""
    def __init__(self, *sections):
        self.sections = list(sections)
        self.base_address = min(s.virtual_addr for s in sections)
        self.image_size = max(s.virtual_addr + s.virtual_size
                              for s in sections) - self.base_address

    def get_section_at_va(self, va):
        for s in self.sections:
            if s.virtual_addr <= va < s.virtual_addr + s.virtual_size:
                return s
        return None

    def get_section_data(self, section):
        return section.data


def _u32(v):
    return v.to_bytes(4, "little")


TEXT = 0x00010000
RDATA = 0x00020000
DSP = 0x00030000      # marked executable, but no function was ever found in it

# .text: four detected functions and the undetected code around them.
#   F1 0x10000  push esi
#        10001  mov eax, 0x10030   -> G, a gap function that returns
#        10006  mov ecx, 0x10003   -> the middle of F1's own mov
#        1000B  mov edx, 0x20000   -> .rdata, which decodes but is not code
#        10010  mov ebx, 0x10050   -> H, a gap thunk that tail-jumps back
#        10015  mov esi, 0x1002C   -> int3 padding in front of G
#        1001A  mov edi, 0x30000   -> DSP, a section with no functions
#        1001F  push 0x10043       -> zero fill after F2
#        10024  pop eax
#        10025  pop esi
#        10026  ret
#        10027  xor eax, eax       <- a block nothing falls into, as an SEH
#        10029  ret                   handler after its function's ret is
#   G  0x10030  xor eax, eax
#        10032  jmp 0x10035        <- forward, inside the window: keep going
#        10034  inc eax
#        10035  ret
#   F2 0x10040  mov eax, ecx
#        10042  ret
#        10043  add [eax], al      <- 00 00 zero fill, then a stray c3
#   H  0x10050  mov ecx, 0x20000
#        10055  jmp 0x10040        <- tail jump, no ret of its own
#   F3 0x10060  xor eax, eax
#        10062  ret
F1 = (b"\x56" + b"\xb8" + _u32(0x10030) + b"\xb9" + _u32(0x10003)
      + b"\xba" + _u32(0x20000) + b"\xbb" + _u32(0x10050)
      + b"\xbe" + _u32(0x1002C) + b"\xbf" + _u32(0x30000)
      + b"\x68" + _u32(0x10043) + b"\x58\x5e\xc3" + b"\x33\xc0\xc3")
G = b"\x33\xc0\xeb\x01\x40\xc3"
F2 = b"\x8b\xc1\xc3" + b"\x00\x00\xc3"
H = b"\xb9" + _u32(0x20000) + b"\xe9" + _u32((0x10040 - 0x1005A) & 0xFFFFFFFF)
F3 = b"\x33\xc0\xc3"

FUNCTIONS = ((0x10000, 0x1002A), (0x10040, 0x10043), (0x10060, 0x10063))


def _layout():
    text = bytearray(b"\xcc" * 0x70)
    for va, code in ((0x10000, F1), (0x10030, G), (0x10040, F2),
                     (0x10050, H), (0x10060, F3)):
        text[va - TEXT:va - TEXT + len(code)] = code
    rdata = (b"\x33\xc0\xc3\x00"   # disassembles to a returning body
             + _u32(0x10001)       # inside F1, straight after `push esi`
             + _u32(0x10002)       # inside F1, mid-instruction
             + _u32(0x10050)       # H, the gap thunk
             + _u32(0x1002C)       # int3 padding in the gap before G
             + _u32(0x10040)       # F2's own start
             + _u32(0x10027)       # inside F1, the block after its ret
             + _u32(0x30000))      # DSP
    return (_Section(".text", TEXT, bytes(text)),
            _Section(".rdata", RDATA, rdata),
            _Section("DSP", DSP, b"\x33\xc0\xc3"))


def _detector():
    text, rdata, dsp = _layout()
    image = _Image(text, rdata, dsp)
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    engine.linear_sweep(dsp)
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = engine
    det.image = image
    det._candidates = {}
    det._alias_entries = {}
    det.functions = {
        start: Function(start=start, end=end, name=f"sub_{start:08X}")
        for start, end in FUNCTIONS
    }
    # DSP is handed over as a section to analyse, like any executable one.
    return det, [text, dsp]


def test_immediate_into_a_gap_that_returns_becomes_a_function():
    det, sections = _detector()
    assert det._pass_imm_ref_targets(sections)
    assert det._candidates.get(0x10030, (0, ""))[1] == "imm_ref_target", \
        det._candidates


def test_immediate_refusals():
    det, sections = _detector()
    det._pass_imm_ref_targets(sections)
    refused = {
        0x10003: "inside F1 -- a start there would truncate it",
        0x20000: ".rdata decodes to a ret but is not analysed as code",
        0x10050: "H only leaves through a tail jump",
        0x1002C: "int3 padding, which walks into G's ret",
        0x10043: "zero fill",
        0x30000: "a section no stronger pass found code in",
    }
    for addr, why in refused.items():
        assert addr not in det._candidates, f"{addr:#x}: {why}"
    assert set(det._candidates) == {0x10030}, det._candidates


def test_returning_probe_follows_forward_jumps_only():
    det, _ = _detector()
    assert det.engine.probes_as_returning_body(0x10030), \
        "a forward jmp over one instruction must not end the probe"
    assert not det.engine.probes_as_returning_body(0x10050), \
        "a backward jmp is tail-call shaped and must end the probe"


def test_data_pointer_to_a_block_after_a_ret_aliases_its_body():
    det, sections = _detector()
    det._pass_data_ptr_targets(sections)
    assert det._alias_entries.get(0x10027) == 0x1002A, det._alias_entries


def test_data_pointer_into_a_gap_thunk_runs_to_the_next_function():
    det, sections = _detector()
    det._pass_data_ptr_targets(sections)
    assert det._alias_entries.get(0x10050) == 0x10060, det._alias_entries


def test_data_pointer_refusals():
    det, sections = _detector()
    det._pass_data_ptr_targets(sections)
    refused = {
        0x10001: "a boundary mid-function that code falls into",
        0x10002: "mid-instruction",
        0x1002C: "int3 padding",
        0x10040: "already a function start",
        0x30000: "a section no stronger pass found code in",
        0x10030: "an immediate in code, not a data-section value",
    }
    for addr, why in refused.items():
        assert addr not in det._alias_entries, f"{addr:#x}: {why}"
    assert set(det._alias_entries) == {0x10027, 0x10050}, det._alias_entries


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
    print("address-taken functions: "
          + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
