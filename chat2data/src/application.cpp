#include "chat2data/application.h"

#include <ostream>
#include <string_view>

namespace chat2data {
namespace {

void print_usage(std::ostream& out)
{
    out << "Usage: chat2data [--help | --version]\n"
           "  --help     Show this help message\n"
           "  --version  Show the project version\n";
}

} // namespace

int run(int argc, const char* const argv[], std::ostream& out, std::ostream& err)
{
    if (argc == 1) {
        out << "chat2data is under development. Use --help for available options.\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(out);
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        out << "chat2data " << CHAT2DATA_VERSION << '\n';
        return 0;
    }
    err << "Invalid arguments.\n";
    print_usage(err);
    return 1;
}

} // namespace chat2data
