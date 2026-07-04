#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

extern "C" int aegis_rs_serve_once(const char* directory, uint16_t port,
                                    double rate, uint64_t* sent, char* error,
                                    size_t error_capacity);

namespace aegis::net::rust {

inline uint64_t serve_once(const std::string& directory, uint16_t port,
                           double rate = 0.0) {
    std::array<char, 512> error{};
    uint64_t sent = 0;
    if (aegis_rs_serve_once(directory.c_str(), port, rate, &sent, error.data(),
                            error.size()) != 0)
        throw std::runtime_error(std::string("Rust feed server: ") + error.data());
    return sent;
}

}  // namespace aegis::net::rust
