// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of mlattrack.py, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include "mlat/models.hpp"
#include <deque>
#include <map>
namespace mlat {
struct ClusterSample {std::shared_ptr<ReceiverModel> receiver;double timestamp,variance;};
struct Cluster {int distinct=0;double first_seen=0;std::vector<ClusterSample> samples;};
std::vector<Cluster> cluster_timestamps(const std::vector<clock::NormalizedStation>& component,
 const std::map<clock::StationId,std::shared_ptr<ReceiverModel>>& receivers,int minimum);
struct MlatStats {std::uint64_t messages=0,valid_groups=0,normalize=0,solve_attempt=0,solve_success=0,solve_used=0,bounded_drops=0;};
struct MlatResult {
 double time;std::uint32_t address;Solution solution;
 std::vector<std::shared_ptr<ReceiverModel>> cluster_receivers,all_receivers;
 int distinct,dof;FilterState filter;double error;
};
class MlatTracker {
public:
 using AircraftLookup=std::function<std::shared_ptr<AircraftModel>(std::uint32_t)>;
 using PairLookup=std::function<std::vector<clock::PairModel>(std::span<const clock::StationId>)>;
 using Output=std::function<void(const MlatResult&)>;
 using TimerId=std::function<std::uint64_t()>;
 MlatTracker(AircraftLookup lookup,PairLookup pairs,Output output,double wall_time,double monotonic_time,TimerId timer_id={});
 void receive(std::shared_ptr<ReceiverModel> receiver,double timestamp,std::span<const std::uint8_t> message,
              double wall_time,double monotonic_time,bool decoded_object=false);
 void advance(double monotonic_time,double wall_time);
 bool advance_one(double monotonic_time,double wall_time);
 std::optional<double> next_deadline()const noexcept;
 std::vector<std::pair<double,std::uint64_t>> ready_timers(double monotonic_time)const;
 const MlatStats& stats()const noexcept{return stats_;}
 std::size_t pending_count()const noexcept{return pending_.size();}
private:
 struct Key {std::string bytes;std::uint64_t unique=0;auto operator<=>(const Key&)const=default;};
 struct Copy {std::shared_ptr<ReceiverModel> receiver;double timestamp,utc;};
 struct Group {Key key;double first_seen;std::vector<Copy> copies;std::map<clock::StationId,std::shared_ptr<ReceiverModel>> receivers;};
 struct Cohort {double created,due;std::vector<std::shared_ptr<Group>> groups;std::uint64_t timer_id=0;};
 void resolve(Group& group,double now);
 AircraftLookup lookup_;PairLookup pairs_;Output output_;MlatStats stats_{};
 std::map<Key,std::shared_ptr<Group>> pending_;std::deque<std::shared_ptr<Cohort>> cohorts_;
 std::shared_ptr<Cohort> current_;std::uint64_t unique_=0;std::size_t pending_receivers_=0;
 TimerId timer_id_;std::uint64_t local_timer_=0;
};
}
