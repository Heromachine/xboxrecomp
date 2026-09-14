"""Guard the guest-visible thread objects ObReferenceObjectByHandle hands out.

Run: py -3 -m pytest tools/kernel_audit/test_thread_objects.py

ObReferenceObjectByHandle returns a POINTER TO A KERNEL OBJECT and the guest
reads fields out of it. The ordinal-246 bridge used to write a plain 0 for
every type (and kernel_ob.c, which is not the live path, returned the handle
value). Breakdown's GetExitCodeThread reads a thread object's signal state at
+4 and its exit status at +0x120, so it read those out of address 4 -- guest
memory the title writes to for unrelated reasons -- never saw a worker finish,
and busy-waited forever: 1.2 billion kernel calls in a 185 s run. HeroLab task
f6bd2dbc.

Nothing here can execute: the bridge is Windows-only and the object region is
guest RAM that only a mapped run has. So this guards the two things that can
be checked from the source alone and that a later edit could plausibly break:

  * the object region's ARITHMETIC -- a stride too small for the fields, or a
    region overlapping the stack, would corrupt guest memory rather than fail;
  * the SHAPE of the four places that have to agree -- the two field offsets,
    the type discrimination, the write ordering, and both exit paths.

The field offsets themselves are ground truth from the title's own code and are
pinned as literals below, NOT read back out of the header: a test that took
them from the same macro it is checking would pass no matter what the macro
said. They come from sub_001A837D:

    0x001A839A  cmp  byte ptr [ecx + 4], 0
    0x001A83A0  mov  eax, dword ptr [ecx + 0x120]
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]

# From the disassembly quoted above. Do not "simplify" these to the macros.
GUEST_SIGNALSTATE_OFFSET = 0x004
GUEST_EXITSTATUS_OFFSET = 0x120


def _macro_text(source, name):
    """The replacement text of a #define, continuation lines included."""
    match = re.search(
        r"^#define[ \t]+%s[ \t]+((?:.*\\\n)*.*)$" % re.escape(name),
        source, re.M)
    if not match:
        raise AssertionError("no #define %s in xbox_memory_layout.h" % name)
    text = match.group(1).replace("\\\n", " ")
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S).strip()


def _macro(source, name):
    """The value of a #define, with its own macro references resolved."""
    expr = _macro_text(source, name)
    for _ in range(8):
        names = set(re.findall(r"\b([A-Z][A-Z0-9_]{3,})\b", expr))
        if not names:
            break
        for dep in names:
            expr = re.sub(r"\b%s\b" % re.escape(dep),
                          "(%s)" % _macro_text(source, dep), expr)
    return eval(expr, {"__builtins__": {}}, {})


class ThreadObjectLayoutTest(unittest.TestCase):
    """The region has to hold the fields and sit somewhere nothing else owns."""

    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/kernel/xbox_memory_layout.h").read_text(
            encoding="utf-8", errors="replace")
        cls.base = _macro(cls.source, "XBOX_THREAD_OBJ_BASE")
        cls.stride = _macro(cls.source, "XBOX_THREAD_OBJ_STRIDE")
        cls.count = _macro(cls.source, "XBOX_THREAD_OBJ_COUNT")
        cls.end = _macro(cls.source, "XBOX_THREAD_OBJ_END")

    def test_field_offsets_match_what_the_title_reads(self):
        self.assertEqual(
            _macro(self.source, "XBOX_THREAD_OBJ_SIGNALSTATE"),
            GUEST_SIGNALSTATE_OFFSET,
            "the signal-state offset no longer matches `cmp byte [ecx+4], 0` "
            "in sub_001A837D")
        self.assertEqual(
            _macro(self.source, "XBOX_THREAD_OBJ_EXITSTATUS"),
            GUEST_EXITSTATUS_OFFSET,
            "the exit-status offset no longer matches `mov eax, [ecx+0x120]` "
            "in sub_001A837D")

    def test_stride_covers_every_field(self):
        """A stride under 0x124 puts one thread's status in the next object.

        Silent if it happens: the write lands in a mapped page belonging to
        another live thread object, so it corrupts a neighbour's state rather
        than faulting.
        """
        self.assertGreaterEqual(
            self.stride, GUEST_EXITSTATUS_OFFSET + 4,
            "XBOX_THREAD_OBJ_STRIDE is smaller than the highest field")

    def test_region_does_not_overlap_the_kernel_data_exports(self):
        kdata = _macro(self.source, "XBOX_KERNEL_DATA_BASE")
        kdata_end = kdata + _macro(self.source, "XBOX_KERNEL_DATA_SIZE")
        self.assertGreaterEqual(
            self.base, kdata_end,
            "thread objects overlap the kernel data exports, so a thread "
            "exiting would rewrite XboxHardwareInfo and friends")

    def test_region_ends_before_the_guest_stack(self):
        """The stack is the nearest thing above it, and it grows DOWN.

        An overlap here would be read as stack corruption in whatever guest
        function happened to be running, which is about the least legible
        failure this project has.
        """
        stack_base = _macro(self.source, "XBOX_STACK_BASE")
        self.assertLessEqual(
            self.end, stack_base,
            "thread objects run into the guest stack region")

    def test_end_is_consistent_with_base_stride_and_count(self):
        self.assertEqual(self.end, self.base + self.stride * self.count)


class ThreadObjectBridgeShapeTest(unittest.TestCase):
    """The four places in kernel_bridge.c that have to keep agreeing."""

    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/kernel/kernel_bridge.c").read_text(
            encoding="utf-8", errors="replace")

    def _func(self, name):
        match = re.search(r"^static \w[\w \*]*%s\(.*?^\}" % re.escape(name),
                          self.source, re.M | re.S)
        self.assertIsNotNone(match, "kernel_bridge.c has no %s" % name)
        return match.group(0)

    def test_thread_handles_resolve_to_a_real_object(self):
        """The literal shape of the bug: a bare zero for every type."""
        body = self._func("bridge_ObReferenceObjectByHandle")
        self.assertIn(
            "KDATA_THREAD_OBJ_TYPE", body,
            "ObReferenceObjectByHandle no longer distinguishes a thread "
            "handle, so it is back to answering the same thing for every type")
        self.assertIn("bridge_thread_obj_for_token", body)

    def test_exit_status_is_published_before_the_signal(self):
        """The poller reads +4 first and only then +0x120, on another thread.

        Signalling first leaves a window where a poll sees "finished" and
        reads a status that has not been written -- rare, wrong, and very hard
        to reproduce. The barrier between the two is the whole guarantee.
        """
        body = self._func("bridge_thread_obj_signal")
        status_at = body.find("XBOX_THREAD_OBJ_EXITSTATUS")
        barrier_at = body.find("MemoryBarrier")
        signal_at = body.find("XBOX_THREAD_OBJ_SIGNALSTATE")
        self.assertNotEqual(status_at, -1, "no exit-status write")
        self.assertNotEqual(signal_at, -1, "no signal-state write")
        self.assertNotEqual(barrier_at, -1, "no barrier between the two")
        self.assertLess(status_at, barrier_at,
                        "the exit status is written after the barrier")
        self.assertLess(barrier_at, signal_at,
                        "the signal is published before the barrier")

    def test_both_ways_a_thread_can_finish_signal_the_object(self):
        """PsTerminateSystemThread is the normal exit; returning is the other.

        Covering only one leaves half the workers looking like they are still
        running forever, which is the original bug for those threads.
        """
        self.assertIn(
            "bridge_thread_obj_signal", self._func("bridge_PsTerminateSystemThread"),
            "a thread that exits via PsTerminateSystemThread no longer "
            "reports its exit")
        self.assertIn(
            "bridge_thread_obj_signal", self._func("bridge_thread_main"),
            "a start routine that simply RETURNS no longer reports its exit")

    def test_the_slot_outlives_the_thread(self):
        """Releasing on exit would break the thing this exists to support.

        GetExitCodeThread is legitimately called after the worker is gone, so
        the object has to survive until the guest closes the handle.
        """
        self.assertIn(
            "bridge_thread_obj_release_token", self._func("bridge_NtClose"),
            "thread-object slots are no longer released by NtClose")
        for name in ("bridge_PsTerminateSystemThread", "bridge_thread_main"):
            self.assertNotIn(
                "bridge_thread_obj_release", self._func(name),
                "%s releases the slot at exit, so a later GetExitCodeThread "
                "reads a recycled or cleared object" % name)

    def test_priority_calls_accept_an_object_not_just_a_handle(self):
        """The out-parameter aliases the handle argument at the call site.

        Breakdown's sub_001A8277 does `lea eax, [ebp+8]`, so by the time it
        calls KeSetBasePriorityThread the handle slot holds the OBJECT. A
        bridge that only understands handle tokens silently sets no priority.
        """
        self.assertIn(
            "bridge_thread_handle_for_object",
            self._func("bridge_KeSetBasePriorityThread"),
            "KeSetBasePriorityThread cannot resolve a thread object back to "
            "its thread")


if __name__ == "__main__":
    unittest.main()
