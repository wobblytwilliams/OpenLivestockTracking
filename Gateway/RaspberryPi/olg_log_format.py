from __future__ import annotations

import csv
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator


BLOCK_MAGIC = 0x31474C4F
BLOCK_VERSION = 1
BLOCK_HEADER = struct.Struct("<IHHIIHHI")

REC_ACC = 1
REC_GPS = 2
REC_BLE = 3

ACC_BODY_LEN = 18
GPS_BODY_LEN = 20
BLE_BODY_LEN = 19

ACC_RECORD = struct.Struct("<BBIQhhh")
GPS_RECORD = struct.Struct("<BBIQff")
BLE_RECORD = struct.Struct("<BBIQ6sb")

CSV_HEADERS = {
    "acc": ["ms", "unix_ms", "x_g", "y_g", "z_g"],
    "gps": ["ms", "unix_ms", "lat", "lon"],
    "ble": ["ms", "unix_ms", "mac", "rssi"],
}


@dataclass(frozen=True)
class Block:
    sequence: int
    offset: int
    end_offset: int
    record_count: int
    rows: dict[str, list[dict]]


class LogFormatError(Exception):
    pass


def mac_text(raw: bytes) -> str:
    return ":".join(f"{b:02X}" for b in raw)


def parse_payload(payload: bytes) -> dict[str, list[dict]]:
    rows: dict[str, list[dict]] = {"acc": [], "gps": [], "ble": []}
    pos = 0

    while pos + 2 <= len(payload):
        rec_type = payload[pos]
        rec_len = payload[pos + 1]
        end = pos + 2 + rec_len
        if end > len(payload):
            raise LogFormatError(f"truncated record at payload offset {pos}")

        rec = payload[pos:end]
        if rec_type == REC_ACC and rec_len == ACC_BODY_LEN:
            _, _, ms, unix_ms, x, y, z = ACC_RECORD.unpack(rec)
            rows["acc"].append(
                {
                    "ms": ms,
                    "unix_ms": unix_ms,
                    "x_g": x * 0.0039,
                    "y_g": y * 0.0039,
                    "z_g": z * 0.0039,
                }
            )
        elif rec_type == REC_GPS and rec_len == GPS_BODY_LEN:
            _, _, ms, unix_ms, lat, lon = GPS_RECORD.unpack(rec)
            rows["gps"].append({"ms": ms, "unix_ms": unix_ms, "lat": lat, "lon": lon})
        elif rec_type == REC_BLE and rec_len == BLE_BODY_LEN:
            _, _, ms, unix_ms, mac, rssi = BLE_RECORD.unpack(rec)
            rows["ble"].append(
                {"ms": ms, "unix_ms": unix_ms, "mac": mac_text(mac), "rssi": rssi}
            )
        else:
            raise LogFormatError(f"unknown record type={rec_type} len={rec_len}")

        pos = end

    if pos != len(payload):
        raise LogFormatError("payload ended with a partial record header")

    return rows


def iter_blocks(file: BinaryIO, *, start_offset: int = 0) -> Iterator[Block]:
    file.seek(start_offset)
    offset = start_offset

    while True:
        header_bytes = file.read(BLOCK_HEADER.size)
        if not header_bytes:
            return
        if len(header_bytes) != BLOCK_HEADER.size:
            print(f"truncated block header at byte {offset}", file=sys.stderr)
            return

        magic, version, header_len, sequence, payload_len, count, _reserved, crc = (
            BLOCK_HEADER.unpack(header_bytes)
        )
        if magic != BLOCK_MAGIC or version != BLOCK_VERSION or header_len != BLOCK_HEADER.size:
            print(f"bad block header at byte {offset}", file=sys.stderr)
            return

        payload = file.read(payload_len)
        if len(payload) != payload_len:
            print(f"truncated block payload at byte {offset}", file=sys.stderr)
            return

        actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
        if actual_crc != crc:
            print(f"CRC mismatch at byte {offset}", file=sys.stderr)
            return

        rows = parse_payload(payload)
        yield Block(
            sequence=sequence,
            offset=offset,
            end_offset=offset + BLOCK_HEADER.size + payload_len,
            record_count=count,
            rows=rows,
        )
        offset += BLOCK_HEADER.size + payload_len


def segment_files(root: Path) -> list[Path]:
    log_dir = root / "LOG"
    if log_dir.exists():
        root = log_dir
    return sorted(root.glob("OLG*.BIN"))


def write_csvs(blocks: Iterable[Block], output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    files = {
        "acc": (output / "ACC.CSV").open("w", newline=""),
        "gps": (output / "GPS.CSV").open("w", newline=""),
        "ble": (output / "BLE.CSV").open("w", newline=""),
    }
    try:
        writers = {
            name: csv.DictWriter(handle, fieldnames=CSV_HEADERS[name])
            for name, handle in files.items()
        }
        for writer in writers.values():
            writer.writeheader()

        for block in blocks:
            for name, rows in block.rows.items():
                for row in rows:
                    writers[name].writerow(format_csv_row(name, row))
    finally:
        for handle in files.values():
            handle.close()


def format_csv_row(name: str, row: dict) -> dict:
    if name == "acc":
        return {
            "ms": int(row["ms"]),
            "unix_ms": int(row["unix_ms"]),
            "x_g": f"{float(row['x_g']):.6f}",
            "y_g": f"{float(row['y_g']):.6f}",
            "z_g": f"{float(row['z_g']):.6f}",
        }
    if name == "gps":
        return {
            "ms": int(row["ms"]),
            "unix_ms": int(row["unix_ms"]),
            "lat": f"{float(row['lat']):.6f}",
            "lon": f"{float(row['lon']):.6f}",
        }
    return {
        "ms": int(row["ms"]),
        "unix_ms": int(row["unix_ms"]),
        "mac": row["mac"],
        "rssi": int(row["rssi"]),
    }
