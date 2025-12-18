#pragma once

#include <cstddef>   // std::size_t
#include <cstdint>   // std::uint32_t, std::int32_t

// Maximum allowed message size for framed protocol payloads.
inline constexpr std::size_t k_max_msg = 4096;

namespace ResponseStatus {
// Key not found
inline constexpr std::uint32_t RES_NX = 1;
// Generic error (kept as uint32_t for wire-compat; value is all-ones)
inline constexpr std::uint32_t RES_ERR = static_cast<std::uint32_t>(-1);
}  // namespace ResponseStatus

// Read exactly n bytes into buf from fd (or throw on error).
std::int32_t read_full(const int& fd, char* buf, std::size_t n);

// Write exactly n bytes from buf to fd (or throw on error).
std::int32_t write_all(const int& fd, const char* buf, std::size_t n);

// Basic signal handler declaration.
void signal_handler(int signum)
;