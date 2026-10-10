// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of solver.py, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include "mlat/geodesy.hpp"
#include <optional>
#include <span>
namespace mlat {
struct Measurement {Vec3 receiver; double timestamp,variance;};
using Covariance3=std::array<std::array<double,3>,3>;
struct Solution {Vec3 position; std::optional<Covariance3> covariance;};
// Diagnostic counters are not acceptance gates; numeric libraries may differ in
// iteration count/status within MINPACK's successful codes 1..4.
struct SolverDiagnostics {int status=0,evaluations=0;double offset=0;};
// mlattrack.py covariance gate: floor(sqrt(abs(trace(cov)))); missing/nonfinite
// covariance is unusable. Caller rejects errors >10000 metres before output.
std::optional<double> tracker_position_error(const Solution& solution) noexcept;
// Measurements must already be ordered by timestamp. At least four equations
// (including optional altitude). Missing covariance is a valid solver result;
// the downstream tracker applies its separate covariance/error gates.
std::optional<Solution> solve(std::span<const Measurement> measurements,
                             std::optional<double> altitude,
                             std::optional<double> altitude_error,
                             Vec3 initial_guess,SolverDiagnostics* diagnostics=nullptr);
}
