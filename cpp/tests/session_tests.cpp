#include "mlat/session.hpp"
#include <fstream>
#include <iostream>
using namespace mlat;using namespace mlat::wire;using nlohmann::json;
std::size_t checks=0;
void require(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
void compare(const json& a,const json& e,const std::string& path){
 if(e.is_object()){require(a.size()==e.size(),path+" fields");for(auto it=e.begin();it!=e.end();++it)compare(a.at(it.key()),it.value(),path+"."+it.key());}
 else if(e.is_array()){require(a.size()==e.size(),path+" length");for(std::size_t i=0;i<e.size();++i)compare(a[i],e[i],path+"["+std::to_string(i)+"]");}
 else require(a==e,path+" expected="+e.dump()+" actual="+a.dump());
}
struct Harness {
 SeededInterestRandom random{1};Coordinator co{random,{},1000000,0};std::shared_ptr<SessionConfig> config=std::make_shared<SessionConfig>();
 std::string compression;bool negotiated=false,fail_write=false;int closes=0;double wall=1000000,mono=0;json events=json::array();Decoder decoder;Encoder encoder;
 std::unique_ptr<Session> session;
 explicit Harness(std::string mode,bool proxy=false,std::size_t queue=262144):compression(std::move(mode)){
  co.start();config->motd="Synthetic test";config->source_url="https://example.invalid/source";config->trusted_proxy=proxy;config->max_queued_bytes=queue;
  config->public_sources={{"synthetic",std::string(40000,'a')}};
  SessionHooks hooks;hooks.handshake.random_integer=[](int low,int){return low+3;};hooks.handshake.random_unit=[](){return .37;};
  hooks.handshake.close_existing=[&](auto id){co.disconnect(id);};hooks.closed=[&](){++closes;};
  hooks.write=[&](std::span<const std::uint8_t> bytes){
   if(fail_write)throw std::runtime_error("synthetic sink failure");
   auto capture=[&](std::string_view line){auto value=json::parse(line);for(auto key:{"start_sending","stop_sending"})if(value.contains(key))std::sort(value[key].begin(),value[key].end());events.push_back(std::move(value));};
   if(negotiated&&compression=="zlib2")decoder.feed(bytes,capture);
   else {std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());std::size_t start=0;while(start<text.size()){auto end=text.find('\n',start);require(end!=text.npos,"raw newline framing");capture(text.substr(start,end-start));start=end+1;}}
  };
  ReceiverInfo peer;peer.source_ip="192.0.2.1";peer.source_port=1234;session=std::make_unique<Session>(co,peer,config,std::move(hooks),wall,mono);
 }
 void time(double m,std::optional<double> w={}){mono=m;wall=w.value_or(1000000+m);co.set_time(wall,mono,0);session->set_time(wall,mono);}
 void bytes(std::span<const std::uint8_t> data){session->receive(data,wall,mono);}
 void raw(std::string_view text){bytes(std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));}
 std::string hello(bool stats=true,std::string format="old",bool results=true){return json{{"version",3},{"user","synthetic"},{"compress",json::array({compression})},{"lat",52.25},{"lon",3.9},{"alt",100},{"return_stats",stats},{"return_results",results},{"return_result_format",format}}.dump()+'\n';}
 void accept(bool stats=true,std::string format="old",bool results=true){raw(hello(stats,format,results));require(session->active(),"handshake active");require(events.size()==1&&events[0].contains("compress"),"handshake always raw");events.clear();negotiated=true;}
 void message(const json& value){auto text=value.dump()+'\n';if(compression=="none")raw(text);else{auto data=encoder.encode(text);bytes(data);}}
 void pump(){session->advance(wall,mono);}
};
int main(int argc,char**argv){try{
 if(argc!=2)throw std::runtime_error("fixture required");std::ifstream file(argv[1]);json fixture;file>>fixture;std::size_t steps=0;
 for(const auto& c:fixture.at("cases")){
  Harness h(c.at("compression"));h.accept(c.at("return_stats"));
  for(std::size_t i=0;i<c.at("steps").size();++i){const auto& s=c.at("steps")[i];h.events.clear();h.time(s.value("mono",h.mono),s.contains("wall")?std::optional(s.at("wall").get<double>()):std::nullopt);
   auto op=s.at("op").get<std::string>();
   if(op=="request")h.session->request_traffic(s.at("addresses").get<std::set<std::uint32_t>>());
   else if(op=="stats")h.session->send_stats(s.at("value"));
   else if(op=="read")h.message({{"heartbeat",json::object()}});
   else if(op=="pump")h.pump();
   auto deadline=h.session->next_deadline();json actual={{"events",h.events},{"closed",h.session->closed()},{"disconnects",h.closes},{"requested",h.session->requested()},
     {"deadline",deadline?json(*deadline):json(nullptr)},{"last_read",h.session->last_read()}};
   compare(actual,c.at("expected")[i],"session "+c.at("compression").get<std::string>()+" step "+std::to_string(i));require(h.session->failures()==0,"no oracle session failures");++steps;
  }
 }
 {Harness h("none");h.time(14.999);h.pump();require(!h.session->closed(),"handshake before deadline");h.time(15);h.pump();require(h.session->closed()&&h.closes==1,"handshake deadline");}
 {Harness h("none",true);h.time(14);h.raw("PROXY TCP4 192.0.2.88 192.0.2.99 2345 31090\r\n");require(h.session->next_deadline()==29,"proxy second deadline");h.time(28.9);h.accept();require(h.session->receiver()->source_ip=="192.0.2.88"&&h.session->receiver()->source_port==2345,"trusted proxy metadata");}
 {Harness h("none");h.raw("PROXY TCP4 192.0.2.88 192.0.2.99 2345 31090\n");require(h.session->closed()&&h.co.receivers().empty(),"untrusted proxy refused");}
 {Harness h("none",true);h.raw("PROXY TCP4 999.1.1.1 192.0.2.99 2345 31090\n");require(h.session->closed(),"bad proxy address refused");}
 {Harness h("zlib2");auto hello=h.hello();auto data=h.encoder.encode("{\"heartbeat\":{}}\n");std::vector<std::uint8_t> joined(hello.begin(),hello.end());joined.insert(joined.end(),data.begin(),data.end());h.bytes(joined);require(h.session->active()&&h.session->receiver()->message_counter==1,"handshake/compressed remainder same read");}
 {Harness h("zlib");h.accept();auto data=h.encoder.encode("{\"heartbeat\":{}}\n");h.time(149);h.bytes(std::span(data).first(2));require(h.session->last_read()==1000000,"partial frame is not activity");h.time(150);h.pump();require(h.session->active(),"150s is not timeout");h.bytes(std::span(data).subspan(2));require(h.session->last_read()==1000150,"complete frame activity");h.time(180);h.pump();require(h.session->active(),"completed frame kept peer alive");}
 {Harness h("none");h.accept();h.time(149);h.raw("{");require(h.session->last_read()==1000000,"partial plain line is not activity");h.time(180);h.pump();require(h.session->closed(),"partial line cannot evade heartbeat timeout");}
 {Harness h("none");h.accept();auto r=h.session->receiver();h.raw("{\"seen\":[\"123456\"]}");h.session->eof();require(r->message_counter==1&&r->dead&&h.co.receivers().empty(),"EOF consumes final raw line then detaches");h.session->close();require(h.closes==1,"close idempotence");}
 {Harness h("zlib2");h.accept();std::array<std::uint8_t,1> byte{0};h.bytes(byte);h.session->eof();require(h.session->closed()&&h.session->failures()==1,"compressed truncation at EOF");}
 {Harness h("none");h.fail_write=true;h.raw(h.hello());require(h.session->closed()&&h.co.receivers().empty(),"failed handshake write detaches receiver");}
 {Harness h("none");h.accept();h.fail_write=true;bool threw=false;try{h.session->send_stats({{"x",1}});}catch(...){threw=true;}require(threw&&h.session->closed()&&h.co.receivers().empty(),"output failure closes and propagates to coordinator");}
 {Harness h("zlib2",false,32768);h.accept();bool threw=false;try{h.session->send_stats({{"x",std::string(40000,'x')}});}catch(...){threw=true;}require(threw&&h.session->closed()&&h.session->queued_bytes()==0,"output queue bounded and cleared");}
 {Harness h("zlib2");h.accept();h.message({{"quine","synthetic"}});require(h.events.empty(),"quine delayed flush");h.time(1);h.pump();require(h.events.size()==1&&h.events[0]["quine"][1].get<std::string>().size()==40000,"large bounded public-source line");require(h.decoder.completed_frames()==1,"completed outbound frame counter");}
 {Harness h("zlib2");h.accept();std::set<std::uint32_t> wanted;for(std::uint32_t i=0;i<5000;++i)wanted.insert(i);h.session->request_traffic(wanted);h.pump();h.time(1);h.pump();std::size_t count=0;for(const auto& e:h.events)count+=e.at("start_sending").size();require(count==5000&&h.session->requested()==wanted&&h.session->failures()==0,"large traffic list message-boundary splitting");}
 {Harness h("zlib2");h.accept();h.time(29);h.session->send_stats({{"before",1}});h.time(30);h.pump();require(h.events.size()==2&&h.events[0].contains("stats")&&h.events[1].contains("heartbeat"),"equal-deadline heartbeat precedes flush by registration order");}
 for(auto format:{"old","ecef"}){Harness h("none");h.accept(true,format);MlatResult r{};AircraftModel ac;r.time=1000000;r.address=0x123456;r.solution.position=llh_to_ecef({52,4,1000});r.all_receivers={h.session->receiver()};r.distinct=1;r.dof=1;ResultCache cache;
  h.session->send_result(r,ac,&cache);h.session->send_result(r,ac,&cache);require(h.events.size()==2&&h.events[0]==h.events[1],"reused result cache");require(std::string(format)=="old"?bool(cache.old):bool(cache.ecef),"correct result format cache");}
 {Harness h("none");h.accept(false,"old",false);h.session->send_stats({{"x",1}});MlatResult r{};AircraftModel ac;h.session->send_result(r,ac);require(h.events.empty(),"disabled result/stats flags");}
 std::cout<<"PASS "<<checks<<" session checks; "<<steps<<" oracle steps plus transport-boundary/fault cases\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
