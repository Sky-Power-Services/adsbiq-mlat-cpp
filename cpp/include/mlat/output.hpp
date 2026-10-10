// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of output.py / jsonclient.py, Copyright (C) 2015 Oliver Jowett.
#pragma once
#include "mlat/mlat_tracker.hpp"
#include <nlohmann/json.hpp>
namespace mlat {
std::string csv_quote(const std::optional<std::string>& text);
std::string output_date(double timestamp);
std::string output_time(double timestamp);
// Unwrapped result object. Session adds {"result": ...}; build once per broadcast.
// Client n/nstations counts ALL original receivers, not only the solving cluster.
nlohmann::json client_result_old(const MlatResult& result,const AircraftModel& aircraft);
nlohmann::json client_result_ecef(const MlatResult& result,const AircraftModel& aircraft);
// CSV returns a record without newline; sink supplies framing/rotation.
std::string csv_result(const MlatResult& result,const AircraftModel& aircraft);
// SBS returns an ASCII record including newline, or nullopt when throttled.
// Mutates aircraft.last_crappy_output (shared across output clients in source).
std::optional<std::string> basestation_result(const MlatResult& result,AircraftModel& aircraft,
                                            bool use_kalman,double send_time);
}
