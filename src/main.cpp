#include "app.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "MaxDisplay: " << error.what() << '\n';
        return 1;
    }
}
