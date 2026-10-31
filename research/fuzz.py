"""Random-program test of PERRY --check: does it ever report a mistake no run makes?

    python3 research/fuzz.py [programs] [seed]

Each random program has check_if_ conditions written as holes. The program is
checked once, then run once for every way of filling the holes with yes_/no_.
A mistake --check reports on line L counts as confirmed if some run stops at L
with an error. Anything else is printed as a suspected false alarm, with the
program, so it can be looked at.

It also measures how many of the money mistakes that runs hit --check found.
"""
import itertools, os, random, re, subprocess, sys, tempfile, collections

PERRY = os.environ.get("PERRY", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "PERRY"))
CURRENCIES = ["ZAR", "USD", "JPY"]
DECIMALS = {"ZAR": 2, "USD": 2, "JPY": 0}

class Gen:
    def __init__(self, rnd):
        self.r = rnd
        self.names = []          # names that surely have a value at this point
        self.lines = []
        self.holes = 0
        self.funcs = []

    def money(self):
        c = self.r.choice(CURRENCIES if self.r.random() < 0.4 else ["ZAR"])
        n = self.r.randint(1, 9000)
        return f"{n}.{self.r.randint(0, 99):02d} {c}" if DECIMALS[c] else f"{n} {c}"

    def literal(self):
        k = self.r.random()
        if k < 0.45: return self.money()
        if k < 0.6:  return str(self.r.randint(1, 50))
        if k < 0.75: return f"{self.r.randint(1, 30)}%"
        if k < 0.88: return f"{self.r.randint(1, 20)}% per {self.r.choice(['year', 'month'])}"
        return f"{self.r.randint(1, 24)} {self.r.choice(['months', 'years', 'days'])}"

    def atom(self):
        if self.names and self.r.random() < 0.6: return self.r.choice(self.names)
        return self.literal()

    def expr(self, depth=0):
        k = self.r.random()
        if depth > 1 or k < 0.3: return self.atom()
        if k < 0.65:
            op = self.r.choice(["+", "-", "+", "*", "/"])
            right = self.atom()
            if op == "/" and right.strip() in ("0", "0%"): right = "2"
            return f"({self.expr(depth + 1)} {op} {right})"
        if k < 0.72: return f"{self.r.randint(1, 30)}% of {self.atom()}"
        if k < 0.78: return f"{self.atom()} per {self.r.choice(['year', 'month'])}"
        if k < 0.84:
            a, b = self.r.sample(CURRENCIES, 2)
            return f"convert_ {self.atom()} to_ {self.r.choice([a, b])} at_rate_ 18.20 {a} per {b}"
        if k < 0.9:
            fn = self.r.choice(["future_value_", "compound_interest_", "simple_interest_"])
            return f"{fn}({self.atom()}, {self.atom()}, {self.r.randint(1, 30)} {self.r.choice(['months', 'years'])})"
        if k < 0.95 and self.funcs:
            return f"{self.r.choice(self.funcs)}({self.atom()})"
        return f"total_of_ [{', '.join(self.atom() for _ in range(self.r.randint(1, 3)))}]"

    def condition(self):
        self.holes += 1
        return f"HOLE{self.holes}"

    def block(self, indent, size):
        for _ in range(size):
            self.statement(indent)

    def statement(self, indent):
        pad = "    " * indent
        k = self.r.random()
        if k < 0.45 or indent >= 2:
            name = self.r.choice(["a", "b", "c", "d", "total", "fee"])
            self.lines.append(f"{pad}{name} = {self.expr()}")
            if indent == 0 and name not in self.names: self.names.append(name)
        elif k < 0.6:
            self.lines.append(f"{pad}show_me_results({self.expr()})")
        elif k < 0.8:
            saved = list(self.names)
            self.lines.append(f"{pad}check_if_ {self.condition()}:")
            self.block(indent + 1, self.r.randint(1, 2))
            if self.r.random() < 0.5:
                self.lines.append(f"{pad}since_then_:")
                self.block(indent + 1, 1)
            self.names = saved
        elif k < 0.9:
            items = ", ".join(self.money() for _ in range(self.r.randint(1, 3)))
            saved = list(self.names)
            self.lines.append(f"{pad}go_through_ [{items}] as_ item:")
            self.names.append("item")
            self.block(indent + 1, self.r.randint(1, 2))
            self.names = saved
        else:
            self.lines.append(f"{pad}show_me_results({self.atom()} is_greater_than_ {self.atom()})")

    def program(self):
        for i in range(self.r.randint(0, 2)):
            name = f"f{i}"
            self.lines.append(f"define_ {name}(x):")
            saved, self.names = self.names, ["x"]
            if self.r.random() < 0.5:
                self.lines.append(f"    check_if_ {self.condition()}:")
                self.lines.append(f"        give_back_ {self.expr()}")
            self.lines.append(f"    give_back_ {self.expr()}")
            self.names = saved
            self.funcs.append(name)
        self.block(0, self.r.randint(3, 8))
        return "\n".join(self.lines) + "\n", self.holes

def run(path, *mode):
    r = subprocess.run([PERRY, *mode, path], capture_output=True, text=True, timeout=20, stdin=subprocess.DEVNULL)
    return r.returncode, [(int(m.group(1)), m.group(2)) for m in re.finditer(r"^.*?:(\d+): error: (.*)$", r.stderr, re.M)]

def inside_loop_or_function(lines, line):
    """Is this line in a function body or a go_through_ body? Then whether it fails
    depends on the call's inputs or the round's item, not only on reaching it."""
    indent = len(lines[line - 1]) - len(lines[line - 1].lstrip())
    for i in range(line - 2, -1, -1):
        text = lines[i]
        here = len(text) - len(text.lstrip())
        if text.strip() and here < indent:
            if text.lstrip().startswith(("define_", "go_through_")): return True
            indent = here
            if indent == 0: return False
    return False

def runs_with_marker(text, holes, line, path):
    """Marks the line and runs every way of filling the holes.
    Returns (runs that reached it and stopped on it, runs that reached it and got past it).
    Stopping inside a function the line calls counts as stopping on the line."""
    lines = text.split("\n")
    target = lines[line - 1]
    pad = target[:len(target) - len(target.lstrip())]
    header = target.rstrip().endswith(":")
    marked = lines[:line - 1] + [pad + 'show_me_results("@@reached@@")', target]
    if not header: marked.append(pad + 'show_me_results("@@after@@")')
    marked += lines[line:]
    marked = "\n".join(marked)
    stopped = passed = 0
    for combo in itertools.product(["yes_", "no_"], repeat=holes):
        it = iter(combo)
        open(path, "w").write(re.sub(r"HOLE\d+", lambda m: next(it, "yes_"), marked))
        r = subprocess.run([PERRY, path], capture_output=True, text=True, timeout=20, stdin=subprocess.DEVNULL)
        if "@@reached@@" not in r.stdout:
            continue
        if header:
            errs = [int(m.group(1)) for m in re.finditer(r"^.*?:(\d+): error:", r.stderr, re.M)]
            got_past = r.returncode == 0 or not errs or errs[0] > line + 1
        else:
            got_past = "@@after@@" in r.stdout
        if got_past: passed += 1
        else:        stopped += 1
    return stopped, passed

def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    rnd = random.Random(seed)
    tmp = tempfile.mkdtemp(prefix="perry-fuzz-")
    stats = collections.Counter()
    false_alarms = []
    for p in range(count):
        text, holes = Gen(rnd).program()
        holes = min(holes, 6)
        text = re.sub(r"HOLE(\d+)", lambda m: m.group(0) if int(m.group(1)) <= 6 else "yes_", text)
        path = os.path.join(tmp, f"p{p}.perry")
        open(path, "w").write(re.sub(r"HOLE\d+", "yes_", text))
        crc, cerr = run(path, "--check")
        if crc == 65 and cerr and "expected" in cerr[0][1]:
            stats["does not parse"] += 1
            continue
        stats["programs"] += 1
        for line in sorted({l for l, _ in cerr}):
            stats["lines --check reports"] += 1
            stopped, passed = runs_with_marker(text, holes, line, path)
            if inside_loop_or_function(text.split("\n"), line):
                stats["in a loop/function: " + ("confirmed by some run" if stopped else
                      "depends on the call or round" if passed else "not reached")] += 1
            elif passed:
                stats["FALSE ALARM (a run passed it)"] += 1
                false_alarms.append((p, line, [m for l, m in cerr if l == line], text))
            elif stopped:
                stats["confirmed (every run reaching it stops)"] += 1
            else:
                stats["not reached by any run"] += 1
        # how many money mistakes that runs actually hit were reported
        reported = {l for l, _ in cerr}
        hit = set()
        for combo in itertools.product(["yes_", "no_"], repeat=holes):
            it = iter(combo)
            open(path, "w").write(re.sub(r"HOLE\d+", lambda m: next(it, "yes_"), text))
            rc, errs = run(path)
            if rc == 70 and errs: hit.add(errs[0][0])
        stats["lines where some run stops"] += len(hit)
        stats["  ...reported by --check"] += len(hit & reported)
    for p, line, msgs, text in false_alarms:
        print(f"--- FALSE ALARM program {p} line {line}: {msgs}\n{text}")
    print(f"\nseed {seed}, files in {tmp}")
    for k, v in stats.items(): print(f"  {k:42} {v}")
    sys.exit(1 if false_alarms else 0)

if __name__ == "__main__":
    main()
