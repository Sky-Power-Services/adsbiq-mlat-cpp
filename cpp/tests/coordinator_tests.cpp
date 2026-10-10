#include "mlat/coordinator.hpp"
#include "mlat/modes.hpp"
#include "mlat/output.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <stdexcept>
using namespace mlat;using nlohmann::json;
std::size_t checks=0;
void require(bool ok,const std::string& label){++checks;if(!ok)throw std::runtime_error(label);}
void compare(const json& a,const json& e,const std::string& path){
 if(e.is_object()){require(a.size()==e.size(),path+" object count");for(auto it=e.begin();it!=e.end();++it)compare(a.at(it.key()),it.value(),path+"."+it.key());return;}
 if(e.is_array()){require(a.size()==e.size(),path+" array count");for(std::size_t i=0;i<e.size();++i)compare(a[i],e[i],path+"["+std::to_string(i)+"]");return;}
 bool ok=a==e;
 if(e.is_number_float()){
  double expected=e.get<double>(),actual=a.get<double>(),tol=1e-9;
  if(path.find(".position[")!=std::string::npos)tol=.03;
  else if(path.find(".covariance[")!=std::string::npos)tol=1e-4+std::abs(expected)*1e-4;
  ok=std::isfinite(actual)&&std::abs(actual-expected)<=tol;
 }
 require(ok,path+" expected="+e.dump()+" actual="+a.dump());
}
struct Tape:InterestRandom {
 json entries=json::array();std::size_t pos=0;
 json next(std::string_view op){require(pos<entries.size(),"random tape exhausted");auto d=entries[pos++];require(d.at("op").get<std::string>()==op,"random operation mismatch");return d;}
 double uniform()override{return next("uniform").at("value");}
 std::vector<std::size_t> choices(std::string_view op,std::size_t n,std::size_t k){auto d=next(op);require(d.at("n")==n&&d.at("k")==k,"random choice dimensions");return d.at("indices").get<std::vector<std::size_t>>();}
 std::vector<std::size_t> sample(std::size_t n,std::size_t k)override{return choices("sample",n,k);}
 std::vector<std::size_t> shuffle(std::size_t n)override{return choices("shuffle",n,n);}
};
json addresses(const AircraftSet& values){std::set<std::uint32_t> out;for(const auto& a:values)out.insert(a->address);return out;}
json ids(const std::vector<std::shared_ptr<ReceiverModel>>& rs){json out=json::array();for(const auto& r:rs)out.push_back(r->id);return out;}
json status(const StatusSnapshot& s){json result={{"sync",s.sync},{"clients",s.clients},{"aircraft",s.aircraft},{"prometheus",s.prometheus},{"title",s.title}};
 for(auto& entry:result["clients"].items())for(const char* name:{"sync_interest","mlat_interest"})std::sort(entry.value()[name].begin(),entry.value()[name].end());
 for(auto& entry:result["aircraft"].items())if(entry.value().contains("tracking_receivers"))std::sort(entry.value()["tracking_receivers"].begin(),entry.value()["tracking_receivers"].end());return result;
}
json state(const Coordinator& co){
 json s={{"active",json::array()},{"aircraft",json::array()},{"pairs",json::array()},{"pending_sync",co.clocks().pending_keys()},{"pending_mlat",co.multilateration().pending_count()},
  {"deadline",co.next_deadline()?json(*co.next_deadline()):json(nullptr)}};
 for(const auto& [id,r]:co.receivers())s["active"].push_back({{"id",id},{"public_id",r->public_id},{"last_sync",r->last_sync},{"bad_syncs",r->bad_syncs},{"reset_counter",r->clock_reset_counter},
  {"tracking",addresses(r->tracking)},{"requested",addresses(r->requested)},{"message_counter",r->message_counter}});
 for(const auto& [address,ac]:co.interests().aircraft())s["aircraft"].push_back({{"address",address},{"seen",ac->seen},{"do_mlat",ac->do_mlat},{"force_mlat",ac->force_mlat},
  {"altitude",ac->altitude?json(*ac->altitude):json(nullptr)},{"last_adsb_time",ac->last_adsb_time},{"messages",ac->mlat_message_count},{"results",ac->mlat_result_count},
  {"kalman",ac->mlat_kalman_count},{"valid",ac->kalman.state().valid},{"position",ac->last_result_position?json(*ac->last_result_position):json(nullptr)}});
 for(const auto& p:co.clocks().pairs())s["pairs"].push_back(json::array({p.first,p.second,p.state.valid,p.state.n,p.state.updated}));
 auto st=co.interval_stats();s["stats"]={{"sync_points",st.sync_points},{"sync_msgs",st.sync_msgs},{"mlat_msgs",st.mlat_msgs},{"valid_groups",st.valid_groups},{"normalize",st.normalize_called},{"solve_attempt",st.solve_attempt},{"solve_success",st.solve_success},{"solve_used",st.solve_used}};return s;
}
SyncMessage frame(const json& value){auto v=modes::from_hex(value.get<std::string>());require(v.size()==14,"frame size");SyncMessage out;std::copy(v.begin(),v.end(),out.begin());return out;}
int main(int argc,char**argv){
 try{
  if(argc!=2)throw std::runtime_error("fixture required");std::ifstream file(argv[1]);json fixture;file>>fixture;std::size_t outputs=0,valid_outputs=0;StatusSnapshot last;
  for(bool inject_faults:{false,true}){
   Tape rng;json events=json::array(),wire=json::array();std::uint64_t faults=0;CoordinatorHooks hooks;Coordinator* active=nullptr;double current_wall=1000000;
   bool full_output=fixture.at("expected").at(0).contains("wire");
   hooks.request_traffic=[&](auto& r,const auto& wanted){events.push_back({{"kind","request"},{"id",r.id},{"addresses",wanted}});};
   hooks.client_stats=[&](auto& r,const auto& stats){events.push_back({{"kind","statistics"},{"id",r.id},{"value",stats}});};
   hooks.status=[&](const auto& value){last=value;events.push_back({{"kind","status"},{"value",status(value)}});};
   hooks.output=[&](const MlatResult& r){++outputs;if(r.filter.valid)++valid_outputs;events.push_back({{"kind","output"},{"time",r.time},{"address",r.address},{"position",r.solution.position},
    {"covariance",r.solution.covariance?json(*r.solution.covariance):json(nullptr)},{"receivers",ids(r.cluster_receivers)},{"distinct",r.distinct},{"dof",r.dof},{"valid",r.filter.valid},{"error",r.error}});
    if(full_output){auto ac=active->interests().find(r.address);json sbs=json::array();
     for(bool filtered:{false,true}){auto line=basestation_result(r,*ac,filtered,current_wall);sbs.push_back(line?json::array({*line}):json::array());}
     wire.push_back({{"kind","output"},{"time",r.time},{"csv",json::array({csv_result(r,*ac)})},{"sbs",sbs}});
    }
   };
   hooks.client_result=[&](auto& receiver,const MlatResult& r){auto all=ids(r.all_receivers);std::sort(all.begin(),all.end());events.push_back({{"kind","client_result"},{"id",receiver.id},{"time",r.time},{"address",r.address},{"receivers",all},{"valid",r.filter.valid},{"dof",r.dof}});
    if(full_output)wire.push_back({{"kind","client_result"},{"id",receiver.id},{"time",r.time},{"value",client_result_ecef(r,*active->interests().find(r.address))}});
    if(inject_faults&&receiver.id==2){++faults;throw std::runtime_error("synthetic client failure");}};
   Coordinator co(rng,std::move(hooks),1000000,0);
   active=&co;
   for(std::size_t i=0;i<fixture.at("steps").size();++i){const auto& s=fixture.at("steps")[i];const auto& e=fixture.at("expected")[i];events=json::array();rng.entries=e.at("random");rng.pos=0;
    wire=json::array();double wall=s.at("wall"),mono=s.at("mono"),cpu=s.at("cpu");current_wall=wall;co.set_time(wall,mono,cpu);std::string op=s.at("op");
    if(op=="start")co.start();else if(op=="pump")co.pump(wall,mono,cpu);
    else if(op=="connect"){const auto& d=s.at("receiver");ReceiverInfo info;info.user=d.at("user");info.clock_type=d.at("clock");info.llh=d.at("llh").get<Vec3>();info.privacy=d.at("privacy");info.connection_info=d.at("connection");info.source_ip=d.at("source_ip");info.source_port=d.at("source_port");if(!d.at("uuid").is_null())info.uuid=d.at("uuid").get<std::string>();auto r=co.connect(info);require(r->id==d.at("id"),"receiver generation assignment");}
    else if(op=="seen"||op=="lost"){auto values=s.at("addresses").get<std::vector<std::uint32_t>>();if(op=="seen")co.seen(s.at("id"),values);else co.lost(s.at("id"),values);}
    else if(op=="rates"){std::map<std::uint32_t,double> report;for(const auto& d:s.at("report"))report.emplace(d[0].get<std::uint32_t>(),d[1].get<double>());co.rates(s.at("id"),report);}
    else if(op=="sync"){co.message_received(s.at("id"));co.sync(s.at("id"),s.at("even_time"),s.at("odd_time"),frame(s.at("even")),frame(s.at("odd")));}
    else if(op=="reset")co.reset(s.at("id"),s.at("input_connected"));
    else if(op=="disconnect")co.disconnect(s.at("id"));else if(op=="location")co.location(s.at("id"),s.at("llh").get<Vec3>());else throw std::runtime_error("unknown operation");
    json client=json::array(),other=json::array();for(const auto& event:events)(event.at("kind")=="client_result"?client:other).push_back(event);
    std::sort(client.begin(),client.end(),[](const auto& a,const auto& b){return a.at("time")!=b.at("time")?a.at("time")<b.at("time"):a.at("id")<b.at("id");});for(const auto& event:client)other.push_back(event);
    compare(other,e.at("events"),"step "+std::to_string(i)+" events");compare(state(co),e.at("state"),"step "+std::to_string(i)+" state");
    if(full_output){json clients=json::array(),rest=json::array();for(const auto& event:wire)(event.at("kind")=="client_result"?clients:rest).push_back(event);
     std::sort(clients.begin(),clients.end(),[](const auto& a,const auto& b){return a.at("time")!=b.at("time")?a.at("time")<b.at("time"):a.at("id")<b.at("id");});
     for(const auto& event:clients)rest.push_back(event);
     compare(rest,e.at("wire"),"step "+std::to_string(i)+" deterministic wire");
    }
    require(rng.pos==rng.entries.size(),"unused random choices");require(co.errors()==faults,"unexpected coordinator callback error");require(!co.clocks().stats().bounded_drops&&!co.multilateration().stats().bounded_drops,"workload bound drop");
   }
   require(!inject_faults||faults>0,"client fault isolation exercised");bool threw=false;try{co.set_time(1000000,0,0);}catch(const std::invalid_argument&){threw=true;}require(threw,"nonmonotonic time rejected");
   co.close();require(co.receivers().empty()&&!co.next_deadline(),"closed coordinator releases receivers and timers");
  }
  require(outputs>=20&&valid_outputs>=4,"insufficient end-to-end solving coverage");
  for(bool valid:{false,true})for(int dof:{-1,0,1,2})require(Coordinator::forward_to_clients(valid,dof)==!(!valid&&dof<1),"client forwarding gate");
  {
   SeededInterestRandom random(1);CoordinatorHooks hooks;int attempts=0;std::string metric;
   hooks.status=[&](const auto& value){if(++attempts==1)throw std::runtime_error("synthetic publication failure");metric=value.prometheus;};
   Coordinator co(random,std::move(hooks),1000000,0);co.start();co.pump(1000000,0,1);co.pump(1000015,15,2);
   require(co.errors()==1&&attempts==2,"status publication failure recovers next interval");
   require(metric.find("mlat_server_cpu_ppm 133333\n")!=std::string::npos,"failed publication preserves CPU accounting checkpoint");
  }
  namespace fs=std::filesystem;auto dir=fs::temp_directory_path()/("mlat-status-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(dir);
  struct Cleanup {fs::path dir;~Cleanup(){for(const char* name:{"sync.json","sync.json.tmp","clients.json","clients.json.tmp","aircraft.json","aircraft.json.tmp","stats.prom"}){std::error_code e;fs::remove(dir/name,e);}std::error_code e;fs::remove(dir,e);}} cleanup{dir};
  publish_status(last,dir.string(),(dir/"stats.prom").string());publish_status(last,dir.string(),(dir/"missing"/"optional.prom").string());
  for(const char* name:{"sync","clients","aircraft"}){std::ifstream f(dir/(std::string(name)+".json"));json actual;f>>actual;compare(actual,status(last).at(name),std::string("published ")+name);require(!fs::exists(dir/(std::string(name)+".json.tmp")),"no temporary publication residue");}
  fs::create_symlink(dir/"clients.json",dir/"sync.json.tmp");bool threw=false;try{publish_status(last,dir.string());}catch(const std::invalid_argument&){threw=true;}require(threw,"publication refuses pre-existing temporary symlink");
  std::cout<<checks<<" coordinator assertions; "<<outputs<<" outputs, "<<valid_outputs<<" valid-filter outputs (including fault-isolation replay)\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
