// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of jsonclient.py, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/protocol.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
namespace mlat::wire {
namespace {
Json parse(std::string_view line){
 if(line.size()>131072)throw std::length_error("protocol line bound");
 for(unsigned char c:line)if(c>127)throw std::invalid_argument("non-ASCII protocol line");
 return Json::parse(line,[](int depth,Json::parse_event_t,Json&){if(depth>64)throw std::length_error("JSON nesting bound");
 return true;});
}
bool truth(const Json& value){
 if(value.is_null())return false;
 if(value.is_boolean())return value.get<bool>();
 if(value.is_number())return value.get<double>()!=0;
 if(value.is_string())return !value.get_ref<const std::string&>().empty();
 return !value.empty();
}
std::string string(const Json& value){
 if(value.is_string())return value.get<std::string>();
 if(value.is_null())return "None";
 if(value.is_boolean())return value.get<bool>()?"True":"False";
 if(value.is_number())return value.dump();
 throw std::invalid_argument("expected scalar string");
}
double number(const Json& value){
 double result;
 if(value.is_number())result=value.get<double>();
 else if(value.is_boolean())result=value.get<bool>()?1:0;
 else if(value.is_string()){
  auto text=value.get<std::string>();auto first=text.find_first_not_of(" \t\n\r\f\v"),last=text.find_last_not_of(" \t\n\r\f\v");
  if(first==std::string::npos)throw std::invalid_argument("empty numeric field");
  const char* begin=text.data()+first;const char* end=text.data()+last+1;if(*begin=='+')++begin;
  auto parsed=std::from_chars(begin,end,result);if(parsed.ec!=std::errc{}||parsed.ptr!=end)throw std::invalid_argument("invalid numeric field");
 }else throw std::invalid_argument("invalid numeric field type");
 if(!std::isfinite(result))throw std::invalid_argument("nonfinite numeric field");
 return result;
}
std::string username(const Json& value){
 auto text=string(value);std::string out;
 for(unsigned char c:text){if((c&0xc0)==0x80)continue;
  bool allowed=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-';
  out+=allowed?static_cast<char>(c):'_';if(out.size()==40)break;}
 return out;
}
int random_integer(const HandshakeHooks& hooks,int low,int high){
 if(!hooks.random_integer)throw std::logic_error("missing handshake RNG");
 auto result=hooks.random_integer(low,high);
 if(result<low||result>=high)throw std::logic_error("invalid handshake RNG");
 return result;
}
double reconnect(const HandshakeHooks& hooks,double base){
 if(!hooks.random_unit)throw std::logic_error("missing handshake jitter RNG");
 double u=hooks.random_unit();
 if(!std::isfinite(u)||u<0||u>=1)throw std::logic_error("invalid handshake jitter RNG");
 return python_round(.9*base+(.2*base)*u);
}
std::uint32_t address(const Json& value){
 if(!value.is_string())throw std::invalid_argument("address must be hex string");
 auto s=value.get<std::string>();
 auto first=s.find_first_not_of(" \t\r\n"),last=s.find_last_not_of(" \t\r\n");if(first==std::string::npos)throw std::invalid_argument("empty address");
 auto text=std::string_view(s).substr(first,last-first+1);if(text.starts_with('+'))text.remove_prefix(1);
 if(text.starts_with("0x")||text.starts_with("0X"))text.remove_prefix(2);
 if(text.empty()||text.size()>6)throw std::invalid_argument("invalid address length");
 std::uint32_t result=0;
 auto p=std::from_chars(text.data(),text.data()+text.size(),result,16);
 if(p.ec!=std::errc{}||p.ptr!=text.data()+text.size()||result>0xffffff)throw std::invalid_argument("invalid address");
 return result;
}
std::vector<std::uint8_t> bytes(const Json& value){
 if(!value.is_string())throw std::invalid_argument("message must be hex string");
 auto text=value.get<std::string>();
 if(text.size()>256)throw std::length_error("hex message bound");
 std::vector<std::uint8_t> out;
 auto nibble=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
 for(std::size_t i=0;i<text.size();){if(std::string_view(" \t\r\n\v\f").find(text[i])!=std::string_view::npos){++i;continue;}
  if(i+1>=text.size())throw std::invalid_argument("odd hex length");
 int a=nibble(text[i]),b=nibble(text[i+1]);
  if(a<0||b<0)throw std::invalid_argument("invalid hex message");
 out.push_back(static_cast<std::uint8_t>(16*a+b));i+=2;
 }
 if(out.size()!=7&&out.size()!=14)throw std::invalid_argument("invalid Mode S length");
 return out;
}
SyncMessage sync_bytes(const Json& value){auto data=bytes(value);if(data.size()!=14)throw std::invalid_argument("short sync frame");
 SyncMessage out{};std::copy(data.begin(),data.end(),out.begin());return out;}
}
HandshakeResult handshake(Coordinator& co,std::string_view line,ReceiverInfo peer,std::string_view motd,std::string_view source_url,const HandshakeHooks& hooks){
 if(motd.size()>8192||source_url.size()>2048)throw std::length_error("handshake server configuration bound");
 HandshakeResult result;std::string deny;
 try{
  auto hs=parse(line);if(!hs.is_object())throw std::invalid_argument("handshake must be an object");
  if(!hs.at("version").is_number()||(hs.at("version")!=2&&hs.at("version")!=3))throw std::invalid_argument("Unsupported version in handshake");
  peer.user=username(hs.at("user"));if(peer.user.size()<3)peer.user+='_'+std::to_string(random_integer(hooks,10,99));
  auto uuid=hs.value("uuid",Json());if(!uuid.is_null()&&!uuid.is_string())throw std::invalid_argument("UUID must be string or null");
  peer.uuid=uuid.is_null()?std::nullopt:std::optional(uuid.get<std::string>());
  auto lookup=[&](const std::string& name)->std::shared_ptr<ReceiverModel>{for(const auto& [id,r]:co.receivers()){(void)id;if(r->user==name)return r;}return {};};
  if(auto existing=lookup(peer.user)){
   if(peer.uuid&&!peer.uuid->empty()&&peer.uuid==existing->uuid){if(!hooks.close_existing)throw std::logic_error("missing duplicate close callback");
 hooks.close_existing(existing->id);}
   else {int attempts=1000;while(attempts--&&lookup(peer.user))peer.user+='_'+std::to_string(random_integer(hooks,100,999));}
  }
  const auto& compression=hs.at("compress");if(!compression.is_array())throw std::invalid_argument("compression must be array");
  for(auto name:{"zlib2","zlib","none"})if(std::find(compression.begin(),compression.end(),Json(name))!=compression.end()){result.compression=name;break;}
  if(result.compression.empty())throw std::invalid_argument("No mutually usable compression type");
  peer.llh={number(hs.at("lat")),number(hs.at("lon")),number(hs.at("alt"))};
  if(peer.llh[0]<-90||peer.llh[0]>90)throw std::invalid_argument("invalid latitude, should be -90 .. 90");
  if(peer.llh[1]<-180||peer.llh[1]>360)throw std::invalid_argument("invalid longitude, should be -180 .. 360");
  if(peer.llh[1]>180)peer.llh[1]-=180;
  if(peer.llh[2]<-1000||peer.llh[2]>10000)throw std::invalid_argument("invalid altitude, should be -1000 .. 10000");
  peer.clock_type=string(hs.value("clock_type",Json("dump1090")));(void)clock::make_clock(peer.clock_type);
  result.return_results=truth(hs.value("return_results",Json(false)));result.return_stats=truth(hs.value("return_stats",Json(false)));
  if(result.return_results){result.result_format=string(hs.value("return_result_format",Json("old")));
   if(result.result_format!="old"&&result.result_format!="ecef")throw std::invalid_argument("invalid return_result_format, should be one of \"old\" or \"ecef\"");}
  peer.privacy=truth(hs.value("privacy",Json(false)));
  peer.connection_info=peer.user+" v"+string(hs.at("version"))+" "+peer.clock_type+" "+string(hs.value("client_version",Json("unknown")))+" tcp "+result.compression;
  // No raw handshake/auth is retained or logged. Authenticator sees it only now.
  result.receiver=co.connect(peer,[&](ReceiverModel& r){if(hooks.authenticate)try{hooks.authenticate(r,hs.value("auth",Json()));}catch(...){throw std::invalid_argument("authentication failed");}});
 }catch(const Json::out_of_range&){deny="Missing field in handshake";}
 catch(const Json::exception&){deny="Badly formatted handshake";}
 catch(const std::invalid_argument& e){deny="Bad values in handshake: "+std::string(e.what());}
 catch(const std::length_error&){deny="Handshake input bound exceeded";}
 // Application failures propagate to the session, with generic diagnostics only.
 if(!deny.empty()){result.response={{"deny",Json::array({deny})},{"reconnect_in",reconnect(hooks,900)}};return result;}
 try{
 std::string expanded="\n\n        "+std::string(motd)+"\n\n        The multilateration server source code is available under\n        the terms of the Affero GPL (v3 or later). You may obtain\n        a copy of this server's source code at the following\n        location: "+std::string(source_url)+"\n        ";
 result.response={{"compress",result.compression},{"reconnect_in",reconnect(hooks,10)},{"selective_traffic",true},{"heartbeat",true},
  {"return_results",result.return_results},{"return_stats",result.return_stats},{"rate_reports",true},{"motd",expanded}};return result;
 }catch(...){if(result.receiver)co.disconnect(result.receiver->id);throw;}
}
Message decode_message(std::string_view line,ReceiverModel& receiver,double now){
 if(!std::isfinite(now))throw std::invalid_argument("invalid server time");
 Message out;Json msg;
 try{msg=parse(line);}catch(const Json::parse_error&){out.kind=MessageKind::malformed;return out;}
 ++receiver.message_counter;
 if(!msg.is_object())throw std::invalid_argument("message must be an object");
 if(msg.contains("sync")){const auto& s=msg.at("sync");out.kind=MessageKind::sync;out.even=number(s.at("et"));out.odd=number(s.at("ot"));out.even_message=sync_bytes(s.at("em"));out.odd_message=sync_bytes(s.at("om"));}
 else if(msg.contains("mlat")){
  if(receiver.bad_syncs>0||now-receiver.last_sync>20)return out;
  const auto& m=msg.at("mlat");out.kind=MessageKind::mlat;out.timestamp=number(m.at("t"));out.bytes=bytes(m.at("m"));
 }else if(msg.contains("seen")||msg.contains("lost")){
  bool seen=msg.contains("seen");out.kind=seen?MessageKind::seen:MessageKind::lost;const auto& list=msg.at(seen?"seen":"lost");
  if(!list.is_array()||list.size()>100000)throw std::invalid_argument("invalid tracking list");
  std::set<std::uint32_t> unique;for(const auto& entry:list)unique.insert(address(entry));out.addresses.assign(unique.begin(),unique.end());
 }else if(msg.contains("input_connected"))out.kind=MessageKind::input_connected;
 else if(msg.contains("input_disconnected")||msg.contains("clock_reset")||msg.contains("clock_jump"))out.kind=MessageKind::reset;
 else if(msg.contains("heartbeat"))out.kind=MessageKind::heartbeat;
 else if(msg.contains("rate_report")){
  out.kind=MessageKind::rates;const auto& rates=msg.at("rate_report");if(!rates.is_object()||rates.size()>100000)throw std::invalid_argument("invalid rate report");
  for(auto it=rates.begin();it!=rates.end();++it){if(!it.value().is_number())throw std::invalid_argument("rate must be number");
 double rate=number(it.value());
   if(rate<0||rate>1000000)throw std::invalid_argument("invalid rate");
 out.rates[address(it.key())]=rate;}
 }else if(msg.contains("quine")){out.kind=MessageKind::quine;out.query=msg.at("quine");}
 return out;
}
Message dispatch_message(Coordinator& co,clock::StationId id,std::string_view line,double now){
 auto it=co.receivers().find(id);if(it==co.receivers().end())throw std::invalid_argument("inactive protocol peer");
 auto out=decode_message(line,*it->second,now);
 switch(out.kind){
 case MessageKind::sync:co.sync(id,out.even,out.odd,out.even_message,out.odd_message);break;
 case MessageKind::mlat:co.mlat(id,out.timestamp,out.bytes);break;
 case MessageKind::seen:co.seen(id,out.addresses);break;
 case MessageKind::lost:co.lost(id,out.addresses);break;
 case MessageKind::input_connected:co.reset(id,true);break;
 case MessageKind::reset:co.reset(id);break;
 case MessageKind::rates:co.rates(id,out.rates);break;
 default:break;
 }return out;
}
Json quine_reply(const Json& request,const std::map<std::string,std::string>& sources){
 Json value=nullptr;
 if(!truth(request)){value=Json::array();for(const auto& [name,source]:sources){(void)source;value.push_back(name);}}
 else if(request.is_string()){auto it=sources.find(request.get<std::string>());if(it!=sources.end()){if(it->second.size()>262144)throw std::length_error("public source bound");
 value=it->second;}}
 return {{"quine",Json::array({request,value})}};
}
}
