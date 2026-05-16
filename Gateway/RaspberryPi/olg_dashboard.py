from __future__ import annotations

import argparse
import csv
import io
import os
import shutil
import sqlite3
import time
import zipfile
from pathlib import Path
from typing import Any

from flask import Flask, Response, abort, render_template_string, send_file

from olg_log_format import CSV_HEADERS, format_csv_row


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

    <div class="actions">
      <a class="button" href="/download/csv.zip">Download CSV ZIP</a>
      <a class="button secondary" href="/status.json">Status JSON</a>
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
        left join segments s on s.logger_id = l.logger_id
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


def row_counts(data_dir: Path) -> dict[str, int]:
    counts = {name: 0 for name in CSV_HEADERS}
    root = parquet_root(data_dir)
    if not root.exists():
        return counts

    import pyarrow.dataset as ds

    for name in counts:
        source = root / name
        if source.exists():
            counts[name] = ds.dataset(source, format="parquet", partitioning="hive").count_rows()
    return counts


def iter_parquet_rows(data_dir: Path, name: str) -> list[dict[str, Any]]:
    source = parquet_root(data_dir) / name
    if not source.exists():
        return []

    import pyarrow.dataset as ds

    table = ds.dataset(source, format="parquet", partitioning="hive").to_table()
    rows = table.to_pylist()
    rows.sort(key=lambda row: (str(row.get("logger_id", "")), int(row.get("ms") or 0)))
    return rows


def csv_bytes(data_dir: Path, name: str) -> bytes:
    text = io.StringIO()
    writer = csv.DictWriter(text, fieldnames=CSV_HEADERS[name])
    writer.writeheader()
    for row in iter_parquet_rows(data_dir, name):
        writer.writerow(format_csv_row(name, row))
    return text.getvalue().encode("utf-8")


def create_csv_zip(data_dir: Path) -> io.BytesIO:
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for name in CSV_HEADERS:
            zf.writestr(f"{name.upper()}.CSV", csv_bytes(data_dir, name))
    out.seek(0)
    return out


def create_app(data_dir: Path) -> Flask:
    app = Flask(__name__)

    @app.get("/")
    def index() -> str:
        db = connect_db(data_dir)
        try:
            counts = row_counts(data_dir)
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
            )
        finally:
            db.close()

    @app.get("/status.json")
    def status_json() -> dict[str, Any]:
        db = connect_db(data_dir)
        try:
            counts = row_counts(data_dir)
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

    @app.get("/download/csv.zip")
    def download_zip() -> Response:
        return send_file(
            create_csv_zip(data_dir),
            mimetype="application/zip",
            as_attachment=True,
            download_name="OpenLivestockGateway_CSV.zip",
        )

    @app.get("/download/<name>.csv")
    def download_one(name: str) -> Response:
        key = name.lower()
        if key not in CSV_HEADERS:
            abort(404)
        return Response(
            csv_bytes(data_dir, key),
            mimetype="text/csv",
            headers={"Content-Disposition": f"attachment; filename={key.upper()}.CSV"},
        )

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
