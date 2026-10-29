# Perry

Created by Mzwandile Zulu.

Perry is an English-style programming language for exact money and finance, written in plain C.

## Build and run (WSL / Linux)

    make
    ./PERRY test.perry

The command is called `PERRY`, in capital letters, so it never clashes with other
programs called "perry". Install it so you can type `PERRY` anywhere (and VS Code
can find it):

    sudo make install

Other modes:

    ./PERRY --tokens test.perry    # what the lexer sees
    ./PERRY --tree test.perry      # what the parser builds
    ./PERRY --check test.perry     # look for mistakes without running
    ./PERRY --version

`make static` builds `PERRY-linux-x64`, a copy that runs on any 64-bit Linux or WSL
with nothing else installed. This is the one packed inside the VS Code extension.

## What works (v0.9.0)

- Numbers, money (`199.99 ZAR`), percentages (`15%`), text, `yes_`, `no_`, `nothing_`
- Exact maths: no floating point, so `0.1 + 0.2` is exactly `0.3`
- Money rules: no mixing currencies, no adding plain numbers to money,
  always rounded to the currency's decimal places (ZAR 2, JPY 0, KWD 3)
- `x = ...` and `never_changes_ X = ...`
- `+ - * /` and `15% of price`
- `show_me_results(...)`
- `check_if_` / `thats_if_` / `since_then_`, and one-line `if_then_ ... : ...`
- `is_equal_to_`, `is_not_equal_to_`, `is_greater_than_`, `is_less_than_`,
  `is_greater_equal_to_`, `is_less_equal_to_`
- `is_similar_to_ ... within_ ...`, `is_really_not_close_to_ ... within_ ...`
- `and_`, `or_`, `not_`
- `exactly_ N times:`, `keep_going_while_ ...:`, `stop_now_`, `skip_this_one_`
- `not_unless_ ...` (stops the program if a requirement isn't met)

New in v0.3:

- Lists: `[1200.00 ZAR, 350.50 ZAR]`, `total_of_`, `count_of_`
- `go_through_ list as_ item:` loops
- `parts = split 100.00 ZAR into 3` gives `[33.34 ZAR, 33.33 ZAR, 33.33 ZAR]`,
  which always adds back up to the original amount
- Functions: `define_ name(a, b):` with `give_back_`. Functions can read names
  from outside but can't change them.
- `because_is_ name` explains how a value was calculated

New in v0.4:

- Rates with periods: `11.75% per year`, and `rate per month` converts it exactly
  (Perry won't let you mix a yearly and a monthly rate by accident)
- Loans: `monthly_payment(loan, rate per month, 240)` and
  `schedule_of_(loan, rate per month, 240)` for the month-by-month table
- Rounding: `round_to_ x 2 decimals`, with `the_bank_way_` (halves to even),
  `up_` or `down_`
- Tax tables: `tax_table_ name:` with `up_to_ ... taxed_at_ ...` lines and a final
  `above_ taxed_at_ ...`, then `name on_ income`
- Bank statements: `statement = go_get_ "statement.csv" as_ ZAR` loads a CSV file
  (the path is relative to your .perry file). Column names become Perry names:
  "Transaction Date" becomes `transaction_date`.
- Tables: `row.amount` for one cell, `statement.amount` for a whole column,
  `go_through_ statement as_ tx:`, `count_of_ statement`
- Filters: `go_get_in_ statement by_ amount that_is_less_than_ 0.00 ZAR`,
  `remove_where_ statement by_ amount that_is_equal_to_ 0.00 ZAR`
- Sorting: `sort_ spending by_ amount highest_first_` (or `lowest_first_`)

New in v0.5:

- Error handling:

      try_this_:
          statement = go_get_ "statement.csv" as_ ZAR
      even_if_ theres_a_file_error_:
          show_me_results("Couldn't read the file")
      even_if_ theres_an_error_:
          show_me_results("Something else went wrong")
      but_then_:
          show_me_results("This always runs")

  Kinds of error: `theres_a_currency_error_` (mixing currencies or money with plain
  numbers), `theres_a_file_error_` (go_get_ problems), `theres_a_balance_error_`
  (a failed not_unless_), and `theres_an_error_` (anything).
  `raise_alarm_ "message"` raises your own error.
- Dates: `2026-12-31`, `today_`, `+ 30 days`, `+ 2 weeks`, `+ 1 month`, `+ 1 year`
  (31 Jan + 1 month = 28 Feb), `+ 5 business days`, `date - date`,
  `days_between_ a and_ b`, `business_days_between_ a and_ b`, `end_of_month_ d`
- `steal_from_ holidays_za` makes business days skip South African public holidays
  (fixed holidays, Good Friday, Family Day, and the Sunday -> Monday rule).
  Once-off holidays such as election days are not included.
- CSV cells like `2026-09-01` become dates, so you can filter and sort by date.
- Loan lengths can be written as `20 years` or `240 months`.

New in v0.6:

- Bookkeeping:

      book_ "Paid rent":
          debit_  rent_expense  8000.00 ZAR
          credit_ bank          8000.00 ZAR

  Every entry must balance (debits = credits) and use one currency, or it is
  refused with a balance error and nothing is recorded. `balance_of_("bank")` is
  the account's debits minus its credits; `journal_()` and `trial_balance_()`
  give tables.
- Saving: `save_to_ "report.csv" statement` writes a table, list or value as CSV.
  Money is written as a plain number (8000.00) so spreadsheets can add it up;
  read it back with `go_get_ "report.csv" as_ ZAR`. Empty cells load as nothing_,
  and total_of_ skips them.
- Exchange rates and conversion: `rate = 18.2034 ZAR per USD`, then
  `convert_ price to_ USD at_rate_ rate on_ 2026-09-25`. The rate and date show up
  in because_is_.

New in v0.7:

- Statistics: `average_of_`, `biggest_of_`, `smallest_of_` on lists and columns
  (numbers, money, dates or text; blanks are skipped)
- `group_by_ statement by_ category`: one row per category with a count and the
  total of every number/money column. `group_by_ list` counts each value.
- `add_where_ expenses 80.00 ZAR` adds to a list; for a table give one value per
  column: `add_where_ statement [2026-09-30, "Coffee", "food", -35.00 ZAR]`
- `erase_ name` removes a name and its value
- VAT, using your own `VAT` rate (e.g. `never_changes_ VAT = 15%`):
  `vat_on_ 1000.00 ZAR` gives 150.00 ZAR, `vat_inside_ 1150.00 ZAR` gives 150.00 ZAR
- Interest: `simple_interest_`, `compound_interest_`, `future_value_`,
  `present_value_`, each with (amount, rate with a period, length of time), e.g.
  `future_value_(10000.00 ZAR, 10% per year, 5 years)`. Compounding happens once
  per rate period, so for monthly compounding use `rate per month` and months.

New in v0.8:

- Records with `blueprint_`:

      blueprint_ Payment:
          date:        date
          description: text
          amount:      money

      rent = Payment(2026-09-01, "Rent", -8500.00 ZAR)
      show_me_results(rent.amount)

  Field types: number, money, percent, text, date, yes_no, list, anything.
  Values are checked when a record is made; `nothing_` is allowed for "not known yet".
  `records.amount` gives that field from a list of records, and `add_where_` adds
  a record to a table when its field names match the columns.
- Modules: `steal_from_ budget_tools` loads budget_tools.perry (next to your file)
  and runs it once. Inside it, `share_ with_vat, VAT` lists what others may take;
  with no share_ line, everything is shared. `in_that_ budget_tools steal_from_
  with_vat` takes just the names you list. Each module keeps its own names, taken
  values never change, and errors point at the module's own file and line.
- `duplicate_where_ statement by_ description` finds items that appear more than
  once, such as a payment charged twice.
- `ask_user_("Your salary?")` asks a question and reads the answer as money
  (25000.00 ZAR), a number, percent, date, yes/no or text. In VS Code, programs
  that ask questions run in the terminal automatically.

New in v0.8.2:

- `ask_user_("Salary?") as_ ZAR` reads amounts however they're typed (25000, R25 000,
  25,000.50, 25 000,50, 25000zar) and asks again if the answer isn't an amount. Also
  `as_ number`, `as_ percent`, `as_ date`, `as_ yes_no` and `as_ text`.
  `ask_user_("Press Enter")` can stand on its own line.
- `x not_ is_equal_to_ 5` works as well as `not_ x is_equal_to_ 5`.
- `define_ hello:` needs no brackets when there are no inputs.
- Clearer messages for a function used without give_back_, `return`, `print`, a missing
  bracket (`tax 100.00 ZAR`), names with different capitals, and words like
  `is_not_greater_than_`.

New in v0.8.3:

- The command is now `PERRY` (was `perry`). After `sudo make install` you can remove
  the old one with `sudo rm /usr/local/bin/perry`.
- `make static` for a stand-alone Linux/WSL build.

New in v0.9.0 (Perry for Universities):

- 12 decimal places (was 6) and numbers with up to 18 digits before the decimal point,
  stored in 128-bit integers. Still no floating point.
- Engineering economics: `npv_(rate, flows)` (first flow is today), `irr_(flows)`,
  `payback_(flows)`, `discounted_payback_(rate, flows)`.
- Depreciation schedules: `straight_line_`, `reducing_balance_`, `sum_of_years_`.
- Annuities: `annuity_`, `annuity_due_`, `deferred_annuity_`, `accumulated_annuity_`,
  `perpetuity_`; bonds: `bond_price_`, `bond_yield_`.
- Statistics, worked out as exact fractions: `variance_`, `standard_deviation_`,
  `population_variance_`, `population_standard_deviation_`, `median_`, `percentile_`,
  `correlation_`, `regression_` (a row with slope, intercept, r_squared, correlation, count).
- Life tables from CSV (`age` with `qx` or `lx`): `survival_`, `life_expectancy_`,
  `life_annuity_`, `temporary_life_annuity_`, `life_insurance_`, `term_insurance_`,
  `pure_endowment_`, `endowment_`. `examples/life_table.csv` is an illustrative table.
- `power_(x, n)` and `square_root_(x)`, exact to 12 decimals.
- All of it was checked against an independent 60-digit reference on more than 40 000
  random cases, with 0 differences.

Examples: `examples/records.perry` (with `examples/budget_tools.perry`), `examples/ask.perry`, `examples/analysis.perry`, `examples/interest.perry`, `examples/books.perry`, `examples/currency.perry`, `examples/dates.perry`, `examples/safe_statement.perry`, `examples/loan.perry`, `examples/statement.perry`, `examples/tax.perry`,
`examples/rounding.perry`.

Limits: numbers with up to 18 digits before the decimal point and up to 12 decimal places. In CSV files,
amounts must be plain numbers like `-1250.50`; values like `1,250.50` stay as text.

## Files

- `src/tokens.h`  - every token and keyword (add new keywords here)
- `src/lexer.c`   - reads the source and produces tokens
- `src/parser.c`  - turns tokens into a syntax tree
- `src/interp.c`  - runs the tree; Perry's money rules live here
- `src/value.c`   - exact decimal arithmetic and printing
- `src/errors.c`  - error messages with the line of code
- `src/csv.c`     - reads CSV files into tables
- `src/dates.c`   - calendar maths and public holidays
- `src/finance.c` - NPV, IRR, depreciation, annuities, bonds, statistics, life tables
- `src/main.c`    - the `PERRY` command
- `examples/`     - error demos, and `tour.perry` (uses features not built yet)
