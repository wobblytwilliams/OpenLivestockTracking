from __future__ import annotations

from dataclasses import dataclass
from typing import Any


MFG_COMPANY_ID = 0xFFFF
ADV_MAGIC = b"OLGA"
ADV_AGE_UNKNOWN = 0xFFFE
ADV_AGE_NEVER = 0xFFFF

ADV_FLAG_DATA_AVAILABLE = 1 << 0
ADV_FLAG_IN_COOLDOWN = 1 << 1
ADV_FLAG_ELIGIBLE = 1 << 2
ADV_FLAG_NEVER = 1 << 3
ADV_FLAG_AGE_UNKNOWN = 1 << 4

CANDIDATE_STALE_S = 60.0
CONNECT_CANDIDATE_MAX_AGE_S = 5.0
FAILURE_BACKOFF_S = 60.0


@dataclass
class LoggerAdvertisement:
    address: str
    device: Any
    logger_id: str
    upload_mask: int
    flags: int
    age_minutes: int
    rssi: int | None
    last_seen_s: float

    @property
    def data_available(self) -> bool:
        return bool(self.flags & ADV_FLAG_DATA_AVAILABLE)

    @property
    def in_cooldown(self) -> bool:
        return bool(self.flags & ADV_FLAG_IN_COOLDOWN)

    @property
    def transfer_eligible(self) -> bool:
        return bool(self.flags & ADV_FLAG_ELIGIBLE)

    @property
    def never_downloaded(self) -> bool:
        return bool(self.flags & ADV_FLAG_NEVER) or self.age_minutes == ADV_AGE_NEVER

    @property
    def age_unknown(self) -> bool:
        return bool(self.flags & ADV_FLAG_AGE_UNKNOWN) or self.age_minutes == ADV_AGE_UNKNOWN

    @property
    def key(self) -> str:
        return self.logger_id if self.logger_id != "unknown" else self.address


def _manufacturer_status(advertisement_data: Any) -> bytes | None:
    manufacturer_data = getattr(advertisement_data, "manufacturer_data", {}) or {}
    data = manufacturer_data.get(MFG_COMPANY_ID)
    if data is not None:
        raw = bytes(data)
        if raw.startswith(ADV_MAGIC):
            return raw
        if len(raw) >= 2 and int.from_bytes(raw[:2], "little") == MFG_COMPANY_ID:
            raw = raw[2:]
            if raw.startswith(ADV_MAGIC):
                return raw

    for value in manufacturer_data.values():
        raw = bytes(value)
        if raw.startswith(ADV_MAGIC):
            return raw
        if len(raw) >= 2 and int.from_bytes(raw[:2], "little") == MFG_COMPANY_ID:
            raw = raw[2:]
            if raw.startswith(ADV_MAGIC):
                return raw

    return None


def parse_logger_advertisement(device: Any, advertisement_data: Any, now_s: float) -> LoggerAdvertisement | None:
    status = _manufacturer_status(advertisement_data)
    if status is None:
        return None
    if len(status) < 17 or not status.startswith(ADV_MAGIC):
        return None
    if status[4] != 1:
        return None

    logger_id = int.from_bytes(status[9:17], "little")
    return LoggerAdvertisement(
        address=str(getattr(device, "address", "unknown")),
        device=device,
        logger_id=f"{logger_id:016X}",
        upload_mask=status[5],
        flags=status[6],
        age_minutes=int.from_bytes(status[7:9], "little"),
        rssi=getattr(advertisement_data, "rssi", getattr(device, "rssi", None)),
        last_seen_s=now_s,
    )


def candidate_priority(candidate: LoggerAdvertisement) -> tuple[int, int, int, float]:
    if candidate.never_downloaded:
        return (2, 0, 0, candidate.last_seen_s)
    if candidate.age_unknown:
        return (1, 0, 0, candidate.last_seen_s)
    age = 0 if candidate.age_minutes >= ADV_AGE_UNKNOWN else candidate.age_minutes
    return (0, age, 0, candidate.last_seen_s)


def select_candidate(
    candidates: dict[str, LoggerAdvertisement],
    failure_backoff_until: dict[str, float],
    now_s: float,
) -> LoggerAdvertisement | None:
    eligible: list[LoggerAdvertisement] = []
    for key, candidate in list(candidates.items()):
        if now_s - candidate.last_seen_s > CANDIDATE_STALE_S:
            del candidates[key]
            continue
        if now_s - candidate.last_seen_s > CONNECT_CANDIDATE_MAX_AGE_S:
            continue
        if failure_backoff_until.get(key, 0.0) > now_s:
            continue
        if failure_backoff_until.get(candidate.key, 0.0) > now_s:
            continue
        if candidate.in_cooldown or not candidate.transfer_eligible:
            continue
        eligible.append(candidate)

    if not eligible:
        return None

    return max(eligible, key=candidate_priority)
