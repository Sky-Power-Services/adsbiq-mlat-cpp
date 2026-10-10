// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "mlat/clock_graph.hpp"
#include "mlat/geodesy.hpp"
#include "mlat/kalman.hpp"
#include <memory>
#include <map>
#include <optional>
#include <set>
namespace mlat {
struct AircraftModel;
struct AircraftOrder {bool operator()(const std::shared_ptr<AircraftModel>& a,const std::shared_ptr<AircraftModel>& b)const;};
using AircraftSet=std::set<std::shared_ptr<AircraftModel>,AircraftOrder>;
// Stable shared ownership: pending sync points retain dead receiver objects
// until expiry. UIDs are connection-generation IDs, not reusable feeder IDs.
struct ReceiverModel:clock::Receiver {
 clock::StationId id;
 clock::StationId public_id;
 std::string user,connection_info,source_ip;
 std::optional<std::string> uuid;
 int source_port=0;
 bool privacy=false;
 double connected_since=0,num_outliers=0,outlier_percent_rolling=0;
 std::size_t peer_count=0;
 std::uint64_t message_counter=0;
 std::optional<double> map_lat,map_lon,map_alt;
 Vec3 position_llh,position;
 bool dead=false;
 std::array<int,5> sync_peers{};
 double sync_range_exceeded=0,last_clock_reset=0;
 int clock_reset_counter=0;
 // Strong aircraft references preserve retired generations in deferred interest
 // sets; aircraft references receivers by UID, so this does not create cycles.
 AircraftSet tracking,adsb_seen,sync_interest,mlat_interest,requested;
 std::optional<std::map<std::uint32_t,double>> last_rate_report;
 ReceiverModel(clock::StationId uid,std::string_view type,Vec3 llh,double now):
  clock::Receiver{clock::make_clock(type),0,0,0,0,0,{}},id(uid),public_id(uid),connected_since(now),position_llh(llh),position(llh_to_ecef(llh)),last_clock_reset(now){}
};
struct AircraftModel:clock::Aircraft {
 std::uint32_t address=0;
 std::uint64_t generation=0;
 double seen=0,last_altitude_time=0;
 std::optional<int> altitude;
 bool do_mlat=false,force_mlat=false;
 std::set<clock::StationId> tracking;
 std::set<clock::StationId> sync_interest,mlat_interest,adsb_seen;
 double last_force_mlat=0,sync_bad_percent=0,last_crappy_output=0;
 bool interesting()const noexcept{return !sync_interest.empty()||(allow_mlat&&!mlat_interest.empty());}
 bool allow_mlat=true;
 std::uint64_t mlat_message_count=0,mlat_result_count=0,mlat_kalman_count=0;
 std::vector<std::pair<double,int>> alt_history;
 std::optional<int> vrate;
 double vrate_time=0,last_resolve_attempt=0,last_result_time=0,last_result_var=0;
 int last_result_dof=0;
 std::optional<Vec3> last_result_position;
 std::optional<std::string> callsign,squawk;
 KalmanCA kalman;
};
inline bool AircraftOrder::operator()(const std::shared_ptr<AircraftModel>& a,const std::shared_ptr<AircraftModel>& b)const {
 return a->generation<b->generation;
}
}
