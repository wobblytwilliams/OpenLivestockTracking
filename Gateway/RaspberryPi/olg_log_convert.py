from __future__ import annotations

import argparse
import csv
import time
from collections import defaultdict
from pathlib import Path
from uuid import uuid4

from olg_log_format import CSV_HEADERS, format_csv_row, iter_blocks, segment_files, write_csvs


PARQUET_FLUSH_ROWS = 500_000


def partition_date(rows: list[dict]) -> str:
    for row in rows:
        unix_ms = int(row.get("unix_ms") or 0)
        if unix_ms > 0:
            return time.strftime("%Y-%m-%d", time.gmtime(unix_ms / 1000))
    return "unsynced"


def sd_to_csv(args: argparse.Namespace) -> None:
    root = Path(args.input)

    def blocks():
        for path in segment_files(root):
            with path.open("rb") as handle:
                yield from iter_blocks(handle)

    write_csvs(blocks(), Path(args.output))


def sd_to_parquet(args: argparse.Namespace) -> None:
    import pyarrow as pa
    import pyarrow.parquet as pq

    root = Path(args.input)
    output = Path(args.output)
    logger_id = args.logger_id
    buffers: dict[tuple[str, str], list[dict]] = defaultdict(list)

    def flush(key: tuple[str, str]) -> None:
        rows = buffers.pop(key, [])
        if not rows:
            return
        name, date = key
        out_dir = output / f"type={name}" / f"logger_id={logger_id}" / f"date={date}"
        out_dir.mkdir(parents=True, exist_ok=True)
        pq.write_table(pa.Table.from_pylist(rows), out_dir / f"part-{uuid4().hex}.parquet", compression="zstd")

    for path in segment_files(root):
        with path.open("rb") as handle:
            for block in iter_blocks(handle):
                for name, rows in block.rows.items():
                    if not rows:
                        continue
                    key = (name, partition_date(rows))
                    buffers[key].extend(rows)
                    if len(buffers[key]) >= PARQUET_FLUSH_ROWS:
                        flush(key)

    for key in list(buffers):
        flush(key)


def parquet_to_csv(args: argparse.Namespace) -> None:
    import pyarrow.dataset as ds

    root = Path(args.input)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)

    for name, headers in CSV_HEADERS.items():
        out_path = output / f"{name.upper()}.CSV"
        source = root / f"type={name}"
        with out_path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=headers)
            writer.writeheader()
            if not source.exists():
                continue

            dataset = ds.dataset(source, format="parquet", partitioning="hive")
            scanner = dataset.scanner(columns=headers)
            for batch in scanner.to_batches():
                for row in batch.to_pylist():
                    writer.writerow(format_csv_row(name, row))


def main() -> None:
    parser = argparse.ArgumentParser(description="OpenLivestock logger conversion tools")
    sub = parser.add_subparsers(required=True)

    sd = sub.add_parser("sd-to-csv", help="convert logger SD binary logs to CSV")
    sd.add_argument("--input", required=True, help="SD root or LOG directory")
    sd.add_argument("--output", required=True, help="output directory for ACC/GPS/BLE CSVs")
    sd.set_defaults(func=sd_to_csv)

    sd_pq = sub.add_parser("sd-to-parquet", help="convert logger SD binary logs to partitioned Parquet")
    sd_pq.add_argument("--input", required=True, help="SD root or LOG directory")
    sd_pq.add_argument("--output", required=True, help="output Gateway-style Parquet directory")
    sd_pq.add_argument("--logger-id", default="sdcard", help="logger ID to use in Parquet partitions")
    sd_pq.set_defaults(func=sd_to_parquet)

    pq = sub.add_parser("parquet-to-csv", help="export gateway Parquet data to CSV")
    pq.add_argument("--input", required=True, help="GatewayData/parquet directory")
    pq.add_argument("--output", required=True, help="output directory for ACC/GPS/BLE CSVs")
    pq.set_defaults(func=parquet_to_csv)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
