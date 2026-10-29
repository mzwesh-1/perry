"""Scores PERRY --check on programs written by other people.

    python3 research/bug-study/score.py <submissions folder> [labels.csv]

The submissions folder holds one folder per participant (any names), each with
.perry files and any CSV files or modules they use. Every program is run once
and checked once. The result is results.csv in the submissions folder, and a
summary on screen.

labels.csv (optional) is filled in by a person after a first run, for the
programs where --check reports a mistake that running did not show. Columns:
    file,line,verdict,note
with verdict one of: real (a real mistake on a path the run did not take),
false-alarm (the checker is wrong), or unsure.
"""
import csv, os, re, subprocess, sys, collections

PERRY = os.environ.get("PERRY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "PERRY"))

# Runtime errors that are money or unit mistakes - the ones --check is meant to find.
MONEY = re.compile(r"can't (add|subtract|compare|divide|mix) |convert one currency|money and |"
                   r"rate per |per year|per month|compounding needs|whole years|that rate is for|"
                   r"cash flows must be|must be the same kind|must be money|needs money|as money|"
                   r"must be a percent|must be a number|can't multiply money|'of' needs a percent|"
                   r"can't do maths with|exchange rate|same period|plain number can't be added to a date|"
                   r"in years or months|one book_ entry can't mix|is kept in")

def run(folder, name, *mode):
    try:
        r = subprocess.run([PERRY, *mode, name], cwd=folder, stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return "timeout", []
    errors = [(int(m.group(1)), m.group(2)) for m in re.finditer(r"^.*?:(\d+): error: (.*)$", r.stderr + r.stdout, re.M)]
    return r.returncode, errors

def outcome(run_rc, run_errs, check_rc, check_errs):
    if run_rc == 65:
        return "does-not-parse"
    if run_rc == "timeout":
        return "run-timeout"
    if run_rc == 0 and not check_errs:
        return "clean"
    if run_rc == 0:
        return "check-only"                       # needs a person: hidden mistake or false alarm?
    line, msg = run_errs[0] if run_errs else (0, "")
    money = bool(MONEY.search(msg))
    if not check_errs:
        return "missed-money" if money else "missed-other"
    if any(l == line for l, _ in check_errs):
        return "found-same-line" if money else "found-other-same-line"
    return "found-other-line"

def main():
    root = sys.argv[1]
    labels = {}
    if len(sys.argv) > 2:
        with open(sys.argv[2], newline="") as f:
            for row in csv.DictReader(f):
                labels[(row["file"], int(row["line"]))] = row["verdict"].strip()
    rows = []
    for folder, _, files in sorted(os.walk(root)):
        for name in sorted(files):
            if not name.endswith(".perry"):
                continue
            rel = os.path.relpath(os.path.join(folder, name), root)
            rrc, rerr = run(folder, name)
            crc, cerr = run(folder, name, "--check")
            kind = outcome(rrc, rerr, crc, cerr)
            verdicts = [labels.get((rel, l), "") for l, _ in cerr] if kind == "check-only" else []
            rows.append({
                "file": rel, "outcome": kind,
                "run_line": rerr[0][0] if rerr else "", "run_error": rerr[0][1] if rerr else "",
                "check_errors": " | ".join(f"{l}: {m}" for l, m in cerr),
                "labels": " ".join(verdicts),
            })
    out = os.path.join(root, "results.csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["file"])
        w.writeheader()
        w.writerows(rows)

    count = collections.Counter(r["outcome"] for r in rows)
    print(f"{len(rows)} programs  ->  {out}\n")
    for k in ["clean", "found-same-line", "found-other-line", "found-other-same-line", "missed-money",
              "missed-other", "check-only", "does-not-parse", "run-timeout"]:
        if count[k]: print(f"  {k:24} {count[k]}")
    money_crashes = count["found-same-line"] + count["found-other-line"] + count["missed-money"]
    if money_crashes:
        found = count["found-same-line"] + count["found-other-line"]
        print(f"\nMoney mistakes that stop the program: {found} of {money_crashes} found before running"
              f" ({100 * found / money_crashes:.0f}%)")
    reports = [v for r in rows for v in r["labels"].split()]
    if count["check-only"]:
        c = collections.Counter(reports)
        print(f"Reports a run doesn't show: {count['check-only']} programs - labelled real {c['real']}, "
              f"false alarm {c['false-alarm']}, unsure {c['unsure']}"
              + ("" if reports else "  (label them in labels.csv)"))

if __name__ == "__main__":
    main()
