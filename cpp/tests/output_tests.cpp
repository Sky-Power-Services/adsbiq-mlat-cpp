#include "mlat/output.hpp"
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
using namespace mlat;using nlohmann::json;
std::size_t checks=0;
void require(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void compare(const json& a,const json& e,const std::string& path){
 if(e.is_object()){require(a.is_object()&&a.size()==e.size(),path+" fields");for(auto it=e.begin();it!=e.end();++it)compare(a.at(it.key()),it.value(),path+"."+it.key());return;}
 if(e.is_array()){require(a.is_array()&&a.size()==e.size(),path+" length");for(std::size_t i=0;i<e.size();++i)compare(a[i],e[i],path+"["+std::to_string(i)+"]");return;}
 require(a==e,path+" expected="+e.dump()+" actual="+a.dump());
 if(e.is_number_float()&&e.get<double>()==0&&a.is_number_float())require(std::signbit(a.get<double>())==std::signbit(e.get<double>()),path+" zero sign");
}
template<typename F>void rejects(F function,const std::string& why){bool threw=false;try{function();}catch(const std::exception&){threw=true;}require(threw,why);}
int main(int argc,char**argv){try{
 if(argc!=2)throw std::runtime_error("fixture required");std::ifstream file(argv[1]);json data;file>>data;
 for(const auto& d:data.at("dates")){require(output_date(d.at("time"))==d.at("date").get<std::string>(),"UTC date");require(output_time(d.at("time"))==d.at("text").get<std::string>(),"UTC time "+d.dump());}
 for(const auto& d:data.at("quotes")){std::optional<std::string> value;if(!d.at("input").is_null())value=d.at("input").get<std::string>();require(csv_quote(value)==d.at("expected").get<std::string>(),"CSV quoting");}
 std::size_t index=0,emitted=0,suppressed=0;
 for(const auto& d:data.at("cases")){
  AircraftModel ac;const auto& a=d.at("aircraft");
  if(!a.at("callsign").is_null())ac.callsign=a.at("callsign").get<std::string>();if(!a.at("squawk").is_null())ac.squawk=a.at("squawk").get<std::string>();
  if(!a.at("altitude").is_null())ac.altitude=a.at("altitude").get<int>();if(!a.at("vrate").is_null())ac.vrate=a.at("vrate").get<int>();
  ac.last_altitude_time=a.at("last_altitude_time");ac.vrate_time=a.at("vrate_time");ac.last_crappy_output=a.at("last_crappy_output");
  MlatResult r{};r.time=d.at("time");r.address=d.at("address");r.solution.position=d.at("position").get<Vec3>();
  if(!d.at("covariance").is_null())r.solution.covariance=d.at("covariance").get<Covariance3>();
  r.distinct=d.at("distinct");r.dof=d.at("dof");r.error=d.at("error");const auto& k=d.at("filter");
  r.filter.valid=k.at("valid");r.filter.last_update=k.at("last_update");r.filter.llh=k.at("position_llh").get<Vec3>();
  r.filter.ground_speed=k.at("ground_speed");r.filter.heading=k.at("heading");r.filter.vertical_speed=k.at("vertical_speed");r.filter.position_error=k.at("position_error");
  for(auto pair:{std::pair{&r.cluster_receivers,"cluster"},std::pair{&r.all_receivers,"all"}}){
   for(const auto& user:d.at(pair.second)){auto rec=std::make_shared<ReceiverModel>(pair.first->size()+1,"beast",Vec3{52,4,10},r.time);rec->user=user.get<std::string>();pair.first->push_back(rec);}}
  const auto& e=d.at("expected");auto path=std::to_string(index++);
  compare(client_result_old(r,ac),e.at("old"),path+" old");compare(client_result_ecef(r,ac),e.at("ecef"),path+" ecef");
  compare(csv_result(r,ac),e.at("csv"),path+" CSV");auto sbs=basestation_result(r,ac,d.at("use_kalman"),d.at("send"));
  compare(sbs?json(*sbs):json(nullptr),e.at("sbs"),path+" SBS");require(ac.last_crappy_output==e.at("last_crappy_output"),path+" output throttle state");
  if(sbs)++emitted;else ++suppressed;
 }
 require(emitted>100&&suppressed>5,"SBS branch coverage");
 rejects([]{output_time(std::numeric_limits<double>::quiet_NaN());},"reject nonfinite timestamp");
 rejects([]{output_date(253402300800.0);},"reject overflowing calendar");
 rejects([]{csv_quote(std::string(65537,'x'));},"bound CSV fields");
 AircraftModel ac;MlatResult r{};r.time=1000000;r.solution.position=llh_to_ecef({52,4,1000});r.dof=1;r.address=0xabcdef;
 ac.callsign="\xc3\xa9";rejects([&]{basestation_result(r,ac,false,r.time);},"source SBS ASCII constraint");
 r.address=0x1000000;rejects([&]{client_result_old(r,ac);},"reject non-24-bit address");
 std::cout<<"PASS "<<checks<<" output checks, "<<index<<" cases, "<<emitted<<" SBS outputs / "<<suppressed<<" throttled\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
