# Retrospective

This is the record of what this project tried, what each attempt cost, why the ones that failed failed, and what is
left. It exists because the value of a negative result is the reason for it, and because the same wrong turns are
cheap to repeat: every row below is a question that has been answered and no longer needs asking.

## What the project was asking

Whether a personal account executed by a program can quote both sides of a market and earn the spread. The engine,
the gates and the pre-registration discipline in `docs/ROADMAP.md` exist to answer that with numbers rather than with
opinions, and the answer now has a shape: **the quoting earns money, and the inventory it must carry decides the
outcome by an order of magnitude.**

## The dead ends, what each cost, and why

| attempt | what it cost | why it ended |
|---|---:|---|
| Crypto perpetuals (Deribit, Binance, OKX) | free archives | a fee of 2.57 ticks against 0.088 ticks captured a fill: the fee is a multiple of the edge, and no lever moves a ratio like that |
| Micro E-mini futures | 0.97 USD | a commission of about half a tick against 0.0131 ticks captured, which is the same wall one market over |
| SPY, the window that started the fee question | 0.36 USD | capture 0.268 ticks against a 0.35 fee, and the latency gate then killed it: 1 ms of order latency took +262 USD to +1.89, and 5 ms took it to -1143 |
| The latency gate itself | free | a personal account does not control its round trip, so this is a structural ceiling rather than a parameter |
| The fee arrangements, to four decimal places | free | the four arrangements differ by 0.28 of a tick and this system's noise is 30: the precision mattered for the **sign** and for nothing else |
| The kill-switch threshold sweep, four thresholds | free | two would have shown the same thing: no threshold paid on the sample, and a dollar threshold against a varying notional is a control on size |
| Top of book as a cheap substitute for ten levels | free | measured against the same window it halves the fills, 1182 against 2324, because a one level book has nothing left when its touch is taken; the real saving was the window length, not the schema |
| Recording our own feed instead of buying history | free, in calendar time | correct in principle and slower than the question needed; it stays the default once a live line exists |
| The commission-only screen rule | 0.52 USD | it ranked a symbol by a fee that is a fifth of what the account is charged, so AAPL passed a rule that the account's own schedule then failed it on |
| The Avellaneda and Stoikov shift, first attempt | free | the shift was given in ticks against a one-tick quote spacing, so a six-tick shift was six half spreads: a unit error, recorded rather than hidden |
| The same shift, at the scale the model asks for | free | it made both columns worse at every width, which closed the quote-placement axis |
| A MES hedge of a MU inventory | free | MES explains 9.4 percent of MU's one second variance, so the hedge removes 8 percent of the inventory term at the horizon a fill lives on against 61 percent at five minutes: the common factor is slower than a quote |
| Reading a table by its sums | free | the summed ratio read as a sevenfold improvement where the dispersion said twenty percent, because the mean of the position term is not its risk |

## The rules that came out of it

1. **Cost the venue before tuning the strategy.** A fee that is a multiple of the edge cannot be optimised away, and
   neither can a spread that is one tick wide.
2. **Separate the edge from the position before reading any result.** Every window here that looked profitable looked
   profitable because of the position, and the one that looked like a pass on the fee was one.
3. **Pre-register the scope as well as the rule.** A rule that does not say which size it applies to produces two
   true readings and no decision, which is what the eight-window campaign found.
4. **A sum is not a measure.** What decides whether an edge is measurable is the dispersion of the term it competes
   with, not the sum of either.
5. **Unverified facts are labelled.** The account's schedule is read from pages rather than from a statement and the
   minimum per order is reported rather than confirmed, and both are marked where they are used.

## What is established

* The quoting margin -- capture less markout less every fee including the minimum per order -- is **positive in
  sixteen of sixteen rows**, eight MU windows at two sizes, averaging 940.59 USD a window at the pinned size against
  AAPL's -181.
* The position term that competes with it has a per window standard deviation of about 10588 USD, so separating the
  edge at two standard deviations needs about **506 windows**, two years of daily two-hour sessions.
* **No quoting parameter moves that ratio by more than twenty percent.** Distance from the touch, quote width, the
  inventory shift, size, the exposure cap, the kill switch and the fee schedule have all been swept.
* Four markets have therefore been falsified with numbers: crypto perpetuals, the micro E-mini, SPY and the US large
  cap equities, and in each case the reason is the ratio of the fee and the spread to the inventory's own move.
* **The equity line is placed in hibernation rather than continued**, and its reasons are numbers rather than a
  shortage of effort: the fee floor binds on seventy-nine percent of order instances, no swept parameter moves the
  ratio by more than twenty percent, and separating a 941 USD capture from a 10,588 USD position term would take five
  hundred and six windows. The C++ core stays as infrastructure; what is closed is the direction.

## What is open, and how each would be verified

* **Whether a US options venue pays a retail account for adding liquidity.** Read the exchange fee schedules and the
  broker's pass-through rather than a summary page; this document cites no source for it yet.
* **Whether a crypto venue offers a zero or rebate maker fee at a retail tier.** The recorded venues charge 0.0002,
  which is positive, so a claim of a negative fee needs a named venue, tier and pair before it is used.
* **Which instrument explains an inventory's move at the horizon it is held.** The MES measurement above is one row
  of that table, and a same industry instrument (SMH or SOXX for a memory name, MU options for MU) is the next.
* **Whether an options book can be read by this engine at all.** The readers take aggregated price levels and an
  options chain is many instruments on one underlying, which is a data contract question before a strategy question.

## What this cleanup did not delete, and why

Nothing in `src/`, `tools/` or `tests/` is unused. `src/lines/legacy/` holds the L1 market maker family that
`src/app/strategy_factory.cpp`, `src/app/strategy_benchmark.h`, `src/backtest/` and the CSV and UDP feeds all
include; and each of the six tools that look unreferenced from `docs/` -- `fetch_okx_trades.py`,
`multi_day_check.sh`, `inspect_source.py`, `check_okx_book.py`, `benchmark_replay_windows.py` and `trace_order.sh` --
is documented in `data/README.md` or `docs/USER_GUIDE.md`. What was actually redundant was generated or ignored: a
stale `site/` build of these documents and the working logs under `out/`, both of which stay out of the repository by
construction. Deleting a referenced strategy would break the gates that make every number above re-derivable, which
is the asset this phase produced.

## The eight lines, and what is left when they are all closed

The project set out to find a market where a personal account's quote pays for itself. It now has eight answers, each
one a number rather than an opinion.

| line | the number that closed it |
|---|---|
| crypto perpetuals, the micro E-mini, SPY | a fee that is a multiple of the spread the venue offers, so a quote cannot pay for itself at any width |
| US large cap equities (MU, MSTR, AMD) | the quoting margin was positive in 16 of 16 windows and the position term was 10,588 USD against 941, which is 506 windows to separate; and the fee floor binds on 79 percent of order instances |
| options at one day to expiry | the spread is payment for tail risk rather than a quoting edge, and the premium can be smaller than the fee floor |
| options from fifteen days out | the fee axis passed for the first time in the project, and the print count then failed: the best of eight classes is R_max 56 against a reference of 71 |
| funding rate carry, BTC, two venues, twelve months | 0 of 12 months clear the level and payback gates, and no run of a positive differential in any month earns the 36 USD round trip |
| a low turnover reversion rule | the sign does not replicate: 4 of MU's 8 windows positive, median net -23.34 cents a share, and the best window earned 149.76 while the worst lost 980.38, so it wins small and loses big |

**Those eight rows are data now, not prose.** `tools/viability.py` re-derives every quotient from the inputs the
rows carry, checks that the first gate to fire is the one each line is recorded as binding on, and prints what each
gate rested on. Two things in it are there because the alternative is a lie: a gate that was not measured prints as
`not measured` rather than as a pass, and a bound taken on the most favourable case -- the widest spread, the smallest
position move -- may close a line and is refused if it is ever read as an opening, because the best case passing
proves nothing about the rest. The MU row's 506 windows is not carried as a number of its own either: it is
`(2 x 10,588 / 941)^2`, at the two standard deviations the row names, so it falls out of the same two inputs the gate
is read from. The inputs are in `tests/fixtures/verdicts.json` and tier 1 of `tools/verify_l2.sh` runs it on every
edit.

**Two of those inputs are no longer copies.** `python3 tools/viability.py --derive` runs the tools that produced them
and compares what comes back with what the fixture recorded: the low turnover rule over MU's eight windows returns a
median of -23.34 cents a share either way, and `tools/venue_ratio.py` over the option series returns 56 prints a fill
as the best of them either way. The option series that second measurement needs were bought again into
`data/v2/options/`, because the first copy lived in `/tmp` and nothing behind it could be re-derived once it was
gone; it cost 0.011938 USD and its print counts and R_max reproduce the table in `docs/ROADMAP.md` row for row. The
campaign is not in the gate, because it needs the archives and takes minutes where the gate runs on every edit.


**Three things survive all of them.** The engine: a market data handler, a matching engine, an order state machine, a
queue model and a fee model, re-derived by four gate tiers on every commit. The criteria, which turned out to be the
same three in a fixed order -- **the fee, then the position's own move, then the number of actions a window contains**
-- with the binding one moving down that list on each new venue. And the recipe, which is what the eight lines actually
bought: **one question a step, stated before the step; the measurement named before the money is spent; every claim
re-derived by the binary; and two non-adjacent windows before anything is believed.**

Total spent on data across all eight: **1.27 USD**, of which the funding histories were free.
