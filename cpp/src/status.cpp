// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of coordinator._write_state / ClockTracker.dump_receiver_state.
#include "mlat/status.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <numeric>
#include <stdexcept>
namespace mlat {
using nlohmann::json;
std::string fixed_decimal(double v,int digits){
 if(!std::isfinite(v)||digits<0||digits>8)throw std::invalid_argument("invalid decimal formatting");
 std::array<char,384> buffer{};auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),v,std::chars_format::fixed,digits);
 if(result.ec!=std::errc{})throw std::range_error("decimal formatting bound");
 return {buffer.data(),result.ptr};
}
double python_round(double v,int digits){
 // Decimal formatting rounds the exact binary value, avoiding multiplication
 // artifacts such as round(2.675,2)==2.68. to_chars uses nearest/ties-to-even.
 auto text=fixed_decimal(v,digits);double rounded=0;auto r=std::from_chars(text.data(),text.data()+text.size(),rounded);
 if(r.ec!=std::errc{})throw std::range_error("rounded value overflow");
 return rounded;
}
namespace {
std::string hex_address(std::uint32_t address,bool uppercase){
 std::array<char,8> data{};auto r=std::to_chars(data.data(),data.data()+data.size(),address,16);std::string text(data.data(),r.ptr);
 if(text.size()<6)text.insert(0,6-text.size(),'0');
 if(uppercase)for(auto& c:text)if(c>='a'&&c<='f')c=static_cast<char>(c-'a'+'A');
 return text;
}
std::string py_repr(const std::string& s){
 char quote=s.find('\'')!=std::string::npos&&s.find('"')==std::string::npos?'"':'\'';std::string out(1,quote);
 constexpr char hex[]="0123456789abcdef";
 for(unsigned char c:s){if(c==quote||c=='\\'){out+='\\';out+=static_cast<char>(c);}else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if(c=='\t')out+="\\t";
  else if(c<32||c==127){out+="\\x";out+=hex[c>>4];out+=hex[c&15];}else out+=static_cast<char>(c);}
 out+=quote;return out;
}
json optional_number(std::optional<double> value){return value?json(*value):json(nullptr);}
}
void StatusEngine::location(ReceiverModel& r,Vec3 llh,double x,double y){
 for(auto v:llh)if(!std::isfinite(v))throw std::invalid_argument("invalid receiver coordinates");
 if(std::abs(llh[0])>90||std::abs(llh[1])>180||llh[2]<-1000||llh[2]>100000)throw std::invalid_argument("receiver coordinates out of range");
 if(!std::isfinite(x)||!std::isfinite(y)||x<0||x>=1||y<0||y>=1)throw std::invalid_argument("invalid map random input");
 r.position_llh=llh;r.position=llh_to_ecef(llh);
 if(r.privacy){r.map_lat.reset();r.map_lon.reset();r.map_alt.reset();}
 else {r.map_lat=python_round(python_round(llh[0]*20)/20+(-1.0/20+1.0/20*x),2);
  r.map_lon=python_round(python_round(llh[1]*20)/20+(-1.0/20+1.0/20*y),2);r.map_alt=50*python_round(llh[2]/50);}
}
StatusSnapshot StatusEngine::update(const std::map<clock::StationId,std::shared_ptr<ReceiverModel>>& receivers,
 const std::map<std::uint32_t,std::shared_ptr<AircraftModel>>& aircraft,std::size_t wanted_count,std::span<const PairSnapshot> pairs,
 const IntervalStats& statistics,double now,double cpu,std::uint32_t partition,std::uint32_t partitions){
 if(!std::isfinite(now)||!std::isfinite(cpu)||cpu<0||!partition||partition>partitions||receivers.size()>4096||aircraft.size()>100000||pairs.size()>1000000)throw std::invalid_argument("invalid status input");
 StatusSnapshot result;std::size_t sync_ac=0,bad_receivers=0;
 for(const auto& [address,ac]:aircraft){auto icao=hex_address(address,true);double elapsed=now-ac->seen;
  json s={{"icao",icao},{"elapsed_seen",python_round(elapsed,1)},{"interesting",ac->interesting()?1:0},{"allow_mlat",ac->allow_mlat?1:0},
   {"tracking",ac->tracking.size()},{"sync_interest",ac->sync_interest.size()},{"mlat_interest",ac->mlat_interest.size()},{"adsb_seen",ac->adsb_seen.size()},
   {"mlat_message_count",ac->mlat_message_count},{"mlat_result_count",ac->mlat_result_count},{"mlat_kalman_count",ac->mlat_kalman_count}};
  double sync_count=python_round(ac->sync_good+ac->sync_bad);s["sync_count_1min"]=sync_count;
  ac->sync_bad_percent=python_round(100*ac->sync_bad/(sync_count+.01),1);s["sync_bad_percent"]=ac->sync_bad_percent;
  ac->sync_dont_use=ac->sync_bad>3&&ac->sync_bad_percent>15;ac->sync_good*=.8;ac->sync_bad*=.8;
  if(ac->last_result_position){s["last_result"]=python_round(now-ac->last_result_time,1);auto llh=ecef_to_llh(*ac->last_result_position);s["lat"]=python_round(llh[0],4);s["lon"]=python_round(llh[1],4);
   if(ac->altitude)s["alt"]=*ac->altitude;
   const auto& filter=ac->kalman.state();if(filter.valid){s["heading"]=python_round(filter.heading);s["speed"]=python_round(filter.ground_speed);}}
  if(elapsed>600){s["tracking_receivers"]=json::array();for(auto id:ac->tracking)s["tracking_receivers"].push_back(receivers.at(id)->public_id);}
  if(!ac->sync_interest.empty())++sync_ac;
  result.aircraft[icao]=std::move(s);
 }
 // Vector order reproduces insertion-order summation and bad_peer_list repr.
 using Peer=std::pair<std::string,std::array<double,8>>;std::map<clock::StationId,std::vector<Peer>> states;
 for(const auto& p:pairs){const auto& s=p.state;if(s.n<2)continue;const auto& a=*receivers.at(p.first);const auto& b=*receivers.at(p.second);
  double outlier=(s.update_total<4?50:100)*s.outlier_total/s.update_total;
  auto make=[&](const ReceiverModel& peer,double drift){return std::array<double,8>{static_cast<double>(s.n),python_round(s.error*1e6,1),python_round(drift*1e6),python_round(peer.bad_syncs,2),static_cast<double>(s.jumped),python_round(outlier,1),python_round(now-s.updated),static_cast<double>(std::accumulate(peer.sync_peers.begin(),peer.sync_peers.end(),0))};};
  states[p.first].emplace_back(b.user,make(b,s.drift));states[p.second].emplace_back(a.user,make(a,s.i_drift));
 }
 double outlier_sum=0,sync_sum=0;
 for(const auto& [id,r]:receivers){std::size_t bad_peers=0,num_peers=8;double sum_outlier=0;json peers=json::object();std::string bad_list="[";
  for(const auto& [username,state]:states[id]){peers[username]=state;if(state[3]>0||state[6]>20||state[7]<8)continue;
   sum_outlier+=state[5];++num_peers;
   if(state[4]!=0||state[5]>35||(state[0]>10&&state[1]>1.2)||(state[0]>3&&state[1]>1.8)||state[1]>2.4){if(bad_peers++)bad_list+=", ";bad_list+=py_repr(username);}}
  bad_list+=']';double n=static_cast<double>(num_peers),bad=static_cast<double>(bad_peers);
  outlier_sum+=sum_outlier/100;sync_sum+=n;double outlier=sum_outlier/n;r->outlier_percent_rolling-=.1*(r->outlier_percent_rolling-outlier);
  if(bad/n>.15&&bad_peers>3)r->bad_syncs+=std::min(.5,2*bad/n)+.1;
  r->bad_syncs=std::max(0.0,std::min(6.0,r->bad_syncs-.1));if(r->bad_syncs>0)++bad_receivers;
  r->recent_pair_jumps=0;r->num_outliers*=.25;r->num_syncs*=.25;r->peer_count=peers.size();
  result.sync[r->user]={{"peers",peers},{"bad_syncs",r->bad_syncs},{"lat",optional_number(r->map_lat)},{"lon",optional_number(r->map_lon)}};
  json c={{"user",r->user},{"uid",r->public_id},{"uuid",r->uuid?json(*r->uuid):json(nullptr)},{"coords",fixed_decimal(r->position_llh[0],6)+","+fixed_decimal(r->position_llh[1],6)},
   {"lat",r->position_llh[0]},{"lon",r->position_llh[1]},{"alt",r->position_llh[2]},{"privacy",r->privacy},{"connection",r->connection_info},
   {"source_ip",r->source_ip},{"source_port",r->source_port},{"message_rate",python_round(static_cast<double>(r->message_counter)/15)},
   {"peer_count",r->peer_count},{"bad_sync_timeout",python_round(r->bad_syncs*15/.1)},{"outlier_percent",python_round(r->outlier_percent_rolling,1)},{"bad_peer_list",bad_list}};
  c["sync_interest"]=json::array();c["mlat_interest"]=json::array();for(const auto& ac:r->sync_interest)c["sync_interest"].push_back(hex_address(ac->address,false));for(const auto& ac:r->mlat_interest)c["mlat_interest"].push_back(hex_address(ac->address,false));
  result.client_statistics[id]={{"peer_count",r->peer_count},{"bad_sync_timeout",c["bad_sync_timeout"]},{"outlier_percent",c["outlier_percent"]}};
  result.clients[r->user]=std::move(c);r->message_counter=0;
 }
 double total_outlier=100*outlier_sum/(sync_sum+.1),cpu_rate=python_round((cpu-last_cpu_seconds_)*(1e6/15));last_cpu_seconds_=cpu;
 auto metric=[&](std::string_view name,double value){result.prometheus+="mlat_server_";result.prometheus+=name;result.prometheus+=' ';result.prometheus+=fixed_decimal(value,0);result.prometheus+='\n';};
 metric("cpu_ppm",cpu_rate);metric("receivers",static_cast<double>(receivers.size()));metric("ac_mlat",static_cast<double>(wanted_count));metric("ac_sync",static_cast<double>(sync_ac));metric("ac_total",static_cast<double>(aircraft.size()));metric("outlier_ppm",total_outlier*1000);
#define METRIC(x) metric(#x,static_cast<double>(statistics.x)/15)
 METRIC(sync_points);METRIC(sync_msgs);METRIC(mlat_msgs);METRIC(valid_groups);METRIC(normalize_called);METRIC(solve_attempt);METRIC(solve_success);METRIC(solve_used);
#undef METRIC
 result.title="Status: ";if(partitions>1)result.title+=std::to_string(partition)+"/"+std::to_string(partitions)+" ("+std::to_string(receivers.size())+" clients)";
 else result.title+="("+std::to_string(receivers.size())+" clients "+std::to_string(bad_receivers)+" bad sync) ("+fixed_decimal(total_outlier,2)+" outlier_percentage)";
 result.title+=" ("+std::to_string(wanted_count)+" mlat "+std::to_string(sync_ac)+" sync "+std::to_string(aircraft.size())+" tracked)";return result;
}
}
