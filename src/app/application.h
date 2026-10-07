#pragma once

#include <fstream>
#include <string>

#include "app/app_config.h"

class Logger;
class Pipeline;
class IMarketEventReader;
struct InstrumentSpec;
struct MatchingEngineConfig;

class Application
{
   public:
    explicit Application(const AppConfig& cfg);
    int run() const;

   private:
    struct OutputFiles
    {
        std::ofstream orders;
        std::ofstream trades;
    };

    OutputFiles open_output_files(const std::string& source,
                                 const std::string& source_type = "source_ticks") const;
    // The parsed flags as the engine's own settings, in one place, because two modes build an engine from
    // the same flags and a mode should read as a list of objects rather than a list of numbers in order.
    MatchingEngineConfig engine_config(const InstrumentSpec& instrument) const;
    // Every mode opens its log the same way and applies the same four pipeline limits, and neither is worth
    // repeating at four call sites: the prologue is two lines of the same two calls, and the limits are the
    // risk controls a reader wants to find by name rather than infer from four adjacent setters.
    Logger make_logger() const;
    void apply_pipeline_limits(Pipeline& pipeline) const;
    void configure_logger(Logger& logger) const;
    void run_legacy_csv_mode() const;
    void run_replay_mode() const;
    // The L2 delivery path, whose only variable is where its two readers come from: the file replay
    // takes them from the depth and trade files, and the live session takes them from a websocket.
    // Everything after the readers exist -- the book, the strategy, the engine, the recorder, the
    // broker and the summary -- is the same code, which is what makes one strategy drive both.
    void run_l2_session_mode() const;
    void run_carry_mode() const;
    void run_legacy_udp_mode() const;

    AppConfig cfg_;
};
