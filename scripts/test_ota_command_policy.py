"""Host-side mirror of OtaService command gating (persistent hw_ota + hw_command abort)."""

import unittest


class OtaPhase:
    IDLE = 0
    RUNNING = 1
    BOOT_POINTER_SWITCHED = 2


def is_in_progress(phase: int) -> bool:
    return phase == OtaPhase.RUNNING


def reject_non_ota_command(phase: int, suffix: str) -> bool:
    return is_in_progress(phase) and not suffix.startswith("ota/")


def reject_ota_confirm_or_rollback_during_transfer(phase: int, suffix: str) -> bool:
    return is_in_progress(phase) and suffix in ("ota/confirm", "ota/rollback")


def accept_ota_start(phase: int) -> bool:
    return phase != OtaPhase.RUNNING


class OtaCommandPolicyTest(unittest.TestCase):
    def test_running_blocks_ping(self) -> None:
        self.assertTrue(reject_non_ota_command(OtaPhase.RUNNING, "ping"))

    def test_running_allows_ota_abort(self) -> None:
        self.assertFalse(reject_non_ota_command(OtaPhase.RUNNING, "ota/abort"))

    def test_idle_allows_ping(self) -> None:
        self.assertFalse(reject_non_ota_command(OtaPhase.IDLE, "ping"))

    def test_running_rejects_confirm(self) -> None:
        self.assertTrue(reject_ota_confirm_or_rollback_during_transfer(OtaPhase.RUNNING, "ota/confirm"))

    def test_idle_allows_confirm(self) -> None:
        self.assertFalse(reject_ota_confirm_or_rollback_during_transfer(OtaPhase.IDLE, "ota/confirm"))

    def test_second_start_rejected_while_running(self) -> None:
        self.assertFalse(accept_ota_start(OtaPhase.RUNNING))


if __name__ == "__main__":
    unittest.main()
