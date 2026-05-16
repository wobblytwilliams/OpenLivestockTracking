from __future__ import annotations

import argparse
import asyncio
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from uuid import uuid4

import pyarrow as pa
import pyarrow.parquet as pq
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakBluetoothNotAvailableError

from olg_log_format import BLOCK_HEADER, iter_blocks


SERVICE_UUID = "8f0a0001-4f4c-4747-4154-455741593031"
INFO_UUID = "8f0a0002-4f4c-4747-4154-455741593031"
CONTROL_UUID = "8f0a0003-4f4c-4747-4154-455741593031"
DATA_UUID = "8f0a0004-4f4c-4747-4154-455741593031"

CMD_PREPARE = bytes([1])
CMD_MANIFEST = bytes([2])
CMD_DONE = bytes([4])

MSG_STATUS = 1
MSG_MANIFEST = 2
MSG_CHUNK = 3

STATUS_OK = 0
STATUS_EOF = 1
STATUS_ERROR = 2

CONTROL_TIMEOUT_S = 20.0
STREAM_IDLE_TIMEOUT_S = 20.0


@dataclass
class Segment:
    index: int
    size: int
    active: bool


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


def logger_id_from_info(raw: bytes) -> str:
    if len(raw) < 16 or raw[:4] != b"OLGI":
        return "unknown"
    logger_id = int.from_bytes(raw[8:16], "little")
    return f"{logger_id:016X}"


def segment_offset(db: sqlite3.Connection, logger_id: str, segment: Segment) -> int:
    row = db.execute(
        "select offset from segments where logger_id=? and segment_index=?",
        (logger_id, segment.index),
    ).fetchone()
    if row is None:
        db.execute(
            "insert into segments(logger_id, segment_index, size, offset) values(?,?,?,0)",
            (logger_id, segment.index, segment.size),
        )
        db.commit()
        return 0
    if row[0] > segment.size:
        db.execute(
            "update segments set size=?, offset=0 where logger_id=? and segment_index=?",
            (segment.size, logger_id, segment.index),
        )
        db.commit()
        return 0
    db.execute(
        "update segments set size=? where logger_id=? and segment_index=?",
        (segment.size, logger_id, segment.index),
    )
    db.commit()
    return int(row[0])


def partition_date(rows: list[dict[str, Any]]) -> str:
    for row in rows:
        unix_ms = int(row.get("unix_ms") or 0)
        if unix_ms > 0:
            return time.strftime("%Y-%m-%d", time.gmtime(unix_ms / 1000))
    return "unsynced"


def write_parquet(parquet_root: Path, logger_id: str, rows_by_type: dict[str, list[dict]]) -> None:
    for name, rows in rows_by_type.items():
        if not rows:
            continue

        out_dir = parquet_root / name / f"logger_id={logger_id}" / f"date={partition_date(rows)}"
        out_dir.mkdir(parents=True, exist_ok=True)
        table = pa.Table.from_pylist(rows)
        pq.write_table(table, out_dir / f"part-{uuid4().hex}.parquet")


def commit_block(
    db: sqlite3.Connection,
    parquet_root: Path,
    logger_id: str,
    segment_index: int,
    block_offset: int,
    block_end: int,
    rows: dict[str, list[dict]],
) -> None:
    exists = db.execute(
        "select 1 from blocks where logger_id=? and segment_index=? and block_offset=?",
        (logger_id, segment_index, block_offset),
    ).fetchone()
    if exists is None:
        write_parquet(parquet_root, logger_id, rows)
        db.execute(
            "insert into blocks(logger_id, segment_index, block_offset, block_end) values(?,?,?,?)",
            (logger_id, segment_index, block_offset, block_end),
        )

    db.execute(
        "update segments set offset=max(offset, ?) where logger_id=? and segment_index=?",
        (block_end, logger_id, segment_index),
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
    parquet_root: Path,
    logger_id: str,
    segment: Segment,
    offset: int,
) -> None:
    assembler = BlockAssembler(offset)
    cmd = bytes([3]) + segment.index.to_bytes(2, "little") + offset.to_bytes(4, "little")
    await client.write_gatt_char(CONTROL_UUID, cmd, response=False)

    while True:
        msg = await next_gateway_msg(queue, STREAM_IDLE_TIMEOUT_S, "stream data")
        if not msg:
            continue
        if msg[0] == MSG_STATUS:
            if msg[1] in (STATUS_OK, STATUS_EOF):
                return
            raise RuntimeError("logger returned stream error")
        if msg[0] != MSG_CHUNK or len(msg) < 8:
            continue

        seg_index = int.from_bytes(msg[1:3], "little")
        chunk_offset = int.from_bytes(msg[3:7], "little")
        chunk_len = msg[7]
        payload = bytes(msg[8 : 8 + chunk_len])
        if seg_index != segment.index:
            continue

        for block_offset, block_end, rows in assembler.feed(chunk_offset, payload):
            commit_block(db, parquet_root, logger_id, segment.index, block_offset, block_end, rows)
            log(f"Committed segment {segment.index}: {block_end}/{segment.size} bytes")


async def handle_device(device, data_dir: Path) -> None:
    db = open_db(data_dir / "gateway.sqlite")
    parquet_root = data_dir / "parquet"
    queue: asyncio.Queue[bytes] = asyncio.Queue()
    set_status(db, "last_device_address", str(getattr(device, "address", "")))
    log(f"Connecting to logger at {getattr(device, 'address', 'unknown address')}")
    session: int | None = None
    logger_id = "unknown"

    async with BleakClient(device) as client:
        info = bytes(await client.read_gatt_char(INFO_UUID))
        logger_id = logger_id_from_info(info)
        log(f"Connected to logger {logger_id}")
        now_ms = int(time.time() * 1000)
        db.execute(
            "insert into loggers(logger_id, last_seen_ms) values(?, ?) "
            "on conflict(logger_id) do update set last_seen_ms=excluded.last_seen_ms",
            (logger_id, now_ms),
        )
        session = db.execute(
            "insert into sessions(logger_id, started_ms, status) values(?,?,?)",
            (logger_id, now_ms, "running"),
        ).lastrowid
        db.commit()
        set_status(db, "active_logger_id", logger_id)

        def on_data(_sender, data: bytearray) -> None:
            queue.put_nowait(bytes(data))

        try:
            await client.start_notify(DATA_UUID, on_data)
            await client.write_gatt_char(CONTROL_UUID, CMD_PREPARE, response=False)
            if await wait_status(queue) != STATUS_OK:
                raise RuntimeError("logger prepare failed")

            manifest = await read_manifest(client, queue)
            log(f"Logger {logger_id} has {len(manifest)} log segment(s)")

            for segment in manifest:
                offset = segment_offset(db, logger_id, segment)
                if offset < segment.size:
                    log(f"Downloading segment {segment.index}: {offset}/{segment.size} bytes")
                    await stream_segment(client, queue, db, parquet_root, logger_id, segment, offset)
                else:
                    log(f"Segment {segment.index} already downloaded ({segment.size} bytes)")

            await client.write_gatt_char(CONTROL_UUID, CMD_DONE, response=False)
            db.execute(
                "update sessions set finished_ms=?, status=? where id=?",
                (int(time.time() * 1000), "ok", session),
            )
            db.commit()
            set_status(db, "last_transfer_ok_ms", str(int(time.time() * 1000)))
            log(f"Transfer complete for logger {logger_id}")
        except Exception:
            if session is not None:
                db.execute(
                    "update sessions set finished_ms=?, status=? where id=?",
                    (int(time.time() * 1000), "failed", session),
                )
                db.commit()
            raise
        finally:
            set_status(db, "active_logger_id", "")


async def run(args: argparse.Namespace) -> None:
    data_dir = Path(args.data_dir)
    db = open_db(data_dir / "gateway.sqlite")
    set_status(db, "process_started_ms", str(int(time.time() * 1000)))
    set_status(db, "last_error", "")
    log(f"Gateway running. Data directory: {data_dir.resolve()}")
    while True:
        set_status(db, "last_scan_ms", str(int(time.time() * 1000)))
        log(f"Scanning for OpenLivestock loggers for {args.scan_timeout:g} seconds...")
        try:
            device = await BleakScanner.find_device_by_filter(
                lambda d, ad: SERVICE_UUID.lower() in [u.lower() for u in ad.service_uuids],
                timeout=args.scan_timeout,
            )
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
        if device is None:
            set_status(db, "last_scan_result", "no logger found")
            log("No logger found. Continuing to scan.")
            continue
        set_status(db, "last_scan_result", "logger found")
        log(f"Logger advertisement found: {getattr(device, 'address', 'unknown address')}")
        try:
            await handle_device(device, data_dir)
        except Exception as exc:
            message = f"gateway transfer failed: {exc}"
            log(message)
            set_status(db, "last_error", message)
            await asyncio.sleep(5)


def main() -> None:
    parser = argparse.ArgumentParser(description="OpenLivestock Raspberry Pi gateway")
    parser.add_argument("--data-dir", default="GatewayData")
    parser.add_argument("--scan-timeout", type=float, default=30.0)
    args = parser.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
