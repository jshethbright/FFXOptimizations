"""Correlate the last FFX graphics run with QPC-stamped texture reads."""

import argparse
import csv
import io
from pathlib import Path
import re


def load_last_run(path):
    lines = path.read_text(encoding="ascii").splitlines()
    index = max(i for i, line in enumerate(lines) if line.startswith("# pid="))
    clock = dict(re.findall(r"(\w+)=(\d+)", lines[index]))
    rows = csv.DictReader(io.StringIO("\n".join(
        line for line in lines[index + 1:] if not line.startswith("#")
    )))
    events = []
    for row in rows:
        if all(row.values()):
            events.append({key: value if key == "kind" else int(value)
                           for key, value in row.items()})
    return clock, events


def slow_reads(path, frequency):
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stamp = re.search(r"elapsed_us=(\d+) bytes=(\d+) end_qpc=(\d+) thread=(\d+)", line)
        if stamp:
            elapsed, size, end, thread = map(int, stamp.groups())
            yield {
                "kind": f"ReadFile ({size / 1024**2:.1f} MiB)",
                "start_qpc": end - elapsed * frequency // 1_000_000,
                "end_qpc": end, "cpu_us": elapsed, "gpu_us": 0, "thread": thread,
            }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="FFX game or graphics fixture directory")
    parser.add_argument("--min-ms", type=float, default=35, help="Minimum frame interval")
    parser.add_argument("--limit", type=int, default=8, help="Maximum number of intervals")
    args = parser.parse_args()
    clock, events = load_last_run(args.directory / "ffx-gpu-texture-trace.csv")
    frequency = int(clock["qpc_hz"])
    frames = sorted(
        (event for event in events
         if event["kind"] == "frame_interval" and event["cpu_us"] >= 1000 * args.min_ms),
        key=lambda event: event["cpu_us"], reverse=True,
    )
    operations = [event for event in events
                  if event["kind"] != "frame_interval" and (event["cpu_us"] or event["gpu_us"])]
    operations.extend(slow_reads(args.directory / "ffx-texture-slow-reads.log", frequency))
    print(f'FFX process {clock["pid"]}, {len(frames)} frame intervals at or above {args.min_ms:g} ms')
    for frame in frames[:args.limit]:
        elapsed = (frame["end_qpc"] - int(clock["qpc"])) / frequency
        print(f'\n{elapsed:+.3f} s from trace start  |  {frame["cpu_us"] / 1000:.2f} ms frame interval')
        overlapping = sorted(
            (event for event in operations if event["start_qpc"] < frame["end_qpc"]
             and event["end_qpc"] > frame["start_qpc"]),
            key=lambda event: max(event["cpu_us"], event["gpu_us"]), reverse=True,
        )
        for event in overlapping[:12]:
            gpu = f'  |  GPU {event["gpu_us"] / 1000:.2f} ms' if event["kind"] in {
                "gpu_update_texture", "gpu_copy_texture", "gpu_copy_texture_region"
            } else ""
            print(f'  {event["kind"]}  |  CPU {event["cpu_us"] / 1000:.2f} ms{gpu}'
                  f'  |  thread {event["thread"]}')


if __name__ == "__main__":
    main()
