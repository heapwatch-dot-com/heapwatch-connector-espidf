# Copyright (c) 2026 Heapwatch. All rights reserved.

"""Host-side tests for connector boot-sync edge detection.

Must stay aligned with connector_boot_sync/ConnectorBootSync.h:
heapwatch_should_enqueue_boot_static().
"""

from __future__ import annotations

import unittest


def should_enqueue_boot_static(
    prev_mqtt_connected: bool, mqtt_connected: bool, boot_static_enqueued: bool
) -> bool:
    return not prev_mqtt_connected and mqtt_connected and not boot_static_enqueued


class ConnectorBootSyncLogicTest(unittest.TestCase):
    def test_first_mqtt_up_enqueues_once(self) -> None:
        self.assertTrue(should_enqueue_boot_static(False, True, False))

    def test_steady_mqtt_does_not_reenqueue(self) -> None:
        self.assertFalse(should_enqueue_boot_static(True, True, True))
        self.assertFalse(should_enqueue_boot_static(True, True, False))

    def test_reconnect_after_boot_flag_set_does_not_reenqueue(self) -> None:
        self.assertFalse(should_enqueue_boot_static(False, True, True))

    def test_mqtt_down_never_enqueues(self) -> None:
        self.assertFalse(should_enqueue_boot_static(True, False, False))
        self.assertFalse(should_enqueue_boot_static(False, False, False))


if __name__ == "__main__":
    unittest.main()
