# Reproducing the results

Every number in the paper comes from one of these scripts, run on this commit.
Build first with `make` (the scripts use `./PERRY`; set `PERRY=...` to use another).

| Result | Command | What it prints |
| --- | --- | --- |
| Checker tests | `make test` | `all --check tests pass` |
| False alarms and errors found on the examples | `sh research/evaluate.sh <examples>` | one line per example: run exit, check exit, both errors |
| Seeded money mistakes (pilot) | `python3 research/seed_bugs.py <examples> <work folder>` | run/check outcome counts; `results.json` in the work folder |
| No false alarms on random programs | `python3 research/fuzz.py 300 <seed>` for seeds 1, 2, 3 | confirmed / not reached / false alarms; exits 1 on any false alarm |
| How much gets a known type (RQ3) | `PERRY_CHECK_STATS=1 ./PERRY --check <file>` | `stats: N expressions, K with a known type` |
| Speed (RQ4) | `sh research/bench.sh <examples> [old PERRY]` | ms for all examples, and for generated programs of 1 000 to 100 000 lines |
| Mistakes written by other people (RQ2) | see `bug-study/README.md` | the study's own scoring |

`<examples>` is the perry-complete-examples folder (164 programs) that goes
with the Perry Tutorial and Perry for Universities.

The old parse-only `--check` for comparison is the baseline commit:
`git worktree add ../perry-base 7e6d948 && make -C ../perry-base`.
