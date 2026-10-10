// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of coordinator._write_state / ClockTracker.dump_receiver_state.
#pragma once
#include "mlat/clock_tracker.hpp"
#include <nlohmann/json.hpp>
namespace mlat {
double python_round(double value,int digits=0);
std::string fixed_decimal(double value,int digits);
struct IntervalStats {
 std::uint64_t sync_points=0,sync_msgs=0,mlat_msgs=0,valid_groups=0,normalize_called=0,solve_attempt=0,solve_success=0,solve_used=0;
};
struct StatusSnapshot {
 nlohmann::json sync=nlohmann::json::object(),clients=nlohmann::json::object(),aircraft=nlohmann::json::object();
 std::map<clock::StationId,nlohmann::json> client_statistics;
 std::string prometheus,title;
};
class StatusEngine {
public:
 // Input pairs must be in Python dictionary-insertion order; model counters
 // are snapshotted BEFORE ClockTracker::decay_status_counters is called once.
 // This method mutates receiver/aircraft quality state, as does the baseline.
 StatusSnapshot update(const std::map<clock::StationId,std::shared_ptr<ReceiverModel>>& receivers,
  const std::map<std::uint32_t,std::shared_ptr<AircraftModel>>& aircraft,
  std::size_t wanted_count,std::span<const PairSnapshot> pairs,const IntervalStats& statistics,
  double wall_time,double process_cpu_seconds,std::uint32_t partition=1,std::uint32_t partitions=1);
 static void location(ReceiverModel& receiver,Vec3 llh,double random_x,double random_y);
 double cpu_checkpoint()const noexcept{return last_cpu_seconds_;}
 void restore_cpu_checkpoint(double value)noexcept{last_cpu_seconds_=value;}
private:double last_cpu_seconds_=0;
};
}
