#!/usr/bin/env python3
"""Compares performance reports of two builds, for .github/workflows/perf.yml.

    scripts/perf/compare.py <folder> [--title T] [--no-fail]

The folder holds reports in the profiler's format (kithcore/perf.h, and manual_sync_stress's,
which follows it): "key value" lines under [section] headings. Each is named
<label>-<anything>.txt, where label is base (the build compared against) or head (the one
being tested); a folder of head reports alone is checked on its own.

Several runs of a build are summed up by their median, which a stray slow run doesn't move.
Then each figure is compared: head regressed if it's worse than base by more than the
figure's share and by more than its floor, both below. CI runners are shared machines with no
GPU, so the floors keep noise from failing the job; a real regression is far bigger.

Checked without a base too, as nothing should ever do them:
  - memory still growing over repeated scroll passes (a leak): past base's growth by more than
    LEAK_MB_PER_PASS, or past LEAK_MB_PER_PASS_ALONE when there is no base;
  - a sync run that failed its own checks ([result] ok 0) or lost or broke files.

Prints a Markdown summary (for $GITHUB_STEP_SUMMARY) and exits 1 on any failure unless
--no-fail.
"""
import argparse
import os
import re
import statistics
import sys

# (share, floor): head fails when head > base * (1 + share) and head - base > floor.
LIMITS = {
    # Scroll frame times, median and p95. Frames are paced by the display, so these move in
    # whole refreshes: the floor is one refresh at the report's refresh_hz, set in main().
    "frame_ms": (0.15, None),
    "paint_ms": (0.20, 0.5),  # the work in a frame
    "view_ms": (0.25, 10.0),  # a view switch
    "startup_ms": (0.25, 50.0),
    "sync_ms": (0.20, 500.0),  # a sync phase's wall time
    "memory_mb": (0.10, 8.0),
}
# Growth of private memory per scroll pass over the last two passes. The first passes fill
# caches, and over a big library memory can climb for a few passes before it levels off, the
# same in both builds; a leak keeps climbing at its rate. So with a base, head fails when it
# grows faster than base by more than LEAK_MB_PER_PASS; alone, past LEAK_MB_PER_PASS_ALONE.
LEAK_MB_PER_PASS = 1.0
LEAK_MB_PER_PASS_ALONE = 4.0


def parse(path):
    """{section: {key: [values...]}} from one report; values as floats where they parse."""
    sections, current = {}, None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = re.fullmatch(r"\[(.+)\]", line)
            if m:
                current = sections.setdefault(m.group(1), {})
                continue
            if current is None:
                continue
            key, _, rest = line.partition(" ")
            values = []
            for v in rest.split():
                try:
                    values.append(float(v))
                except ValueError:
                    values.append(v)
            current[key] = values
    return sections


def figures(report):
    """{name: (kind, value)} for every figure compared, from one report."""
    out = {}
    for section, keys in report.items():
        if section.startswith("scroll auto_"):
            series = section[len("scroll auto_"):]
            for key, kind in (("median_ms", "frame_ms"), ("p95_ms", "frame_ms"),
                              ("paint_median_ms", "paint_ms"), ("paint_p95_ms", "paint_ms")):
                if key in keys:
                    out[f"scroll {series} {key}"] = (kind, keys[key][0])
        elif section == "views":
            for view, v in keys.items():
                if view != "settings" and len(v) >= 2:  # Settings opens after the test
                    out[f"view {view} median_ms"] = ("view_ms", v[1])
        elif section == "startup":
            if "first_content_painted" in keys:
                out["startup first_content_painted_ms"] = ("startup_ms", keys["first_content_painted"][0])
        elif section.startswith("sync "):
            if "ms" in keys:
                out[f"{section} ms"] = ("sync_ms", keys["ms"][0])
        elif section == "memory":
            samples = list(keys.items())
            if samples:
                peak = max(v[2] for _, v in samples if len(v) >= 4)
                out["memory peak_mb"] = ("memory_mb", peak)
                last = samples[-1][1]
                if len(last) >= 4:
                    out[f"memory private_mb at {samples[-1][0]}"] = ("memory_mb", last[3])
    return out


def leak(report):
    """MB of private memory gained per scroll pass over the last two passes (never counting
    pass_1's), or None without 3 passes."""
    mem = report.get("memory", {})
    passes = sorted((int(k[5:]), v[3]) for k, v in mem.items() if re.fullmatch(r"pass_\d+", k) and len(v) >= 4)
    if len(passes) < 3:
        return None
    (n0, m0), (n1, m1) = passes[max(1, len(passes) - 3)], passes[-1]
    return (m1 - m0) / (n1 - n0)


def sync_problems(report, name):
    problems = []
    if "result" in report and report["result"].get("ok", [1])[0] != 1:
        problems.append(f"{name}: the sync run failed its own checks (see its log)")
    for section, keys in report.items():
        if section.startswith("sync "):
            if keys.get("corrupt", [0])[0]:
                problems.append(f"{name}: {section} had {int(keys['corrupt'][0])} corrupt files")
            if keys.get("missing", [0])[0]:
                problems.append(f"{name}: {section} left {int(keys['missing'][0])} files missing")
            if keys.get("converged", [1])[0] != 1:
                problems.append(f"{name}: {section} didn't converge")
    return problems


def median_figures(reports):
    merged = {}
    for r in reports:
        for name, (kind, v) in figures(r).items():
            merged.setdefault(name, (kind, []))[1].append(v)
    return {name: (kind, statistics.median(vs), len(vs)) for name, (kind, vs) in merged.items()}


def fmt(v):
    return f"{v:.2f}" if abs(v) < 100 else f"{v:.0f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--title", default="Performance")
    ap.add_argument("--no-fail", action="store_true")
    args = ap.parse_args()

    runs = {"base": [], "head": []}
    for name in sorted(os.listdir(args.folder)):
        label = name.split("-", 1)[0]
        if name.endswith(".txt") and label in runs:
            runs[label].append((name, parse(os.path.join(args.folder, name))))
    if not runs["head"]:
        print(f"No head-*.txt reports in {args.folder}.", file=sys.stderr)
        return 1

    failures = []
    lines = [f"## {args.title}", ""]
    head_run = runs["head"][0][1].get("run", {})
    hz = head_run.get("refresh_hz", [60.0])[0]
    LIMITS["frame_ms"] = (LIMITS["frame_ms"][0], 1000.0 / hz if isinstance(hz, float) and hz > 0 else 1000.0 / 60)
    info = ", ".join(f"{k} {' '.join(str(x) for x in v)}" for k, v in head_run.items()
                     if k in ("platform", "build", "cpus", "refresh_hz", "library_tracks", "tracks", "file_kb"))
    lines.append(f"{len(runs['head'])} head and {len(runs['base'])} base runs; {info}. "
                 "Medians over runs; lower is better.")
    lines.append("")

    for name, r in runs["head"]:
        failures += sync_problems(r, name)
    for name, r in runs["base"]:
        for p in sync_problems(r, name):
            lines.append(f"- note, base: {p}")

    head = median_figures(r for _, r in runs["head"])
    base = median_figures(r for _, r in runs["base"])
    lines += ["| figure | base | head | change | |", "|---|---:|---:|---:|---|"]
    for name in sorted(head):
        kind, h, _ = head[name]
        if name not in base:
            lines.append(f"| {name} | | {fmt(h)} | | new |")
            continue
        b = base[name][1]
        share, floor = LIMITS[kind]
        change = (h - b) / b * 100 if b else 0.0
        verdict = ""
        if h > b * (1 + share) and h - b > floor:
            verdict = "**regressed**"
            failures.append(f"{name}: {fmt(b)} -> {fmt(h)} ({change:+.0f}%, limit +{share * 100:.0f}% and +{fmt(floor)})")
        elif b > h * (1 + share) and b - h > floor:
            verdict = "faster"
        lines.append(f"| {name} | {fmt(b)} | {fmt(h)} | {change:+.0f}% | {verdict} |")
    lines.append("")

    head_leaks = [x for x in (leak(r) for _, r in runs["head"]) if x is not None]
    base_leaks = [x for x in (leak(r) for _, r in runs["base"]) if x is not None]
    if head_leaks:
        hl = statistics.median(head_leaks)
        if base_leaks:
            bl = statistics.median(base_leaks)
            limit = max(bl, 0.0) + LEAK_MB_PER_PASS
            lines.append(f"Private memory per scroll pass, over the last two: head {hl:+.2f} MB, base {bl:+.2f} MB "
                         f"(limit {limit:+.2f}: base's plus {LEAK_MB_PER_PASS:.2f}).")
        else:
            limit = LEAK_MB_PER_PASS_ALONE
            lines.append(f"Private memory per scroll pass, over the last two: head {hl:+.2f} MB "
                         f"(limit {limit:+.2f}, as there's no base).")
        if hl > limit:
            failures.append(f"memory grows {hl:.2f} MB a scroll pass over the same views: a leak?")
        lines.append("")

    if failures:
        lines.append("### Failed")
        lines += [f"- {f}" for f in failures]
    else:
        lines.append("No regressions.")
    print("\n".join(lines))
    return 1 if failures and not args.no_fail else 0


if __name__ == "__main__":
    sys.exit(main())
