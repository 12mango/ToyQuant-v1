#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/types.h"

enum class AppMode
{
    LegacyCsv,
    LegacyUdp,
    Replay,
    L2Replay,
    // A live session: the same L2 delivery path as L2Replay, with the two readers taken from a
    // websocket instead of from files. It carries the same flags, the book, the matching engine, the
    // recorder and the paper broker, because the engine cannot tell a live feed from a replayed one.
    WebSocket,
    // A two leg carry measured over two funding histories rather than replayed against a book: the
    // position is stated, and what is measured is what holding it pays and costs.
    Carry
};

struct AppConfig
{
    AppMode mode = AppMode::LegacyCsv;
    std::string path_or_port = "data/scenarios/synthetic_ticks.csv";
    std::string quotes_path;
    std::string symbol;
    // The live websocket endpoint of a ws-mode run: host:port from the positional argument, and the
    // HTTP target the handshake asks for, which defaults to "/" and is set by --ws-target=.
    std::string ws_host;
    uint16_t ws_port = 0;
    std::string ws_target = "/";
    // Wall-clock seconds a ws session streams before it stops on its own; 0 runs until the peer
    // closes the connection. It is what a real capture needs, where no one closes the socket for you.
    double ws_seconds = 0.0;
    int delay = 0;
    std::string strategy_name = "optimized";
    uint64_t quantity_scale = 1000000;
    // The calibrated model, not the skeptical one. tools/calibrate_queue_model.py measures 99.96% of
    // the size that leaves a book level as cancel or amendment rather than trade, so the only free
    // parameter left is where in the queue those cancels sit, and pro-rata is the neutral assumption
    // about that. `conservative` (nothing comes off the queue ahead) and `optimistic` (all of it does)
    // bracket it and remain available as the sensitivity bounds they always were.
    QueueModel queue_model = QueueModel::ProRata;
    bool discard_output = false;
    bool fast_validation = false;
    // 0 keeps the value derived from the instrument spec, which is what every earlier run used:
    // order_size = quantity_scale/1000, inventory_limit = quantity_scale/10. Those derivations read
    // as if quantity_scale encoded an economic scale, but for an instrument whose quantities are
    // already contract counts it does not, and BTC-PERPETUAL ends up with an order size of one
    // contract and an inventory limit of one. These overrides let a run model a size that does not
    // come from the quantity scale, without changing any existing command line.
    uint64_t order_size_override = 0;
    int64_t inventory_limit_override = 0;
    // Pre-trade position limit in quantity units for the pipeline gate. 0 leaves the gate off, which
    // is the historical behaviour; the run still reports the marked exposure against the collateral.
    int64_t position_limit = 0;
    // The two hard limits, both off at 0. `max_exposure_usd` refuses an order whose projected marked
    // exposure would pass it, in USD rather than in shares because the same share count is three times the
    // exposure on a thousand dollar share. `kill_switch_loss_usd` stops the run quoting once realised plus
    // unrealised reaches that loss, and it does not flatten: a switch that loses money on the way out is a
    // worse control than one that stops.
    double max_exposure_usd = 0.0;
    double kill_switch_loss_usd = 0.0;
    // Quote placement, as a half-spread in ticks: 1 puts the quote at the venue touch, 2 is the
    // default the recorded runs use. 0 means "use the default". It exists because the fill count is
    // dominated by where the quote sits, and that has to be measurable rather than argued about.
    double base_spread_ticks = 0.0;
    // How far the inventory-aware strategy moves its quote centre at a full inventory, in ticks: the
    // Avellaneda and Stoikov reservation price in the form this engine can use, where the shift is
    // proportional to the position rather than solved from a risk aversion. 0 uses the strategy's own 2.0.
    double inventory_shift_ticks = 0.0;
    // QueueModel::Lumpy settings: how many whole orders a level decrease is read as, and the seed for
    // the draws. Both default to the model's own choice, so a run without them is reproducible.
    uint64_t queue_lump_chunks = 0;
    uint64_t queue_seed = 0;
    // The share of the displayed size a new quote starts behind, in [0, 1]. 1.0 is the FIFO arrival rule
    // the engine uses by default; smaller values place the quote inside the queue instead of at its back.
    double arrival_share = 1.0;
    // One-way latency between the strategy sending an order and the exchange having it, in microseconds.
    // 0 is a synchronous engine and is what every recorded run uses.
    uint64_t order_latency_us = 0;
    // One-way latency between the strategy sending a cancel and the exchange applying it. 0 is a
    // synchronous engine and is what every recorded run uses.
    uint64_t cancel_latency_us = 0;

    // The order whose queue the run should trace, 0 for none. Reports as [QUEUE_TRACE] lines.
    uint64_t trace_order = 0;
    // Deliver the replay against the wall clock instead of as fast as the file reads, at this speed. A rehearsal
    // is the point: a strategy that watches its own quote age or cancels on a timer only behaves like itself when
    // the market arrives at its real speed, and the delivery's lateness against the schedule is the feed-side
    // number a live session has to watch. Off, which is the default, leaves every recorded number untouched,
    // because pacing changes no decision, only when the decision is made.
    bool live_pacing = false;
    double live_pacing_speed = 1.0;
    // Records the delivered stream instead of only measuring it: --record=PREFIX writes
    // PREFIX_trades.csv and PREFIX_depth.csv in the shapes the replay reads, so a run that was
    // paced or fed live becomes a file that can be replayed again. Empty, the default, records
    // nothing and leaves every recorded number untouched.
    std::string record_prefix;
    // Runs the feed on its own thread and hands each event to the strategy thread over a bounded
    // single-producer ring, which is the shape a live path has and the reason the latency report
    // below exists. Off, the default, keeps the direct path and its output exactly as it was.
    bool feed_thread = false;
    // Ring capacity for the threaded path, 0 for the feed's own default. A capacity the strategy
    // cannot keep up with shows up as backpressure on the feed rather than as a lost event.
    std::size_t ring_capacity = 0;
    // The paper broker: a decorator over the matching engine that models what a broker does and a venue
    // does not, and keeps its own statement of what the venue executed so the run's accounting can be
    // reconciled against a second account of the same session. Off, the default, is the plain engine,
    // and every recorded number stays what it was.
    bool paper_broker = false;
    // The broker's faults, each a count of orders or fills rather than a drawn probability, so a faulted
    // run is reproducible from its command line alone. 0 is no fault, and each needs --paper-broker.
    uint64_t broker_reject_every = 0;
    uint64_t broker_partial_every = 0;
    uint64_t broker_drop_every = 0;
    uint64_t broker_outage_after = 0;
    uint64_t broker_outage_holds = 0;
    // A maker fee rate the command line supplies, for the runs that ask what a different fee schedule would
    // have produced. The venues' rates are in the instrument specifications, so this is a sensitivity input
    // rather than a schedule, and it is unset for every run whose numbers are recorded as a venue's own.
    bool has_maker_fee_rate = false;
    double maker_fee_rate = 0.0;
    // A whole broker schedule, for the runs that ask what the account's own terms would have produced: the
    // floor under one order's commission, the fee on the proceeds of a sale, the per-share fees on every
    // execution, and the venue's credit for adding liquidity. `--fee-schedule=min=0.35,regulatory=0.0000206,
    // clearing=0.0002,audit=0.000003,rebate=0.0013` sets all five, any subset sets the rest to zero, and a
    // run that names none is priced under the venue's own schedule, which is every recorded row.
    struct ScheduleTerms
    {
        double min_commission_per_order{0.0};
        double regulatory_fee_rate{0.0};
        double clearing_fee_per_share{0.0};
        double audit_fee_per_share{0.0};
        double maker_rebate_per_share{0.0};
    };
    bool has_fee_schedule = false;
    ScheduleTerms fee_schedule;
    // The adverse-selection gate: quote only where the capture also covers what passive fills at that distance
    // have actually cost, measured from the market's own prints. It exists on the L2 decision layer that carries
    // the expected-value gate, so it is refused for the other strategies rather than silently ignored.
    bool markout_cover = false;
    // Inventory discipline: above this many quantity units the L2 decision layer reduces the position with a
    // taker order at the touch instead of carrying it. 0, the default, leaves the position carried into the
    // final mark, which is what every run before this flag did.
    int64_t flatten_band = 0;
    // A funding rate history for the replayed instrument, empty for no funding. The replay applies one
    // settlement each time the event clock passes one, reported as a [FUNDING] line that appears only when
    // this is set, so a run without it reproduces the recorded output byte for byte.
    std::string funding_rates_path;
    // The carry mode's second leg and its two symbols. `path_or_port` holds the first leg's funding file
    // there, because the first path of any mode is the one the positional arguments start with.
    std::string funding_b_path;
    std::string symbol_b;
    // USD held on each carry leg: one leg long and one short, so the account's own exposure is the
    // difference between them and this is the size of each.
    double carry_notional_usd = 10000.0;
    // Which leg is the long one. The mode holds the position the command states rather than choosing a side
    // from the data it is about to measure, so this is an input and not a decision.
    bool carry_leg_a_long = true;
    // How far the fair price has to move before a resting quote is pulled and replaced, in ticks.
    // 0 means "use the strategy default". This is the other half of the fill-rate question: a quote
    // that is replaced on every twitch never holds a queue position long enough to reach the front.
    uint64_t refresh_price_ticks_override = 0;
    // The decision layer's expected-value gate: a quote must cover the maker fee plus this many extra
    // ticks, and 0 leaves the gate off, which is what every run recorded in the documents asks for.
    double edge_cover_ticks = 0.0;
    // The conditional rule: quote only when the top of book is at least this many ticks wide, and 0 leaves it
    // off, which is what every run recorded in the documents asks for.
    double min_spread_ticks = 0.0;
    // 0 disables per-stage profiling. Otherwise this is the sampling interval in events,
    // so 64 times the 64th event of every 64.
    uint64_t profile_sample_interval = 0;
};

std::string to_abs_path(const std::string& input_path);
bool parse_config(int argc, char** argv, AppConfig& cfg, std::string& error);
void print_usage(const char* executable);
