from __future__ import annotations

import argparse
import asyncio
import sqlite3
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from uuid import uuid4

import pyarrow as pa
import pyarrow.dataset as ds
import pyarrow.parquet as pq
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakBluetoothNotAvailableError

from olg_log_format import BLOCK_HEADER, iter_blocks
from olg_gateway_priority import (
    CONNECT_CANDIDATE_MAX_AGE_S,
    FAILURE_BACKOFF_S,
    LoggerAdvertisement,
    parse_logger_advertisement,
    select_candidate,
)


INFO_UUID = "8f0a0002-4f4c-4747-4154-455741593031"
CONTROL_UUID = "8f0a0003-4f4c-4747-4154-455741593031"
DATA_UUID = "8f0a0004-4f4c-4747-4154-455741593031"

CMD_PREPARE = bytes([1])
CMD_MANIFEST = bytes([2])
CMD_DONE = bytes([4])

MSG_STATUS = 1
MSG_MANIFEST = 2
MSG_CHUNK = 3
MSG_BLOCK = 4

STATUS_OK = 0
STATUS_EOF = 1
STATUS_ERROR = 2
STATUS_NOT_ELIGIBLE = 3

CONTROL_TIMEOUT_S = 20.0
STREAM_IDLE_TIMEOUT_S = 20.0
PARQUET_FLUSH_ROWS = 500_000
PARQUET_COMPACT_MIN_FILES = 8
PARQUET_COMPACT_MAX_BYTES = 256 * 1024 * 1024


class PartialTransfer(Exception):
    pass


class NotEligible(Exception):
    pass


@dataclass
class Segment:
    index: int
    size: int
    active: bool


@dataclass
class LoggerStatus:
    logger_id: str
    upload_mask: int
    status_flags: int
    age_minutes: int
    cooldown_remaining_minutes: int

    @property
    def in_cooldown(self) -> bool:
        return bool(self.status_flags & (1 << 1))

    @property
    def transfer_eligible(self) -> bool:
        return bool(self.status_flags & (1 << 2))

    @property
    def age_label(self) -> str:
        if self.status_flags & (1 << 3):
            return "never"
        if self.status_flags & (1 << 4):
            return "unknown"
        return f"{self.age_minutes} min"


class BlockAssembler:
    def __init__(self, start_offset: int):
        self.base_offset = start_offset
        self.buffer = bytearray()

    def feed(self, offset: int, data: bytes) -> list[tuple[int, int, dict[str, list[dict]]]]:
        if offset != self.base_offset + len(self.buffer):
            raise ValueError(f"unexpected chunk offset {offset}, wanted {self.base_offset + len(self.buffer)}")

        self.buffer.extend(data)
        ready: list[tuple[int, int, dict[str, list[dict]]]] = []

        while len(self.buffer) >= BLOCK_HEADER.size:
            _magic, _version, _header_len, _seq, payload_len, _count, _res, _crc = (
                BLOCK_HEADER.unpack(self.buffer[: BLOCK_HEADER.size])
            )
            block_len = BLOCK_HEADER.size + payload_len
            if len(self.buffer) < block_len:
                break

            block_offset = self.base_offset
            raw = bytes(self.buffer[:block_len])
            del self.buffer[:block_len]
            self.base_offset += block_len

            import io

            block = next(iter_blocks(io.BytesIO(raw), start_offset=0))
            ready.append((block_offset, self.base_offset, block.rows))

        return ready


class FilteredBlockAssembler:
    def __init__(self):
        self.raw_start: int | None = None
        self.raw_end: int | None = None
        self.block_len = 0
        self.buffer = bytearray()

    def feed(
        self,
        raw_start: int,
        raw_end: int,
        part_offset: int,
        block_len: int,
        data: bytes,
    ) -> tuple[int, int, dict[str, list[dict]]] | None:
        if self.raw_start != raw_start or self.block_len != block_len:
            if part_offset != 0:
                raise ValueError("filtered block did not start at part offset 0")
            self.raw_start = raw_start
            self.raw_end = raw_end
            self.block_len = block_len
            self.buffer = bytearray(block_len)
        if self.raw_end != raw_end:
            raise ValueError("filtered block raw end changed mid-block")
        if part_offset + len(data) > self.block_len:
            raise ValueError("filtered block chunk exceeds advertised block length")

        self.buffer[part_offset : part_offset + len(data)] = data
        if part_offset + len(data) < self.block_len:
            return None

        import io

        block = next(iter_blocks(io.BytesIO(bytes(self.buffer)), start_offset=0))
        result = (self.raw_start, self.raw_end or raw_end, block.rows)
        self.raw_start = None
        self.raw_end = None
        self.block_len = 0
        self.buffer = bytearray()
        return result


def open_db(path: Path) -> sqlite3.Connection:
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path)
    db.execute("pragma journal_mode=wal")
    db.execute(
        """
        create table if not exists loggers (
            logger_id text primary key,
            last_seen_ms integer not null
        )
        """
    )
    db.execute(
        """
        create table if not exists segments (
            logger_id text not null,
            segment_index integer not null,
            size integer not null default 0,
            offset integer not null default 0,
            primary key (logger_id, segment_index)
        )
        """
    )
    db.execute(
        """
        create table if not exists blocks (
            logger_id text not null,
            segment_index integer not null,
            block_offset integer not null,
            block_end integer not null,
            primary key (logger_id, segment_index, block_offset)
        )
        """
    )
    db.execute(
        """
        create table if not exists segment_offsets (
            logger_id text not null,
            profile text not null,
            segment_index integer not null,
            size integer not null default 0,
            offset integer not null default 0,
            primary key (logger_id, profile, segment_index)
        )
        """
    )
    db.execute(
        """
        create table if not exists row_counts (
            logger_id text not null,
            type text not null,
            date text not null,
            rows integer not null default 0,
            parquet_bytes integer not null default 0,
            updated_ms integer not null,
            primary key (logger_id, type, date)
        )
        """
    )
    db.execute(
        """
        create table if not exists sessions (
            id integer primary key autoincrement,
            logger_id text not null,
            started_ms integer not null,
            finished_ms integer,
            status text not null
        )
        """
    )
    db.execute(
        """
        create table if not exists gateway_status (
            key text primary key,
            value text not null,
            updated_ms integer not null
        )
        """
    )
    db.commit()
    return db


def set_status(db: sqlite3.Connection, key: str, value: str) -> None:
    db.execute(
        "insert into gateway_status(key, value, updated_ms) values(?,?,?) "
        "on conflict(key) do update set value=excluded.value, updated_ms=excluded.updated_ms",
        (key, value, int(time.time() * 1000)),
    )
    db.commit()


def log(message: str) -> None:
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"{stamp} {message}", flush=True)


def logger_status_from_info(raw: bytes) -> LoggerStatus:
    if len(raw) < 16 or raw[:4] != b"OLGI":
        return LoggerStatus(
            logger_id="unknown",
            upload_mask=0,
            status_flags=0,
            age_minutes=0,
            cooldown_remaining_minutes=0,
        )

    version = raw[4]
    upload_mask = raw[5]
    logger_id = int.from_bytes(raw[8:16], "little")
    status_flags = 0
    age_minutes = 0
    cooldown_remaining_minutes = 0
    if version >= 3 and len(raw) >= 30:
        status_flags = raw[24]
        age_minutes = int.from_bytes(raw[26:28], "little")
        cooldown_remaining_minutes = int.from_bytes(raw[28:30], "little")

    return LoggerStatus(
        logger_id=f"{logger_id:016X}",
        upload_mask=upload_mask,
        status_flags=status_flags,
        age_minutes=age_minutes,
        cooldown_remaining_minutes=cooldown_remaining_minutes,
    )


def upload_profile_from_info(raw: bytes) -> str:
    if len(raw) >= 16 and raw[:4] == b"OLGI" and raw[4] >= 2:
        return f"mask{raw[5]:02x}"
    return "raw"


def segment_offset(db: sqlite3.Connection, logger_id: str, profile: str, segment: Segment) -> int:
    row = db.execute(
        "select offset from segment_offsets where logger_id=? and profile=? and segment_index=?",
        (logger_id, profile, segment.index),
    ).fetchone()
    if row is None:
        legacy_offset = 0
        if profile == "raw":
            legacy = db.execute(
                "select offset from segments where logger_id=? and segment_index=?",
                (logger_id, segment.index),
            ).fetchone()
            if legacy is not None:
                legacy_offset = min(int(legacy[0] or 0), segment.size)
        db.execute(
            "insert into segment_offsets(logger_id, profile, segment_index, size, offset) values(?,?,?,?,?)",
            (logger_id, profile, segment.index, segment.size, legacy_offset),
        )
        db.commit()
        return legacy_offset
    if row[0] > segment.size:
        db.execute(
            "update segment_offsets set size=?, offset=0 where logger_id=? and profile=? and segment_index=?",
            (segment.size, logger_id, profile, segment.index),
        )
        db.commit()
        return 0
    db.execute(
        "update segment_offsets set size=? where logger_id=? and profile=? and segment_index=?",
        (segment.size, logger_id, profile, segment.index),
    )
    db.commit()
    return int(row[0])


def partition_date(rows: list[dict[str, Any]]) -> str:
    for row in rows:
        unix_ms = int(row.get("unix_ms") or 0)
        if unix_ms > 0:
            return time.strftime("%Y-%m-%d", time.gmtime(unix_ms / 1000))
    return "unsynced"


def parquet_partition_dir(parquet_root: Path, name: str, logger_id: str, date: str) -> Path:
    return parquet_root / f"type={name}" / f"logger_id={logger_id}" / f"date={date}"


def add_row_count(
    db: sqlite3.Connection,
    logger_id: str,
    name: str,
    date: str,
    rows: int,
    parquet_bytes: int,
) -> None:
    db.execute(
        "insert into row_counts(logger_id, type, date, rows, parquet_bytes, updated_ms) "
        "values(?,?,?,?,?,?) "
        "on conflict(logger_id, type, date) do update set "
        "rows=row_counts.rows+excluded.rows, "
        "parquet_bytes=row_counts.parquet_bytes+excluded.parquet_bytes, "
        "updated_ms=excluded.updated_ms",
        (logger_id, name, date, rows, parquet_bytes, int(time.time() * 1000)),
    )


def set_partition_parquet_bytes(
    db: sqlite3.Connection,
    logger_id: str,
    name: str,
    date: str,
    parquet_bytes: int,
) -> None:
    db.execute(
        "update row_counts set parquet_bytes=?, updated_ms=? "
        "where logger_id=? and type=? and date=?",
        (parquet_bytes, int(time.time() * 1000), logger_id, name, date),
    )


class ParquetBatchWriter:
    def __init__(self, db: sqlite3.Connection, parquet_root: Path, logger_id: str):
        self.db = db
        self.parquet_root = parquet_root
        self.logger_id = logger_id
        self.rows: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
        self.dirty_keys: set[tuple[str, str]] = set()

    def add(self, rows_by_type: dict[str, list[dict]]) -> None:
        for name, rows in rows_by_type.items():
            if not rows:
                continue
            date = partition_date(rows)
            key = (name, date)
            self.rows[key].extend(rows)
            if len(self.rows[key]) >= PARQUET_FLUSH_ROWS:
                self.flush_key(key)

    def flush_key(self, key: tuple[str, str]) -> None:
        rows = self.rows.pop(key, [])
        if not rows:
            return

        name, date = key
        out_dir = parquet_partition_dir(self.parquet_root, name, self.logger_id, date)
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / f"part-{uuid4().hex}.parquet"
        table = pa.Table.from_pylist(rows)
        pq.write_table(table, out_path, compression="zstd")
        add_row_count(self.db, self.logger_id, name, date, len(rows), out_path.stat().st_size)
        self.dirty_keys.add(key)

    def compact_key(self, key: tuple[str, str]) -> None:
        name, date = key
        out_dir = parquet_partition_dir(self.parquet_root, name, self.logger_id, date)
        files = sorted(out_dir.glob("part-*.parquet"))
        if len(files) < PARQUET_COMPACT_MIN_FILES:
            return

        total_bytes = sum(path.stat().st_size for path in files)
        if total_bytes > PARQUET_COMPACT_MAX_BYTES:
            return

        table = ds.dataset(out_dir, format="parquet").to_table()
        tmp_path = out_dir / f"compacting-{uuid4().hex}.parquet"
        final_path = out_dir / f"part-compact-{uuid4().hex}.parquet"
        pq.write_table(table, tmp_path, compression="zstd")
        for path in files:
            path.unlink()
        tmp_path.rename(final_path)
        set_partition_parquet_bytes(
            self.db,
            self.logger_id,
            name,
            date,
            sum(path.stat().st_size for path in out_dir.glob("part-*.parquet")),
        )

    def flush_all(self) -> None:
        for key in list(self.rows):
            self.flush_key(key)
        for key in list(self.dirty_keys):
            self.compact_key(key)
        self.dirty_keys.clear()
        self.db.commit()


def commit_block(
    _db: sqlite3.Connection,
    writer: ParquetBatchWriter,
    _logger_id: str,
    _segment_index: int,
    _block_offset: int,
    _block_end: int,
    rows: dict[str, list[dict]],
) -> None:
    writer.add(rows)


def commit_segment_offset(
    db: sqlite3.Connection,
    logger_id: str,
    profile: str,
    segment_index: int,
    offset: int,
) -> None:
    db.execute(
        "update segment_offsets set offset=max(offset, ?) where logger_id=? and profile=? and segment_index=?",
        (offset, logger_id, profile, segment_index),
    )
    db.commit()


async def next_gateway_msg(queue: asyncio.Queue[bytes], timeout: float, context: str) -> bytes:
    try:
        return await asyncio.wait_for(queue.get(), timeout=timeout)
    except asyncio.TimeoutError as exc:
        raise TimeoutError(f"timed out waiting for {context}") from exc


async def wait_status(queue: asyncio.Queue[bytes]) -> int:
    while True:
        msg = await next_gateway_msg(queue, CONTROL_TIMEOUT_S, "gateway status")
        if msg and msg[0] == MSG_STATUS:
            return msg[1]


async def read_manifest(client: BleakClient, queue: asyncio.Queue[bytes]) -> list[Segment]:
    segments: list[Segment] = []
    await client.write_gatt_char(CONTROL_UUID, CMD_MANIFEST, response=False)

    while True:
        msg = await next_gateway_msg(queue, CONTROL_TIMEOUT_S, "manifest data")
        if not msg:
            continue
        if msg[0] == MSG_STATUS:
            if msg[1] != STATUS_OK:
                raise RuntimeError("logger returned manifest error")
            return segments
        if msg[0] == MSG_MANIFEST and len(msg) >= 9:
            index = int.from_bytes(msg[1:3], "little")
            size = int.from_bytes(msg[3:7], "little")
            active = bool(msg[7])
            segments.append(Segment(index=index, size=size, active=active))


async def stream_segment(
    client: BleakClient,
    queue: asyncio.Queue[bytes],
    db: sqlite3.Connection,
    writer: ParquetBatchWriter,
    logger_id: str,
    profile: str,
    segment: Segment,
    offset: int,
) -> None:
    assembler = BlockAssembler(offset)
    filtered = FilteredBlockAssembler()
    progress_offset = offset
    cmd = bytes([3]) + segment.index.to_bytes(2, "little") + offset.to_bytes(4, "little")
    await client.write_gatt_char(CONTROL_UUID, cmd, response=False)

    while True:
        try:
            msg = await next_gateway_msg(queue, STREAM_IDLE_TIMEOUT_S, "stream data")
        except TimeoutError as exc:
            if max(assembler.base_offset, progress_offset) > offset:
                progress_offset = max(assembler.base_offset, progress_offset)
                writer.flush_all()
                commit_segment_offset(db, logger_id, profile, segment.index, progress_offset)
                raise PartialTransfer(
                    f"partial segment {segment.index} transfer stopped at "
                    f"{progress_offset}/{segment.size} bytes"
                ) from exc
            raise
        if not msg:
            continue
        if msg[0] == MSG_STATUS:
            if msg[1] in (STATUS_OK, STATUS_EOF):
                writer.flush_all()
                commit_segment_offset(db, logger_id, profile, segment.index, segment.size)
                return
            raise RuntimeError("logger returned stream error")
        if msg[0] == MSG_CHUNK and len(msg) >= 8:
            seg_index = int.from_bytes(msg[1:3], "little")
            chunk_offset = int.from_bytes(msg[3:7], "little")
            chunk_len = msg[7]
            if len(msg) < 8 + chunk_len:
                continue
            payload = bytes(msg[8 : 8 + chunk_len])
            if seg_index != segment.index:
                continue

            for block_offset, block_end, rows in assembler.feed(chunk_offset, payload):
                commit_block(db, writer, logger_id, segment.index, block_offset, block_end, rows)
                progress_offset = block_end
                log(f"Committed segment {segment.index}: {block_end}/{segment.size} bytes")
        elif msg[0] == MSG_BLOCK and len(msg) >= 16:
            seg_index = int.from_bytes(msg[1:3], "little")
            raw_start = int.from_bytes(msg[3:7], "little")
            raw_end = int.from_bytes(msg[7:11], "little")
            part_offset = int.from_bytes(msg[11:13], "little")
            block_len = int.from_bytes(msg[13:15], "little")
            chunk_len = msg[15]
            if len(msg) < 16 + chunk_len:
                continue
            payload = bytes(msg[16 : 16 + chunk_len])
            if seg_index != segment.index:
                continue
            if block_len == 0 and chunk_len == 0:
                progress_offset = raw_end
                commit_segment_offset(db, logger_id, profile, segment.index, raw_end)
                log(f"Skipped filtered segment {segment.index}: {raw_end}/{segment.size} bytes")
                continue

            ready = filtered.feed(raw_start, raw_end, part_offset, block_len, payload)
            if ready is not None:
                block_offset, block_end, rows = ready
                commit_block(db, writer, logger_id, segment.index, block_offset, block_end, rows)
                progress_offset = block_end
                log(f"Committed filtered segment {segment.index}: {block_end}/{segment.size} bytes")


async def handle_device(device, data_dir: Path, expected_logger_id: str | None = None) -> str:
    db = open_db(data_dir / "gateway.sqlite")
    parquet_root = data_dir / "parquet"
    queue: asyncio.Queue[bytes] = asyncio.Queue()
    set_status(db, "last_device_address", str(getattr(device, "address", "")))
    log(f"Connecting to logger at {getattr(device, 'address', 'unknown address')}")
    session: int | None = None
    logger_id = "unknown"

    async with BleakClient(device) as client:
        info = bytes(await client.read_gatt_char(INFO_UUID))
        logger_status = logger_status_from_info(info)
        logger_id = logger_status.logger_id
        upload_profile = upload_profile_from_info(info)
        if expected_logger_id and expected_logger_id != logger_id:
            log(f"Advertisement logger {expected_logger_id} connected as {logger_id}; using connected status")
        log(
            f"Connected to logger {logger_id} ({upload_profile}, "
            f"age={logger_status.age_label}, cooldown={logger_status.cooldown_remaining_minutes} min)"
        )
        now_ms = int(time.time() * 1000)
        db.execute(
            "insert into loggers(logger_id, last_seen_ms) values(?, ?) "
            "on conflict(logger_id) do update set last_seen_ms=excluded.last_seen_ms",
            (logger_id, now_ms),
        )
        db.commit()
        if not logger_status.transfer_eligible:
            set_status(db, "active_logger_id", "")
            set_status(db, "active_upload_profile", "")
            reason = "inside cooldown" if logger_status.in_cooldown else "not transfer-eligible"
            raise NotEligible(f"logger {logger_id} is {reason}")

        writer = ParquetBatchWriter(db, parquet_root, logger_id)
        session = db.execute(
            "insert into sessions(logger_id, started_ms, status) values(?,?,?)",
            (logger_id, now_ms, "running"),
        ).lastrowid
        db.commit()
        set_status(db, "active_logger_id", logger_id)
        set_status(db, "active_upload_profile", upload_profile)

        def on_data(_sender, data: bytearray) -> None:
            queue.put_nowait(bytes(data))

        try:
            await client.start_notify(DATA_UUID, on_data)
            await client.write_gatt_char(CONTROL_UUID, CMD_PREPARE, response=False)
            prepare_status = await wait_status(queue)
            if prepare_status == STATUS_NOT_ELIGIBLE:
                raise NotEligible(f"logger {logger_id} rejected prepare inside cooldown")
            if prepare_status != STATUS_OK:
                raise RuntimeError("logger prepare failed")

            manifest = await read_manifest(client, queue)
            log(f"Logger {logger_id} has {len(manifest)} log segment(s)")

            for segment in manifest:
                offset = segment_offset(db, logger_id, upload_profile, segment)
                if offset < segment.size:
                    log(f"Downloading segment {segment.index}: {offset}/{segment.size} bytes")
                    await stream_segment(client, queue, db, writer, logger_id, upload_profile, segment, offset)
                else:
                    log(f"Segment {segment.index} already downloaded ({segment.size} bytes)")

            writer.flush_all()
            await client.write_gatt_char(CONTROL_UUID, CMD_DONE, response=False)
            done_status = await wait_status(queue)
            if done_status != STATUS_OK:
                raise RuntimeError("logger did not confirm gateway download success")
            db.execute(
                "update sessions set finished_ms=?, status=? where id=?",
                (int(time.time() * 1000), "ok", session),
            )
            db.commit()
            set_status(db, "last_transfer_ok_ms", str(int(time.time() * 1000)))
            log(f"Transfer complete for logger {logger_id}")
            return logger_id
        except PartialTransfer as exc:
            writer.flush_all()
            log(str(exc))
            if session is not None:
                db.execute(
                    "update sessions set finished_ms=?, status=? where id=?",
                    (int(time.time() * 1000), "partial", session),
                )
                db.commit()
            set_status(db, "last_error", "")
            raise
        except NotEligible:
            writer.flush_all()
            if session is not None:
                db.execute(
                    "update sessions set finished_ms=?, status=? where id=?",
                    (int(time.time() * 1000), "not_eligible", session),
                )
                db.commit()
            raise
        except Exception:
            writer.flush_all()
            if session is not None:
                db.execute(
                    "update sessions set finished_ms=?, status=? where id=?",
                    (int(time.time() * 1000), "failed", session),
                )
                db.commit()
            raise
        finally:
            set_status(db, "active_logger_id", "")
            set_status(db, "active_upload_profile", "")


async def scan_for_candidates(
    timeout_s: float,
    candidates: dict[str, LoggerAdvertisement],
) -> int:
    found = 0
    loop = asyncio.get_running_loop()
    eligible_seen = asyncio.Event()

    def on_detect(device, advertisement_data) -> None:
        nonlocal found
        candidate = parse_logger_advertisement(device, advertisement_data, time.monotonic())
        if candidate is None:
            return
        candidates[candidate.key] = candidate
        found += 1
        if candidate.transfer_eligible and not candidate.in_cooldown:
            loop.call_soon_threadsafe(eligible_seen.set)

    scanner = BleakScanner(detection_callback=on_detect)
    await scanner.start()
    try:
        try:
            await asyncio.wait_for(eligible_seen.wait(), timeout=timeout_s)
            await asyncio.sleep(min(1.0, timeout_s))
        except asyncio.TimeoutError:
            pass
    finally:
        await scanner.stop()

    return found


async def run(args: argparse.Namespace) -> None:
    data_dir = Path(args.data_dir)
    db = open_db(data_dir / "gateway.sqlite")
    candidates: dict[str, LoggerAdvertisement] = {}
    failure_backoff_until: dict[str, float] = {}
    started_ms = int(time.time() * 1000)
    db.execute(
        "update sessions set finished_ms=?, status=? where finished_ms is null",
        (started_ms, "interrupted"),
    )
    db.commit()
    set_status(db, "process_started_ms", str(started_ms))
    set_status(db, "last_error", "")
    set_status(db, "active_logger_id", "")
    set_status(db, "active_upload_profile", "")
    log(f"Gateway running. Data directory: {data_dir.resolve()}")
    while True:
        set_status(db, "last_scan_ms", str(int(time.time() * 1000)))
        log(f"Scanning for OpenLivestock loggers for {args.scan_timeout:g} seconds...")
        try:
            seen = await scan_for_candidates(args.scan_timeout, candidates)
        except BleakBluetoothNotAvailableError:
            message = (
                "Bluetooth adapter is not powered. Run "
                "`sudo systemctl restart bluetooth && bluetoothctl power on`, "
                "or reboot the Pi after setup."
            )
            log(message)
            set_status(db, "last_scan_result", "bluetooth unavailable")
            set_status(db, "last_error", message)
            await asyncio.sleep(30)
            continue
        candidate = select_candidate(candidates, failure_backoff_until, time.monotonic())
        if candidate is None:
            visible = len(candidates)
            now_s = time.monotonic()
            backoff = sum(1 for c in candidates.values() if failure_backoff_until.get(c.key, 0.0) > now_s)
            stale = sum(1 for c in candidates.values() if now_s - c.last_seen_s > CONNECT_CANDIDATE_MAX_AGE_S)
            cooldown = sum(1 for c in candidates.values() if c.in_cooldown)
            not_ready = sum(1 for c in candidates.values() if not c.transfer_eligible and not c.in_cooldown)
            detail = (
                f"{visible} visible, no ready logger "
                f"({backoff} backoff, {cooldown} cooldown, {stale} stale, {not_ready} not eligible)"
            )
            set_status(db, "last_scan_result", detail)
            if seen == 0 and visible == 0:
                log("No logger status advertisements found. Continuing to scan.")
            else:
                log(detail)
            continue

        set_status(db, "last_scan_result", f"servicing {candidate.logger_id}")
        set_status(db, "last_error", "")
        age_label = (
            "never"
            if candidate.never_downloaded
            else "unknown"
            if candidate.age_unknown
            else f"{candidate.age_minutes} min"
        )
        log(
            f"Selected logger {candidate.logger_id} "
            f"(age={age_label}, rssi={candidate.rssi})"
        )
        try:
            await handle_device(candidate.device, data_dir, expected_logger_id=candidate.logger_id)
            candidates.pop(candidate.key, None)
            failure_backoff_until.pop(candidate.key, None)
        except NotEligible as exc:
            log(str(exc))
            candidates.pop(candidate.key, None)
            set_status(db, "last_error", "")
            await asyncio.sleep(1)
        except PartialTransfer as exc:
            detail = str(exc) or exc.__class__.__name__
            message = f"gateway transfer paused: {detail}"
            log(message)
            set_status(db, "last_error", message)
            candidates.pop(candidate.key, None)
            await asyncio.sleep(1)
        except Exception as exc:
            detail = str(exc) or exc.__class__.__name__
            message = f"gateway transfer failed: {detail}"
            log(message)
            set_status(db, "last_error", message)
            failure_backoff_until[candidate.key] = time.monotonic() + FAILURE_BACKOFF_S
            candidates.pop(candidate.key, None)
            await asyncio.sleep(5)


def main() -> None:
    parser = argparse.ArgumentParser(description="OpenLivestock Raspberry Pi gateway")
    parser.add_argument("--data-dir", default="GatewayData")
    parser.add_argument("--scan-timeout", type=float, default=30.0)
    args = parser.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
