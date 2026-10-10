// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of clocktrack.pyx, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include "mlat/clock_pair.hpp"
#include <cstdint>
#include <vector>
namespace mlat::clock {
using StationId=std::uint64_t;
struct Timestamp {double ticks, utc;};
struct StationSamples {StationId id; Clock clock; std::vector<Timestamp> samples;};
// Snapshot taken by the coordinator; base must sort before peer.
struct PairModel {StationId base,peer; State state;};
struct NormalizedStation {StationId id; double variance; std::vector<Timestamp> samples;};
// Matches normalize2: return only the largest component, with at least 3 nodes.
// Output timestamps are seconds in the chosen central receiver's timescale,
// NOT UTC. Timestamp.utc metadata is preserved unchanged.
std::vector<NormalizedStation> normalize(const std::vector<StationSamples>& stations,
                                        const std::vector<PairModel>& pairs,double now);
}
