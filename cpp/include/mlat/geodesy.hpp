// SPDX-License-Identifier: GPL-3.0-or-later
// Port of mlat-server geodesy.pyx, Copyright (C) 2015 Oliver Jowett.
#pragma once
#include <array>
#include <numbers>

namespace mlat {
using Vec3 = std::array<double, 3>;
namespace constants {
inline constexpr double cair = 299792458.0 / 1.00032;
inline constexpr double dtor = std::numbers::pi / 180.0;
inline constexpr double rtod = 180.0 / std::numbers::pi;
// Deployed baseline uses 0.3038; do NOT silently substitute 0.3048.
inline constexpr double ftom = 0.3038;
inline constexpr double mtof = 1.0 / ftom;
inline constexpr double ms_to_kts = 1.9438;
inline constexpr double ms_to_fpm = mtof * 60.0;
}
Vec3 llh_to_ecef(const Vec3& llh) noexcept;
Vec3 ecef_to_llh(const Vec3& ecef) noexcept;
double ecef_distance(const Vec3& a, const Vec3& b) noexcept;
double great_circle(const Vec3& a, const Vec3& b) noexcept;
}
