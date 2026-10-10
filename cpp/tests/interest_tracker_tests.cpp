#include "mlat/interest_tracker.hpp"
#include <nlohmann/json.hpp>
#include <deque>
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
#include <stdexcept>
using nlohmann::json;
using namespace mlat;
std::size_t checks=0;
void require(bool ok,const std::string& label){++checks;if(!ok)throw std::runtime_error(label);}
void compare(const json& a,const json& e,const std::string& path){
 if(e.is_object()){for(auto it=e.begin();it!=e.end();++it)compare(a.at(it.key()),it.value(),path+"."+it.key());return;}
 if(e.is_array()){require(a.size()==e.size(),path+" length");for(std::size_t i=0;i<e.size();++i)compare(a[i],e[i],path+"["+std::to_string(i)+"]");return;}
 bool ok=e.is_number_float()?a.is_number()&&std::isfinite(a.get<double>())&&std::abs(a.get<double>()-e.get<double>())<1e-9:a==e;
 // Python initializes this logical flag as integer 0; status writes 0/1,
 // whereas test setters also use bool. Compare its value, not JSON storage type.
 if(path.ends_with(".sync_dont_use")&&a.is_boolean()&&e.is_number_integer())ok=e.get<int>()==(a.get<bool>()?1:0);
 require(ok,path+" expected="+e.dump()+" actual="+a.dump());
}
struct Tape:InterestRandom {
 json entries;std::size_t pos=0;
 json next(const std::string& op){require(pos<entries.size(),"random tape exhausted");auto d=entries[pos++];require(d.at("op")==op,"random operation mismatch");return d;}
 double uniform()override{return next("uniform").at("value");}
 std::vector<std::size_t> choice(const std::string& op,std::size_t n,std::size_t k){auto d=next(op);require(d.at("n")==n,"random population mismatch");require(d.at("k")==k,"random count mismatch");return d.at("indices").get<std::vector<std::size_t>>();}
 std::vector<std::size_t> sample(std::size_t n,std::size_t k)override{return choice("sample",n,k);}
 std::vector<std::size_t> shuffle(std::size_t n)override{return choice("shuffle",n,n);}
};
json aids(const AircraftSet& s){json out=json::array();for(const auto& ac:s)out.push_back(ac->generation);return out;}
json state(const InterestTracker& tracker,const std::map<clock::StationId,std::shared_ptr<ReceiverModel>>& receivers,
 const std::map<std::uint64_t,std::shared_ptr<AircraftModel>>& all,const std::deque<std::shared_ptr<ReceiverModel>>& pending,const json& refreshes){
 json aa=json::array(),rr=json::array(),active=json::array(),queue=json::array();
 for(const auto& [generation,ac]:all){(void)generation;json d;
#define FIELD(x) d[#x]=ac->x
  FIELD(generation);FIELD(allow_mlat);FIELD(seen);FIELD(last_adsb_time);FIELD(last_force_mlat);FIELD(force_mlat);FIELD(do_mlat);FIELD(sync_bad_percent);FIELD(sync_dont_use);
  FIELD(tracking);FIELD(adsb_seen);FIELD(sync_interest);FIELD(mlat_interest);
#undef FIELD
  d["icao"]=ac->address;d["interesting"]=ac->interesting();d["altitude"]=ac->altitude?json(*ac->altitude):json(nullptr);aa.push_back(d);
 }
 for(const auto& [id,r]:receivers){json d={{"id",id},{"dead",r->dead},{"bad_syncs",r->bad_syncs}};
  json rates=nullptr;if(r->last_rate_report){rates=json::array();for(const auto& [address,rate]:*r->last_rate_report)rates.push_back(json::array({address,rate}));}d["rates"]=rates;
#define SET(x) d[#x]=aids(r->x)
  SET(tracking);SET(adsb_seen);SET(sync_interest);SET(mlat_interest);SET(requested);
#undef SET
  rr.push_back(d);
 }
 for(const auto& [id,ac]:tracker.aircraft())active.push_back(json::array({id,ac->generation}));
 for(const auto& r:pending)queue.push_back(r->id);
 return {{"aircraft",aa},{"receivers",rr},{"active",active},{"wanted",aids(tracker.wanted())},{"wanted_time",tracker.wanted_time()},{"pending",queue},{"refreshes",refreshes}};
}
int main(int argc,char**argv){
 try{
  if(argc!=2)throw std::runtime_error("fixture required");std::ifstream f(argv[1]);json data;f>>data;
  std::size_t steps=0;
  for(const auto& c:data.at("cases")){
   Tape rng;std::map<clock::StationId,std::shared_ptr<ReceiverModel>> receivers;std::map<std::uint64_t,std::shared_ptr<AircraftModel>> all;
   std::deque<std::shared_ptr<ReceiverModel>> pending;json refreshes;
   InterestOptions options;options.partition=c.at("partition")[0];options.partitions=c.at("partition")[1];
   InterestTracker tracker(rng,[&](auto r){pending.push_back(r);},1000000,options);
   for(std::size_t i=0;i<c.at("steps").size();++i){++steps;const auto& s=c.at("steps")[i];const auto& e=c.at("expected")[i];
    rng.entries=e.at("random");rng.pos=0;refreshes=json::array();std::string op=s.at("op");double now=s.at("now");
    if(op=="attach"){auto r=std::make_shared<ReceiverModel>(s.at("id"),"beast",Vec3{40,-75,100},now);receivers.emplace(r->id,r);tracker.attach(r);}
    else if(op=="add"){auto addresses=s.at("addresses").get<std::vector<std::uint32_t>>();tracker.add(*receivers.at(s.at("id")),addresses,now);for(auto id:addresses){auto ac=tracker.find(id);all.emplace(ac->generation,ac);}}
    else if(op=="remove"){auto addresses=s.at("addresses").get<std::vector<std::uint32_t>>();tracker.remove(*receivers.at(s.at("id")),addresses);}
    else if(op=="update")tracker.update_interest(*receivers.at(s.at("id")),now);
    else if(op=="rates"){auto& report=receivers.at(s.at("id"))->last_rate_report;report.reset();if(!s.at("rates").is_null()){report.emplace();for(const auto& d:s.at("rates"))report->emplace(d[0].get<std::uint32_t>(),d[1].get<double>());}}
    else if(op=="bad")receivers.at(s.at("id"))->bad_syncs=s.at("value");
    else if(op=="aircraft"){auto ac=tracker.find(s.at("address"));for(auto it=s.at("fields").begin();it!=s.at("fields").end();++it){
      if(it.key()=="altitude")ac->altitude=it.value().get<int>();else if(it.key()=="sync_dont_use")ac->sync_dont_use=it.value();
      else if(it.key()=="last_adsb_time")ac->last_adsb_time=it.value();else if(it.key()=="last_force_mlat")ac->last_force_mlat=it.value();
      else if(it.key()=="sync_bad_percent")ac->sync_bad_percent=it.value();else throw std::runtime_error("unsupported aircraft setter");
    }}
    else if(op=="disconnect"){auto r=receivers.at(s.at("id"));r->dead=true;tracker.disconnect(*r);}
    else if(op=="flush"){while(!pending.empty()){auto r=pending.front();pending.pop_front();InterestTracker::refresh_requests(*r);std::set<std::uint32_t> addresses;for(const auto& ac:r->requested)addresses.insert(ac->address);refreshes.push_back(json::array({r->id,addresses}));}}
    else throw std::runtime_error("unknown operation");
    require(rng.pos==rng.entries.size(),"unused random choices");compare(state(tracker,receivers,all,pending,refreshes),e.at("state"),c.at("name").get<std::string>()+" step "+std::to_string(i));
   }
  }
  // Port-only configuration and ingress safety checks, not oracle equivalence.
  SeededInterestRandom rng(1);InterestOptions options;options.min_tracking=2;options.mark_adsb_time=false;
  InterestTracker tracker(rng,{},1000000,options);auto r1=std::make_shared<ReceiverModel>(1,"beast",Vec3{40,-75,10},0);auto r2=std::make_shared<ReceiverModel>(2,"beast",Vec3{40,-75,10},0);
  tracker.attach(r1);tracker.attach(r2);std::array<std::uint32_t,1> ids{0x123456};tracker.add(*r1,ids,1000000);tracker.add(*r2,ids,1000000);
  r1->last_rate_report=std::map<std::uint32_t,double>{{ids[0],1}};tracker.update_interest(*r1,1000001);
  require(tracker.find(ids[0])->do_mlat,"min-tracking option");require(tracker.find(ids[0])->last_adsb_time==0,"mark-adsb option");
  bool threw=false;try{tracker.attach(r1);}catch(const std::invalid_argument&){threw=true;}require(threw,"duplicate receiver guard");
  auto count=tracker.aircraft().size();std::array<std::uint32_t,2> invalid{1,0x1000000};threw=false;try{tracker.add(*r1,invalid,1000001);}catch(const std::invalid_argument&){threw=true;}require(threw&&tracker.aircraft().size()==count,"address validation atomicity");
  (*r1->last_rate_report)[ids[0]]=std::numeric_limits<double>::quiet_NaN();threw=false;try{tracker.update_interest(*r1,1000002);}catch(const std::invalid_argument&){threw=true;}require(threw,"nonfinite rate guard");
  threw=false;try{InterestTracker bad(rng,{},0,InterestOptions{2,1,3,true});}catch(const std::invalid_argument&){threw=true;}require(threw,"partition configuration guard");
  std::weak_ptr<AircraftModel> retired=tracker.find(ids[0]);InterestTracker::refresh_requests(*r1);
  tracker.remove(*r1,ids);tracker.remove(*r2,ids);require(!tracker.find(ids[0])&&!retired.expired(),"deferred aircraft lifetime");
  r1->last_rate_report.reset();tracker.update_interest(*r1,1000003);InterestTracker::refresh_requests(*r1);
  require(retired.expired(),"retired aircraft released after deferred references clear");
  std::cout<<checks<<" interest assertions across "<<steps<<" operations\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
