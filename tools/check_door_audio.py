"""Check an SM_AUDIO_PROBE CSV for a real doorway crossing and audio gaps."""
import argparse
import csv
import json
from pathlib import Path


def read_trace(path):
    with Path(path).open(newline="") as stream:
        return [{key: float(value) if key in ("seconds", "guest_ms") else
                 int(value, 16 if key in ("door_step", "room") else 10)
                 for key, value in row.items()}
                for row in csv.DictReader(stream)]


def analyze(rows, post_frames=120, output_rate=None):
    # A successful process exit or zero underflows at the title screen must
    # never count as a passing door test. Loading before guest reset used to
    # erase the save and silently produce exactly that false positive.
    starts = [i for i in range(1, len(rows))
              if rows[i-1]["state"] == 8 and 9 <= rows[i]["state"] <= 11]
    if not starts:
        raise ValueError("No gameplay-to-door transition; verify save load and input")
    results = []
    for start in starts:
        end = start
        while end < len(rows) and 9 <= rows[end]["state"] <= 11:
            end += 1
        if end + post_frames >= len(rows):
            raise ValueError("Incomplete doorway or post-door measurement window")
        before, after, post = rows[start-1], rows[end], rows[end+post_frames]
        if after["state"] != 8 or after["room"] == before["room"]:
            raise ValueError("Door did not return to gameplay in a different room")
        if any(r["state"] != 8 for r in rows[end:end+post_frames+1]):
            raise ValueError("Post-door window interrupted by another game state")
        rate = output_rate or before.get("output_rate", 0)
        if rate <= 0:
            raise ValueError("Missing output sample rate")
        # Counters are cumulative: exclude startup and loading the save.
        def delta(key, a, b):
            value = b[key] - a[key]
            if value < 0:
                raise ValueError("Counter reset inside measurement window")
            return value
        elapsed = post["seconds"] - after["seconds"]
        if elapsed <= 0:
            raise ValueError("Non-increasing wall clock")
        zero_start = None
        zero_ms = 0
        for prev, cur in zip(rows[start-1:end], rows[start:end+1]):
            if cur["produced"] == prev["produced"]:
                if zero_start is None:
                    zero_start = prev["seconds"]
                zero_ms = max(zero_ms, (cur["seconds"]-zero_start)*1000)
            else:
                zero_start = None
        results.append({
            "from_room": f"{before['room']:04x}", "to_room": f"{after['room']:04x}",
            "first_frame": rows[start]["frame"], "exit_frame": after["frame"],
            "transition_ms": round((after["seconds"]-before["seconds"])*1000, 3),
            "underflows": delta("underflows", before, after),
            "missing_ms": round(delta("missing_frames", before, after)*1000/rate, 3),
            "post_missing_ms": round(delta("missing_frames", after, post)*1000/rate, 3),
            "post_fps": round(delta("frame", after, post)/elapsed, 3),
            "produced_samples": delta("produced", before, after),
            "longest_no_pcm_ms": round(zero_ms, 3),
            **({"dropped_samples": delta("dropped", before, after),
                "post_dropped_samples": delta("dropped", after, post)}
               if "dropped" in before else {}),
        })
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace")
    parser.add_argument("--post-frames", type=int, default=120)
    parser.add_argument("--output-rate", type=int, help="For older traces without output_rate")
    parser.add_argument("--max-missing-ms", type=float, default=0)
    parser.add_argument("--max-post-fps", type=float, default=63)
    args = parser.parse_args()
    if args.post_frames < 60:
        parser.error("--post-frames must cover at least 60 gameplay frames")
    try:
        results = analyze(read_trace(args.trace), args.post_frames, args.output_rate)
    except (ValueError, KeyError, OSError) as error:
        print(json.dumps({"valid": False, "error": str(error)}))
        return 2
    passed = all(r["missing_ms"] <= args.max_missing_ms and
                 r["post_missing_ms"] <= args.max_missing_ms and
                 r.get("dropped_samples", 0) == 0 and
                 r.get("post_dropped_samples", 0) == 0 and
                 r["post_fps"] <= args.max_post_fps for r in results)
    print(json.dumps({"valid": True, "passed": passed, "transitions": results}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
