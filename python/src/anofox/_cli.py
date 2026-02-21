"""
Command-line interface for anofox-tabular.

Usage::

    anofox profile <file> [--format table|csv|json] [--ext <path>]
    anofox quality <file> [--volume-min N] [--null-max F] [--column COL] [--format table|json] [--ext <path>]

Supported file formats: .parquet, .csv, .tsv, .json, .ndjson
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Optional

_SUPPORTED_EXTENSIONS = {
    ".parquet": "parquet",
    ".csv": "csv",
    ".tsv": "tsv",
    ".json": "json",
    ".ndjson": "ndjson",
}


def _read_fn(file_path: str) -> str:
    """Return the DuckDB read function call string for the given file."""
    suffix = Path(file_path).suffix.lower()
    if suffix not in _SUPPORTED_EXTENSIONS:
        supported = ", ".join(_SUPPORTED_EXTENSIONS)
        raise SystemExit(
            f"Unsupported file type '{suffix}'. Supported extensions: {supported}"
        )
    fmt = _SUPPORTED_EXTENSIONS[suffix]
    escaped = file_path.replace("'", "''")
    if fmt == "parquet":
        return f"read_parquet('{escaped}')"
    elif fmt == "tsv":
        return f"read_csv('{escaped}', delim='\\t')"
    elif fmt == "csv":
        return f"read_csv('{escaped}')"
    elif fmt in ("json", "ndjson"):
        return f"read_json('{escaped}')"
    raise SystemExit(f"Unsupported format: {fmt}")  # unreachable


def _connect(ext: Optional[str]):
    """Create an AnofoxConnection, resolving extension path."""
    import anofox

    ext_path = ext or os.environ.get("ANOFOX_EXT_PATH")
    return anofox.connect(extension_path=ext_path)


def _cmd_profile(args, console=None) -> int:
    from rich.console import Console
    from rich.table import Table

    if console is None:
        console = Console()

    read_fn = _read_fn(args.file)  # validate early before connecting
    conn = _connect(args.ext)
    try:
        conn.execute(
            f"CREATE OR REPLACE TABLE _cli_profile AS SELECT * FROM {read_fn}"
        )
        result = conn.profile("_cli_profile")
        conn.execute("DROP TABLE IF EXISTS _cli_profile")
    finally:
        conn.close()

    if args.format == "table":
        table = Table(title="Profile")
        for col in result.columns:
            table.add_column(col)
        for _, row in result.iterrows():
            table.add_row(*[str(v) for v in row])
        console.print(table)
    elif args.format == "csv":
        print(result.to_csv(index=False), end="")
    elif args.format == "json":
        print(result.to_json(orient="records", indent=2))
    return 0


def _cmd_quality(args, console=None) -> int:
    from rich.console import Console

    if console is None:
        console = Console()

    # Validate args before connecting
    read_fn = _read_fn(args.file)
    if args.null_max is not None and not args.column:
        raise SystemExit("--null-max requires --column")
    if args.volume_min is None and args.null_max is None:
        raise SystemExit(
            "No quality rules specified. Use --volume-min or --null-max."
        )

    conn = _connect(args.ext)
    try:
        conn.execute(
            f"CREATE OR REPLACE TABLE _cli_quality AS SELECT * FROM {read_fn}"
        )

        from anofox import quality as q

        results: dict = {}
        if args.volume_min is not None:
            results["volume"] = q.volume(
                conn, "_cli_quality", min_rows=args.volume_min
            )
        if args.null_max is not None:
            results["null_rate"] = q.null_rate(
                conn, "_cli_quality",
                column_name=args.column,
                max_null_rate=args.null_max,
            )

        conn.execute("DROP TABLE IF EXISTS _cli_quality")
    finally:
        conn.close()

    overall_status = "pass" if all(
        r.get("status") == "pass" for r in results.values()
    ) else "fail"

    if args.format == "json":
        print(json.dumps({"status": overall_status, "checks": results}, indent=2))
    else:
        status_str = (
            f"[green]{overall_status}[/green]"
            if overall_status == "pass"
            else f"[red]{overall_status}[/red]"
        )
        console.print(f"status: {status_str}")
        for name, check in results.items():
            s = check.get("status", "?")
            styled = f"[green]{s}[/green]" if s == "pass" else f"[red]{s}[/red]"
            console.print(f"  {name}: {styled}")

    return 0 if overall_status == "pass" else 1


def main(console=None) -> None:
    parser = argparse.ArgumentParser(
        prog="anofox",
        description="anofox-tabular CLI — data quality and profiling",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    # --- profile ---
    p = sub.add_parser("profile", help="Profile columns of a file")
    p.add_argument(
        "file",
        help="Path to input file (.parquet, .csv, .tsv, .json, .ndjson)",
    )
    p.add_argument(
        "--ext",
        help="Path to anofox extension binary (overrides ANOFOX_EXT_PATH)",
    )
    p.add_argument(
        "--format",
        choices=["table", "csv", "json"],
        default="table",
        help="Output format (default: table)",
    )

    # --- quality ---
    q = sub.add_parser("quality", help="Run quality checks on a file")
    q.add_argument("file", help="Path to input file")
    q.add_argument(
        "--volume-min", type=int, metavar="N", help="Minimum row count"
    )
    q.add_argument(
        "--null-max",
        type=float,
        metavar="F",
        help="Maximum null rate (0.0–1.0)",
    )
    q.add_argument("--column", help="Column name for null-rate check")
    q.add_argument("--ext", help="Path to anofox extension binary")
    q.add_argument(
        "--format",
        choices=["table", "json"],
        default="table",
        help="Output format (default: table)",
    )

    args = parser.parse_args()
    if args.cmd == "profile":
        sys.exit(_cmd_profile(args, console=console))
    elif args.cmd == "quality":
        sys.exit(_cmd_quality(args, console=console))
