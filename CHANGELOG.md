# Changelog

## The live paper milestone (current)

The websocket reader, the paper mode and the recorder half of a live path, which is roadmap phase 2: the
seam it closes is the one the roadmap names, that a live feed is another reader and paper trading is the
existing book and matching engine against it. So the live path is the L2 replay path with its two readers
moved onto a socket, and everything downstream of them is the same code. Its entry point is the `ws` mode in
[User Guide](docs/USER_GUIDE.md), and the numbers the other documents quote are re-derived by
`bash tools/verify_l2.sh fast`, which this milestone leaves unchanged.

### Highlights

- **A websocket feed behind the reader interface.** `src/market/ws_feed.*` speaks RFC 6455 -- the handshake
  and text frames -- with no external dependency, and implements `IMarketEventReader` as two readers over one
  connection, so the book, the strategy, the engine, the recorder and the paper broker cannot tell a live
  session from a replayed one.
- **The wire is the replay format.** One CSV row per text message, its kind read from its column count, in the
  two shapes `--record` writes, so a recording streamed back over the feed reproduces itself byte for byte.
- **A desk loop with no credential.** `tools/websocket_sender.py` stands a WebSocket server in for a venue and
  streams a trade and a depth file, interleaved by their own timestamps, so the whole path runs locally. A
  unit test closes the same loop inside `ctest`, with a server thread standing in for the venue.
- **One mode, reused.** `ws` accepts every `l2_replay` flag -- the recorder, the feed thread, the profiler and
  the paper broker -- because it is that mode with the readers swapped, and `run_l2_replay_mode` is now
  `run_l2_session_mode`, which owns both sources.

## The measured line

The line that adds the queue model, the fee model, the two latency legs, the order trace and the queue
distribution, the strategy sweeps, and the four-tier gate. Its entry point is the
[Design Review](docs/DESIGN_REVIEW.md), and every number it quotes is re-derived by
`bash tools/verify_l2.sh fast`.

### Highlights

- **Queue-aware fills.** Four queue models from a conservative lower bound to an optimistic upper bound,
  plus an arrival share, so a fill depends on the queue in front of a quote rather than on its price.
- **Fees charged on the instrument's face value**, after a model that charged price times quantity on an
  inverse contract overstated every fee by about 630 times and turned a strategy comparison into a
  measurement of the overcharge.
- **Two latency legs with opposite signs.** A late arrival quotes wider and fills less; a late cancel leaves
  a stale quote standing and is the leg that costs money.
- **Mechanism visibility:** a per-order trace, and a queue distribution over every order. On the mainline
  window the median quote never came within 3.9% of the front of its queue, and every order that reached the
  front traded.
- **Strategy sweeps:** quote width, requote threshold, order size and inventory limit, plus a four-strategy
  comparison with enough fills to read. The fee-to-edge ratio does not move across any of them.
- **A four-tier gate:** unit tests, pinned invariants and documented numbers, a full-file output hash, and an
  AddressSanitizer/UndefinedBehaviorSanitizer tier.

## v1.0-demo — the frozen demo

Tagged `v1.0-demo`, and kept for reproducing the earlier results and as a regression target. The simple
line: Binance Trade+BBO replay, legacy CSV and UDP inputs, the L1 strategy set, and an L1 fill that is a
price event because that path models no queue.
