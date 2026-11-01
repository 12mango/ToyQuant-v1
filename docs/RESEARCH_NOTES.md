# Research Notes

This is not a plan and it re-derives none of the numbers the gate checks. It records two things the
repository could not measure from its own data: **what individual quantitative traders have actually
done** (and which of those routes were a business rather than a bet), and **what a Hong Kong personal
account on Interactive Brokers Pro can legitimately do about the fee** that closed eight lines in
[Performance History](PERFORMANCE_HISTORY.md).

**Every external claim carries a verification mark, because the value of a success story is its
provenance.** `[read]` means the page was fetched during the session and the claim is in it.
`[reported]` means it is widely published but the primary source was behind a paywall or a block, so it
is a lead rather than a fact. Numbers inside the repository are the exception: they are re-derived by
`bash tools/verify_l2.sh fast` and need no mark.

---

## 1. What the individual cases actually are

### 1.1 Documented individual quant that worked, and the shape of the work

| Case | Technical route | Verdict | What decided it |
|---|---|---|---|
| **Ed Seykota** `[read]` | Punched-card computers from 1970, exponential moving averages after Donchian's 5/20-day rules, volatility-scaled size | **Business**, with a caveat | Positive-skew trend following: cut losses, let winners run. The record comes from a Schwager interview and is **not audited**, and the article carries a neutrality dispute, so the number is a reputation rather than a statement |
| **Richard Dennis and the Turtles** `[read]` | Donchian breakout, ATR-based sizing, 2 percent risk a trade; 23 trainees (21 men, two women) in two intakes from December 1983 | **The rule was a business; the people were a distribution** | The same rules and the same years produced a handful of lasting traders and a majority of failures `[reported]`. Dennis ran the experiment to settle a bet; the honest reading is that a rule can be taught and its execution cannot |
| **Rob Carver** `[read]` | Independent systematic futures trader, ex-Barclays, volatility targeting with trend and carry, code published as `pysystemtrade` | **Business, at a modest size** | His own summary is the most useful line here: he is "not sure if genuine skill exists in the investment world, but he knows for sure he doesn't have any". His recurring themes are small accounts, costs and drawdown |
| **Kevin Davey** `[reported]` | Futures algorithms behind an incubation process: out-of-sample forward testing, parameter robustness, portfolio de-correlation | **Business, process first** | The method is the asset: a strategy is proven non-fragile before it is sized |
| **Andreas Clenow** `[reported]` | Multi-market trend, ATR sizing, monthly rebalance, rules published in full | **Business** | Replicable by a reader, which is the cheapest possible audit |
| **Meb Faber, Gary Antonacci** `[reported]` | ETF rotation and dual momentum, monthly or quarterly turnover | **Business, but decaying** | Low turnover keeps the fee term small; the premium has been arbitraged down since publication |
| **Numerai participants** `[read]` | Encrypted feature tournament, weekly submissions, staking a token | **Lottery for almost everyone** | The platform returned 20 percent in 2022 on 100 million USD of inflows; payouts to the crowd are concentrated in the head of a long distribution, and staking adds token risk |
| **Collective2 and audited retail track records** `[reported]` | Individual strategies offered for copying | **The only visible denominator** | A few percent survive three years with a controlled drawdown; the rest are the base rate that everyone else quotes as a success story |

### 1.2 Routes that looked like a business and were bets

| Case | Technical route | Why it was a bet |
|---|---|---|
| **Victor Niederhoffer** `[read]` | Statistics doctorate, selling index options with leverage | Negative skew plus leverage plus doubling after a loss. He blew up in 1997 and again in 2007; the first method was right about direction and wrong about survival |
| **Long-Term Capital Management** `[read]` | Relative-value convergence, leverage of roughly 25 to 1, two Nobel laureates on the board, returns of about 21, 43 and 41 percent in its first three years | Lost 4.6 billion USD in under four months in 1998 and needed a 3.65 billion recapitalisation. The models assumed that correlation and liquidity stay put, and in the tail the two fail together |
| **Short-volatility notes such as XIV** `[reported]` | Selling short-dated VIX futures, packaged for retail | "Stable carry" that gave up about 96 percent in one session on 2018-02-05 and was terminated. Every cent earned was payment for standing in front of a tail |
| **"Karen the Supertrader"** `[reported]` | Retail short options, a high published win rate, heavy leverage | A 2018 blowup. A 95 percent win rate decides nothing when the remaining 5 percent is unbounded |
| **Optionsellers and James Cordier** `[reported]` | Naked natural-gas options sold inside client accounts | Clients wiped out and left owing money in November 2018 |
| **Grid and martingale bots** `[reported]` | Add to losers inside a range | Perfect equity curves in chop, one trend to zero. The smoothness of the backtest is the warning sign rather than the selling point |
| **Contest leaders in Chinese commodity futures** `[reported]` | Leverage and concentration over three to twelve months | A contest is a short sample, and long-shot strategies win short samples by construction |
| **Knight Capital, 2012-08-01** `[read]` | Not a strategy bet but an **engineering** one: a deployment left old code live on seven of eight servers | 440 to 460 million USD lost in about 45 minutes, the equity value largely gone, and the firm absorbed the following year. Operational risk is a tail of the same family |

### 1.3 The base rate

| Study | Finding | Mark |
|---|---|---|
| Chague and De-Losso, Brazilian day traders | 97 percent of those who persisted 300 days or more lost money; 1.1 percent earned more than the minimum wage and 0.5 percent more than a bank teller | `[reported]` |
| Barber, Lee, Liu and Odean, Taiwan | Under 1 percent of day traders are consistently profitable | `[reported]` |
| Barber and Odean, individual investors | The more a household trades, the worse it does, by 1.5 to 6.5 points a year against the market | `[reported]` |
| Bailey and Lopez de Prado, backtest overfitting | With enough trials any Sharpe ratio can be manufactured; the deflated Sharpe and the reality check are what separate a result from a search | `[reported]` |
| Haghani's 5,000-coin-toss experiment | A year of outperformance cannot distinguish skill from luck at that sample size | `[reported]` |

**The number that matters is the one nobody publishes: the denominator.** Twenty-three Turtles,
hundreds of thousands of users over the nine years before Quantopian shut down `[read]`, thousands of
retail strategies on the copy-trading platforms, and a global population of grid-bot users. The
numerator is what gets interviewed.

---

## 2. The nine tests that separate the two columns

The first three are the ones this repository has already run on its own money-shaped problems.

| # | Test | A business looks like | A bet looks like |
|---|---|---|---|
| 1 | **The shape of the return distribution** | Positive skew: many small losses, a few large gains | Negative skew: many small gains, one terminal loss |
| 2 | **Turnover times cost against gross edge** | Gross edge several times the measured cost | The backtest quietly ignores cost, or cost is more than half the gross |
| 3 | **Breadth** | The information ratio scales with the square root of the number of independent bets | One instrument, one window, one draw |
| 4 | **Leverage against the tail** | Survives its worst plausible day at its own leverage | Leverage that turns a three-sigma day into insolvency |
| 5 | **The backtest method** | Out of sample, walk-forward, parameter and cost sensitivity, deflated Sharpe | One in-sample optimum, chosen after seeing the answer |
| 6 | **Worst outcome against the mean** | Worst trade inside a small multiple of the mean | Worst trade hundreds of times the mean |
| 7 | **Capital and capacity** | Notional per position large enough to amortise the fee | Wide and small at once, where the minimum per order decides everything |
| 8 | **Operational risk** | Kill switch, reconciliation, staged deployment | One process, one server, one deploy |
| 9 | **Provenance** | Audited, continuous, losing years included | Multiples and screenshots, winning years only |

---

## 3. The fee question for one Hong Kong account

**The account can only be Interactive Brokers Pro, and for a maker that is not a disadvantage.** IBKR's
own schedule lists `IBKR Lite` as **US residents only** `[read]`, so it is not something a Hong Kong
account is being denied. More to the point, Lite could not run a market-making strategy at all: it
routes to wholesalers, so a resting order never posts to a venue and never earns a rebate. **Pro Tiered
is the correct account for anything that wants to be paid for adding liquidity**, and the levers below
are what it offers.

### 3.1 What IBKR Pro actually charges for US shares `[read]`

| Monthly volume, shares | Pro Tiered, USD a share | Pro Fixed |
|---|---:|---:|
| up to 300,000 | 0.0035 | 0.005 |
| 300,001 to 3,000,000 | **0.0020** | 0.005 |
| 3,000,001 to 20,000,000 | **0.0015** | 0.005 |
| 20,000,001 to 100,000,000 | 0.0010 | 0.005 |
| above 100,000,000 | 0.0005 | 0.005 |
| minimum an order | 0.35 | 1.00 |
| maximum an order | 1 percent of trade value | 1 percent of trade value |
| third-party fees | regulatory, exchange, clearing, pass-through | regulatory only |

The third-party terms are the ones this repository already models: the SEC fee of 0.0000206 of aggregate
sales, the consolidated audit trail at 0.000003 a share, NSCC and DTC clearing at 0.00020 a share, and
pass-through fees charged as a fraction of the commission. **Tiered is the only plan that passes the
exchange fees and the maker rebate through**; Fixed folds the exchange fee into its 0.005 and credits
nothing.

### 3.2 The six levers, in order of size

1. **The monthly volume tier, which is free, and which the strategy that trades most does not collect.** A
   window of the size already measured crosses it: the AAPL window's 108,667 shares in two hours are about 2.2
   million a month and therefore **tier 2 at 0.0020**, and the MU window's 164,599 are about 3.3 million and reach
   **tier 3 at 0.0015** — a 43 to 57 percent cut of the commission line. Measured through the engine on that
   window, **the total fee does not move at all**: the floor absorbs every cent of the discount, because a quoting
   strategy's order fills about 24 shares at a time and therefore pays a quarter of its commission against the
   whole 0.35 floor. The tier pays in full only where orders fill in full, which is a factor or taker book, and
   about 15 percent where they do not; the measured table is in
   [Performance History](PERFORMANCE_HISTORY.md#what-the-queue-model-decides-and-what-the-per-order-floor-does).
2. **Tiered rather than Fixed**, for any maker. The rebate is 0.0013 a share, worth 37 percent of a
   0.0035 commission, and Fixed neither collects nor passes it through.
3. **The minimum per order has to move with the tier, or the discount is confiscated.** The floor is 0.35
   an order, so a clip of 100 shares pays exactly 0.35 at tier 1 and no more, but at tier 2 the same clip
   pays 0.20 of commission against a 0.35 floor. The clip that keeps the floor invisible **for a complete fill**
   is `0.35 / rate` shares: **100 shares at tier 1, 175 at tier 2, 234 at tier 3**. A strategy that halves
   its rate without raising its clip hands the difference back.
4. **The instrument class, which is worth more than any of the above in basis points.**

   | Instrument and plan | Cost per unit per side | Notional per unit | Basis points per side |
   |---|---:|---:|---:|
   | ES future, Pro Tiered | about 2.23 USD, 0.85 commission plus exchange | about 340,000 | **0.066** |
   | HSI future, Pro Tiered | about 18.5 HKD, 8.50 plus exchange | about 1.25 million HKD | **0.15** |
   | MES future, Pro Tiered | about 1.20 USD | about 34,000 | **0.35** |
   | AAPL, a clip of 100 shares, tier 1 | about 0.37 USD | 33,200 | 0.11 |
   | a 20 dollar share, a clip of 100 shares | about 0.35 USD | 2,000 | **1.75** |
   | a 20 dollar share, a clip of 500 shares | about 1.78 USD | 10,000 | 1.78 |
   | OKX BTC perpetual, maker | 2 basis points of notional | any | **2.0** |

   Two things fall out. **The per-share terms are a fixed number of dollars a share, so a cheap share is
   expensive in basis points and an expensive one is cheap**, and the tick has the same property: one
   cent is 5 basis points of a 20 dollar share and 0.3 of a 332 dollar one. And **futures are five to
   twenty times cheaper in basis points than shares**, which is the arithmetic behind the trend and carry
   routes in section 1.1 and the reason a low-turnover book should not default to stocks.
5. **Regulatory fees cannot be avoided, but they dilute.** The SEC fee is charged on the proceeds of a
   sale only, 0.103 basis points of value whatever the price; the audit trail and the clearing fees are
   charged a share, so they shrink in basis points as the share price rises. A market-neutral book sells
   on half its fills, which is why the model halves the SEC term.
6. **What is deliberately not on the list, because it is a different risk rather than a saving.**
   Zero-commission retail brokers route to wholesalers, so a resting order cannot post to a venue or earn
   a rebate; trading extra volume to reach a tier sooner is the fee saving eating a negative gross edge;
   and a venue with a zero maker fee has a spread that pays for it.

### 3.3 Two Hong Kong-specific leaks, both larger than the rebate

- **Dividend withholding.** The Hong Kong and United States arrangement gives no treaty rate, so US
  dividends are taxed at 30 percent at source, while an Irish-domiciled fund holding the same shares
  suffers 15 percent. On a 2 percent yield that is 30 basis points a year, against the 141 USD a window
  the maker rebate was worth on the AAPL measurement.
- **No capital gains tax.** That is an advantage worth protecting, and the reason a personal account
  should not be routed through a structure that creates a US taxable presence.

---

## 4. What this record changes in the repository

Nothing the gate re-derives. Two assumptions change, and both make the earlier conclusions stronger:

1. **The micro E-mini's fee was understated.** `micro_es_spec` charges 0.62 USD a contract, which is
   IBKR's rate only above 1,000 contracts a month `[read]`; a retail account at or below that volume pays
   **0.85 plus the exchange fee**, roughly twice the model's figure. The recorded row that already lost
   2,826 USD on 1,675 fills loses more, so its verdict stands rather than weakening.
2. **The rebate is worth 6.4 percent of one window's loss, not a sign change.** The four-schedule run in
   [Performance History](PERFORMANCE_HISTORY.md) prices the credit at 141.27 USD against a gross that is
   negative before any fee, and the volume tier moves the same order of magnitude: a 43 percent commission
   cut on a 1,087 USD fee saves about 163 USD on a window that loses 2,202. **The tier is the largest
   legitimate fee lever this account has, and it is smaller than the hole.**

The conclusion the fee work has been converging on is unchanged and now has an external control: the cases
in section 1.1 earn their money on a positive gross edge and use cheap instruments and low turnover to
keep the cost term small, while the cases in section 1.2 failed on distribution shape, leverage or
operations, and not on the fee.
