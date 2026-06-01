from __future__ import annotations

import argparse
import csv
import io
import os
import shutil
import sqlite3
import threading
import time
import zipfile
from datetime import date, datetime, time as datetime_time, timedelta, timezone
from pathlib import Path
from typing import Any
from uuid import uuid4
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from flask import Flask, Response, abort, redirect, render_template_string, request, send_file, url_for

from olg_log_format import CSV_HEADERS, format_csv_row


EXPORT_FORMATS = {"csv", "parquet"}
EXPORT_TYPES = tuple(CSV_HEADERS.keys())
CSV_BYTES_PER_ROW_DEFAULT = {"acc": 72, "gps": 56, "ble": 54}
PARQUET_BYTES_PER_ROW_DEFAULT = {"acc": 24, "gps": 24, "ble": 28}
DEFAULT_EXPORT_TIMEZONE = "Australia/Brisbane"


PAGE = """
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OpenLivestock Gateway</title>
  <style>
    :root {
      color-scheme: light;
      --ink: #16201b;
      --muted: #64706a;
      --line: #d8dfda;
      --panel: #ffffff;
      --field: #f4f7f5;
      --accent: #2d6a4f;
      --warn: #9a5a00;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: var(--field);
      color: var(--ink);
      line-height: 1.4;
    }
    header {
      padding: 18px 16px 12px;
      border-bottom: 1px solid var(--line);
      background: var(--panel);
    }
    main {
      width: min(1120px, 100%);
      margin: 0 auto;
      padding: 16px;
    }
    h1 {
      margin: 0;
      font-size: 1.35rem;
      letter-spacing: 0;
    }
    h2 {
      margin: 26px 0 10px;
      font-size: 1rem;
      letter-spacing: 0;
    }
    .sub {
      margin-top: 4px;
      color: var(--muted);
      font-size: 0.9rem;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
      gap: 10px;
    }
    .metric, .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
    }
    .metric {
      padding: 12px;
      min-height: 82px;
    }
    .label {
      color: var(--muted);
      font-size: 0.78rem;
      text-transform: uppercase;
    }
    .value {
      margin-top: 5px;
      font-size: 1.18rem;
      font-weight: 700;
      overflow-wrap: anywhere;
    }
    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin: 16px 0 4px;
    }
    a.button {
      min-height: 42px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 9px 14px;
      border-radius: 7px;
      background: var(--accent);
      color: #fff;
      text-decoration: none;
      font-weight: 700;
    }
    a.secondary {
      background: #33443c;
    }
    form.export {
      display: grid;
      gap: 12px;
      padding: 14px;
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
    }
    .form-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
      gap: 10px;
      align-items: end;
    }
    label {
      display: grid;
      gap: 5px;
      color: var(--muted);
      font-size: 0.82rem;
      font-weight: 700;
    }
    input, select {
      width: 100%;
      min-height: 40px;
      padding: 8px;
      border: 1px solid var(--line);
      border-radius: 7px;
      background: #fff;
      color: var(--ink);
      font: inherit;
    }
    .checks {
      display: flex;
      flex-wrap: wrap;
      gap: 12px;
      align-items: center;
    }
    .checks label {
      display: inline-flex;
      grid-template-columns: none;
      gap: 6px;
      align-items: center;
      color: var(--ink);
      font-weight: 600;
    }
    .checks input {
      width: auto;
      min-height: auto;
    }
    button {
      min-height: 42px;
      border: 0;
      border-radius: 7px;
      background: var(--accent);
      color: #fff;
      padding: 9px 14px;
      font-weight: 700;
      cursor: pointer;
    }
    button.secondary {
      background: #33443c;
    }
    .estimate {
      padding: 10px;
      border-radius: 7px;
      background: #eef5f1;
      color: var(--ink);
    }
    .hint {
      margin-top: -8px;
      color: var(--muted);
      font-size: 0.84rem;
    }
    .table-wrap {
      overflow-x: auto;
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.9rem;
    }
    th, td {
      padding: 9px 10px;
      border-bottom: 1px solid var(--line);
      text-align: left;
      white-space: nowrap;
    }
    th {
      color: var(--muted);
      font-size: 0.76rem;
      text-transform: uppercase;
      background: #f9fbfa;
    }
    tr:last-child td { border-bottom: 0; }
    .empty {
      padding: 14px;
      color: var(--muted);
    }
    .status-ok { color: var(--accent); font-weight: 700; }
    .status-warn { color: var(--warn); font-weight: 700; }
    footer {
      color: var(--muted);
      font-size: 0.82rem;
      padding: 12px 16px 22px;
      text-align: center;
    }
    @media (max-width: 640px) {
      main { padding: 12px; }
      .value { font-size: 1rem; }
      th, td { padding: 8px; }
    }
  </style>
</head>
<body>
  <header>
    <h1>OpenLivestock Gateway</h1>
    <div class="sub">Local diagnostics from {{ host }} at {{ generated }}</div>
  </header>
  <main>
    <div class="grid">
      <div class="metric">
        <div class="label">Gateway</div>
        <div class="value {{ 'status-ok' if gateway_age_ok else 'status-warn' }}">{{ gateway_state }}</div>
      </div>
      <div class="metric">
        <div class="label">Loggers</div>
        <div class="value">{{ logger_count }}</div>
      </div>
      <div class="metric">
        <div class="label">Parquet Rows</div>
        <div class="value">{{ total_rows }}</div>
      </div>
      <div class="metric">
        <div class="label">Storage Free</div>
        <div class="value">{{ free_space }}</div>
      </div>
    </div>

    <h2>Prepare Data Export</h2>
    <form class="export" method="post" action="/exports/prepare" id="export-form">
      <div class="form-grid">
        <label>Logger
          <select name="logger_id" id="logger_id">
            <option value="all">All loggers</option>
            {% for row in loggers %}
            <option value="{{ row.logger_id }}">{{ row.logger_id }}</option>
            {% endfor %}
          </select>
        </label>
        <label>Start date
          <input type="date" name="start_date" id="start_date" value="{{ today }}">
        </label>
        <label>End date
          <input type="date" name="end_date" id="end_date" value="{{ today }}">
        </label>
        <label>Format
          <select name="format" id="format">
            <option value="csv">CSV ZIP</option>
            <option value="parquet">Parquet ZIP</option>
          </select>
        </label>
      </div>
      <div class="hint">Export dates use gateway local time: {{ export_timezone }}.</div>
      <div class="checks">
        <label><input type="checkbox" name="types" value="acc"> ACC</label>
        <label><input type="checkbox" name="types" value="gps" checked> GPS</label>
        <label><input type="checkbox" name="types" value="ble" checked> BLE</label>
      </div>
      <div class="estimate" id="estimate">Choose a date range to estimate export size.</div>
      <div class="actions">
        <button type="submit">Prepare Export</button>
        <a class="button secondary" href="/status.json">Status JSON</a>
      </div>
    </form>

    <h2>Prepared Exports</h2>
    <div class="table-wrap">
      {% if jobs %}
      <table>
        <thead><tr><th>Created</th><th>Request</th><th>Status</th><th>Size</th><th>Actions</th></tr></thead>
        <tbody>
          {% for job in jobs %}
          <tr>
            <td>{{ job.created }}</td>
            <td>{{ job.request }}</td>
            <td>{{ job.status }}</td>
            <td>{{ job.size }}</td>
            <td>
              {% if job.ready %}
              <a href="/exports/{{ job.id }}/download">Download</a>
              <form method="post" action="/exports/{{ job.id }}/copy-usb" style="display:inline">
                <button class="secondary" type="submit">Copy To USB</button>
              </form>
              {% else %}
              -
              {% endif %}
            </td>
          </tr>
          {% endfor %}
        </tbody>
      </table>
      {% else %}
      <div class="empty">No exports have been prepared yet.</div>
      {% endif %}
    </div>

    <h2>Gateway Status</h2>
    <div class="table-wrap">
      {% if status %}
      <table>
        <thead><tr><th>Key</th><th>Value</th><th>Updated</th></tr></thead>
        <tbody>
          {% for row in status %}
          <tr><td>{{ row.key }}</td><td>{{ row.value }}</td><td>{{ row.updated }}</td></tr>
          {% endfor %}
        </tbody>
      </table>
      {% else %}
      <div class="empty">No gateway status has been written yet.</div>
      {% endif %}
    </div>

    <h2>Loggers</h2>
    <div class="table-wrap">
      {% if loggers %}
      <table>
        <thead><tr><th>Logger</th><th>Last Seen</th><th>Segments</th><th>Downloaded</th></tr></thead>
        <tbody>
          {% for row in loggers %}
          <tr>
            <td>{{ row.logger_id }}</td>
            <td>{{ row.last_seen }}</td>
            <td>{{ row.segments }}</td>
            <td>{{ row.downloaded }}</td>
          </tr>
          {% endfor %}
        </tbody>
      </table>
      {% else %}
      <div class="empty">No logger has connected yet.</div>
      {% endif %}
    </div>

    <h2>Recent Sessions</h2>
    <div class="table-wrap">
      {% if sessions %}
      <table>
        <thead><tr><th>Logger</th><th>Started</th><th>Finished</th><th>Status</th></tr></thead>
        <tbody>
          {% for row in sessions %}
          <tr>
            <td>{{ row.logger_id }}</td>
            <td>{{ row.started }}</td>
            <td>{{ row.finished }}</td>
            <td>{{ row.status }}</td>
          </tr>
          {% endfor %}
        </tbody>
      </table>
      {% else %}
      <div class="empty">No transfer sessions yet.</div>
      {% endif %}
    </div>

    <h2>Rows Available</h2>
    <div class="table-wrap">
      <table>
        <thead><tr><th>Table</th><th>Rows</th></tr></thead>
        <tbody>
          {% for name, count in row_counts.items() %}
          <tr><td>{{ name.upper() }}</td><td>{{ count }}</td></tr>
          {% endfor %}
        </tbody>
      </table>
    </div>
  </main>
  <footer>Connect to the Raspberry Pi hotspot and open this page from your phone.</footer>
  <script>
    const form = document.getElementById('export-form');
    const estimate = document.getElementById('estimate');
    function selectedTypes() {
      return Array.from(form.querySelectorAll('input[name="types"]:checked')).map(el => el.value);
    }
    async function refreshEstimate() {
      const params = new URLSearchParams({
        logger_id: document.getElementById('logger_id').value,
        start_date: document.getElementById('start_date').value,
        end_date: document.getElementById('end_date').value,
        types: selectedTypes().join(',')
      });
      if (!params.get('start_date') || !params.get('end_date') || !params.get('types')) {
        estimate.textContent = 'Choose a date range and at least one data type.';
        return;
      }
      const response = await fetch('/api/export/estimate?' + params.toString());
      const data = await response.json();
      estimate.textContent = `Days selected: ${data.days}. Estimated CSV: ${data.csv_size}. Estimated Parquet: ${data.parquet_size}. ${data.recommendation}`;
    }
    form.addEventListener('change', refreshEstimate);
    refreshEstimate();
  </script>
</body>
</html>
"""


def db_path(data_dir: Path) -> Path:
    return data_dir / "gateway.sqlite"


def connect_db(data_dir: Path) -> sqlite3.Connection:
    data_dir.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(db_path(data_dir))
    db.row_factory = sqlite3.Row
    ensure_schema(db)
    return db


def ensure_schema(db: sqlite3.Connection) -> None:
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
        create table if not exists export_jobs (
            id text primary key,
            created_ms integer not null,
            finished_ms integer,
            status text not null,
            logger_id text not null,
            start_date text not null,
            end_date text not null,
            types text not null,
            format text not null,
            path text,
            size_bytes integer not null default 0,
            message text not null default ''
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


def fmt_ms(value: Any) -> str:
    if value in (None, "", 0):
        return "-"
    try:
        seconds = int(value) / 1000
    except (TypeError, ValueError):
        return str(value)
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(seconds))


def fmt_bytes(value: int) -> str:
    units = ["B", "KB", "MB", "GB", "TB"]
    size = float(value)
    for unit in units:
        if size < 1024 or unit == units[-1]:
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} B"
        size /= 1024
    return f"{value} B"


def parquet_root(data_dir: Path) -> Path:
    return data_dir / "parquet"


def exports_root(data_dir: Path) -> Path:
    return data_dir / "exports"


def parse_date(value: str) -> date:
    return datetime.strptime(value, "%Y-%m-%d").date()


def date_strings(start: str, end: str) -> list[str]:
    start_date = parse_date(start)
    end_date = parse_date(end)
    if end_date < start_date:
        raise ValueError("end date is before start date")
    days = (end_date - start_date).days + 1
    return [(start_date + timedelta(days=i)).isoformat() for i in range(days)]


def export_timezone() -> ZoneInfo:
    name = os.environ.get("OLG_EXPORT_TIMEZONE", DEFAULT_EXPORT_TIMEZONE)
    try:
        return ZoneInfo(name)
    except ZoneInfoNotFoundError:
        return ZoneInfo("UTC")


def local_date_range_ms(start: str, end: str) -> tuple[int, int]:
    tz = export_timezone()
    start_date = parse_date(start)
    end_date = parse_date(end)
    if end_date < start_date:
        raise ValueError("end date is before start date")
    start_dt = datetime.combine(start_date, datetime_time.min, tzinfo=tz)
    end_dt = datetime.combine(end_date + timedelta(days=1), datetime_time.min, tzinfo=tz)
    start_ms = int(start_dt.astimezone(timezone.utc).timestamp() * 1000)
    end_ms = int(end_dt.astimezone(timezone.utc).timestamp() * 1000)
    return start_ms, end_ms


def utc_partition_dates_for_local_range(start: str, end: str) -> list[str]:
    start_ms, end_ms = local_date_range_ms(start, end)
    start_date = datetime.fromtimestamp(start_ms / 1000, timezone.utc).date()
    end_date = datetime.fromtimestamp((end_ms - 1) / 1000, timezone.utc).date()
    days = (end_date - start_date).days + 1
    return [(start_date + timedelta(days=i)).isoformat() for i in range(days)]


def read_status(db: sqlite3.Connection) -> list[dict[str, str]]:
    rows = db.execute(
        "select key, value, updated_ms from gateway_status order by key"
    ).fetchall()
    return [
        {"key": row["key"], "value": row["value"], "updated": fmt_ms(row["updated_ms"])}
        for row in rows
    ]


def logger_rows(db: sqlite3.Connection) -> list[dict[str, str]]:
    rows = db.execute(
        """
        select
            l.logger_id,
            l.last_seen_ms,
            count(s.segment_index) as segments,
            coalesce(sum(s.offset), 0) as downloaded
        from loggers l
        left join segment_offsets s on s.logger_id = l.logger_id
        group by l.logger_id, l.last_seen_ms
        order by l.last_seen_ms desc
        """
    ).fetchall()
    return [
        {
            "logger_id": row["logger_id"],
            "last_seen": fmt_ms(row["last_seen_ms"]),
            "segments": str(row["segments"]),
            "downloaded": fmt_bytes(int(row["downloaded"] or 0)),
        }
        for row in rows
    ]


def session_rows(db: sqlite3.Connection) -> list[dict[str, str]]:
    rows = db.execute(
        """
        select logger_id, started_ms, finished_ms, status
        from sessions
        order by started_ms desc
        limit 12
        """
    ).fetchall()
    return [
        {
            "logger_id": row["logger_id"],
            "started": fmt_ms(row["started_ms"]),
            "finished": fmt_ms(row["finished_ms"]),
            "status": row["status"],
        }
        for row in rows
    ]


def status_value(db: sqlite3.Connection, key: str) -> str | None:
    row = db.execute("select value from gateway_status where key=?", (key,)).fetchone()
    return None if row is None else str(row["value"])


def status_updated_ms(db: sqlite3.Connection, key: str) -> int | None:
    row = db.execute("select updated_ms from gateway_status where key=?", (key,)).fetchone()
    return None if row is None else int(row["updated_ms"])


def row_counts(data_dir: Path, db: sqlite3.Connection) -> dict[str, int]:
    counts = {name: 0 for name in CSV_HEADERS}
    rows = db.execute("select type, coalesce(sum(rows), 0) as rows from row_counts group by type").fetchall()
    for row in rows:
        if row["type"] in counts:
            counts[row["type"]] = int(row["rows"] or 0)
    return counts


def estimate_export(
    db: sqlite3.Connection,
    logger_id: str,
    start_date: str,
    end_date: str,
    types: list[str],
) -> dict[str, Any]:
    try:
        dates = date_strings(start_date, end_date)
    except ValueError:
        return {
            "days": 0,
            "rows": 0,
            "csv_bytes": 0,
            "parquet_bytes": 0,
            "csv_size": fmt_bytes(0),
            "parquet_size": fmt_bytes(0),
            "recommendation": "Check the date range.",
        }

    rows_total = 0
    csv_bytes = 0
    parquet_bytes = 0
    partition_dates = utc_partition_dates_for_local_range(start_date, end_date)
    for name in types:
        if name not in CSV_HEADERS:
            continue
        params: list[Any] = [name, partition_dates[0], partition_dates[-1]]
        logger_clause = ""
        if logger_id != "all":
            logger_clause = " and logger_id=?"
            params.append(logger_id)
        row = db.execute(
            "select coalesce(sum(rows), 0) as rows, coalesce(sum(parquet_bytes), 0) as parquet_bytes "
            "from row_counts where type=? and date>=? and date<=?" + logger_clause,
            params,
        ).fetchone()
        rows = int(row["rows"] or 0)
        observed_parquet = int(row["parquet_bytes"] or 0)
        rows_total += rows
        csv_bytes += rows * CSV_BYTES_PER_ROW_DEFAULT[name]
        parquet_bytes += observed_parquet if observed_parquet > 0 else rows * PARQUET_BYTES_PER_ROW_DEFAULT[name]

    if csv_bytes < 500 * 1024 * 1024:
        recommendation = "CSV should be phone-friendly."
    elif csv_bytes < 10 * 1024 * 1024 * 1024:
        recommendation = "Use a laptop for CSV, or choose Parquet for a lighter download."
    else:
        recommendation = "Prepare this on the Pi and copy to a USB SSD."

    return {
        "days": len(dates),
        "rows": rows_total,
        "csv_bytes": csv_bytes,
        "parquet_bytes": parquet_bytes,
        "csv_size": fmt_bytes(csv_bytes),
        "parquet_size": fmt_bytes(parquet_bytes),
        "recommendation": recommendation,
    }


def parquet_dataset(data_dir: Path, name: str):
    import pyarrow.dataset as ds

    root = parquet_root(data_dir) / f"type={name}"
    if not root.exists():
        return None
    return ds.dataset(root, format="parquet", partitioning="hive")


def export_filter(logger_id: str, start_date: str, end_date: str):
    import pyarrow.dataset as ds

    start_ms, end_ms = local_date_range_ms(start_date, end_date)
    partition_dates = utc_partition_dates_for_local_range(start_date, end_date)
    expr = (
        (ds.field("date") >= partition_dates[0])
        & (ds.field("date") <= partition_dates[-1])
        & (ds.field("unix_ms") >= start_ms)
        & (ds.field("unix_ms") < end_ms)
    )
    if logger_id != "all":
        expr = expr & (ds.field("logger_id") == logger_id)
    return expr


def write_csv_export(data_dir: Path, out_path: Path, logger_id: str, start_date: str, end_date: str, types: list[str]) -> None:
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for name in types:
            if name not in CSV_HEADERS:
                continue
            dataset = parquet_dataset(data_dir, name)
            with zf.open(f"{name.upper()}.CSV", "w") as raw:
                with io.TextIOWrapper(raw, encoding="utf-8", newline="") as text:
                    writer = csv.DictWriter(text, fieldnames=CSV_HEADERS[name])
                    writer.writeheader()
                    if dataset is None:
                        continue
                    scanner = dataset.scanner(
                        filter=export_filter(logger_id, start_date, end_date),
                        columns=CSV_HEADERS[name],
                    )
                    for batch in scanner.to_batches():
                        for row in batch.to_pylist():
                            writer.writerow(format_csv_row(name, row))


def logger_ids_for_export(db: sqlite3.Connection, logger_id: str) -> list[str]:
    if logger_id != "all":
        return [logger_id]
    rows = db.execute("select logger_id from loggers order by logger_id").fetchall()
    return [str(row["logger_id"]) for row in rows]


def write_parquet_export(data_dir: Path, db: sqlite3.Connection, out_path: Path, logger_id: str, start_date: str, end_date: str, types: list[str]) -> None:
    root = parquet_root(data_dir)
    dates = utc_partition_dates_for_local_range(start_date, end_date)
    loggers = logger_ids_for_export(db, logger_id)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for name in types:
            if name not in CSV_HEADERS:
                continue
            for selected_logger in loggers:
                for selected_date in dates:
                    part_dir = root / f"type={name}" / f"logger_id={selected_logger}" / f"date={selected_date}"
                    if not part_dir.exists():
                        continue
                    for path in sorted(part_dir.glob("*.parquet")):
                        zf.write(path, path.relative_to(root))


def export_jobs(db: sqlite3.Connection) -> list[dict[str, Any]]:
    rows = db.execute(
        """
        select id, created_ms, status, logger_id, start_date, end_date, types, format,
               path, size_bytes, message
        from export_jobs
        order by created_ms desc
        limit 12
        """
    ).fetchall()
    jobs = []
    for row in rows:
        request_text = (
            f"{row['logger_id']} {row['start_date']} to {row['end_date']} "
            f"{row['types']} {row['format'].upper()}"
        )
        jobs.append(
            {
                "id": row["id"],
                "created": fmt_ms(row["created_ms"]),
                "request": request_text,
                "status": row["status"] if not row["message"] else f"{row['status']}: {row['message']}",
                "size": fmt_bytes(int(row["size_bytes"] or 0)),
                "ready": row["status"] == "ready" and row["path"],
            }
        )
    return jobs


def run_export_job(data_dir: Path, job_id: str) -> None:
    db = connect_db(data_dir)
    try:
        job = db.execute("select * from export_jobs where id=?", (job_id,)).fetchone()
        if job is None:
            return
        out_dir = exports_root(data_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        suffix = "csv.zip" if job["format"] == "csv" else "parquet.zip"
        out_path = out_dir / f"olg-export-{job_id}.{suffix}"
        types = [name for name in str(job["types"]).split(",") if name in CSV_HEADERS]
        db.execute("update export_jobs set status=?, message=? where id=?", ("running", "", job_id))
        db.commit()
        if job["format"] == "csv":
            write_csv_export(data_dir, out_path, job["logger_id"], job["start_date"], job["end_date"], types)
        else:
            write_parquet_export(data_dir, db, out_path, job["logger_id"], job["start_date"], job["end_date"], types)
        db.execute(
            "update export_jobs set status=?, finished_ms=?, path=?, size_bytes=?, message=? where id=?",
            ("ready", int(time.time() * 1000), str(out_path), out_path.stat().st_size, "", job_id),
        )
        db.commit()
    except Exception as exc:
        db.execute(
            "update export_jobs set status=?, finished_ms=?, message=? where id=?",
            ("failed", int(time.time() * 1000), str(exc), job_id),
        )
        db.commit()
    finally:
        db.close()


def usb_candidates() -> list[Path]:
    candidates: list[Path] = []
    for root in [Path("/media"), Path("/mnt")]:
        if not root.exists():
            continue
        for path in root.iterdir():
            if path.is_dir() and os.access(path, os.W_OK):
                candidates.append(path)
            if path.is_dir():
                try:
                    children = list(path.iterdir())
                except OSError:
                    continue
                for child in children:
                    if child.is_dir() and os.access(child, os.W_OK):
                        candidates.append(child)
    return sorted(candidates, key=lambda path: shutil.disk_usage(path).free, reverse=True)


def create_app(data_dir: Path) -> Flask:
    app = Flask(__name__)
    export_tz = export_timezone()
    today_text = lambda: datetime.now(export_tz).date().isoformat()

    @app.get("/")
    def index() -> str:
        db = connect_db(data_dir)
        try:
            counts = row_counts(data_dir, db)
            last_scan = status_updated_ms(db, "last_scan_ms")
            now_ms = int(time.time() * 1000)
            gateway_age_ok = last_scan is not None and (now_ms - last_scan) < 180000
            gateway_state = "Active" if gateway_age_ok else "No heartbeat"
            free = shutil.disk_usage(data_dir).free if data_dir.exists() else shutil.disk_usage(".").free
            return render_template_string(
                PAGE,
                host=os.uname().nodename if hasattr(os, "uname") else "gateway",
                generated=time.strftime("%Y-%m-%d %H:%M:%S"),
                gateway_state=gateway_state,
                gateway_age_ok=gateway_age_ok,
                logger_count=db.execute("select count(*) from loggers").fetchone()[0],
                total_rows=sum(counts.values()),
                free_space=fmt_bytes(free),
                status=read_status(db),
                loggers=logger_rows(db),
                sessions=session_rows(db),
                row_counts=counts,
                jobs=export_jobs(db),
                today=today_text(),
                export_timezone=str(export_tz),
            )
        finally:
            db.close()

    @app.get("/status.json")
    def status_json() -> dict[str, Any]:
        db = connect_db(data_dir)
        try:
            counts = row_counts(data_dir, db)
            return {
                "generated_ms": int(time.time() * 1000),
                "status": read_status(db),
                "loggers": logger_rows(db),
                "sessions": session_rows(db),
                "row_counts": counts,
                "last_error": status_value(db, "last_error") or "",
            }
        finally:
            db.close()

    @app.get("/api/export/estimate")
    def estimate_export_api() -> dict[str, Any]:
        db = connect_db(data_dir)
        try:
            types = [name for name in request.args.get("types", "").split(",") if name in CSV_HEADERS]
            return estimate_export(
                db,
                request.args.get("logger_id", "all"),
                request.args.get("start_date", today_text()),
                request.args.get("end_date", today_text()),
                types,
            )
        finally:
            db.close()

    @app.post("/exports/prepare")
    def prepare_export() -> Response:
        logger_id = request.form.get("logger_id", "all")
        start_date = request.form.get("start_date", today_text())
        end_date = request.form.get("end_date", today_text())
        export_format = request.form.get("format", "csv")
        selected_types = [name for name in request.form.getlist("types") if name in CSV_HEADERS]
        if export_format not in EXPORT_FORMATS or not selected_types:
            abort(400)
        try:
            date_strings(start_date, end_date)
        except ValueError:
            abort(400)

        job_id = uuid4().hex
        db = connect_db(data_dir)
        try:
            db.execute(
                "insert into export_jobs(id, created_ms, status, logger_id, start_date, end_date, types, format) "
                "values(?,?,?,?,?,?,?,?)",
                (
                    job_id,
                    int(time.time() * 1000),
                    "queued",
                    logger_id,
                    start_date,
                    end_date,
                    ",".join(selected_types),
                    export_format,
                ),
            )
            db.commit()
        finally:
            db.close()

        thread = threading.Thread(target=run_export_job, args=(data_dir, job_id), daemon=True)
        thread.start()
        return redirect(url_for("index"))

    @app.get("/exports/<job_id>/download")
    def download_export(job_id: str) -> Response:
        db = connect_db(data_dir)
        try:
            job = db.execute("select * from export_jobs where id=?", (job_id,)).fetchone()
            if job is None or job["status"] != "ready" or not job["path"]:
                abort(404)
            path = Path(job["path"])
            if not path.exists():
                abort(404)
            return send_file(path, mimetype="application/zip", as_attachment=True, download_name=path.name)
        finally:
            db.close()

    @app.post("/exports/<job_id>/copy-usb")
    def copy_export_usb(job_id: str) -> Response:
        db = connect_db(data_dir)
        try:
            job = db.execute("select * from export_jobs where id=?", (job_id,)).fetchone()
            if job is None or job["status"] != "ready" or not job["path"]:
                abort(404)
            source = Path(job["path"])
            targets = usb_candidates()
            if not targets:
                db.execute(
                    "update export_jobs set message=? where id=?",
                    ("No writable USB drive found under /media or /mnt", job_id),
                )
                db.commit()
                return redirect(url_for("index"))
            target = targets[0] / source.name
            shutil.copy2(source, target)
            if hasattr(os, "sync"):
                os.sync()
            db.execute(
                "update export_jobs set message=? where id=?",
                (f"Copied to {target}. Safe to remove after the drive light stops.", job_id),
            )
            db.commit()
            return redirect(url_for("index"))
        finally:
            db.close()

    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="OpenLivestock local gateway dashboard")
    parser.add_argument("--data-dir", default="GatewayData")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    app = create_app(Path(args.data_dir))
    app.run(host=args.host, port=args.port)


if __name__ == "__main__":
    main()
