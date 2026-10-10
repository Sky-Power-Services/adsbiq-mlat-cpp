// SPDX-License-Identifier: GPL-3.0-or-later
// Port of mlat-server geodesy.pyx, Copyright (C) 2015 Oliver Jowett.
#include "mlat/geodesy.hpp"
#include <cmath>

namespace mlat {
namespace {
constexpr double a = 6378137.0;
constexpr double f = 1.0 / 298.257223563;
constexpr double b = a * (1.0 - f);
constexpr double ecc_sq = 1.0 - b * b / (a * a);
const double ep = std::sqrt((a * a - b * b) / (b * b));
const double ep2_b = ep * ep * b;
constexpr double e2_a = ecc_sq * a;
}
Vec3 llh_to_ecef(const Vec3& llh) noexcept {
    const double lat = llh[0] * constants::dtor, lon = llh[1] * constants::dtor;
    const double slat = std::sin(lat), clat = std::cos(lat);
    const double rn = a / std::sqrt(1.0 - slat * slat * ecc_sq);
    return {(rn + llh[2]) * clat * std::cos(lon),
            (rn + llh[2]) * clat * std::sin(lon),
            (rn * (1.0 - ecc_sq) + llh[2]) * slat};
}
Vec3 ecef_to_llh(const Vec3& ecef) noexcept {
    const auto [x, y, z] = ecef;
    const double lon = std::atan2(y, x);
    const double p = std::sqrt(x*x + y*y);
    const double th = std::atan2(a*z, b*p);
    const double st = std::sin(th), ct = std::cos(th);
    const double lat = std::atan2(z + ep2_b * st*st*st, p - e2_a * ct*ct*ct);
    const double slat = std::sin(lat);
    const double n = a / std::sqrt(1.0 - ecc_sq * slat*slat);
    return {lat * constants::rtod, lon * constants::rtod, p / std::cos(lat) - n};
}
double ecef_distance(const Vec3& p, const Vec3& q) noexcept {
    const double x = p[0]-q[0], y = p[1]-q[1], z = p[2]-q[2];
    return std::sqrt(x*x + y*y + z*z);
}
double great_circle(const Vec3& p, const Vec3& q) noexcept {
    const double lat0 = p[0]*constants::dtor, lat1 = q[0]*constants::dtor;
    const double lon0 = p[1]*constants::dtor, lon1 = q[1]*constants::dtor;
    return 6371e3 * std::acos(std::sin(lat0)*std::sin(lat1) +
                            std::cos(lat0)*std::cos(lat1)*std::cos(std::abs(lon0-lon1)));
}
}
