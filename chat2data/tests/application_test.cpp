#include "chat2data/application.h"

#include <iostream>
#include <sstream>
#include <string>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool test_run(int argc, const char* const argv[], int expected_status,
              const std::string& expected_text, bool expect_error)
{
    std::ostringstream out;
    std::ostringstream err;
    const int status = chat2data::run(argc, argv, out, err);
    return check(status == expected_status, "unexpected exit status") &&
           check((expect_error ? err.str() : out.str()).find(expected_text) != std::string::npos,
                 "expected output is missing") &&
           check((expect_error ? out.str() : err.str()).empty(), "unexpected output stream");
}

} // namespace

int main()
{
    const char* default_args[] = {"chat2data"};
    const char* help_args[] = {"chat2data", "--help"};
    const char* version_args[] = {"chat2data", "--version"};
    const char* invalid_args[] = {"chat2data", "--unknown"};
    const char* multiple_args[] = {"chat2data", "--help", "--version"};
    return test_run(1, default_args, 0, "under development", false) &&
                   test_run(2, help_args, 0, "Usage:", false) &&
                   test_run(2, version_args, 0, "chat2data ", false) &&
                   test_run(2, invalid_args, 1, "Invalid arguments", true) &&
                   test_run(3, multiple_args, 1, "Invalid arguments", true)
               ? 0
               : 1;
}
