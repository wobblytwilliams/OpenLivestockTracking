from __future__ import annotations

import unittest

from olg_gateway_priority import (
    ADV_FLAG_AGE_UNKNOWN,
    ADV_FLAG_DATA_AVAILABLE,
    ADV_FLAG_ELIGIBLE,
    ADV_FLAG_IN_COOLDOWN,
    ADV_FLAG_NEVER,
    ADV_AGE_UNKNOWN,
    MFG_COMPANY_ID,
    parse_logger_advertisement,
    select_candidate,
)


class DummyDevice:
    def __init__(self, address: str):
        self.address = address


class DummyAdvertisement:
    def __init__(self, payload: bytes, rssi: int = -60):
        self.manufacturer_data = {MFG_COMPANY_ID: payload}
        self.rssi = rssi
        self.service_uuids = []


def payload(logger_id: int, flags: int, age_minutes: int, upload_mask: int = 0x06) -> bytes:
    return (
        b"OLGA"
        + bytes([1, upload_mask, flags])
        + age_minutes.to_bytes(2, "little")
        + logger_id.to_bytes(8, "little")
    )


def candidate(logger_id: int, flags: int, age_minutes: int, now_s: float = 0.0):
    parsed = parse_logger_advertisement(
        DummyDevice(f"AA:BB:CC:DD:EE:{logger_id:02X}"),
        DummyAdvertisement(payload(logger_id, flags, age_minutes)),
        now_s,
    )
    assert parsed is not None
    return parsed


class GatewayPriorityTests(unittest.TestCase):
    def test_never_downloaded_ranks_first(self) -> None:
        base_flags = ADV_FLAG_DATA_AVAILABLE | ADV_FLAG_ELIGIBLE
        candidates = {
            "known": candidate(1, base_flags, 500, 1.0),
            "unknown": candidate(2, base_flags | ADV_FLAG_AGE_UNKNOWN, ADV_AGE_UNKNOWN, 2.0),
            "never": candidate(3, base_flags | ADV_FLAG_NEVER, 0xFFFF, 3.0),
        }

        selected = select_candidate(candidates, {}, 4.0)

        self.assertIsNotNone(selected)
        self.assertEqual(selected.logger_id, "0000000000000003")

    def test_unknown_age_ranks_ahead_of_known_age(self) -> None:
        base_flags = ADV_FLAG_DATA_AVAILABLE | ADV_FLAG_ELIGIBLE
        candidates = {
            "known": candidate(1, base_flags, 10000, 1.0),
            "unknown": candidate(2, base_flags | ADV_FLAG_AGE_UNKNOWN, ADV_AGE_UNKNOWN, 2.0),
        }

        selected = select_candidate(candidates, {}, 3.0)

        self.assertIsNotNone(selected)
        self.assertEqual(selected.logger_id, "0000000000000002")

    def test_cooldown_and_failed_loggers_are_skipped(self) -> None:
        base_flags = ADV_FLAG_DATA_AVAILABLE | ADV_FLAG_ELIGIBLE
        candidates = {
            "cooldown": candidate(1, ADV_FLAG_DATA_AVAILABLE | ADV_FLAG_IN_COOLDOWN, 1, 9.0),
            "failed": candidate(2, base_flags, 200, 9.2),
            "ready": candidate(3, base_flags, 100, 9.5),
        }

        selected = select_candidate(candidates, {"0000000000000002": 60.0}, 10.0)

        self.assertIsNotNone(selected)
        self.assertEqual(selected.logger_id, "0000000000000003")

    def test_stale_eligible_logger_is_not_selected_for_connection(self) -> None:
        base_flags = ADV_FLAG_DATA_AVAILABLE | ADV_FLAG_ELIGIBLE
        candidates = {
            "stale": candidate(1, base_flags, 500, 1.0),
            "fresh": candidate(2, base_flags, 100, 9.5),
        }

        selected = select_candidate(candidates, {}, 10.0)

        self.assertIsNotNone(selected)
        self.assertEqual(selected.logger_id, "0000000000000002")


if __name__ == "__main__":
    unittest.main()
