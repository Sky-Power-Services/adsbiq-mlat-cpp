#include "mlat/mlat_tracker.hpp"
#include "mlat/modes.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
using nlohmann::json;
using namespace mlat;
std::size_t checks=0;
void compare(const json& a,const json& e,const std::string& path) {
 if(e.is_object()){for(auto it=e.begin();it!=e.end();++it)compare(a.at(it.key()),it.value(),path+"."+it.key());return;}
 if(e.is_array()){++checks;if(a.size()!=e.size())throw std::runtime_error(path+" array length "+std::to_string(a.size())+" != "+std::to_string(e.size()));
  for(std::size_t i=0;i<e.size();++i)compare(a[i],e[i],path+"["+std::to_string(i)+"]");return;}
 ++checks;bool ok;
 if(e.is_number_float()){
  double actual=a,expected=e,tolerance=1e-10;
  if(path.find("covariance")!=std::string::npos)tolerance=1e-4+std::abs(expected)*1e-4;
  else if(path.find("position[")!=std::string::npos||path.find("mean[")!=std::string::npos)tolerance=.02;
  else if(path.find("last_result_var")!=std::string::npos)tolerance=1e-3+std::abs(expected)*1e-4;
  ok=std::isfinite(actual)&&std::abs(actual-expected)<=tolerance;
 }else ok=a==e;
 if(!ok)throw std::runtime_error(path+" expected="+e.dump()+" actual="+a.dump());
}
json aircraft(const AircraftModel& ac) {
 json result;
#define FIELD(x) result[#x]=ac.x
 FIELD(seen);FIELD(last_resolve_attempt);FIELD(alt_history);FIELD(mlat_message_count);FIELD(mlat_result_count);FIELD(mlat_kalman_count);
#undef FIELD
 result["altitude"]=ac.altitude?json(*ac.altitude):json(nullptr);
 result["last_altitude_time"]=ac.last_altitude_time?json(ac.last_altitude_time):json(nullptr);
 result["vrate"]=ac.vrate?json(*ac.vrate):json(nullptr);result["vrate_time"]=ac.vrate?json(ac.vrate_time):json(nullptr);
 result["last_result_position"]=ac.last_result_position?json(*ac.last_result_position):json(nullptr);
 result["last_result_time"]=ac.last_result_position?json(ac.last_result_time):json(nullptr);
 result["last_result_var"]=ac.last_result_position?json(ac.last_result_var):json(nullptr);
 result["last_result_dof"]=ac.last_result_position?json(ac.last_result_dof):json(nullptr);
 result["callsign"]=ac.callsign?json(*ac.callsign):json(nullptr);result["squawk"]=ac.squawk?json(*ac.squawk):json(nullptr);
 const auto& f=ac.kalman.state();
 result["filter"]={{"valid",f.valid},{"acquiring",f.acquiring},{"outliers",f.outliers},{"last_update",f.last_update},
                    {"mean",f.initialized?json(f.mean):json(nullptr)},{"covariance",f.initialized?json(f.covariance):json(nullptr)}};
 return result;
}
int main(int argc,char**argv) {
 try {
  if(argc!=2)throw std::runtime_error("fixture required");std::ifstream in(argv[1]);json fixture;in>>fixture;
  std::size_t outputs=0;
  for(const auto& c:fixture.at("cases")) {
   auto ac=std::make_shared<AircraftModel>();ac->address=0x40621d;ac->seen=1000000;
   std::map<clock::StationId,std::shared_ptr<ReceiverModel>> receivers;
   for(const auto& d:c.at("receivers")){auto r=std::make_shared<ReceiverModel>(d.at("id"),"beast",d.at("llh").get<Vec3>(),1000000);receivers.emplace(r->id,r);}
   std::vector<clock::PairModel> pairs;
   for(const auto& p:c.at("pairs")){clock::State s;s.n=20;s.valid=true;s.updated=p.at("updated");s.variance=p.at("variance");s.factor=s.i_factor=1;pairs.push_back({p.at("base"),p.at("peer"),s});}
   json events=json::array();
   MlatTracker tracker([&](auto id){return id==ac->address?ac:nullptr;},[&](auto){return pairs;},[&](const MlatResult& r){
    json cluster=json::array(),all=json::array();for(const auto& rx:r.cluster_receivers)cluster.push_back(rx->id);for(const auto& rx:r.all_receivers)all.push_back(rx->id);
    events.push_back({{"time",r.time},{"address",r.address},{"position",r.solution.position},{"covariance",*r.solution.covariance},
     {"cluster",cluster},{"all",all},{"distinct",r.distinct},{"dof",r.dof},{"error",r.error},{"filter_valid",r.filter.valid}});++outputs;
   },1000000,0);
   for(std::size_t i=0;i<c.at("steps").size();++i) {
    events=json::array();const auto& s=c.at("steps")[i];auto op=s.at("op").get<std::string>();
    if(op=="receive"){auto msg=modes::from_hex(s.at("message").get<std::string>());tracker.receive(receivers.at(s.at("id")),s.at("timestamp"),msg,s.at("wall"),s.at("mono"),s.at("decoded"));}
    else if(op=="advance")tracker.advance(s.at("mono"),s.at("wall"));
    else if(op=="allow")ac->allow_mlat=s.at("value");else throw std::runtime_error("bad operation");
    const auto& st=tracker.stats();json stats;
#define FIELD(x) stats[#x]=st.x
    FIELD(messages);FIELD(valid_groups);FIELD(normalize);FIELD(solve_attempt);FIELD(solve_success);FIELD(solve_used);
#undef FIELD
    json actual={{"aircraft",aircraft(*ac)},{"pending",tracker.pending_count()},{"outputs",events},{"stats",stats}};
    compare(actual,c.at("expected")[i],c.at("name").get<std::string>()+" step "+std::to_string(i));
    ++checks;if(st.bounded_drops)throw std::runtime_error("unexpected bound drop");
   }
  }
  if(outputs<10)throw std::runtime_error("insufficient successful output coverage");
  std::cout<<checks<<" MLAT pipeline assertions; "<<outputs<<" emitted solutions\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
