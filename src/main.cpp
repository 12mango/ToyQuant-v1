#include <exception>
#include <iostream>
#include <string>

#include "app/app_config.h"
#include "app/application.h"

int main(int argc, char** argv)
{
    AppConfig cfg;
    std::string error;
    if (!parse_config(argc, argv, cfg, error))
    {
        if (!error.empty())
        {
            std::cerr << "Error: " << error << "\n";
        }
        print_usage(argv[0]);
        return error.empty() ? 0 : 1;
    }

    try
    {
        return Application(cfg).run();
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}