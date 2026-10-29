# Bug study: does `PERRY --check` find real money mistakes?

The pilot used bugs seeded by a script written by the people who built the
checker. This study uses mistakes written by other people, so the result can
be trusted. It answers RQ2 (how many money mistakes are found before running,
and how many false alarms) and gives RQ3 real programs to count annotations on.

## Two sources of mistakes

**A. Real student programs (the main evidence).** Students in a course that
teaches Perry do the tasks in `tasks.md`. Every version they save is kept,
not only the final one, because the mistakes are in the early versions.

**B. Mistakes written on purpose by people who know the course (a second
check).** Two or three tutors or lecturers who have marked student finance
work are asked: "write each task the way a weak student would, with the
money mistakes you most often see". They must not see `src/check.c`, the
checker's tests or the pilot results first.

Source B is quicker but is still guesswork about students; report A and B
separately and never pool them.

## Before you start

- **Ethics approval.** Collecting student work for research needs approval
  from your university's research ethics committee, informed consent from
  each student, and a plan for personal information that meets POPIA. The
  information sheet below is a starting point for that application, not a
  substitute for it.
- **Fix the checker version.** Tag the commit used (`git tag study-v1`) and
  don't change `src/check.c` until scoring is done. Fixing the checker on the
  study's own programs and then reporting the score on them inflates it.
- **Write the measures down first** (this file is enough): the outcomes and
  percentages that `score.py` prints are the ones you report.
- **Independence.** Because the checker is by Perry's author, someone else
  should label the "check-only" reports (see below), without knowing which
  outcome is hoped for.

## Collecting

1. Give students `tasks.md` and the Perry Tutorial. No help with the checker:
   turn `perrylang.checkOnSave` off in VS Code for the study, so students see
   only runtime errors, as with Perry 0.9.0 today.
2. Keep every saved version of every `.perry` file with its CSV files. In VS
   Code, a simple way is a folder under version control that commits on every
   save, or Local History.
3. Remove names and student numbers from file and folder names and from the
   code (comments may hold them). Use codes such as `s017/task3_v04.perry`.

Folder layout for scoring:

    submissions/
      s001/  task1_v01.perry  task1_v02.perry  statement.csv ...
      s002/  ...
      tutors/ t1/ ...          (source B, scored separately)

## Scoring

    make                                         # build PERRY at the study tag
    python3 research/bug-study/score.py submissions

Each program gets one outcome:

| Outcome | Meaning |
| --- | --- |
| clean | runs, and `--check` finds nothing |
| found-same-line | stops with a money mistake, and `--check` reports that line |
| found-other-line | stops with a money mistake, `--check` reports it elsewhere |
| missed-money | stops with a money mistake, `--check` finds nothing |
| missed-other | stops for another reason (file, input, requirement) |
| check-only | runs without error, but `--check` reports a mistake |
| does-not-parse | a syntax error; both report it the same way |

**check-only needs a person.** For each one, open the program and decide:

- **real**: the mistake is there, on a path the run didn't take. Write a
  run that takes that path (different input, a different date) and confirm it
  stops with the same error. Note the input you used.
- **false-alarm**: no input makes the program stop there; the checker is wrong.
- **unsure**: can't tell; say why.

Put the verdicts in `labels.csv` (`file,line,verdict,note`) and score again:

    python3 research/bug-study/score.py submissions labels.csv

## What to report

- Money mistakes that stop the program, found before running:
  found / (found + missed-money), with the count of programs and students.
- False alarm rate: false-alarm / all `--check` reports on programs that run.
- Hidden mistakes found: check-only labelled real.
- The missed-money programs, grouped by why they were missed.
- Count each mistake once per student per task: many saved versions repeat
  the same mistake, so also report the counts with only each student's first
  version that shows it.

## Information sheet for students (template)

> **Study: finding money mistakes in Perry programs before they run.**
> We are testing a tool that looks for mistakes in Perry programs, such as
> adding rand to dollars. If you agree, the Perry files you write for these
> tasks are kept, including unfinished versions. Your name and student number
> are removed before anyone studies them. Taking part, or not, has no effect
> on your marks, and you can withdraw until [date] by contacting [name, email].
> The files are kept on [where] until [date], then deleted. Questions:
> [researcher], and [ethics committee contact, reference number].
