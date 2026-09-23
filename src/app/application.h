#pragma once

#include <fstream>
#include <string>

#include "app/app_config.h"

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
    void run_legacy_csv_mode() const;
    void run_replay_mode() const;
    void run_l2_replay_mode() const;
    void run_legacy_udp_mode() const;

    AppConfig cfg_;
};
