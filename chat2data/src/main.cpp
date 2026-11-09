#include "chat2data/application.h"

#include <exception>
#include <iostream>

int main(int argc, char* argv[])
{
    try {
        return chat2data::run(argc, argv, std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "chat2data failed: " << error.what() << '\n';
        return 1;
    }
}
