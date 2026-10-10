// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of modes_cython/message.pyx, Copyright (C) 2015 Oliver Jowett.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mlat::modes {
enum class ESType { id_and_category=1, airborne_position=2, surface_position=3,
                    airborne_velocity=4, other=5 };
struct Message {
    int df{};
    std::uint32_t address{};
    std::optional<int> altitude;
    std::optional<std::string> callsign, squawk;
    std::optional<bool> crc_ok;
    std::optional<ESType> estype;
    std::optional<int> nuc, f, lat, lon, ss, saf, ac12, t, category;
    std::optional<int> ca, cf, aa, vs, cc, sl, ri, fs, dr, um, ac, id;
};
std::vector<std::uint8_t> from_hex(std::string_view text);
std::optional<Message> decode(std::span<const std::uint8_t> data);
std::uint32_t crc_residual(std::span<const std::uint8_t> data);
std::optional<int> decode_ac13(unsigned value) noexcept;
std::optional<int> decode_ac12(unsigned value) noexcept;
std::string decode_id13(unsigned value);
std::array<double,4> decode_cpr(int lat_even, int lon_even, int lat_odd, int lon_odd);
}
