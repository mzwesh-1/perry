"""Seeds one money mistake at a time into each working example and compares
running it with PERRY --check.

    python3 research/seed_bugs.py <examples folder> <work folder>
"""
import re, subprocess, sys, os, shutil, json, collections
P = os.environ.get("PERRY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "PERRY"))
SRC = sys.argv[1]; WORK = sys.argv[2]
os.makedirs(WORK, exist_ok=True)
for f in os.listdir(SRC):
    shutil.copy(os.path.join(SRC, f), WORK)   # data files and modules the mutants need
MONEY = re.compile(r'(?<![\w.])(-?\d+(?:\.\d+)?) ([A-Z]{3})\b')
PER = re.compile(r'\bper (year|month)\b')
def run(path, *args):
    r = subprocess.run([P, *args, path], cwd=WORK, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20)
    return r.returncode, (r.stderr + r.stdout)
def mutants(text):
    for m in MONEY.finditer(text):
        num, cur = m.group(1), m.group(2)
        if text[max(0, m.start()-3):m.start()].strip().endswith("as_"): continue
        other = "USD" if cur != "USD" else "ZAR"
        yield "currency", text[:m.start(2)] + other + text[m.end(2):], m.start()
        yield "drop-currency", text[:m.end(1)] + text[m.end(2):], m.start()
    for m in PER.finditer(text):
        swap = "month" if m.group(1) == "year" else "year"
        yield "period", text[:m.start(1)] + swap + text[m.end(1):], m.start()
rows = []
for f in sorted(os.listdir(SRC)):
    if not f.endswith(".perry"): continue
    text = open(os.path.join(SRC, f)).read()
    if run(os.path.join(SRC, f))[0] != 0: continue            # only programs that work
    for i, (kind, mutant, pos) in enumerate(mutants(text)):
        name = f"{f[:-6]}__m{i}.perry"
        open(os.path.join(WORK, name), "w").write(mutant)
        rc, out = run(name); crc, cout = run(name, "--check")
        line = text[:pos].count("\n") + 1
        rows.append(dict(file=f, mutant=name, kind=kind, line=line, run=rc, check=crc,
                         run_err=next((l for l in out.splitlines() if ": error:" in l), ""),
                         check_err=next((l for l in cout.splitlines() if ": error:" in l), "")))
json.dump(rows, open(os.path.join(WORK, "results.json"), "w"), indent=1)
c = collections.Counter((r["run"], r["check"]) for r in rows)
print("mutants:", len(rows))
for k, v in sorted(c.items()): print(f"  run exit {k[0]:>3}  check exit {k[1]:>3}: {v}")
