#include "mlat/clock_pair.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <stdexcept>
using nlohmann::json;
using namespace mlat::clock;
int main(int argc,char** argv) {
 try {
  if(argc!=2) throw std::runtime_error("fixture argument required");
  std::ifstream in(argv[1]); json fixture; in>>fixture;
  std::size_t assertions=0, valid=0;
  for(const auto& c:fixture.at("cases")) {
   Receiver b{make_clock(c.at("base_clock").get<std::string>())};
   Receiver p{make_clock(c.at("peer_clock").get<std::string>())};
   Aircraft ac; ClockPair pair(b,p);
   for(std::size_t i=0;i<c.at("steps").size();++i) {
    const auto& step=c.at("steps")[i]; json accepted=nullptr;
    if(step[0]=="update") {
     ac.sync_dont_use=step[6]; b.bad_syncs=step[7]; p.bad_syncs=step[8];
     accepted=pair.update(step[1],step[2],step[3],step[4],step[5],ac);
    } else if(step[0]=="check") accepted=pair.check_valid(step[1]);
    else pair.reset_offsets();
    const auto& s=pair.state(); json actual;
    actual["accepted"]=accepted;
#define FIELD(x) actual[#x]=s.x
    FIELD(valid); FIELD(n); FIELD(updated); FIELD(variance); FIELD(factor); FIELD(i_factor);
    FIELD(base_avg); FIELD(peer_avg); FIELD(raw_drift); FIELD(drift); FIELD(i_drift);
    FIELD(drift_n); FIELD(drift_outliers); FIELD(cooldown); FIELD(outlier_total);
    FIELD(update_total); FIELD(outliers); FIELD(cumulative_error); FIELD(error);
    FIELD(var_sum); FIELD(jumped); FIELD(update_attempted); FIELD(update_last_sync);
#undef FIELD
    actual["base_last_sync"]=b.last_sync; actual["peer_last_sync"]=p.last_sync;
    actual["base_num_syncs"]=b.num_syncs; actual["peer_num_syncs"]=p.num_syncs;
    actual["base_jumps"]=b.recent_pair_jumps; actual["peer_jumps"]=p.recent_pair_jumps;
    actual["sync_good"]=ac.sync_good; actual["sync_bad"]=ac.sync_bad;
    actual["last_adsb_time"]=ac.last_adsb_time;
    actual["predict_peer"]=s.n?json(pair.predict_peer(s.base_avg+1234)):json(nullptr);
    actual["predict_base"]=s.n?json(pair.predict_base(s.peer_avg+1234)):json(nullptr);
    if(s.n<0 || s.n>32) throw std::runtime_error("sample bound");
    valid+=s.valid;
    for(auto it=c.at("expected")[i].begin();it!=c.at("expected")[i].end();++it) {
     const auto& a=actual.at(it.key()); bool ok;
     if(it.value().is_number_float()) {
      double e=it.value(),v=a;
      ok=std::isfinite(v) && std::abs(v-e)<=std::max(1e-20,std::abs(e)*2e-14);
     } else ok=a==it.value();
     ++assertions;
     if(!ok) throw std::runtime_error(c.at("name").get<std::string>()+" step "+std::to_string(i)+" "+it.key()+" expected "+it.value().dump()+" actual "+a.dump());
    }
   }
  }
  if(!valid) throw std::runtime_error("no valid synchronization exercised");
  Receiver b{make_clock("dump1090")},p{make_clock("beast")}; Aircraft ac; ClockPair pair(b,p);
  for(double bad:{0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
   if(pair.update(1,2,bad,4,5,ac)||pair.state().update_attempted!=0) throw std::runtime_error("invalid interval mutated state");
   ++assertions;
  }
  bool threw=false; try {pair.predict_peer(1);} catch(const std::invalid_argument&) {threw=true;}
  if(!threw) throw std::runtime_error("empty prediction accepted"); ++assertions;
  threw=false; try {make_clock("invalid");} catch(const std::invalid_argument&) {threw=true;}
  if(!threw) throw std::runtime_error("unknown clock accepted"); ++assertions;
  int resets=0; b.total_sync_peers=12; b.on_clock_reset=[&]{++resets;};
  for(int i=0;i<4;++i) b.increment_jumps();
  if(resets!=1||b.recent_pair_jumps!=0||b.bad_syncs!=0.1) throw std::runtime_error("reset callback failed"); ++assertions;
  std::cout<<assertions<<" clock assertions passed; "<<valid<<" valid states\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
