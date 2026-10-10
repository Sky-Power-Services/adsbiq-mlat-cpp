#include "mlat/clock_tracker.hpp"
#include "mlat/modes.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cmath>
#include <numeric>
#include <stdexcept>
using nlohmann::json;
using namespace mlat;
json state_json(const clock::State& s) {
 json j;
#define FIELD(x) j[#x]=s.x
 FIELD(valid);FIELD(n);FIELD(updated);FIELD(variance);FIELD(factor);FIELD(i_factor);FIELD(base_avg);FIELD(peer_avg);
 FIELD(raw_drift);FIELD(drift);FIELD(i_drift);FIELD(drift_n);FIELD(drift_outliers);FIELD(cooldown);FIELD(outlier_total);
 FIELD(update_total);FIELD(outliers);FIELD(cumulative_error);FIELD(error);FIELD(var_sum);FIELD(jumped);FIELD(update_attempted);FIELD(update_last_sync);
#undef FIELD
 return j;
}
std::size_t checks=0;
void compare(const json& actual,const json& expected,const std::string& context) {
 if(expected.is_object()) {for(auto it=expected.begin();it!=expected.end();++it)compare(actual.at(it.key()),it.value(),context+"."+it.key());return;}
 if(expected.is_array()) {
  ++checks;if(actual.size()!=expected.size())throw std::runtime_error(context+" length mismatch "+std::to_string(actual.size())+" != "+std::to_string(expected.size()));
  for(std::size_t i=0;i<expected.size();++i)compare(actual[i],expected[i],context+"["+std::to_string(i)+"]");return;
 }
 ++checks;bool ok;
 if(expected.is_number_float()) {
  double e=expected,a=actual;
  double tolerance=std::abs(e)<1e-3?std::max(1e-20,std::abs(e)*1e-10):std::abs(e)*2e-13;
  ok=std::isfinite(a)&&std::abs(a-e)<=tolerance;
 }else ok=actual==expected;
 if(!ok)throw std::runtime_error(context+" expected="+expected.dump()+" actual="+actual.dump());
}
SyncMessage message(const std::string& hex){auto v=modes::from_hex(hex);if(v.size()!=14)throw std::runtime_error("bad test frame");SyncMessage m;std::copy(v.begin(),v.end(),m.begin());return m;}
int main(int argc,char**argv) {
 try {
  if(argc!=2)throw std::runtime_error("fixture required");std::ifstream in(argv[1]);json fixture;in>>fixture;
  for(const auto& c:fixture.at("cases")) {
   auto ac=std::make_shared<AircraftModel>();ac->address=0x40621d;ac->do_mlat=true;ac->tracking={1,2,3};
   json events=json::array();
   ClockTracker tracker([&](std::uint32_t address){return address==ac->address?ac:nullptr;},
    [&](ReceiverModel& r,double t,const SyncMessage&,double now,bool decoded){events.push_back({{"receiver",r.id},{"timestamp",t},{"now",now},{"decoded",decoded}});});
   std::map<clock::StationId,std::shared_ptr<ReceiverModel>> receivers;
   for(const auto& d:c.at("receivers")) {
    auto r=std::make_shared<ReceiverModel>(d.at("id"),d.at("clock").get<std::string>(),d.at("llh").get<Vec3>(),1000000);
    receivers.emplace(r->id,r);tracker.attach(r);
   }
   for(std::size_t index=0;index<c.at("steps").size();++index) {
    const auto& s=c.at("steps")[index];const auto op=s.at("op").get<std::string>();double now=s.at("now");events=json::array();
    if(op=="sync")tracker.sync(receivers.at(s.at("id").get<clock::StationId>()),s.at("even_time"),s.at("odd_time"),message(s.at("even")),message(s.at("odd")),now);
    else if(op=="reset")tracker.clock_reset(*receivers.at(s.at("id")),now);
    else if(op=="disconnect")tracker.disconnect(*receivers.at(s.at("id")));
    else if(op=="cleanup")tracker.cleanup(now);
    else if(op=="clear")tracker.clear_sync_points();
    else if(op=="dump")tracker.decay_status_counters();
    else if(op=="ignore")ac->sync_dont_use=s.at("value");
    else if(op=="peers"){auto& r=*receivers.at(s.at("id"));r.sync_peers.fill(s.at("value"));r.total_sync_peers=std::accumulate(r.sync_peers.begin(),r.sync_peers.end(),0);}
    else if(op=="connect") {
     const auto& d=s.at("receiver");auto r=std::make_shared<ReceiverModel>(d.at("id"),d.at("clock").get<std::string>(),d.at("llh").get<Vec3>(),now);
     receivers.emplace(r->id,r);tracker.attach(r);
    }
    else throw std::runtime_error("unexpected operation");
    json actual={{"pairs",json::array()},{"pending_keys",tracker.pending_keys()},{"messages",tracker.stats().messages},
                 {"points",tracker.stats().points},{"events",events},{"receivers",json::array()},{"aircraft",json::object()}};
    for(const auto& p:tracker.pairs())actual["pairs"].push_back({{"first",p.first},{"second",p.second},{"category",p.category},{"state",state_json(p.state)}});
    auto ordered=tracker.status_pairs();
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return std::tie(a.first,a.second)<std::tie(b.first,b.second);});
    json status_pairs=json::array();for(const auto& p:ordered)status_pairs.push_back({{"first",p.first},{"second",p.second},{"category",p.category},{"state",state_json(p.state)}});
    ++checks;if(status_pairs!=actual["pairs"])throw std::runtime_error("status snapshot changed pair state or membership");
    for(const auto& [id,ptr]:receivers) {
     const auto& r=*ptr;json row={{"id",id}};
#define FIELD(x) row[#x]=r.x
     FIELD(last_sync);FIELD(bad_syncs);FIELD(num_syncs);FIELD(recent_pair_jumps);FIELD(sync_peers);FIELD(last_clock_reset);FIELD(clock_reset_counter);FIELD(sync_range_exceeded);FIELD(dead);
#undef FIELD
     actual["receivers"].push_back(row);
    }
#define FIELD(x) actual["aircraft"][#x]=ac->x
    FIELD(seen);FIELD(sync_good);FIELD(sync_bad);FIELD(last_adsb_time);FIELD(last_altitude_time);
#undef FIELD
    actual["aircraft"]["altitude"]=ac->altitude?json(*ac->altitude):json(nullptr);
    compare(actual,c.at("expected")[index],c.at("name").get<std::string>()+" step "+std::to_string(index));
    std::vector<clock::StationId> subset;
    for(const auto& [id,ptr]:receivers){(void)ptr;if(id%2)subset.push_back(id);}
    auto selected=tracker.normalization_models(subset);std::size_t expected_count=0;
    for(const auto& p:tracker.pairs())if(p.first%2&&p.second%2)++expected_count;
    ++checks;if(selected.size()!=expected_count)throw std::runtime_error("normalization subset mismatch");
    for(const auto& p:selected){++checks;if(!(p.base%2&&p.peer%2))throw std::runtime_error("normalization subset leaked other pairs");}
    ++checks;if(tracker.stats().bounded_drops)throw std::runtime_error("fixture exceeded resource bounds");
   }
  }
  // Shared lifetime and callback teardown safety, independent of oracle stubs.
  auto receiver=std::make_shared<ReceiverModel>(1,"beast",Vec3{52,4,100},1000);
  {
   ClockTracker tracker([](auto){return std::shared_ptr<AircraftModel>{};},{});tracker.attach(receiver);
   tracker.disconnect(*receiver);
   ++checks;if(receiver->on_clock_reset)throw std::runtime_error("disconnect retained tracker callback");
   bool threw=false;try{tracker.attach(std::make_shared<ReceiverModel>(1,"beast",Vec3{52,4,100},1000));}catch(const std::invalid_argument&){threw=true;}
   ++checks;if(!threw)throw std::runtime_error("reused UID accepted");
   auto r2=std::make_shared<ReceiverModel>(2,"beast",Vec3{52,4,100},1000);tracker.attach(r2);receiver=r2;
  }
  ++checks;if(receiver->on_clock_reset)throw std::runtime_error("destructor retained tracker callback");
  std::cout<<checks<<" ClockTracker assertions across "<<fixture.at("cases").size()<<" scenarios\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
