// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of coordinator.py, Oliver Jowett and contributors.
#pragma once
#include "mlat/interest_tracker.hpp"
#include "mlat/mlat_tracker.hpp"
#include "mlat/status.hpp"
namespace mlat {
struct ReceiverInfo {
 std::string user,clock_type="dump1090",connection_info,source_ip;
 std::optional<std::string> uuid;Vec3 llh{};bool privacy=false;int source_port=0;
};
struct CoordinatorHooks {
 std::function<void(ReceiverModel&,const std::set<std::uint32_t>&)> request_traffic;
 std::function<void(ReceiverModel&,const nlohmann::json&)> client_stats;
 std::function<void(const StatusSnapshot&)> status;
 std::function<void(const MlatResult&)> output;
 std::function<void(ReceiverModel&,const MlatResult&)> client_result;
 std::function<void(std::string_view)> error;
};
class Coordinator {
public:
 Coordinator(InterestRandom& random,CoordinatorHooks hooks,double wall,double mono,InterestOptions options={});
 ~Coordinator();
 Coordinator(const Coordinator&)=delete;Coordinator& operator=(const Coordinator&)=delete;
 void start();void close();
 // Explicit replay/production clocks. Setting time does not execute callbacks.
 void set_time(double wall,double monotonic,double process_cpu_seconds);
 // One asyncio-like iteration: previous ready work, then due timers. Newly
 // queued work is deferred to the next iteration. Host must pump while ready.
 void pump(double wall,double monotonic,double process_cpu_seconds);
 std::optional<double> next_deadline()const;
 std::shared_ptr<ReceiverModel> connect(const ReceiverInfo& info,std::function<void(ReceiverModel&)> authenticate={});
 void disconnect(clock::StationId id);
 void location(clock::StationId id,Vec3 llh);
 void seen(clock::StationId id,std::span<const std::uint32_t> addresses);
 void lost(clock::StationId id,std::span<const std::uint32_t> addresses);
 void rates(clock::StationId id,const std::map<std::uint32_t,double>& report);
 void reset(clock::StationId id,bool input_connected=false);
 void sync(clock::StationId id,double even,double odd,const SyncMessage& em,const SyncMessage& om);
 // Gating direct JSON MLAT belongs in the JSON session, not here: sync and UDP
 // paths intentionally have different source behavior.
 void mlat(clock::StationId id,double timestamp,std::span<const std::uint8_t> message);
 void message_received(clock::StationId id);
 const auto& receivers()const noexcept{return receivers_;}
 const InterestTracker& interests()const noexcept{return interests_;}
 const ClockTracker& clocks()const noexcept{return clocks_;}
 const MlatTracker& multilateration()const noexcept{return mlat_;}
 IntervalStats interval_stats()const;
 std::uint64_t errors()const noexcept{return errors_;}
 static bool forward_to_clients(bool filter_valid,int dof)noexcept{return filter_valid||dof>=1;}
private:
 struct Timer {double due;std::uint64_t sequence;};
 enum class Work {refresh,maintenance,cleanup,cohort};
 struct Ready {Work kind;std::shared_ptr<ReceiverModel> receiver;};
 void maintenance();void result(const MlatResult& output);void report_error(std::string_view where)noexcept;
 std::shared_ptr<ReceiverModel> receiver(clock::StationId id)const;
 InterestRandom& random_;CoordinatorHooks hooks_;InterestOptions options_;
 double wall_,mono_,cpu_=0;bool started_=false,closed_=false,pumping_=false;
 std::uint64_t timer_sequence_=0,generation_=0,public_uid_=0,errors_=0;
 Timer cleanup_,maintenance_{};
 std::map<clock::StationId,std::shared_ptr<ReceiverModel>> receivers_;
 std::map<std::string,clock::StationId> usernames_;
 std::deque<Ready> ready_;
 InterestTracker interests_;ClockTracker clocks_;MlatTracker mlat_;StatusEngine status_;
 SyncStats sync_checkpoint_{};MlatStats mlat_checkpoint_{};
};
// JSON files are individually atomically replaced, as in the baseline. The
// caller owns a private/trusted work directory; publication never runs in ctor.
void publish_status(const StatusSnapshot& snapshot,const std::string& work_directory,const std::optional<std::string>& prometheus_file={});
}
