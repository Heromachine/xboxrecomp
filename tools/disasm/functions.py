"""
Function boundary detection for the disassembler.

Implements multi-pass function detection with confidence scoring:
1. Known addresses (entry point)
2. Standard prologues (push ebp; mov ebp, esp)
3. CC padding boundaries (CC run after ret)
4. Call targets (destinations of call instructions)
5. Cross-validation and overlap resolution
"""

import bisect
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from . import config
from .engine import DisasmEngine, Instruction
from .loader import BinaryImage, SectionInfo
from .xrefs import XRefTracker
from .labels import LabelManager, Label, LabelType


@dataclass
class Function:
    """A detected function with boundaries and metadata."""
    start: int
    end: int           # Address after last instruction
    name: str
    section: str = ""
    confidence: float = 0.0
    detection_method: str = ""

    # Call graph data
    calls_to: List[int] = field(default_factory=list)     # Functions this calls
    called_by: List[int] = field(default_factory=list)     # Functions that call this

    # Instruction stats
    num_instructions: int = 0
    has_prologue: bool = False

    @property
    def size(self) -> int:
        return self.end - self.start

    def to_dict(self) -> dict:
        return {
            "start": f"0x{self.start:08X}",
            "end": f"0x{self.end:08X}",
            "size": self.size,
            "name": self.name,
            "section": self.section,
            "confidence": self.confidence,
            "detection_method": self.detection_method,
            "num_instructions": self.num_instructions,
            "has_prologue": self.has_prologue,
            "calls_to": [f"0x{a:08X}" for a in self.calls_to],
            "called_by": [f"0x{a:08X}" for a in self.called_by],
        }


class FunctionDetector:
    """
    Multi-pass function boundary detector.

    Identifies function start addresses through multiple heuristics,
    then determines function boundaries by following instruction flow
    until the next function or a terminal instruction.
    """

    def __init__(self, engine: DisasmEngine, image: BinaryImage,
                 xrefs: XRefTracker, labels: LabelManager):
        self.engine = engine
        self.image = image
        self.xrefs = xrefs
        self.labels = labels

        # Candidate function starts: address -> (confidence, method)
        self._candidates: Dict[int, Tuple[float, str]] = {}

        # Final function list
        self.functions: Dict[int, Function] = {}

        # Tail-jump targets landing inside another function: addr -> that
        # function's end. Kept out of self._candidates so they cannot truncate
        # the function they land in.
        self._alias_entries: Dict[int, int] = {}

    def detect_all(self, sections: Optional[List[SectionInfo]] = None) -> int:
        """
        Run all detection passes and build the function database.

        Args:
            sections: Sections to analyze. If None, uses all executable sections.

        Returns:
            Number of functions detected.
        """
        if sections is None:
            sections = self.image.get_code_sections()

        # Pass 1: Known addresses
        self._pass_known_addresses()

        # Pass 2: Prologue patterns
        for sec in sections:
            self._pass_prologues(sec)

        # Pass 3: CC padding boundaries
        for sec in sections:
            self._pass_cc_boundaries(sec)

        # Pass 4: Call targets
        self._pass_call_targets(sections)

        # Pass 5: Build functions from candidates
        self._build_functions(sections)

        # Pass 6: Tail-jump targets. A function reached only by "jmp" and never
        # by "call" is invisible to every pass above, so it is emitted as a stub
        # that returns without unwinding the frame its jumping caller built --
        # silently corrupting the simulated stack for everything upstream. Halo
        # had 127 of these; one of them (the CRT two-arg error handler) leaked
        # 0x24 bytes per call and turned a 6-iteration init loop into 21,938
        # allocations that exhausted the heap.
        #
        # Needs the bodies from pass 5 to tell a tail jump from an ordinary
        # intra-function branch, so it runs after and rebuilds. Iterate: a newly
        # found function can itself tail-jump somewhere new.
        for _round in range(8):
            before = len(self._candidates)
            if not self._pass_tail_jump_targets(sections):
                break
            print(f"  tail-jump pass {_round}: "
                  f"+{len(self._candidates) - before} standalone, "
                  f"{len(self._alias_entries)} aliases")
            self.functions.clear()
            self._build_functions(sections)

        # Functions whose address is only ever taken as an immediate. Runs
        # once, after the bodies exist: the test is whether the target lands in
        # a gap, which needs the gaps to be known. One rebuild picks up what it
        # finds. A start in a gap cannot move an existing function's end: had
        # that function reached past the start, the start would not be in a gap.
        if self._pass_imm_ref_targets(sections):
            self.functions.clear()
            self._build_functions(sections)

        # Then addresses that only ever exist as table entries. After the
        # immediate pass, so its results narrow the gaps first. These become
        # aliases rather than function starts, so no rebuild: aliases are
        # materialised by _build_alias_entries once boundaries are final.
        self._pass_data_ptr_targets(sections)

        self._build_alias_entries()

        # Populate call graph
        self._build_call_graph()

        return len(self.functions)

    def _add_candidate(self, addr: int, confidence: float, method: str) -> None:
        """Add a function start candidate, keeping highest confidence."""
        existing = self._candidates.get(addr)
        if existing is None or confidence > existing[0]:
            self._candidates[addr] = (confidence, method)

    def _pass_known_addresses(self) -> None:
        """Pass 1: Add known function addresses."""
        # Entry point
        self._add_candidate(
            self.image.entry_point,
            config.CONFIDENCE_KNOWN,
            "entry_point"
        )

    def _pass_prologues(self, section: SectionInfo) -> None:
        """
        Pass 2: Scan for standard function prologues.

        Looks for: push ebp (0x55); mov ebp, esp (0x8BEC or 0x89E5)
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0
        while i < len(data) - 2:
            # Check for push ebp; mov ebp, esp
            if data[i] == 0x55:
                if (i + 2 < len(data) and
                        data[i + 1] == 0x8B and data[i + 2] == 0xEC):
                    addr = va_start + i
                    # Verify this address has a decoded instruction
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue"
                        )
                    i += 3
                    continue
                elif (i + 2 < len(data) and
                      data[i + 1] == 0x89 and data[i + 2] == 0xE5):
                    addr = va_start + i
                    if addr in self.engine.instructions:
                        self._add_candidate(
                            addr,
                            config.CONFIDENCE_PROLOGUE,
                            "prologue_alt"
                        )
                    i += 3
                    continue
            i += 1

    def _pass_cc_boundaries(self, section: SectionInfo) -> None:
        """
        Pass 3: Find function boundaries at CC padding.

        Pattern: ret instruction, followed by one or more 0xCC bytes,
        followed by the start of the next function.
        """
        data = self.image.get_section_data(section)
        if not data:
            return

        va_start = section.virtual_addr
        i = 0

        while i < len(data):
            # Look for CC padding runs
            if data[i] == config.CC_PADDING:
                cc_start = i
                while i < len(data) and data[i] == config.CC_PADDING:
                    i += 1

                cc_run_length = i - cc_start

                if cc_run_length >= config.MIN_CC_RUN and i < len(data):
                    # Check if instruction before CC run was a ret
                    before_addr = va_start + cc_start
                    # Look for a ret instruction ending right at the CC run
                    found_ret = False
                    for check_offset in range(1, 4):  # ret can be 1-3 bytes
                        check_addr = before_addr - check_offset
                        insn = self.engine.get_instruction(check_addr)
                        if insn and insn.is_ret and insn.end_address == before_addr:
                            found_ret = True
                            break

                    if found_ret:
                        next_addr = va_start + i
                        if next_addr in self.engine.instructions:
                            self._add_candidate(
                                next_addr,
                                config.CONFIDENCE_CC_BOUNDARY,
                                "cc_boundary"
                            )
            else:
                i += 1

    def _pass_call_targets(self, sections: List[SectionInfo]) -> None:
        """
        Pass 4: Add destinations of direct call instructions as function starts.
        """
        # Build set of valid code ranges
        code_ranges = set()
        for sec in sections:
            for addr in range(sec.virtual_addr,
                              sec.virtual_addr + sec.virtual_size):
                code_ranges.add(addr)

        call_targets = self.engine.get_call_targets()
        # How many distinct call sites name each target: the corroboration the
        # alignment rule below is standing in for.
        call_sites: Dict[int, int] = {}
        for insn in self.engine.instructions.values():
            if insn.call_target is not None:
                call_sites[insn.call_target] = \
                    call_sites.get(insn.call_target, 0) + 1

        realigned = unaligned = 0
        for target in call_targets:
            section = self.image.get_section_at_va(target)
            if not (section and section.executable):
                continue
            # A direct call to an executable address is the strongest evidence
            # of a function start there is -- stronger than a prologue match,
            # which is a guess about bytes. It used to be discarded whenever the
            # linear sweep had stepped over that exact address, which happens
            # wherever the sweep passes through data and comes out of phase.
            # Decode there instead of dropping the candidate; see
            # Engine.decode_at. On Halo 2276 this is 46 functions that were
            # becoming `g_esp += 4` no-op stubs.
            if target not in self.engine.instructions:
                # Manufacturing an instruction here is creating evidence, not
                # reading it, so require corroboration. A call operand decoded
                # out of data produces a plausible-looking in-section address
                # that is almost never aligned; a real MSVC function start
                # almost always is. Targets that already decoded are untouched,
                # whatever their alignment -- that is pre-existing behaviour.
                if (target % config.CALL_TARGET_REALIGN_ALIGNMENT
                        and call_sites.get(target, 0)
                        < config.CALL_TARGET_UNALIGNED_MIN_SITES):
                    unaligned += 1
                    continue
                if self.engine.decode_at(target):
                    realigned += 1
                else:
                    continue  # genuinely undecodable: not code
            self._add_candidate(
                target,
                config.CONFIDENCE_CALL_TARGET,
                "call_target"
            )
        if realigned or unaligned:
            print(f"  Realigned {realigned} call targets the sweep stepped over"
                  f" ({unaligned} rejected as unaligned)")

    def _pass_imm_ref_targets(self, sections: List[SectionInfo]) -> bool:
        """
        An immediate that points into unclaimed code and decodes to a ret is a
        function whose address was taken.

        C++ and callback-driven C install a function by writing its address
        somewhere -- `mov dword ptr [eax+20h], 1B900h` -- and call it later as
        `call dword ptr [reg+N]`. Nothing ever names it in a call or a jump,
        so no other pass sees it, it gets no dispatch entry, and every
        indirect call to it logs "[ICALL] Failed to resolve VA" and returns 0.

        Breakdown hit four of these one after another. Once its boot sequence
        finished, the front-end object switched its update callback to
        0x0001B900, so nothing after the opening movie ran and the screen
        stayed black. 0x00113630 and 0x00129240 had been failing every frame
        since early boot. Each was found only by running the title, reading
        the failure log and adding a seed.

        The evidence is weak -- an address that merely appears as an immediate
        -- so the filter carries the weight, and the *gap* does most of the
        work. The target must land in a code section, in bytes no function
        already covers, and must reach a ret (probes_as_returning_body). A
        string or table address fails the decode; an offset into a real
        function fails the gap. A start inside a function would truncate it,
        which is worse than missing one.

        Only the sections being analysed count as code. An XBE marks .rdata
        and .data executable, and .rdata disassembles happily into
        bound/popal/arpl; upstream had a string table become a function that
        failed to compile before the pass took the caller's section list.

        Two filters go beyond upstream, both measured on Breakdown. The target
        must not begin on padding or zero fill: four immediates were constants
        that landed in int3 runs, where a probe walks the padding into the next
        function's ret. And a section only counts once some stronger pass has
        found a function in it. Breakdown's DOLBY section is marked executable
        but holds the audio DSP's firmware, and two immediates pointing at it
        would otherwise have become x86 functions.

        Distinct targets are collected before probing, since the same address
        is taken over and over.

        Ported from xboxrecomp work/v0.7.0-non-local (d89b8af, 572b8aa).
        Returns whether anything was added, so the caller can rebuild.
        """
        bounds = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bounds]

        def inside_a_function(addr: int) -> bool:
            i = bisect.bisect_right(starts, addr) - 1
            return i >= 0 and addr < bounds[i][1]

        code_ranges = self._proven_code_ranges(sections)

        def in_code_section(addr: int) -> bool:
            return any(lo <= addr < hi for lo, hi in code_ranges)

        targets = set()
        for insn in self.engine.instructions.values():
            target = insn.imm_ref
            if target is None or target in self.functions:
                continue
            if inside_a_function(target) or not in_code_section(target):
                continue
            targets.add(target)

        found = 0
        for target in sorted(targets):
            if self._starts_on_filler(target):
                continue
            if not self.engine.probes_as_returning_body(target):
                continue
            if target not in self.engine.instructions:
                if not self.engine.decode_at(target):
                    continue
            self._add_candidate(target, config.CONFIDENCE_IMM_REF,
                                "imm_ref_target")
            found += 1

        if found:
            print(f"  {found} function address(es) taken as an immediate")
        return found > 0

    def _pass_data_ptr_targets(self, sections: List[SectionInfo]) -> bool:
        """
        A code pointer stored in a data section is an entry point whose
        address is only ever taken at run time.

        _pass_imm_ref_targets catches an address that appears as an immediate
        in code. It cannot catch one that only ever exists as a *value in a
        table* -- a vtable in .rdata, a callback array, a dispatch table.
        Breakdown's seed list is mostly these: DSOUND vtable methods and
        deleting destructors, each found by running the title and reading one
        more "[ICALL] Failed to resolve VA".

        These are alias entries, not candidates, and upstream measured why.
        Registering them as starts on Half-Life 2 turned 5,305 C++ constructors
        into 5,260, one clobbered callee-saved register into 13, and shrank the
        generated code by 70%. A table scan is noisy enough to put starts where
        bodies legitimately continue. An alias is built after every boundary is
        final, so it adds an entry point without moving anyone's end.

        Inside a function the bytes are already known to be code, and the
        alias shares the enclosing end, as tail-jump aliases do. Upstream asked
        only that the address be an instruction boundary. On Breakdown that
        accepted 225 entries, and 194 of them were small integers from .data
        and .XTLID that happened to hit a boundary mid-function: 0x00020007,
        0x00030007, 0x000F0001. Real entries into a body are blocks nothing
        falls into. The CRT's SEH filters and handlers, named by .rdata scope
        tables, follow a `jmp` or `ret`; a run of merged constructor thunks
        follows the previous thunk's `jmp`. So the address must also follow a
        ret, an unconditional jmp or padding. That keeps 31 entries, 28 of them
        SEH filter and handler blocks, and drops the 194.

        In a gap nothing vouches for the bytes, so the address must be a
        boundary the sweep already recorded, must not be padding, and must
        reach a ret or a tail jump. A tail jump counts because a constructor
        thunk is `mov ecx, <this>; jmp <ctor>`.

        Ported from xboxrecomp work/v0.7.0-non-local (d9e7aaa, 1069312).
        """
        code_ranges = self._proven_code_ranges(sections)
        code_names = {sec.name for sec in sections}

        def in_code_section(addr: int) -> bool:
            return any(lo <= addr < hi for lo, hi in code_ranges)

        bounds = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bounds]

        targets = set()
        for sec in self.image.sections:
            if sec.name in code_names:
                continue                    # scan data, not code
            data = self.image.get_section_data(sec)
            if not data:
                continue
            for off in range(0, len(data) - 3, 4):
                value = int.from_bytes(data[off:off + 4], "little")
                if in_code_section(value):
                    targets.add(value)

        section_end = {sec.name: sec.virtual_addr + sec.virtual_size
                       for sec in sections}

        found = 0
        for target in sorted(targets):
            if target in self.functions or target in self._alias_entries:
                continue
            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bounds[j][0] < target < bounds[j][1]:
                if target not in self.engine.instructions:
                    continue
                if not self._follows_terminator(target):
                    continue
                end = bounds[j][1]
            else:
                if target not in self.engine.instructions:
                    continue
                first = self.engine.instructions[target]
                if first.mnemonic.lower() in ("int3", "nop"):
                    continue
                if not self.engine.probes_as_function_body(target,
                                                           max_insns=64):
                    continue
                i = bisect.bisect_right(starts, target)
                sec = self.image.get_section_at_va(target)
                end = starts[i] if i < len(starts) else section_end.get(
                    sec.name if sec else "", target + 4)
            if end <= target:
                continue
            self._alias_entries[target] = end
            found += 1

        if found:
            print(f"  {found} function address(es) found in data tables")
        return found > 0

    def _proven_code_ranges(self, sections: List[SectionInfo]):
        """Ranges of the analysed sections that already hold a function.

        The address-taken passes run on weak evidence, so they only trust a
        section some stronger pass has already found x86 code in. The
        executable bit is no help here: an XBE sets it on data and on firmware
        for other processors alike.
        """
        ranges = []
        for sec in sections:
            lo, hi = sec.virtual_addr, sec.virtual_addr + sec.virtual_size
            if any(lo <= start < hi for start in self.functions):
                ranges.append((lo, hi))
        return ranges

    _FILLER_MNEMONICS = ("int3", "nop")

    def _starts_on_filler(self, addr: int) -> bool:
        """True if `addr` begins with padding or zero fill rather than code."""
        insn = self.engine.instructions.get(addr)
        if insn is not None and insn.mnemonic.lower() in self._FILLER_MNEMONICS:
            return True
        section = self.image.get_section_at_va(addr)
        data = self.image.get_section_data(section) if section else None
        if not data:
            return False
        offset = addr - section.virtual_addr
        head = data[offset:offset + 2]
        # 00 00 decodes as `add byte ptr [eax], al`; 0xCC and 0x90 are padding
        # even where the sweep has not recorded an instruction.
        return head == b"\x00\x00" or head[:1] in (b"\xcc", b"\x90")

    def _follows_terminator(self, addr: int) -> bool:
        """True if an instruction the sweep decoded ends exactly at `addr` and
        is a ret, an unconditional jmp or padding -- nothing falls into it."""
        for back in range(1, 16):
            insn = self.engine.instructions.get(addr - back)
            if insn is None or insn.end_address != addr:
                continue
            if insn.is_ret or (insn.is_jump and not insn.is_cond_jump):
                return True
            if insn.mnemonic.lower() in self._FILLER_MNEMONICS:
                return True
        return False

    def _pass_tail_jump_targets(self, sections: List[SectionInfo]) -> bool:
        """
        Pass 6: Add the target of every jmp -- conditional or not -- that
        leaves the body of the function containing it.

        Returns True if any new candidate was added.
        """
        bodies = sorted((f.start, f.end) for f in self.functions.values())
        starts = [b[0] for b in bodies]
        added = False

        for insn in self.engine.instructions.values():
            # Conditional branches count too. The translator emits a tail call
            # for an out-of-body branch whichever kind it is -- a jne to another
            # function's code becomes "if (cond) { sub_target(); return; }" --
            # so a target reachable only that way still needs a function at it.
            # Skipping them left the target undetected and the generated call
            # resolved to an empty stub that returns immediately, silently
            # skipping real code. Breakdown reaches its CRT floating-point
            # helpers exactly this way: jne 0x1b2a30 at 0x001B2A99 lands in the
            # gap between sub_001B2930 and sub_001B2A61, and hand-written CRT
            # asm shares tails like this constantly.
            if not insn.is_branch:
                continue
            target = insn.jump_target
            if target is None or target in self._candidates:
                continue
            if target not in self.engine.instructions:
                continue

            # The jump is a tail jump only if it leaves its own function.
            i = bisect.bisect_right(starts, insn.address) - 1
            if i < 0:
                continue
            body_start, body_end = bodies[i]
            if insn.address >= body_end:
                continue            # not inside any known function
            if body_start <= target < body_end:
                continue            # ordinary intra-function branch

            section = self.image.get_section_at_va(target)
            if section is None or not section.executable:
                continue

            # Does the target land inside some *other* function? The CRT does
            # this constantly -- _startOneArgErrorHandling jumps into the middle
            # of _startTwoArgErrorHandling to share its tail. Registering that
            # address as an ordinary candidate would truncate the function it
            # lands in, breaking the very code it wanted to reach. Record it as
            # an alias entry instead: same end address, translated separately.
            j = bisect.bisect_right(starts, target) - 1
            if j >= 0 and bodies[j][0] < target < bodies[j][1]:
                if target not in self._alias_entries:
                    self._alias_entries[target] = bodies[j][1]
                    added = True
                continue

            self._add_candidate(target, config.CONFIDENCE_TAIL_JUMP,
                                "tail_jump_target")
            added = True

        return added

    def _build_alias_entries(self) -> None:
        """
        Emit a Function for each tail-jump target that lands inside another
        function, running from the target to that function's end.

        The overlap is deliberate: the translator produces a second body for the
        shared tail, which costs a little code size and makes the entry point
        callable. The alternative -- a stub that returns immediately -- silently
        skips the epilogue and leaks the caller's frame.
        """
        for addr, end in sorted(self._alias_entries.items()):
            if addr in self.functions:
                continue
            insns = self.engine.get_instructions_in_range(addr, end)
            if not insns:
                continue
            section = self.image.get_section_at_va(addr)
            sec_name = section.name if section else ""
            label = self.labels.get(addr)
            name = label.name if label else f"sub_{addr:08X}"
            if not label:
                self.labels.auto_name_function(
                    addr, sec_name, config.CONFIDENCE_TAIL_JUMP)
            self.functions[addr] = Function(
                start=addr,
                end=end,
                name=name,
                section=sec_name,
                confidence=config.CONFIDENCE_TAIL_JUMP,
                detection_method="tail_jump_alias",
                num_instructions=len(insns),
                has_prologue=False,
            )

    def _build_functions(self, sections: List[SectionInfo]) -> None:
        """
        Pass 5: Build Function objects from candidates.

        Determines function boundaries by finding the extent of each
        function (up to the next function start or unreachable point).
        """
        # Sort candidates by address
        sorted_starts = sorted(self._candidates.keys())
        if not sorted_starts:
            return

        # Build section boundary lookup
        sec_ranges = {}
        for sec in sections:
            sec_ranges[sec.name] = (sec.virtual_addr,
                                    sec.virtual_addr + sec.virtual_size)

        # Create functions
        for idx, start_addr in enumerate(sorted_starts):
            confidence, method = self._candidates[start_addr]

            # Determine section
            section = self.image.get_section_at_va(start_addr)
            sec_name = section.name if section else ""

            # Determine end address:
            # Walk instructions until we hit the next function start,
            # leave the section, or reach an unconditional terminator
            # with no fall-through.
            if idx + 1 < len(sorted_starts):
                next_func = sorted_starts[idx + 1]
            else:
                next_func = None

            # Section end boundary
            sec_end = None
            if section:
                sec_end = section.virtual_addr + section.virtual_size

            end_addr = self._find_function_end(start_addr, next_func, sec_end)

            # Count instructions
            insns = self.engine.get_instructions_in_range(start_addr, end_addr)
            num_insns = len(insns)

            if num_insns == 0:
                continue

            # Check for prologue
            first_insn = self.engine.get_instruction(start_addr)
            has_prologue = (first_insn is not None and
                            first_insn.mnemonic == "push" and
                            first_insn.op_str == "ebp")

            # Get or create name
            label = self.labels.get(start_addr)
            if label:
                name = label.name
            else:
                name = f"sub_{start_addr:08X}"
                self.labels.auto_name_function(
                    start_addr, sec_name, confidence)

            func = Function(
                start=start_addr,
                end=end_addr,
                name=name,
                section=sec_name,
                confidence=confidence,
                detection_method=method,
                num_instructions=num_insns,
                has_prologue=has_prologue,
            )
            self.functions[start_addr] = func

    # A compiled switch table is contiguous 32-bit code pointers. Stop at the
    # first entry that is not one rather than trusting a length from anywhere:
    # the table is usually followed immediately by more code or by another
    # table, and reading past it would drag unrelated addresses into the
    # function's extent. The cap is a backstop for a table that never stops
    # looking plausible.
    _JUMP_TABLE_MAX_ENTRIES = 256

    def _read_jump_table_targets(self, table_va: int, start: int,
                                 upper: int) -> List[int]:
        """Read a compiled switch table, returning only entries that belong
        inside the function being measured.

        `upper` is already clamped to the next known function start, so an
        entry within [start, upper) is an internal arm; anything else is not
        ours to claim and is skipped. Returning [] leaves boundary detection
        exactly as it was, so a table this cannot make sense of costs nothing.
        """
        targets: List[int] = []
        if self.image.get_section_at_va(table_va) is None:
            return targets

        for i in range(self._JUMP_TABLE_MAX_ENTRIES):
            entry = self.image.read_u32_at_va(table_va + i * 4)
            if entry is None:
                break
            # An entry must at least point into a real section to be a code
            # pointer at all; the first that does not ends the table.
            if self.image.get_section_at_va(entry) is None:
                break
            if start <= entry < upper:
                targets.append(entry)
        return targets

    def _find_function_end(self, start: int, next_func: Optional[int],
                           sec_end: Optional[int]) -> int:
        """
        Determine where a function ends.

        Walks forward from start, tracking the furthest reachable point
        through fall-through and internal jumps.
        """
        max_addr = start   # exclusive end of the code decoded so far
        max_target = start  # highest branch target that must be *inside* it
        addr = start

        # Upper bound
        upper = sec_end if sec_end else start + 0x100000
        if next_func and next_func < upper:
            upper = next_func

        while addr < upper:
            insn = self.engine.get_instruction(addr)
            if insn is None:
                break

            end = insn.end_address
            if end > max_addr:
                max_addr = end

            # Track internal forward jumps to extend function bounds.
            #
            # Unconditional jumps count too, not just conditional ones. A body
            # ending in "jmp <forward>" - the tail of an if/else, or a jump
            # over an interleaved block - otherwise hit the break below with
            # max_addr still short of the target, truncating the function
            # mid-body. Everything past the cut then looked like separate code,
            # and the function's own jump targets became calls to empty stubs.
            #
            # `upper` is already clamped to the next known function start, so a
            # target inside these bounds is internal rather than a tail call.
            # is_jump and is_cond_jump are mutually exclusive; is_branch is both.
            if insn.is_branch and insn.jump_target is not None:
                target = insn.jump_target
                if start <= target < upper and target > max_target:
                    # This jump goes forward within bounds, extend
                    max_target = target

            # Same reasoning, for a compiled switch: `jmp [reg*4 + TABLE]` has
            # no single jump_target, so without this the arms sitting right
            # after it fall outside the function. That matters beyond a cosmetic
            # boundary -- the lifter only turns an indirect jump into real gotos
            # when EVERY table entry lands inside the function (see
            # _analyze_switch_table); otherwise it emits RECOMP_ITAIL, which at
            # run time finds no function registered at an arm address, logs
            # "[ICALL] Failed to resolve VA ...", and silently returns eax = 0.
            #
            # Breakdown showed how quiet that failure is: sub_001C5DD0 is an 18
            # byte D3D helper ending in exactly this dispatch, so its arms --
            # each of which writes a float constant through a caller-supplied
            # pointer -- never ran. The caller's output variable kept its
            # garbage, a size computed from it came out as 0xFFFFE09F, and the
            # first visible symptom was an access violation inside memset deep
            # in the heap allocator, with nothing on the stack pointing back
            # here. HeroLab: Xbox Recompiler, task 999ce5ca (2026-09-04).
            if (insn.is_jump and not insn.is_cond_jump
                    and insn.jump_table_base is not None):
                for target in self._read_jump_table_targets(
                        insn.jump_table_base, start, upper):
                    if target > max_target:
                        max_target = target

            if insn.is_ret or (insn.is_jump and not insn.is_cond_jump):
                # Stop only once we have decoded *past* every internal branch
                # target. A target is an address that must be inside the
                # function, so landing exactly on it is not coverage -- the
                # instruction there still has to be decoded. Using the target
                # as an exclusive end cut functions off at their own
                # out-of-line tail: MSVC routinely emits "jmp <backward>" and
                # then parks a conditional branch's target after it. Halo's
                # get_edge_vertex ended at the branch target, so the tail was
                # lifted as a separate function and the jump to it became a
                # tail call that returned without running the epilogue --
                # leaking the whole 28-byte frame on every call.
                if insn.end_address > max_target:
                    break
                # There might be more code after (jumped over)
                addr = insn.end_address
                continue

            addr = insn.end_address

        return max_addr

    def _build_call_graph(self) -> None:
        """Populate calls_to and called_by for all functions."""
        func_starts = set(self.functions.keys())

        for func in self.functions.values():
            insns = self.engine.get_instructions_in_range(func.start, func.end)
            callees = set()
            for insn in insns:
                if insn.call_target is not None:
                    callees.add(insn.call_target)

            func.calls_to = sorted(callees)

            for callee_addr in callees:
                callee = self.functions.get(callee_addr)
                if callee is not None:
                    callee.called_by.append(func.start)

        # Sort called_by lists
        for func in self.functions.values():
            func.called_by = sorted(set(func.called_by))

    def get_function_at(self, addr: int) -> Optional[Function]:
        """Get the function containing an address."""
        # First check direct match
        if addr in self.functions:
            return self.functions[addr]
        # Search for containing function
        for func in self.functions.values():
            if func.start <= addr < func.end:
                return func
        return None

    def get_functions_in_section(self, section_name: str) -> List[Function]:
        """Get all functions in a section, sorted by address."""
        return sorted(
            [f for f in self.functions.values() if f.section == section_name],
            key=lambda f: f.start
        )

    def summary(self) -> dict:
        """Return summary statistics."""
        by_method: Dict[str, int] = {}
        by_section: Dict[str, int] = {}
        total_insns = 0
        with_prologue = 0

        for func in self.functions.values():
            by_method[func.detection_method] = by_method.get(
                func.detection_method, 0) + 1
            by_section[func.section] = by_section.get(func.section, 0) + 1
            total_insns += func.num_instructions
            if func.has_prologue:
                with_prologue += 1

        return {
            "total_functions": len(self.functions),
            "total_instructions_in_functions": total_insns,
            "with_prologue": with_prologue,
            "by_detection_method": by_method,
            "by_section": by_section,
        }
