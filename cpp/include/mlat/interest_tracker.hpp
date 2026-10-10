// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of tracker.py / Receiver interest methods, Oliver Jowett and contributors.
#pragma once
#include "mlat/models.hpp"
#include <random>
namespace mlat {
// Random decisions are injectable: a Python seed alone cannot reproduce its
// object-set iteration. Tests replay choice tapes, not a different algorithm.
class InterestRandom {
public:
 virtual ~InterestRandom()=default;
 virtual double uniform()=0;
 virtual std::vector<std::size_t> sample(std::size_t n,std::size_t k)=0;
 virtual std::vector<std::size_t> shuffle(std::size_t n)=0;
};
class SeededInterestRandom final:public InterestRandom {
public:
 explicit SeededInterestRandom(std::uint64_t seed):engine_(seed){}
 double uniform()override;
 std::vector<std::size_t> sample(std::size_t n,std::size_t k)override;
 std::vector<std::size_t> shuffle(std::size_t n)override;
private:std::mt19937_64 engine_;
};
struct InterestOptions {std::uint32_t partition=1,partitions=1;std::size_t min_tracking=3;bool mark_adsb_time=true;};
class InterestTracker {
public:
 using Refresh=std::function<void(const std::shared_ptr<ReceiverModel>&)>;
 // Refresh must enqueue call_soon-equivalent work; call refresh_requests at the
 // deferred event-loop boundary, not synchronously during update_interest.
 InterestTracker(InterestRandom& random,Refresh schedule,double now,InterestOptions options={});
 void attach(std::shared_ptr<ReceiverModel> receiver);
 void add(ReceiverModel& receiver,std::span<const std::uint32_t> addresses,double now);
 void remove(ReceiverModel& receiver,std::span<const std::uint32_t> addresses);
 void disconnect(ReceiverModel& receiver);
 void update_interest(ReceiverModel& receiver,double now);
 static void refresh_requests(ReceiverModel& receiver);
 bool in_local_partition(std::uint32_t address)const noexcept;
 std::shared_ptr<AircraftModel> find(std::uint32_t address)const;
 const std::map<std::uint32_t,std::shared_ptr<AircraftModel>>& aircraft()const noexcept{return aircraft_;}
 const AircraftSet& wanted()const noexcept{return wanted_;}
 double wanted_time()const noexcept{return wanted_time_;}
private:
 void check_receiver(const ReceiverModel& receiver)const;
 void erase_active(const std::shared_ptr<AircraftModel>& aircraft);
 AircraftSet sample(const AircraftSet& candidates,std::size_t count);
 void set_interest(ReceiverModel& receiver,AircraftSet sync,AircraftSet mlat,AircraftSet adsb);
 double uniform();
 InterestRandom& random_;Refresh schedule_;InterestOptions options_;
 std::map<clock::StationId,std::shared_ptr<ReceiverModel>> receivers_;
 std::map<std::uint32_t,std::shared_ptr<AircraftModel>> aircraft_;
 std::map<std::uint64_t,std::shared_ptr<AircraftModel>> creation_order_;
 AircraftSet wanted_;double wanted_time_;std::uint64_t next_generation_=0;
 std::optional<clock::StationId> high_uid_;
 std::size_t tracking_memberships_=0;
};
}
