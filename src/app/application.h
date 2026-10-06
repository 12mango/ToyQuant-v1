#pragma once

#include <fstream>
#include <string>

#include "app/app_config.h"

class Logger;
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
    void configure_logger(Logger& logger) const;
    void run_legacy_csv_mode() const;
    void run_replay_mode() const;
    void run_l2_replay_mode() const;
    void run_carry_mode() const;
    void run_legacy_udp_mode() const;

    AppConfig cfg_;
};
