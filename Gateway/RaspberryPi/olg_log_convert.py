from __future__ import annotations

import argparse
import csv
from pathlib import Path

from olg_log_format import CSV_HEADERS, format_csv_row, iter_blocks, segment_files, write_csvs


def sd_to_csv(args: argparse.Namespace) -> None:
    root = Path(args.input)

    def blocks():
        for path in segment_files(root):
            with path.open("rb") as handle:
                yield from iter_blocks(handle)

    write_csvs(blocks(), Path(args.output))


def parquet_to_csv(args: argparse.Namespace) -> None:
    import pyarrow.dataset as ds

    root = Path(args.input)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)

    for name, headers in CSV_HEADERS.items():
        source = root / name
        out_path = output / f"{name.upper()}.CSV"
        with out_path.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=headers)
            writer.writeheader()
            if not source.exists():
                continue

            table = ds.dataset(source, format="parquet", partitioning="hive").to_table()
            for row in table.to_pylist():
                writer.writerow(format_csv_row(name, row))


def main() -> None:
    parser = argparse.ArgumentParser(description="OpenLivestock logger conversion tools")
    sub = parser.add_subparsers(required=True)

    sd = sub.add_parser("sd-to-csv", help="convert logger SD binary logs to CSV")
    sd.add_argument("--input", required=True, help="SD root or LOG directory")
    sd.add_argument("--output", required=True, help="output directory for ACC/GPS/BLE CSVs")
    sd.set_defaults(func=sd_to_csv)

    pq = sub.add_parser("parquet-to-csv", help="export gateway Parquet data to CSV")
    pq.add_argument("--input", required=True, help="GatewayData/parquet directory")
    pq.add_argument("--output", required=True, help="output directory for ACC/GPS/BLE CSVs")
    pq.set_defaults(func=parquet_to_csv)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
