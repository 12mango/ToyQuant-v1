// The CLI of the C++ relay. It mirrors `tools/venue_relay.py`'s flags so the two are interchangeable:
// the engine connects to `--listen`, the venue is `--venue`, and a window can be recorded directly or
// captured raw and replayed with no network.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <string>

#include "lines/venue_relay.h"

namespace
{
void usage(const char* executable)
{
    std::cerr << "Usage: " << executable << " [options]\n"
              << "  --venue=URL             the venue's websocket url (default OKX public)\n"
              << "  --instrument=SYM        the instrument to subscribe (default BTC-USDT-SWAP)\n"
              << "  --books-channel=CH      books | books5 | bbo-tbt (default books)\n"
              << "  --levels=N              levels kept from a snapshot, 0 keeps all (default 25)\n"
              << "  --contract-size=X       contract value for the quantity scale (default 0.01)\n"
              << "  --quantity-scale=N      engine units per base unit (default 1000000)\n"
              << "  --listen=HOST:PORT      serve the engine here; empty disables serving\n"
              << "  --capture=FILE          append the venue's raw messages, for --replay\n"
              << "  --record=PREFIX         write PREFIX_trades.csv and PREFIX_depth.csv directly\n"
              << "  --replay=FILE           serve a capture instead of connecting; no network\n"
              << "  --max-seconds=X         stop after X seconds; 0 runs until stopped\n"
              << "  --self-test             decode the shapes a live capture showed, then exit\n";
}

std::string option(const std::map<std::string, std::string>& options, const std::string& name,
                   const std::string& fallback)
{
    const auto found = options.find(name);
    return found == options.end() ? fallback : found->second;
}
}  // namespace

int main(int argc, char** argv)
{
    RelayConfig config;
    std::map<std::string, std::string> options;
    bool self_test = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--self-test")
        {
            self_test = true;
            continue;
        }
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument.rfind("--", 0) != 0)
        {
            std::cerr << "Error: unexpected argument " << argument << "\n";
            return 1;
        }
        const std::size_t equals = argument.find('=');
        if (equals != std::string::npos)
        {
            options[argument.substr(0, equals)] = argument.substr(equals + 1);
        }
        else
        {
            if (index + 1 >= argc)
            {
                std::cerr << "Error: " << argument << " needs a value\n";
                return 1;
            }
            options[argument] = argv[++index];
        }
    }

    try
    {
        config.venue = option(options, "--venue", config.venue);
        config.instrument = option(options, "--instrument", config.instrument);
        config.books_channel = option(options, "--books-channel", config.books_channel);
        config.listen = option(options, "--listen", config.listen);
        config.capture = option(options, "--capture", config.capture);
        config.record = option(options, "--record", config.record);
        config.replay = option(options, "--replay", config.replay);
        const auto present = [&options](const std::string& name) { return options.count(name) != 0; };
        if (present("--levels")) config.levels = std::stoul(options["--levels"]);
        if (present("--contract-size")) config.contract_size = std::stod(options["--contract-size"]);
        if (present("--quantity-scale"))
            config.quantity_scale = std::stoull(options["--quantity-scale"]);
        if (present("--max-seconds")) config.max_seconds = std::stod(options["--max-seconds"]);

        return self_test ? relay_self_test() : run_relay(config);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
