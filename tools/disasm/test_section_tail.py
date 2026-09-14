"""
Self-check for code that runs into a section's zero-filled virtual tail.

Run: py -3 tools/disasm/test_section_tail.py

Linkers trim trailing zeros off a section's file data; the Xbox loader maps
virtual_size bytes and zero-fills the rest. Breakdown's D3DX section ends on
disk at `c9 c2 10` -- the `00` of `ret 10h` is in the tail. Reading only the
file bytes lost that ret: sub_001D0DB9 ended at `leave`, never popped its
return address and 16 bytes of arguments, and the level load aborted when
sub_001A20A0 checked its /GS cookie 20 bytes off.
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.abi_analysis.xbe_min import XbeFile  # noqa: E402
from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.loader import BinaryImage, SectionInfo  # noqa: E402

VA = 0x001D0B80
# push ebp; mov ebp, esp; leave; ret 10h -- with the final 00 trimmed.
CODE = b"\x55\x8b\xec\xc9\xc2\x10"


def _image(raw, vsize):
    sec = SectionInfo(name="D3DX", virtual_addr=VA, virtual_size=vsize,
                      raw_addr=0, raw_size=len(raw), writable=False,
                      executable=True, flags="")
    return BinaryImage(filepath="", raw_data=raw, base_address=VA,
                       image_size=vsize, entry_point=VA, kernel_thunk_addr=0,
                       sections=[sec])


def test_sweep_decodes_the_ret_split_by_the_trim():
    image = _image(CODE, 0x40)
    engine = DisasmEngine(image)
    engine.linear_sweep(image.sections[0])
    ret = engine.instructions.get(VA + 4)
    assert ret is not None and ret.mnemonic == "ret", engine.instructions
    assert ret.size == 3, ret


def test_section_data_pads_one_instruction_not_the_whole_tail():
    image = _image(CODE, 0x1000)
    assert image.get_section_data(image.sections[0]) == CODE + b"\x00" * 15
    image = _image(CODE, len(CODE) + 1)
    assert image.get_section_data(image.sections[0]) == CODE + b"\x00"
    image = _image(CODE, len(CODE))
    assert image.get_section_data(image.sections[0]) == CODE


def _xbe(raw, vsize):
    """An XBE with one section whose file data is `raw`."""
    base = 0x00010000
    hdr = bytearray(0x200)
    hdr[0:4] = b"XBEH"
    struct.pack_into("<I", hdr, 0x104, base)
    struct.pack_into("<II", hdr, 0x11C, 1, base + 0x180)
    struct.pack_into("<IIIIII", hdr, 0x180, 7, VA, vsize, len(hdr), len(raw),
                     base + 0x1C0)
    hdr[0x1C0:0x1C5] = b"D3DX\0"
    f = tempfile.NamedTemporaryFile(suffix=".xbe", delete=False)
    f.write(bytes(hdr) + raw)
    f.close()
    return f.name


def test_abi_reader_zero_fills_into_the_tail():
    path = _xbe(CODE, 0x40)
    try:
        xbe = XbeFile(path)
        assert xbe.read_bytes_at_va(VA, 7) == CODE + b"\x00"
        assert xbe.read_bytes_at_va(VA + 4, 0x100) == \
            b"\xc2\x10" + b"\x00" * (0x40 - 6), "capped at virtual_size"
        assert xbe.read_bytes_at_va(VA + 8, 4) is None, "no file bytes at all"
    finally:
        os.unlink(path)


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
    print("section tail: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
