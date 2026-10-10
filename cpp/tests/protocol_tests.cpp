#include "mlat/protocol.hpp"
#include <fstream>
#include <iostream>
using namespace mlat;using namespace mlat::wire;using nlohmann::json;
std::size_t checks=0;
void require(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void compare(const json& actual,const json& expected,const std::string& path){
 if(expected.is_object()){require(actual.size()==expected.size(),path+" field count");for(auto it=expected.begin();it!=expected.end();++it)compare(actual.at(it.key()),it.value(),path+"."+it.key());}
 else if(expected.is_array()){require(actual.size()==expected.size(),path+" length");for(std::size_t i=0;i<expected.size();++i)compare(actual[i],expected[i],path+"["+std::to_string(i)+"]");}
 else require(actual==expected,path+" expected="+expected.dump()+" actual="+actual.dump());
}
std::string hex(std::span<const std::uint8_t> bytes){constexpr char alphabet[]="0123456789abcdef";std::string out;for(auto b:bytes){out+=alphabet[b>>4];out+=alphabet[b&15];}return out;}
template<typename F>void rejects(F f,const std::string& why){bool threw=false;try{f();}catch(const std::exception&){threw=true;}require(threw,why);}
int main(int argc,char**argv){try{
 if(argc!=2)throw std::runtime_error("fixture required");std::ifstream input(argv[1]);json fixture;input>>fixture;
 std::size_t index=0;
 for(const auto& c:fixture.at("handshakes")){
  SeededInterestRandom rng(1);Coordinator co(rng,{},1000000,0);co.start();json closed=json::array(),draws=json::array();
  for(const auto& existing:c.at("existing")){ReceiverInfo info;info.user=existing.at("user");info.llh={52,4,100};if(!existing.at("uuid").is_null())info.uuid=existing.at("uuid").get<std::string>();co.connect(info);}
  HandshakeHooks hooks;
  hooks.random_integer=[&](int low,int high){draws.push_back({low,high,low+3});return low+3;};hooks.random_unit=[](){return .37;};
  hooks.close_existing=[&](auto id){closed.push_back(id);co.disconnect(id);};
  ReceiverInfo peer;peer.source_ip="192.0.2.1";peer.source_port=1234;
  auto result=handshake(co,c.at("handshake").dump(-1,' ',true),peer,"Synthetic test","https://example.invalid/public-source",hooks);
  const auto& e=c.at("expected");auto path="handshake "+std::to_string(index++);
  require(bool(result.receiver)==e.at("accepted").get<bool>(),path+" accepted");compare(closed,e.at("closed"),path+" duplicate closure");compare(draws,e.at("random"),path+" RNG");
  if(result.receiver){compare(result.response,e.at("response"),path+" response");const auto& r=*result.receiver;
   json receiver={{"uuid",r.uuid?json(*r.uuid):json(nullptr)},{"user",r.user},{"clock_type",c.at("handshake").value("clock_type",json("dump1090"))},
    {"position_llh",r.position_llh},{"privacy",r.privacy},{"connection_info",r.connection_info}};
   compare(receiver,e.at("receiver"),path+" receiver");require(r.source_ip=="192.0.2.1"&&r.source_port==1234,path+" peer metadata");
  }else{require(result.response.contains("deny")&&result.response.at("deny").size()==1,path+" denial schema");compare(result.response.at("reconnect_in"),e.at("response").at("reconnect_in"),path+" denial jitter");}
 }
 index=0;
 for(const auto& d:fixture.at("dispatch")){
  ReceiverModel r(1,"dump1090",{52,4,100},0);r.bad_syncs=d.at("bad_syncs");r.last_sync=d.at("last_sync");r.clock_reset_counter=4;
  auto m=decode_message(d.at("line").get<std::string>(),r,d.at("now"));json events=json::array();
  switch(m.kind){
   case MessageKind::sync:events.push_back({{"kind","sync"},{"even",m.even},{"odd",m.odd},{"em",hex(m.even_message)},{"om",hex(m.odd_message)}});break;
   case MessageKind::mlat:events.push_back({{"kind","mlat"},{"timestamp",m.timestamp},{"bytes",hex(m.bytes)},{"now",d.at("now")}});break;
   case MessageKind::seen:case MessageKind::lost:events.push_back({{"kind",m.kind==MessageKind::seen?"seen":"lost"},{"addresses",m.addresses}});break;
   case MessageKind::rates:{json rates=json::object();for(auto [a,rate]:m.rates)rates[std::to_string(a)]=rate;events.push_back({{"kind","rates"},{"rates",rates}});break;}
   case MessageKind::quine:events.push_back({{"kind","quine"},{"query",m.query}});break;
   case MessageKind::input_connected:case MessageKind::reset:events.push_back({{"kind","reset"}});break;
   default:break;
  }
  auto expected=d.at("expected").at("events");for(auto& e:expected)if(e.at("kind")=="reset")e.erase("reason");
  auto path="dispatch "+std::to_string(index++);compare(events,expected,path);require(r.message_counter==d.at("expected").at("counter"),path+" counter");
 }
 // Exercise the real coordinator bridge, not only decoded callback records.
 SeededInterestRandom rng(2);Coordinator co(rng,{},1000000,0);co.start();ReceiverInfo info;info.user="bridge";info.llh={52,4,100};auto r=co.connect(info);auto id=r->id;
 dispatch_message(co,id,R"({"seen":["123456","123456","abcdef"]})",1000000);require(r->tracking.size()==2,"coordinator seen bridge");
 dispatch_message(co,id,R"({"lost":["abcdef"]})",1000000);require(r->tracking.size()==1,"coordinator lost bridge");
 dispatch_message(co,id,R"({"rate_report":{"123456":2.5}})",1000000);require(r->last_rate_report&&r->last_rate_report->at(0x123456)==2.5,"coordinator rate bridge");
 dispatch_message(co,id,R"({"clock_jump":true})",1000000);require(r->clock_reset_counter==1,"coordinator reset bridge");
 dispatch_message(co,id,R"({"input_connected":true})",1000000);require(r->clock_reset_counter==0,"coordinator input-connected reset");
 require(r->message_counter==5,"coordinator bridge counter");
 auto before=r->message_counter;require(decode_message("{",*r,1000000).kind==MessageKind::malformed,"malformed JSON ignored");require(r->message_counter==before,"malformed JSON not counted");
 require(decode_message(R"({"mlat":null})",*r,1000000).kind==MessageKind::ignored,"stale receiver gate precedes malformed MLAT fields");
 r->last_sync=1000000;rejects([&]{decode_message(R"({"mlat":{"t":"nan","m":"20000000000000"}})",*r,1000000);},"nonfinite ticks rejected");
 rejects([&]{decode_message(R"({"seen":["1000000"]})",*r,1000000);},"24-bit address bound");
 rejects([&]{decode_message(R"({"rate_report":{"123456":-1}})",*r,1000000);},"negative rate rejected");
 rejects([&]{decode_message(std::string(65,'[')+"0"+std::string(65,']'),*r,1000000);},"JSON depth bound");
 rejects([&]{decode_message(std::string(131073,' '),*r,1000000);},"JSON line bound");
 rejects([&]{decode_message(R"({"mlat":{"t":1,"m":"20 0 0 00 00 00 00 00"}})",*r,1000000);},"whitespace not accepted between hex nibbles");
 compare(quine_reply("../private",{{"public","source"}}),{{"quine",json::array({"../private",nullptr})}},"quine cannot read files");
 compare(quine_reply("",{{"public","source"}}),{{"quine",json::array({"",json::array({"public"})})}},"quine restricted manifest");
 compare(quine_reply("public",{{"public","source"}}),{{"quine",json::array({"public","source"})}},"quine bundled public source");
 HandshakeHooks hooks;hooks.random_integer=[](int low,int){return low;};hooks.random_unit=[](){return .5;};
 auto hs=fixture.at("handshakes")[0].at("handshake");hs["user"]="auth-test";hs["auth"]="synthetic-private-value";
 hooks.authenticate=[](ReceiverModel&,const json&){throw std::invalid_argument("synthetic-private-value");};
 auto denied=handshake(co,hs.dump(),{},"test","https://example.invalid/source",hooks);
 require(!denied.receiver&&denied.response.dump().find("synthetic-private-value")==std::string::npos,"authentication error redacted");require(co.receivers().size()==1,"failed authentication does not attach");
 hooks.authenticate={};hs.erase("auth");hs["lat"]="nan";require(!handshake(co,hs.dump(),{},"test","source",hooks).receiver,"nonfinite handshake coordinates rejected");
 hs["lat"]=52;hs["clock_type"]="unsupported";require(!handshake(co,hs.dump(),{},"test","source",hooks).receiver,"unknown clock denied cleanly");
 hs.erase("clock_type");hs["return_results"]="";hs["return_stats"]="";hs["privacy"]="";
 auto accepted=handshake(co,hs.dump(),{},"test","source",hooks);require(accepted.receiver&&!accepted.return_results&&!accepted.return_stats&&!accepted.receiver->privacy,"Python empty-string truthiness");
 // TCP-only v1 follows the baseline with no UDP listener: omit the UDP
 // response field, keep the accepted TCP session and its normal dispatch.
 hs["user"]="udp-fallback";hs["udp_transport"]=2;
 auto fallback=handshake(co,hs.dump(),{},"test","source",hooks);
 require(fallback.receiver&&!fallback.response.contains("udp_transport"),"UDP offer cleanly falls back to TCP");
 require(fallback.receiver->connection_info.find(" tcp ")!=std::string::npos,"fallback metadata advertises TCP");
 dispatch_message(co,fallback.receiver->id,R"({"seen":["123456"]})",1000000);
 require(fallback.receiver->tracking.size()==1,"UDP fallback retains functional TCP ingress");
 hs["user"]="bad-rng";hooks.random_unit=[](){return 1.;};auto count=co.receivers().size();
 rejects([&]{handshake(co,hs.dump(),{},"test","source",hooks);},"invalid jitter RNG rejected");require(co.receivers().size()==count,"reply failure rolls back attached receiver");
 std::cout<<"PASS "<<checks<<" protocol checks; "<<fixture.at("handshakes").size()<<" handshakes, "<<index<<" dispatch cases\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
