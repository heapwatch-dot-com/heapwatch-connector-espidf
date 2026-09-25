# Copyright 2026 Heapwatch
# SPDX-License-Identifier: Apache-2.0

"""Host-side policy tests for ParameterStore bounds dirty semantics."""

from __future__ import annotations

import unittest


class SlotFlags:
    IN_USE = 1 << 0
    HAS_VALUE = 1 << 2
    HAS_MIN = 1 << 3
    HAS_MAX = 1 << 4
    BOUNDS_DIRTY = 1 << 5


def has_publishable_content(flags: int) -> bool:
    return bool(flags & SlotFlags.HAS_VALUE) or bool(flags & SlotFlags.BOUNDS_DIRTY)


def include_bounds_in_payload(flags: int) -> bool:
    return bool(flags & SlotFlags.BOUNDS_DIRTY)


def acknowledge_bounds_sent(flags: int) -> int:
    if flags & SlotFlags.BOUNDS_DIRTY:
        return flags & ~SlotFlags.BOUNDS_DIRTY
    return flags


class ParameterBoundsPolicyTest(unittest.TestCase):
    def test_limits_only_dirty_slot_is_publishable(self) -> None:
        flags = SlotFlags.IN_USE | SlotFlags.HAS_MAX | SlotFlags.BOUNDS_DIRTY
        self.assertTrue(has_publishable_content(flags))

    def test_clean_limits_only_slot_is_not_publishable(self) -> None:
        flags = SlotFlags.IN_USE | SlotFlags.HAS_MAX
        self.assertFalse(has_publishable_content(flags))

    def test_bounds_included_only_when_dirty(self) -> None:
        dirty = SlotFlags.BOUNDS_DIRTY | SlotFlags.HAS_MAX
        clean = SlotFlags.HAS_MAX
        self.assertTrue(include_bounds_in_payload(dirty))
        self.assertFalse(include_bounds_in_payload(clean))

    def test_ack_clears_dirty_keeps_other_flags(self) -> None:
        flags = SlotFlags.IN_USE | SlotFlags.HAS_VALUE | SlotFlags.HAS_MAX | SlotFlags.BOUNDS_DIRTY
        acked = acknowledge_bounds_sent(flags)
        self.assertFalse(acked & SlotFlags.BOUNDS_DIRTY)
        self.assertTrue(acked & SlotFlags.HAS_VALUE)
        self.assertTrue(acked & SlotFlags.HAS_MAX)


if __name__ == "__main__":
    unittest.main()
