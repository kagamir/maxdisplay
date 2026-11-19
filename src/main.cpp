#include "app.hpp"

#include <exception>
#include <iostream>

#ifdef _WIN32
#include <windows.h>

#include <stdexcept>
#include <vector>

namespace {
int run_cli(int argc, wchar_t** wide_argv) {
    // Both devices derive their keys from UTF-8, independent of the Windows code page.
    std::vector<std::string> utf8(argc);
    std::vector<char*> argv(argc + 1, nullptr);
    for (int i = 0; i < argc; ++i) {
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                                nullptr, 0, nullptr, nullptr);
        if (!length) throw std::runtime_error("Invalid Unicode command-line argument");
        utf8[i].resize(length);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                 utf8[i].data(), length, nullptr, nullptr))
            throw std::runtime_error("Could not read UTF-8 command-line arguments");
        argv[i] = utf8[i].data();
    }
    return ::run_cli(argc, argv.data());
}
} // namespace

int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "MaxDisplay: " << error.what() << '\n';
        return 1;
    }
}
