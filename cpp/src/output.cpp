// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of output.py / jsonclient.py, Copyright (C) 2015 Oliver Jowett.
#include "mlat/output.hpp"
#include "mlat/status.hpp"
#include <charconv>
#include <cmath>
#include <ctime>
#include <stdexcept>
namespace mlat {
using nlohmann::json;
namespace {
std::string address(std::uint32_t a,bool upper=false){
 if(a>0xffffff)throw std::invalid_argument("invalid output address");
 std::array<char,8> b{};auto end=std::to_chars(b.data(),b.data()+b.size(),a,16).ptr;
 std::string s(b.data(),end);s.insert(0,6-s.size(),'0');
 if(upper)for(auto& c:s)if(c>='a'&&c<='f')c=static_cast<char>(c-'a'+'A');
 return s;
}
json nullable(const std::optional<std::string>& s){return s?json(*s):json(nullptr);}
std::string integer(std::optional<int> i){return i?std::to_string(*i):"None";}
bool current_filter(const MlatResult& r){return r.filter.valid&&r.filter.last_update>=r.time;}
bool fresh(double when,double now){return when!=0&&now-when<5;}
std::string rounded_integer(double d){auto s=fixed_decimal(d,0);return s=="-0"?"0":s;}
// Python str(float): shortest round-trip with fixed notation at exponents -4..15.
std::string py_float(double value){
 if(!std::isfinite(value))throw std::invalid_argument("nonfinite output");
 std::array<char,64> b{};auto r=std::to_chars(b.data(),b.data()+b.size(),value,std::chars_format::scientific);
 if(r.ec!=std::errc{})throw std::range_error("output float bound");
 std::string s(b.data(),r.ptr);auto e=s.find('e');int exponent=std::stoi(s.substr(e+1));
 if(exponent < -4 || exponent>=16)return s;
 bool negative=s.front()=='-';std::string digits=s.substr(negative?1:0,e-(negative?1:0));
 auto dot=digits.find('.');if(dot!=std::string::npos)digits.erase(dot,1);
 int point=exponent+1;std::string out;
 if(point<=0)out="0."+std::string(static_cast<std::size_t>(-point),'0')+digits;
 else if(static_cast<std::size_t>(point)>=digits.size())out=digits+std::string(static_cast<std::size_t>(point)-digits.size(),'0')+".0";
 else {out=digits;out.insert(static_cast<std::size_t>(point),1,'.');}
 return negative?"-"+out:out;
}
std::tm utc(double t){
 // Bound conversion before casting; service timestamps fit years 1900..9999.
 if(!std::isfinite(t)||t < -2208988800.0 || t>=253402300800.0)throw std::invalid_argument("output timestamp bound");
 auto seconds=static_cast<std::time_t>(std::floor(t));std::tm out{};
#ifdef _WIN32
 if(gmtime_s(&out,&seconds)!=0)throw std::range_error("UTC conversion");
#else
 if(!gmtime_r(&seconds,&out))throw std::range_error("UTC conversion");
#endif
 return out;
}
std::string calendar(double t,const char* format){auto value=utc(t);std::array<char,32> b{};
 if(!std::strftime(b.data(),b.size(),format,&value))throw std::range_error("UTC formatting");
 return b.data();}
std::string join(const std::vector<std::string>& fields){std::string out;bool first=true;for(const auto& f:fields){if(!first)out+=',';first=false;out+=f;}return out;}
}
std::string csv_quote(const std::optional<std::string>& s){
 if(!s)return "";
 if(s->size()>65536)throw std::length_error("output text bound");
 // Spaces and carriage returns alone are NOT quoted by the pinned source.
 if(s->find_first_of("\n\",")==std::string::npos)return *s;
 std::string out="\"";for(char c:*s){out+=c;if(c=='\"')out+='\"';}return out+'\"';
}
std::string output_date(double t){return calendar(t,"%Y/%m/%d");}
std::string output_time(double t){
 auto base=calendar(t,"%H:%M:%S");double integral=0;auto fraction=fixed_decimal(std::modf(t,&integral)*1000,0);
 // Source formats width 3, including sign, and does not carry .1000.
 if(fraction.size()<3)fraction.insert(fraction.front()=='-'?1:0,3-fraction.size(),'0');
 return base+'.'+fraction;
}
json client_result_old(const MlatResult& r,const AircraftModel& ac){
 auto llh=ecef_to_llh(r.solution.position);
 return {{"@",python_round(r.time,3)},{"addr",address(r.address)},{"lat",python_round(llh[0],5)},
 {"lon",python_round(llh[1],5)},{"alt",python_round(llh[2]*constants::mtof)},
 {"callsign",nullable(ac.callsign)},{"squawk",nullable(ac.squawk)},
 {"hdop",0.0},{"vdop",0.0},{"tdop",0.0},{"gdop",0.0},{"nstations",r.all_receivers.size()}};
}
json client_result_ecef(const MlatResult& r,const AircraftModel& ac){
 json out={{"@",python_round(r.time,3)},{"addr",address(r.address)},{"n",r.all_receivers.size()},{"nd",r.distinct},{"cov",nullptr}};
 if(r.solution.covariance){const auto& c=*r.solution.covariance;
  out["cov"]={python_round(c[0][0]),python_round(c[0][1]),python_round(c[0][2]),python_round(c[1][1]),python_round(c[1][2]),python_round(c[2][2])};}
 if(current_filter(r)){double speed=r.filter.ground_speed*constants::ms_to_kts,angle=r.filter.heading*constants::dtor;
  out["nsvel"]=python_round(std::cos(angle)*speed,1);out["ewvel"]=python_round(std::sin(angle)*speed,1);}
 if(fresh(ac.vrate_time,r.time))out["vrate"]=ac.vrate?json(*ac.vrate):json(nullptr);
 out["ecef"]={python_round(r.solution.position[0]),python_round(r.solution.position[1]),python_round(r.solution.position[2])};return out;
}
std::string csv_result(const MlatResult& r,const AircraftModel& ac){
 auto llh=ecef_to_llh(r.solution.position);double error=-1;
 if(r.solution.covariance){const auto& c=*r.solution.covariance;double variance=c[0][0]+c[1][1]+c[2][2];if(variance>=0)error=std::sqrt(variance);}
 std::vector<std::string> names;for(const auto& rec:r.cluster_receivers)names.push_back(rec->user);auto users=join(names);
 auto vrate=fresh(ac.vrate_time,r.time)?integer(ac.vrate):"";
 std::vector<std::string> fields={fixed_decimal(r.time,3),address(r.address,true),csv_quote(ac.callsign),csv_quote(ac.squawk),
  fixed_decimal(llh[0],4),fixed_decimal(llh[1],4),fresh(ac.last_altitude_time,r.time)?integer(ac.altitude):"",
  fixed_decimal(error,0),std::to_string(r.cluster_receivers.size()),std::to_string(r.distinct),csv_quote(users),std::to_string(r.dof)};
 if(current_filter(r)){const auto& k=r.filter;fields.insert(fields.end(),{fixed_decimal(k.llh[0],4),fixed_decimal(k.llh[1],4),
  fixed_decimal(k.llh[2]*constants::mtof,0),fixed_decimal(k.heading,0),fixed_decimal(k.ground_speed*constants::ms_to_kts,0),vrate,fixed_decimal(k.position_error,0)});}
 else fields.push_back(vrate);
 return join(fields);
}
std::optional<std::string> basestation_result(const MlatResult& r,AircraftModel& ac,bool use_kalman,double send_time){
 std::string speed,heading;
 if(use_kalman){
  if(!r.filter.valid&&r.dof<1){
   if(r.time-ac.last_crappy_output>60){ac.last_crappy_output=r.time-1;return {};}
   if(r.time-ac.last_crappy_output>30||r.time==ac.last_crappy_output)ac.last_crappy_output=r.time;
   else return {};
  }
  if(current_filter(r)){speed=rounded_integer(r.filter.ground_speed*constants::ms_to_kts);heading=rounded_integer(r.filter.heading);}
 }
 auto llh=ecef_to_llh(r.solution.position);
 // The source overwrites UKF vertical speed with fresh decoded barometric rate.
 std::vector<std::string> fields={"MSG","3","1","1",address(r.address,true),"1",output_date(r.time),output_time(r.time),
  output_date(send_time),output_time(send_time),csv_quote(ac.callsign),fresh(ac.last_altitude_time,r.time)?integer(ac.altitude):"",
  speed,heading,py_float(python_round(llh[0],6)),py_float(python_round(llh[1],6)),
  fresh(ac.vrate_time,r.time)?integer(ac.vrate):"",csv_quote(ac.squawk),std::to_string(r.cluster_receivers.size()),rounded_integer(r.error),"",""};
 auto line=join(fields)+'\n';for(unsigned char c:line)if(c>127)throw std::invalid_argument("non-ASCII basestation result");return line;
}
}
