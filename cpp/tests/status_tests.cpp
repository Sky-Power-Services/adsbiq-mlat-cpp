#include "mlat/status.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cmath>
using namespace mlat;using nlohmann::json;
std::size_t checks=0;
void require(bool ok,const std::string& path){++checks;if(!ok)throw std::runtime_error(path);}
void compare(const json& a,const json& e,const std::string& path){
 if(e.is_object()){require(a.size()==e.size(),path+" field count");for(auto it=e.begin();it!=e.end();++it)compare(a.at(it.key()),it.value(),path+"."+it.key());return;}
 if(e.is_array()){require(a.size()==e.size(),path+" length");for(std::size_t i=0;i<e.size();++i)compare(a[i],e[i],path+"["+std::to_string(i)+"]");return;}
 bool ok=e.is_number_float()?a.is_number()&&std::isfinite(a.get<double>())&&std::abs(a.get<double>()-e.get<double>())<=1e-9:a==e;
 require(ok,path+" expected="+e.dump()+" actual="+a.dump());
}
int main(int argc,char**argv){
 try{
  if(argc!=2)throw std::runtime_error("fixture required");std::ifstream file(argv[1]);json data;file>>data;
  for(const auto& d:data.at("rounding")){require(python_round(d.at("value"),d.at("digits"))==d.at("expected").get<double>(),"Python decimal rounding");require(fixed_decimal(d.at("value"),d.at("digits"))==d.at("text").get<std::string>(),"Python fixed formatting");}
  std::size_t steps=0;
  for(const auto& c:data.at("cases")){
   StatusEngine engine;std::map<clock::StationId,std::shared_ptr<ReceiverModel>> receivers;std::map<std::uint32_t,std::shared_ptr<AircraftModel>> aircraft;
   for(const auto& d:c.at("receivers")){auto r=std::make_shared<ReceiverModel>(d.at("id"),"beast",d.at("llh").get<Vec3>(),1000000);
    r->user=d.at("user");if(!d.at("uuid").is_null())r->uuid=d.at("uuid").get<std::string>();r->privacy=d.at("privacy");r->connection_info=d.at("connection");r->source_ip=d.at("source_ip");r->source_port=d.at("source_port");
#define FIELD(x) r->x=d.at(#x)
    FIELD(message_counter);FIELD(bad_syncs);FIELD(outlier_percent_rolling);FIELD(num_outliers);FIELD(num_syncs);FIELD(recent_pair_jumps);
#undef FIELD
    r->sync_peers=d.at("sync_peers").get<std::array<int,5>>();StatusEngine::location(*r,d.at("llh").get<Vec3>(),d.at("random")[0],d.at("random")[1]);receivers.emplace(r->id,r);
   }
   std::uint64_t generation=0;
   for(const auto& d:c.at("aircraft")){auto ac=std::make_shared<AircraftModel>();ac->generation=++generation;ac->address=d.at("address");
#define FIELD(x) ac->x=d.at(#x)
    FIELD(allow_mlat);FIELD(seen);FIELD(sync_good);FIELD(sync_bad);FIELD(mlat_message_count);FIELD(mlat_result_count);FIELD(mlat_kalman_count);
#undef FIELD
    if(!d.at("altitude").is_null())ac->altitude=d.at("altitude").get<int>();ac->tracking=d.at("tracking").get<std::set<clock::StationId>>();
#define SET(x) ac->x=d.at(#x).get<std::set<clock::StationId>>();for(auto id:ac->x)receivers.at(id)->x.insert(ac)
    SET(adsb_seen);SET(sync_interest);SET(mlat_interest);
#undef SET
    if(!d.at("last_llh").is_null()){ac->last_result_position=llh_to_ecef(d.at("last_llh").get<Vec3>());ac->last_result_time=d.at("last_result_time");}aircraft.emplace(ac->address,ac);
   }
   std::vector<PairSnapshot> pairs;
   for(const auto& d:c.at("pairs")){clock::State s;
#define FIELD(x) s.x=d.at(#x)
    FIELD(n);FIELD(error);FIELD(drift);FIELD(i_drift);FIELD(updated);FIELD(jumped);FIELD(outlier_total);FIELD(update_total);
#undef FIELD
    pairs.push_back({d.at("first"),d.at("second"),0,s});
   }
   for(std::size_t i=0;i<c.at("steps").size();++i){++steps;const auto& step=c.at("steps")[i];
    for(const auto& edit:step.value("receiver_updates",json::array())){auto r=receivers.at(edit[0].get<clock::StationId>());for(auto it=edit[1].begin();it!=edit[1].end();++it){if(it.key()=="message_counter")r->message_counter=it.value();else if(it.key()=="bad_syncs")r->bad_syncs=it.value();else throw std::runtime_error("unsupported receiver edit");}}
    IntervalStats st;
#define FIELD(x) st.x=step.at("stats").at(#x)
    FIELD(sync_points);FIELD(sync_msgs);FIELD(mlat_msgs);FIELD(valid_groups);FIELD(normalize_called);FIELD(solve_attempt);FIELD(solve_success);FIELD(solve_used);
#undef FIELD
    auto output=engine.update(receivers,aircraft,step.at("wanted"),pairs,st,step.at("wall"),step.at("cpu"),c.at("partition")[0],c.at("partition")[1]);
    for(auto& p:pairs)if(p.state.n>=2){p.state.update_total/=2;p.state.outlier_total/=2;p.state.jumped=0;}
    json client_stats=json::object();for(const auto& [id,s]:output.client_statistics)client_stats[std::to_string(id)]=s;
    for(auto& entry:output.clients.items())for(const std::string field:{"sync_interest","mlat_interest"})std::sort(entry.value()[field].begin(),entry.value()[field].end());
    json actual={{"sync",output.sync},{"clients",output.clients},{"aircraft",output.aircraft},{"prometheus",output.prometheus},{"title",output.title},{"statistics",client_stats}};
    actual["receiver_state"]=json::array();for(const auto& [id,r]:receivers){json state={{"id",id},{"map_alt",r->map_alt?json(*r->map_alt):json(nullptr)}};
#define FIELD(x) state[#x]=r->x
     FIELD(bad_syncs);FIELD(outlier_percent_rolling);FIELD(num_outliers);FIELD(num_syncs);FIELD(recent_pair_jumps);FIELD(peer_count);FIELD(message_counter);
#undef FIELD
     actual["receiver_state"].push_back(state);
    }
    actual["aircraft_state"]=json::array();for(const auto& [address,ac]:aircraft)actual["aircraft_state"].push_back({{"address",address},{"sync_good",ac->sync_good},{"sync_bad",ac->sync_bad},{"sync_bad_percent",ac->sync_bad_percent},{"sync_dont_use",ac->sync_dont_use}});
    compare(actual,c.at("expected")[i],c.at("name").get<std::string>()+" step "+std::to_string(i));
   }
  }
  bool threw=false;try{python_round(std::numeric_limits<double>::infinity(),1);}catch(const std::invalid_argument&){threw=true;}require(threw,"nonfinite round guard");
  ReceiverModel r(1,"beast",Vec3{40,-75,10},0);auto before=r.position;threw=false;try{StatusEngine::location(r,{91,0,0},.1,.2);}catch(const std::invalid_argument&){threw=true;}require(threw&&r.position==before,"invalid location atomicity");
  std::cout<<checks<<" status assertions across "<<steps<<" status cycles\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
