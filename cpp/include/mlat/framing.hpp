// SPDX-License-Identifier: AGPL-3.0-or-later
// Protocol derived from jsonclient.py, Copyright (C) 2015 Oliver Jowett.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <zlib.h>

namespace mlat::wire {
// Persistent zlib dictionaries across frames; two-byte network-order length;
// omitted Z_SYNC_FLUSH suffix 00 00 ff ff restored by the reader.
class Decoder {
public:
    using LineHandler = std::function<void(std::string_view)>;
    explicit Decoder(std::size_t max_expanded_frame = 4*1024*1024);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    void feed(std::span<const std::uint8_t> bytes, const LineHandler& handler);
    void finish() const;
    std::uint64_t completed_frames() const noexcept { return completed_frames_; }
private:
    void frame(const LineHandler& handler);
    z_stream stream_{};
    std::array<std::uint8_t,2> header_{};
    std::size_t header_used_=0, expected_=0;
    std::vector<std::uint8_t> payload_;
    std::size_t max_expanded_;
    bool failed_=false;
    std::uint64_t completed_frames_=0;
};
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    // Caller coalesces complete JSON lines; max 32 KiB uncompressed per call.
    // Large interest lists must be split at JSON-message boundaries, not bytes.
    // Explicit larger bound permits a single compressible source/quine line;
    // compressed payload must still fit uint16. Default remains 32 KiB.
    std::vector<std::uint8_t> encode(std::string_view complete_lines,std::size_t max_input=32768);
private:
    z_stream stream_{};
    bool failed_=false;
};
}
