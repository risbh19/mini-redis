#!/usr/bin/env python3
"""Summarize GCC's JSON coverage for core/protocol/persistence, no pip needed."""
import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-coverage").resolve()
files = {}
with tempfile.TemporaryDirectory(prefix="mini-redis-gcov-") as temporary:
    for data in (build / "CMakeFiles/mini_redis_lib.dir/src").rglob("*.gcda"):
        subprocess.run(["gcov", "--json-format", str(data)], cwd=temporary, check=True, stdout=subprocess.DEVNULL)
        for archive in Path(temporary).glob("*.gcov.json.gz"):
            with gzip.open(archive, "rt") as stream:
                report = json.load(stream)
            for file in report["files"]:
                name = file["file"].replace("\\", "/")
                if not any(f"/src/{part}/" in name for part in ("core", "protocol", "persistence")):
                    continue
                short = "src/" + name.split("/src/", 1)[1]
                lines = files.setdefault(short, {})
                for line in file["lines"]:
                    number = line["line_number"]
                    lines[number] = lines.get(number, 0) + line["count"]
            archive.unlink()
summary = []
for name, lines in sorted(files.items()):
    summary.append({"file": name, "covered": sum(count > 0 for count in lines.values()), "lines": len(lines)})
covered = sum(row["covered"] for row in summary)
total = sum(row["lines"] for row in summary)
print(json.dumps({"covered": covered, "lines": total, "line_coverage_percent": 100 * covered / total if total else 0,
                  "files": summary}, indent=2))
