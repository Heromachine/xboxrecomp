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
#   S  0x10070  mov eax, [esp+4]
#        10074  jmp [eax*4 + 0x10090]  <- a switch through a table in .text
#        1007B  ret
#   S2 0x10080  mov eax, [esp+4]
#        10084  jmp [eax*4 + 0x20000]  <- a "table" in .rdata: not a switch
#        1008B  ret
#   F5 0x10090  mov eax, 0x10070       -> S
#        10095  mov ecx, 0x10080       -> S2
#        1009A  ret
#   T  0x100A0  jne 0x100A7            <- a later case, past the jump out
#        100A2  jmp 0x10000            <- a tail call on one path only
#        100A7  ret
#   L  0x100B0  inc eax x70, ret       <- 71 instructions, after int3 padding
#   L2 0x10110  inc eax x70, ret       <- the same, after an inc eax
#   F6 0x10158  mov eax, 0x100B0       -> L
#        1015D  mov ecx, 0x10110       -> L2
#        10162  ret
#   J  0x101A0  mov eax, [esp+4]
#        101A4  jmp [eax*4 + 0x101C0]  <- a switch whose table follows its arms
#        101AB  ret                    <- arm 0
#        101B8  xor eax, eax; ret      <- arm 1
#        101C0  dd 101ABh, 101B8h      <- `00 b8 ...` sweeps out of phase
#   K  0x101C8  mov eax, [esp+4]; ret  <- starts on the byte after the table
F1 = (b"\x56" + b"\xb8" + _u32(0x10030) + b"\xb9" + _u32(0x10003)
      + b"\xba" + _u32(0x20000) + b"\xbb" + _u32(0x10050)
      + b"\xbe" + _u32(0x1002C) + b"\xbf" + _u32(0x30000)
      + b"\x68" + _u32(0x10043) + b"\x58\x5e\xc3" + b"\x33\xc0\xc3")
G = b"\x33\xc0\xeb\x01\x40\xc3"
F2 = b"\x8b\xc1\xc3" + b"\x00\x00\xc3"
H = b"\xb9" + _u32(0x20000) + b"\xe9" + _u32((0x10040 - 0x1005A) & 0xFFFFFFFF)
F3 = b"\x33\xc0\xc3"
S = b"\x8b\x44\x24\x04" + b"\xff\x24\x85" + _u32(0x10090) + b"\xc3"
S2 = b"\x8b\x44\x24\x04" + b"\xff\x24\x85" + _u32(0x20000) + b"\xc3"
F5 = b"\xb8" + _u32(0x10070) + b"\xb9" + _u32(0x10080) + b"\xc3"
T = b"\x75\x05" + b"\xe9" + _u32((0x10000 - 0x100A7) & 0xFFFFFFFF) + b"\xc3"
LONG = b"\x40" * 70 + b"\xc3"
F6 = b"\xb8" + _u32(0x100B0) + b"\xb9" + _u32(0x10110) + b"\xc3"
J = (b"\x8b\x44\x24\x04" + b"\xff\x24\x85" + _u32(0x101C0) + b"\xc3"
     + b"\xcc" * 12 + b"\x33\xc0\xc3" + b"\xcc" * 5
     + _u32(0x101AB) + _u32(0x101B8))
K = b"\x8b\x44\x24\x04\xc3"

FUNCTIONS = ((0x10000, 0x1002A), (0x10040, 0x10043), (0x10060, 0x10063),
             (0x10090, 0x1009B), (0x10158, 0x10163), (0x101A0, 0x101C0))


def _layout():
    text = bytearray(b"\xcc" * 0x1D0)
    for va, code in ((0x10000, F1), (0x10030, G), (0x10040, F2),
                     (0x10050, H), (0x10060, F3), (0x10070, S),
                     (0x10080, S2), (0x10090, F5), (0x100A0, T),
                     (0x100B0, LONG), (0x1010F, b"\x40" + LONG),
                     (0x10158, F6), (0x101A0, J), (0x101C8, K)):
        text[va - TEXT:va - TEXT + len(code)] = code
    rdata = (b"\x33\xc0\xc3\x00"   # disassembles to a returning body
             + _u32(0x10001)       # inside F1, straight after `push esi`
             + _u32(0x10002)       # inside F1, mid-instruction
             + _u32(0x10050)       # H, the gap thunk
             + _u32(0x1002C)       # int3 padding in the gap before G
             + _u32(0x10040)       # F2's own start
             + _u32(0x10027)       # inside F1, the block after its ret
             + _u32(0x30000)       # DSP
             + _u32(0x100B0)       # L, long, after padding
             + _u32(0x10110)       # L2, long, after an ordinary instruction
             + _u32(0x101C8))      # K, straight after J's switch table
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
        0x10080: "an indexed jump through a table in another section",
        0x10110: "a ret past 64 instructions with no boundary in front",
    }
    for addr, why in refused.items():
        assert addr not in det._candidates, f"{addr:#x}: {why}"
    assert set(det._candidates) == {0x10030, 0x10070, 0x100B0}, det._candidates


def test_returning_probe_follows_forward_jumps_only():
    det, _ = _detector()
    assert det.engine.probes_as_returning_body(0x10030), \
        "a forward jmp over one instruction must not end the probe"
    assert not det.engine.probes_as_returning_body(0x10050), \
        "a backward jmp is tail-call shaped and must end the probe"


def test_returning_probe_continues_past_a_jump_out_an_earlier_branch_skips():
    det, _ = _detector()
    assert det.engine.probes_as_returning_body(0x100A0), \
        "jne past a tail call leaves the ret after it reachable"


def test_returning_probe_steps_over_a_switch():
    det, _ = _detector()
    assert det.engine.probes_as_returning_body(0x10070), \
        "jmp [reg*4 + table in the same section] is a switch, not an exit"
    assert not det.engine.probes_as_returning_body(0x10080), \
        "a table in another section is not a switch this probe trusts"


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
        0x10110: "a long body with no function boundary in front of it",
    }
    for addr, why in refused.items():
        assert addr not in det._alias_entries, f"{addr:#x}: {why}"
    assert set(det._alias_entries) == {0x10027, 0x10050, 0x100B0, 0x101C8}, \
        det._alias_entries


def test_data_pointer_to_a_long_body_after_padding_is_taken():
    det, sections = _detector()
    det._pass_data_ptr_targets(sections)
    assert 0x100B0 in det._alias_entries, det._alias_entries


def test_data_pointer_after_a_switch_table_realigns_the_sweep():
    det, sections = _detector()
    assert 0x101C8 not in det.engine.instructions, \
        "the table must leave the sweep out of phase for this to test anything"
    assert det._follows_jump_table(0x101C8)
    assert not det._follows_jump_table(0x101C4), "mid-table"
    det._pass_data_ptr_targets(sections)
    assert det._alias_entries.get(0x101C8) == sections[0].virtual_addr \
        + sections[0].virtual_size, det._alias_entries


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
