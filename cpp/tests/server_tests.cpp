#include "mlat/server.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <csignal>
using namespace mlat;using nlohmann::json;
std::size_t checks=0;
void require(bool ok,const std::string& why){++checks;if(!ok)throw std::runtime_error(why);}
struct Directory {
 std::string path;
 Directory(){char pattern[]="/tmp/mlat-server-tests-XXXXXX";auto* p=mkdtemp(pattern);if(!p)throw std::runtime_error("temporary directory");path=p;}
 ~Directory(){for(auto name:{"sync.json","clients.json","aircraft.json","sync.json.tmp","clients.json.tmp","aircraft.json.tmp","cli.log","positions.csv","handshakes.log","handshakes.log.1","handshakes.log.2"})std::filesystem::remove(std::filesystem::path(path)/name);std::filesystem::remove(path);}
};
struct Client {
 int fd=-1;std::string user,mode,raw_buffer;bool negotiated=false,ended=false;std::size_t sent=0;
 wire::Encoder encoder;wire::Decoder decoder;std::vector<json> messages;
 Client(const Endpoint& target,std::string name,std::string compression):user(std::move(name)),mode(std::move(compression)){
  fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)throw std::runtime_error("client socket");
  sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(target.port);inet_pton(AF_INET,target.host.c_str(),&address.sin_addr);
  if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof address)<0)throw std::runtime_error("loopback connect");
  int yes=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof yes);
 }
 explicit Client(int descriptor):fd(descriptor),mode("none"){}
 ~Client(){if(fd>=0)::close(fd);}Client(const Client&)=delete;
 void close(){if(fd>=0){shutdown(fd,SHUT_RDWR);::close(fd);fd=-1;}}
 void bytes(std::span<const std::uint8_t> data){while(!data.empty()){auto n=send(fd,data.data(),data.size(),MSG_NOSIGNAL);if(n<=0)throw std::runtime_error("client send");data=data.subspan(static_cast<std::size_t>(n));}}
 void text(std::string_view value){bytes(std::span(reinterpret_cast<const std::uint8_t*>(value.data()),value.size()));}
 json hello(Vec3 llh={52,4,100},std::optional<std::string> uuid={}){json value={{"version",3},{"user",user},{"compress",json::array({mode})},{"clock_type","beast"},{"lat",llh[0]},{"lon",llh[1]},{"alt",llh[2]},{"return_results",true},{"return_result_format","ecef"},{"return_stats",true}};if(uuid)value["uuid"]=*uuid;return value;}
 void handshake(const json& value){text(value.dump()+'\n');}
 void message(const json& value){auto line=value.dump()+'\n';if(mode=="none")text(line);else{auto data=encoder.encode(line);bytes(data);}++sent;}
 void consume(std::span<const std::uint8_t> input){
  auto record=[&](std::string_view line){messages.push_back(json::parse(line));};
  if(negotiated&&mode=="zlib2"){decoder.feed(input,record);return;}
  raw_buffer.append(reinterpret_cast<const char*>(input.data()),input.size());
  while(true){auto end=raw_buffer.find('\n');if(end==std::string::npos)break;auto value=json::parse(raw_buffer.substr(0,end));raw_buffer.erase(0,end+1);messages.push_back(value);
   if(value.contains("compress")){negotiated=true;if(mode=="zlib2"){if(!raw_buffer.empty()){auto remainder=std::move(raw_buffer);raw_buffer.clear();decoder.feed(std::span(reinterpret_cast<const std::uint8_t*>(remainder.data()),remainder.size()),record);}break;}}
  }
 }
 void drain(){if(fd<0)return;std::array<std::uint8_t,65536> buffer{};while(true){auto n=recv(fd,buffer.data(),buffer.size(),MSG_DONTWAIT);if(n>0)consume(std::span(buffer).first(static_cast<std::size_t>(n)));else{if(n==0)ended=true;else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)throw std::runtime_error("client receive");break;}}}
};
struct SinkListener {
 int fd=-1;Endpoint address;
 explicit SinkListener(std::uint16_t port=0,bool accepting=true,int backlog=16){fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)throw std::runtime_error("sink socket");int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
  sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
  if(bind(fd,reinterpret_cast<sockaddr*>(&addr),sizeof addr)||(accepting&&listen(fd,backlog)))throw std::runtime_error("sink listen");
  socklen_t length=sizeof addr;if(getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&length))throw std::runtime_error("sink port");address={"127.0.0.1",ntohs(addr.sin_port)};
 }
 ~SinkListener(){close();}void close(){if(fd>=0)::close(fd);fd=-1;}
 std::unique_ptr<Client> accept(){int peer=accept4(fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);if(peer<0)throw std::runtime_error("sink accept");return std::make_unique<Client>(peer);}
};
struct Lab {
 Directory directory;ServerTime time{1000000,0,0};std::shared_ptr<wire::SessionConfig> config=std::make_shared<wire::SessionConfig>();std::unique_ptr<Server> server;
 explicit Lab(std::size_t clients=16,std::size_t socket_budget=1024*1024,bool sbs=false,std::size_t max_sbs=128,std::vector<SbsEndpoint> connectors={}){
  config->motd="offline test";config->source_url="https://example.invalid/source";config->public_sources={{"large",std::string(40000,'x')}};
  ServerOptions options;options.work_directory=directory.path;options.session=config;options.max_clients=clients;options.per_socket_queue=socket_budget;options.csv_files={directory.path+"/positions.csv"};
  if(sbs)options.basestation_listeners={{{"127.0.0.1",0},false},{{"127.0.0.1",0},true},{{"127.0.0.1",0},false,1}};
  options.max_sbs_clients=max_sbs;options.basestation_connectors=std::move(connectors);
  server=std::make_unique<Server>(options,[this](){return time;});
 }
 void tick(){server->tick(0);}
 template<typename F>void until(F predicate,std::span<Client*> clients={}){
  for(int i=0;i<2000;++i){if(i%32==31)server->tick(1);else tick();for(auto* c:clients)c->drain();if(predicate())return;}
  throw std::runtime_error("loopback condition not reached: messages="+std::to_string(server->stats().protocol_messages)+" accepted="+std::to_string(server->stats().accepted)+" closed="+std::to_string(server->stats().closed)+" at mono="+std::to_string(time.mono));
 }
 void settle(std::span<Client*> clients){for(int i=0;i<12;++i){tick();for(auto* c:clients)c->drain();}}
};
std::string drain_sbs(Client& c){std::string result;std::array<char,4096> data{};while(true){auto n=recv(c.fd,data.data(),data.size(),MSG_DONTWAIT);if(n>0)result.append(data.data(),static_cast<std::size_t>(n));else{if(n==0)c.ended=true;else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)throw std::runtime_error("SBS receive");break;}}return result;}
std::string address(std::uint32_t value){char data[7];snprintf(data,sizeof data,"%06x",value);return data;}
int main(int argc,char**argv){try{
 {Lab lab;lab.settle({});
  require(lab.server->next_deadline()==.9,"deadline includes initial empty MLAT cohort before clock cleanup");
  auto iterations=lab.server->stats().iterations;
  require(lab.server->next_deadline()==.9&&lab.server->stats().iterations==iterations,"deadline query does not tick server");
  Client client(lab.server->endpoints()[0],"virtual-deadline","zlib2");std::array<Client*,1> clients{&client};
  client.handshake(client.hello());lab.until([&](){return client.negotiated;},clients);
  lab.time={1000015,15,.01};lab.settle(clients);
  require(lab.server->next_deadline()==16.,"deadline includes deferred zlib2 flush");
  lab.time={1000016,16,.01};lab.settle(clients);
  require(lab.server->next_deadline()==25.,"flushed deadline advances to next clock cleanup");
  lab.server->stop();require(!lab.server->next_deadline(),"stopped server has no timer deadline");
 }
 {Lab lab(16,1024*1024,true);lab.settle({});lab.time={1000005,5,0};lab.tick();
  Client sink(lab.server->basestation_endpoints()[0].endpoint,"virtual-sbs","none");
  lab.until([&](){return lab.server->basestation_connections()==1;});
  lab.time={1000015,15,0};lab.settle({});lab.time={1000030,30,0};lab.settle({});
  require(lab.server->next_deadline()==35.,"deadline includes SBS heartbeat before maintenance");
  lab.time={1000035,35,0};lab.settle({});require(drain_sbs(sink)=="\n","SBS deadline executes original heartbeat");
 }
 for(double interval:{15.,60.,-1.,0.}){
  Directory directory;ServerTime now{1000000,0,0};std::vector<std::string> logs;
  ServerOptions options;options.work_directory=directory.path;options.session=std::make_shared<wire::SessionConfig>();
  options.status_interval=interval;options.status_logger=[&](const std::string& title){logs.push_back(title);};
  Server server(options,[&](){return now;});require(logs.empty(),"status logging waits beyond initial timestamp");
  for(int step=1;step<=8;++step){now={1000000.+step*15.,step*15.,0};server.tick(0);}
  require(logs.size()==(interval<0?0U:interval==60?2U:8U),"status logging uses Python interval and 15s cadence");
  for(const auto& line:logs)require(line.starts_with("Status: (0 clients"),"aggregate status title logged");
  require(std::filesystem::exists(directory.path+"/aircraft.json"),"disabled logging preserves status writes");
 }
 require(argc==3,"coordinator fixture and executable required");
 {Lab lab;auto endpoint=lab.server->endpoints().at(0);Client a(endpoint,"raw","none"),b(endpoint,"zlib","zlib"),c(endpoint,"zlib2","zlib2");std::array<Client*,3> clients{&a,&b,&c};
  auto hello=a.hello().dump()+'\n';a.text(hello.substr(0,5));b.handshake(b.hello());c.handshake(c.hello());lab.settle(clients);require(!a.negotiated&&b.negotiated&&c.negotiated,"fragmented/mixed handshakes");a.text(hello.substr(5));lab.until([&](){return a.negotiated;},clients);
  for(auto* client:clients)client->message({{"seen",json::array({"40621d"})}});
  lab.settle(clients);lab.time={1000002,2,.01};lab.settle(clients);
  bool traffic=false;for(auto* client:clients)for(const auto& m:client->messages)traffic|=m.contains("start_sending");require(traffic,"coordinator interest reaches TCP clients");
  require(lab.server->coordinator().receivers().size()==3,"three real TCP feeders");require(lab.server->stats().read_bytes>0&&lab.server->stats().written_bytes>0,"socket byte counters");
  require(std::filesystem::exists(lab.directory.path+"/clients.json"),"status publication wired");
  lab.time={1000150,150,.02};lab.settle(clients);require(lab.server->connections()==3,"exact150s idle survives TCP heartbeat");lab.time={1000180,180,.03};lab.settle(clients);require(lab.server->connections()==0&&lab.server->coordinator().receivers().empty(),"idle TCP cleanup");
  require(lab.server->stats().queued_bytes==0,"socket queues accounted after close");}
 {Lab lab;auto endpoint=lab.server->endpoints()[0];Client first(endpoint,"same","none");std::array<Client*,1> a{&first};first.handshake(first.hello({52,4,100},"synthetic-uuid"));lab.until([&](){return first.negotiated;},a);
  Client second(endpoint,"same","none");std::array<Client*,2> both{&first,&second};second.handshake(second.hello({52,4,100},"synthetic-uuid"));lab.until([&](){return second.negotiated&&first.ended;},both);require(lab.server->connections()==1&&lab.server->coordinator().receivers().size()==1,"UUID replacement closes old socket");second.close();lab.until([&](){return lab.server->connections()==0;});}
 {Lab lab(1);auto endpoint=lab.server->endpoints()[0];Client first(endpoint,"first","none");lab.until([&](){return lab.server->connections()==1;});Client excess(endpoint,"excess","none");lab.until([&](){return lab.server->stats().refused==1;});require(lab.server->connections()==1,"connection count bound");}
 {Lab lab(8,32768);Client client(lab.server->endpoints()[0],"queue","none");std::array<Client*,1> clients{&client};client.handshake(client.hello());lab.until([&](){return client.negotiated;},clients);client.message({{"quine","large"}});lab.until([&](){return lab.server->connections()==0;},clients);require(lab.server->stats().backpressure_closes==1&&lab.server->stats().queued_bytes==0,"actual transport output bound closes without leak");}
 {Lab lab;Client client(lab.server->endpoints()[0],"deny","none");std::array<Client*,1> clients{&client};client.handshake({{"version",1}});lab.until([&](){return client.ended;},clients);require(client.messages.size()==1&&client.messages[0].contains("deny"),"denial drains before socket close");}
 {Lab lab;Client client(lab.server->endpoints()[0],"eof","none");std::array<Client*,1> clients{&client};client.handshake(client.hello());lab.until([&](){return client.negotiated;},clients);client.text("{\"heartbeat\":{}}");shutdown(client.fd,SHUT_WR);lab.until([&](){return lab.server->connections()==0;},clients);require(lab.server->stats().protocol_messages==1,"EOF final raw message included in cumulative counter");}
 // Source heartbeat task sleeps on monotonic time, rechecks wall last_output,
 // and drains/discards inbound bytes rather than parsing them as feeder JSON.
 {Lab lab(1,1024*1024,true,1);Client sbs(lab.server->basestation_endpoints()[0].endpoint,"","none");lab.until([&](){return lab.server->basestation_connections()==1;});
  Client excess(lab.server->basestation_endpoints()[0].endpoint,"","none");lab.until([&](){return lab.server->stats().refused==1;});
  Client feeder(lab.server->endpoints()[0],"independent-cap","none");lab.until([&](){return lab.server->connections()==1;});require(lab.server->basestation_connections()==1,"SBS cap separate from feeder cap");
  sbs.text("untrusted non-JSON input\n");for(int i=0;i<10;++i)lab.tick();require(lab.server->stats().protocol_messages==0&&lab.server->stats().protocol_errors==0,"SBS inbound bytes discarded");
  lab.time={1000029.999,29.999,0};for(int i=0;i<5;++i)lab.tick();require(drain_sbs(sbs).empty(),"SBS no premature heartbeat");
  lab.time={1000030,30,0};for(int i=0;i<5;++i)lab.tick();require(drain_sbs(sbs)=="\n","SBS exact30s heartbeat");
  lab.time={1000020,60,0};for(int i=0;i<5;++i)lab.tick();require(drain_sbs(sbs).empty(),"backward wall jump delays heartbeat");
  lab.time={1000059.95,100,0};for(int i=0;i<5;++i)lab.tick();require(drain_sbs(sbs)=="\n","source0.1s heartbeat tolerance");
  sbs.close();lab.until([&](){return lab.server->basestation_connections()==0;});require(lab.server->stats().queued_bytes==0,"SBS EOF releases queues");
 }
 // Failed connection and EOF both retry30s after failure, never tight-loop.
 {SinkListener reserved(0,false);auto endpoint=reserved.address;Lab lab(16,1024*1024,false,128,{{endpoint,false}});
  lab.until([&](){return lab.server->stats().connector_failures==1;});auto attempts=lab.server->stats().connector_attempts;require(attempts==1,"initial refused numeric connector attempt");
  reserved.close();SinkListener sink(endpoint.port);lab.time={1000029.999,29.999,0};for(int i=0;i<10;++i)lab.tick();require(lab.server->stats().connector_attempts==attempts,"no early connector retry");
  lab.time={1000030,30,0};lab.until([&](){return lab.server->basestation_connections()==1;});auto peer=sink.accept();require(lab.server->stats().connector_attempts==2,"connector retries at30s");
  lab.time={1000060,60,0};for(int i=0;i<10;++i)lab.tick();require(drain_sbs(*peer)=="\n","outgoing SBS heartbeat");peer->close();lab.until([&](){return lab.server->basestation_connections()==0;});
  lab.time={1000089.999,89.999,0};for(int i=0;i<10;++i)lab.tick();require(lab.server->stats().connector_attempts==2,"EOF reconnect delay preserved");
  lab.time={1000090,90,0};lab.until([&](){return lab.server->basestation_connections()==1;});auto replacement=sink.accept();require(lab.server->stats().connector_attempts==3,"EOF reconnect succeeds");
  lab.server->stop();require(lab.server->basestation_connections()==0&&lab.server->stats().queued_bytes==0,"connector shutdown releases queues");
 }
 // Saturate a real loopback accept queue to leave a nonblocking connect in
 // SYN-SENT without external hosts/firewall edits. Virtual time tests the cap.
 {SinkListener sink(0,true,1);Client first(sink.address,"","none"),second(sink.address,"","none");Lab lab(16,1024*1024,false,128,{{sink.address,false}});
  lab.until([&](){return lab.server->stats().connector_attempts==1;});for(int i=0;i<10;++i)lab.tick();require(lab.server->basestation_connections()==0,"saturated backlog leaves connector pending");
  lab.time={1000029.999,29.999,0};for(int i=0;i<10;++i)lab.tick();require(lab.server->stats().connector_failures==0,"pending connect survives before30s timeout");
  lab.time={1000030,30,0};lab.until([&](){return lab.server->stats().connector_failures==1;});require(lab.server->basestation_connections()==0,"pending connector times out at30s");
  lab.time={1000059.999,59.999,0};for(int i=0;i<10;++i)lab.tick();require(lab.server->stats().connector_attempts==1,"timeout followed by independent30s retry delay");
 }
 {SinkListener sink;auto endpoint=sink.address;endpoint.host="localhost";Lab lab(16,1024*1024,false,128,{{endpoint,true}});lab.until([&](){return lab.server->basestation_connections()==1;});auto peer=sink.accept();require(lab.server->stats().connector_attempts>=1,"hostname resolves off event loop, address fallback supported");}
 // End-to-end acquired-clock trajectory through real TCP, zlib2, coordinator,
 // solver, UKF and result fanout. No direct injection into internal model state.
 std::size_t wire_results=0;
 {SinkListener destination;Lab lab(16,1024*1024,true,128,{{destination.address,true}});auto endpoints=lab.server->basestation_endpoints();Client unfiltered(endpoints[0].endpoint,"","none"),filtered(endpoints[1].endpoint,"","none"),bounded(endpoints[2].endpoint,"","none");lab.until([&](){return lab.server->basestation_connections()==4;});auto connector=destination.accept();
  std::ifstream input(argv[1]);json oracle;input>>oracle;std::map<int,std::unique_ptr<Client>> clients;std::map<double,Vec3> expected;
  std::map<double,std::set<int>> fanout;std::set<std::pair<double,int>> expected_routes,delivered;
  for(const auto& row:oracle.at("expected"))for(const auto& event:row.at("events")){
   if(event.at("kind")=="output")expected[python_round(event.at("time"),3)]=event.at("position").get<Vec3>();
   else if(event.at("kind")=="client_result"){double time=python_round(event.at("time"),3);fanout[time]=event.at("receivers").get<std::set<int>>();expected_routes.emplace(time,event.at("id").get<int>());}
  }
  for(const auto& step:oracle.at("steps")){
   if(step.at("mono").get<double>()>=50)break;
   lab.time={step.at("wall"),step.at("mono"),step.at("cpu")};auto op=step.at("op").get<std::string>();
   if(op=="connect"){const auto& r=step.at("receiver");auto c=std::make_unique<Client>(lab.server->endpoints()[0],r.at("user"),"zlib2");c->handshake(c->hello(r.at("llh").get<Vec3>()));clients.emplace(r.at("id"),std::move(c));}
   else if(op=="seen"){json list=json::array();for(auto a:step.at("addresses"))list.push_back(address(a));clients.at(step.at("id"))->message({{"seen",list}});}
   else if(op=="rates"){json report=json::object();for(const auto& pair:step.at("report"))report[address(pair[0])]=pair[1];clients.at(step.at("id"))->message({{"rate_report",report}});}
   else if(op=="sync")clients.at(step.at("id"))->message({{"sync",{{"et",step.at("even_time")},{"ot",step.at("odd_time")},{"em",step.at("even")},{"om",step.at("odd")}}}});
   std::vector<Client*> peers;for(auto& [id,c]:clients){(void)id;peers.push_back(c.get());}
   lab.until([&](){std::size_t sent=0;for(auto* client:peers){sent+=client->sent;if(!client->negotiated)return false;bool found=false;for(const auto& [id,r]:lab.server->coordinator().receivers()){(void)id;if(r->user==client->user)found=true;}if(!found)return false;}return lab.server->stats().protocol_messages==sent;},peers);lab.settle(peers);
  }
  lab.time={1000051,51,.51};std::vector<Client*> peers;for(auto& [id,c]:clients){(void)id;peers.push_back(c.get());}lab.settle(peers);
  lab.time={1000053,53,.53};lab.settle(peers);std::set<double> times;
  for(const auto& [id,client]:clients)for(const auto& value:client->messages)if(value.contains("result")){
   const auto& result=value.at("result");double time=result.at("@");require(expected.contains(time),"wire solution timestamp matches Python oracle");auto position=result.at("ecef").get<Vec3>();require(ecef_distance(position,expected.at(time))<=3,"wire solution position within rounding tolerance");require(result.at("n")==fanout.at(time).size(),"receiver-count schema matches Python fanout");require(expected_routes.contains({time,id})&&delivered.emplace(time,id).second,"exact Python recipient, no duplicate result");times.insert(time);++wire_results;
  }
  require(lab.server->stats().outputs==expected.size(),"full TCP solution count matches Python oracle");require(times.size()==expected.size(),"all unique solutions delivered");require(wire_results==expected_routes.size()&&delivered==expected_routes,"exact Python receiver fanout for every solution");
  require(lab.server->stats().io_errors==0&&lab.server->stats().protocol_errors==0&&lab.server->stats().backpressure_closes==1&&lab.server->coordinator().errors()==0,"only injected SBS queue failure; no feeder drops/errors");
  std::ifstream csv(lab.directory.path+"/positions.csv");std::string csvline;std::set<double> csvtimes;std::size_t csvrows=0;
  while(std::getline(csv,csvline)){++csvrows;double time=std::stod(csvline.substr(0,csvline.find(',')));require(expected.contains(time),"CSV sink receives actual TCP solved timestamp");csvtimes.insert(time);}
  require(csvrows==expected.size()&&csvtimes.size()==expected.size()&&lab.server->stats().output_errors==1,"CSV writes every solution despite injected SBS sink failure");
  require(drain_sbs(bounded).empty()&&bounded.ended,"SBS queue overflow closes only failed sink");
  require(lab.server->basestation_connections()==3&&lab.server->stats().sbs_records==expected.size()*3,"healthy SBS listeners and connector receive every solution");
  std::map<std::string,Vec3> expected_llh;for(const auto& [time,position]:expected)expected_llh[output_date(time)+" "+output_time(time)]=ecef_to_llh(position);
  for(auto* sink:{&unfiltered,&filtered,connector.get()}){
   std::istringstream stream(drain_sbs(*sink));std::string line;std::set<std::string> stamps;std::size_t count=0;
   while(std::getline(stream,line)){if(line.empty())continue;++count;std::vector<std::string> fields;std::istringstream row(line);std::string field;while(std::getline(row,field,','))fields.push_back(field);
    require(fields.size()>=20&&fields[0]=="MSG"&&fields[1]=="3","actual SBS wire schema");auto stamp=fields[6]+" "+fields[7];require(expected_llh.contains(stamp)&&stamps.insert(stamp).second,"SBS exact Python solution timestamps/no duplicate");
    const auto& llh=expected_llh.at(stamp);require(std::abs(std::stod(fields[14])-llh[0])<0.000002&&std::abs(std::stod(fields[15])-llh[1])<0.000002,"SBS wire raw position matches Python oracle");
   }
   require(count==expected.size(),"all solutions reach each SBS subscriber");
  }
  lab.server->stop();require(lab.server->connections()==0&&lab.server->stats().queued_bytes==0,"server shutdown releases connections and queues");
 }
 // Execute the actual CLI with real clocks and bounded runtime in this container.
 {Directory directory;SinkListener sink;auto connector_address="127.0.0.1:"+std::to_string(sink.address.port);auto pid=fork();if(pid<0)throw std::runtime_error("CLI test fork");if(pid==0){int fd=open((directory.path+"/cli.log").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)_exit(90);dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);::close(fd);execl(argv[2],argv[2],"--client-listen","127.0.0.1:0","--work-dir",directory.path.c_str(),"--source-url","https://example.invalid/source","--run-seconds","0.2","--write-csv",(directory.path+"/positions.csv").c_str(),"--basestation-listen","127.0.0.1:0","--filtered-basestation-listen","127.0.0.1:0","--basestation-connect",connector_address.c_str(),static_cast<char*>(nullptr));_exit(91);}
  int status=0;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"actual CLI run exits cleanly");std::ifstream log(directory.path+"/cli.log");std::string first,last;std::getline(log,first);std::getline(log,last);require(json::parse(first).at("ready")==true&&json::parse(last).at("stopped")==true,"CLI readiness and final summary");require(std::filesystem::exists(directory.path+"/aircraft.json"),"CLI status files produced");require(std::filesystem::exists(directory.path+"/positions.csv"),"CLI CSV sink opened");require(json::parse(first).at("basestation_listeners").size()==2,"CLI both SBS listener modes bound");require(json::parse(last).at("connector_attempts").get<int>()>=1,"CLI connector flag executed");}
 // Execute both option spellings, defaults and malformed values. Readiness
 // reports the options passed to the actual Coordinator, not a second parser.
 for(const auto& entry:std::vector<std::pair<std::vector<std::string>,bool>>{
  {{},true},{{"--min-tracking","2","--mark-adsb-time=false"},true},
  {{"--min-tracking=4","--mark-adsb-time","true"},true},
  {{"--min-tracking","0"},false},{{"--min-tracking=-1"},false},
  {{"--min-tracking","999999999"},false},{{"--mark-adsb-time=not-a-bool"},false},
  {{"--mark-adsb-time="},false},{{"--min-tracking"},false}}){
  Directory directory;auto pid=fork();if(pid<0)throw std::runtime_error("option test fork");
  if(pid==0){int fd=open((directory.path+"/cli.log").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)_exit(90);dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);::close(fd);
   std::vector<std::string> args{argv[2],"--client-listen","127.0.0.1:0","--work-dir",directory.path,"--source-url","https://example.invalid/source","--run-seconds","0.001"};
   args.insert(args.end(),entry.first.begin(),entry.first.end());std::vector<char*> pointers;for(auto& arg:args)pointers.push_back(arg.data());pointers.push_back(nullptr);execv(argv[2],pointers.data());_exit(91);
  }
  int status=0;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==(entry.second?0:2),"CLI tracking flags exit status");
  std::ifstream log(directory.path+"/cli.log");std::string line;std::getline(log,line);
  if(entry.second){auto ready=json::parse(line);auto expected=entry.first.empty()?3:entry.first[0]=="--min-tracking"?2:4;
   require(ready.at("min_tracking")==expected,"CLI minimum tracking reaches server");require(ready.at("mark_adsb_time")== (expected!=2),"CLI ADS-B marking reaches server");
  }else require(line.find("not-a-bool")==std::string::npos&&line.find("https:")==std::string::npos,"invalid CLI arguments not echoed");
 }
 for(const auto& entry:std::vector<std::pair<std::vector<std::string>,bool>>{
  {{"--status-interval","60","--tag","synthetic-mlat"},true},
  {{"--status-interval=-1","--tag=synthetic"},true},
  {{"--private-unknown","DO_NOT_LOG_SECRET","--status-interval","60"},true},
  {{"--private-unknown=DO_NOT_LOG_SECRET","--unused-switch","--status-interval=60"},true},
  {{"--status-interval","nan"},false},{{"--status-interval=inf"},false},
  {{"--status-interval"},false},{{"--tag"},false}}){
  Directory directory;auto pid=fork();if(pid<0)throw std::runtime_error("compatibility option fork");
  if(pid==0){int fd=open((directory.path+"/cli.log").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)_exit(90);dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);::close(fd);
   std::vector<std::string> args{argv[2],"--client-listen","127.0.0.1:0","--work-dir",directory.path,"--source-url","https://example.invalid/source","--run-seconds","0.001"};
   args.insert(args.end(),entry.first.begin(),entry.first.end());std::vector<char*> pointers;for(auto& arg:args)pointers.push_back(arg.data());pointers.push_back(nullptr);execv(argv[2],pointers.data());_exit(91);
  }
  int status=0;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==(entry.second?0:2),"CLI compatibility exit status");
  std::ifstream log(directory.path+"/cli.log");std::string line;bool ready=false,warned=false;
  while(std::getline(log,line)){
   require(line.find("DO_NOT_LOG_SECRET")==std::string::npos&&line.find("private-unknown")==std::string::npos,"unknown option names and values never logged");
   if(line.starts_with("Warning:"))warned=true;
   if(line.starts_with('{')){auto row=json::parse(line);if(row.value("ready",false)){
    ready=true;require(row.at("status_interval")== (entry.first[0]=="--status-interval=-1"?-1:60),"status interval reaches actual CLI server");
   }}
  }
  require(ready==entry.second,"compatibility startup result");
  if(entry.first[0].starts_with("--private-unknown"))require(warned,"unknown options produce fixed warning");
 }
 // Real-process maintenance signal: keep an established TCP session, accept
 // another afterwards, then still honor normal SIGTERM shutdown. Bounded child
 // lifetime and failure cleanup prevent orphaning a test server.
 {Directory directory;auto pid=fork();if(pid<0)throw std::runtime_error("signal test fork");
  if(pid==0){int fd=open((directory.path+"/cli.log").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)_exit(90);dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);::close(fd);
   execl(argv[2],argv[2],"--client-listen","127.0.0.1:0","--work-dir",directory.path.c_str(),"--source-url","https://example.invalid/source","--run-seconds","5",static_cast<char*>(nullptr));_exit(91);}
  struct Reap {pid_t pid;~Reap(){if(pid>0){int s=0;if(waitpid(pid,&s,WNOHANG)==0){kill(pid,SIGKILL);waitpid(pid,&s,0);}}}} cleanup{pid};
  json ready;for(int i=0;i<200;++i){std::ifstream log(directory.path+"/cli.log");std::string line;if(std::getline(log,line)&&!line.empty()){ready=json::parse(line);break;}usleep(10000);}
  require(ready.value("ready",false),"signal test readiness");
  Endpoint endpoint{ready.at("listeners")[0].at("host"),ready.at("listeners")[0].at("port")};
  Client first(endpoint,"signal-before","none");first.text(first.hello().dump()+"\n");
  for(int i=0;i<100&&!first.negotiated;++i){first.drain();usleep(10000);}
  require(first.negotiated&&!first.ended,"pre-HUP session negotiated");
  require(kill(pid,SIGHUP)==0,"deliver HUP to actual CLI");usleep(20000);
  first.text("{\"heartbeat\":{}}\n");first.drain();
  Client second(endpoint,"signal-after","none");second.text(second.hello().dump()+"\n");
  for(int i=0;i<100&&!second.negotiated;++i){first.drain();second.drain();usleep(10000);}
  require(second.negotiated&&!first.ended,"HUP preserves connection and listener");
  require(kill(pid,SIGTERM)==0,"deliver TERM after HUP");int status=0;
  require(waitpid(pid,&status,0)==pid,"reap signal test child");cleanup.pid=-1;
  require(WIFEXITED(status)&&WEXITSTATUS(status)==0,"TERM remains graceful after HUP");
  std::ifstream log(directory.path+"/cli.log");std::string line;json last;while(std::getline(log,line))if(line.starts_with('{'))last=json::parse(line);
  require(last.value("stopped",false)&&last.at("accepted")==2&&last.at("closed")==2,"HUP never replaced or reset existing server");
  std::ifstream handshakes(directory.path+"/handshakes.log");std::size_t count=0;
  while(std::getline(handshakes,line)){++count;require(line=="{\"event\":\"handshake\",\"redacted\":true}","handshake log contains only fixed privacy-safe event");}
  require(count==2,"actual CLI logs each handshake exactly once");
 }
 std::cout<<"PASS "<<checks<<" real-loopback checks; "<<wire_results<<" wire results through six TCP feeders\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
