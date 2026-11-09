#pragma once

#include <iosfwd>

namespace chat2data {

// Runs the command-line entry point. The caller owns the streams and arguments.
int run(int argc, const char* const argv[], std::ostream& out, std::ostream& err);

} // namespace chat2data
