// SPDX-License-Identifier: AGPL-3.0-or-later
// Linux event loop for the pinned-source-compatible coordinator and sessions.
#include "mlat/server.hpp"
#include "mlat/csv_sink.hpp"
#include "mlat/resolver.hpp"
#include <list>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
namespace mlat {
namespace {
struct Fd {
 int value=-1;Fd()=default;explicit Fd(int v):value(v){}~Fd(){if(value>=0)::close(value);}
 Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
 Fd(Fd&& other)noexcept:value(std::exchange(other.value,-1)){}
 Fd& operator=(Fd&& other)noexcept{if(this!=&other){if(value>=0)::close(value);value=std::exchange(other.value,-1);}return *this;}
};
constexpr std::uint64_t listener_bit=1ULL<<63,resolver_token=~0ULL;
Endpoint endpoint(const sockaddr* address,socklen_t length){
 std::array<char,NI_MAXHOST> host{};std::array<char,NI_MAXSERV> service{};
 if(getnameinfo(address,length,host.data(),host.size(),service.data(),service.size(),NI_NUMERICHOST|NI_NUMERICSERV))throw std::runtime_error("numeric endpoint lookup failed");
 return {host.data(),static_cast<std::uint16_t>(std::stoi(service.data()))};
}
}
struct Server::Impl {
 struct Listener {Fd fd;Endpoint address;bool sbs=false,filtered=false;std::size_t queue_limit=0;};
 struct Connection {
  Fd fd;std::uint64_t token;std::unique_ptr<wire::Session> session;
  std::deque<std::vector<std::uint8_t>> writes;std::size_t offset=0,queued=0;
  std::optional<clock::StationId> receiver;bool closing=false;double close_due=0;
  bool sbs=false,filtered=false;std::size_t queue_limit=0;double last_output=0,heartbeat_due=0;
  std::optional<std::size_t> connector;bool connecting=false;double connect_due=0;
  Connection(int descriptor,std::uint64_t identity):fd(descriptor),token(identity){}
 };
 struct Connector {
  SbsEndpoint options;enum class Phase {idle,resolving,connecting,connected,trying} phase=Phase::idle;
  double retry_due=0;std::uint64_t query=0,peer=0;std::vector<ResolvedAddress> addresses;std::size_t next=0;
 };
 std::vector<Connector> connectors;std::unique_ptr<AsyncResolver> resolver;std::uint64_t query_sequence=0;
 ServerOptions options;SeededInterestRandom random;std::mt19937_64 handshake_random;Clock clock;
 ServerTime now;Fd epoll;std::vector<Listener> listeners;std::unique_ptr<Coordinator> co;
 std::unordered_map<std::uint64_t,std::unique_ptr<Connection>> clients;
 std::map<clock::StationId,std::uint64_t> receiver_tokens;
 std::vector<std::unique_ptr<CsvSink>> csv_sinks;
 std::unique_ptr<CsvSink> handshake_log;
 std::list<std::uint64_t> sbs_clients;
 wire::ResultCache result_cache;ServerStats stats;std::uint64_t token=0;bool stopped=false;
 double next_status_log=0;
 explicit Impl(ServerOptions o,Clock time):options(std::move(o)),random(options.random_seed),handshake_random(options.random_seed^0xd36af25cULL),
  clock(time?std::move(time):Server::system_time),now(clock()),epoll(epoll_create1(EPOLL_CLOEXEC)){
  if(epoll.value<0)throw std::runtime_error("epoll creation failed");
  if(!std::isfinite(options.status_interval))throw std::invalid_argument("invalid status interval");
  next_status_log=options.status_interval<0?std::numeric_limits<double>::infinity():now.wall+options.status_interval*.95;
  if(!options.session||options.listeners.empty()||options.listeners.size()>16||options.max_clients==0||options.max_clients>4096||
     options.per_socket_queue<32768||options.per_socket_queue>4*1024*1024||options.global_socket_queue<options.per_socket_queue||options.global_socket_queue>256*1024*1024||
     options.global_session_queue<32768||options.global_session_queue>256*1024*1024||!std::filesystem::is_directory(options.work_directory))throw std::invalid_argument("invalid server configuration");
  if(options.basestation_listeners.size()>16||options.max_sbs_clients==0||options.max_sbs_clients>1024)throw std::invalid_argument("invalid SBS limits");
  if(options.basestation_connectors.size()>16||options.basestation_connectors.size()>options.max_sbs_clients)throw std::invalid_argument("SBS connector count bound");
  for(const auto& target:options.basestation_connectors){
   if(target.endpoint.host.empty()||target.endpoint.host.size()>255||!target.endpoint.port||target.queue_limit<1||target.queue_limit>4*1024*1024)throw std::invalid_argument("SBS connector bounds");
   Connector connector;connector.options=target;connector.retry_due=now.mono;connectors.push_back(std::move(connector));
  }
  if(!connectors.empty()){
   resolver=std::make_unique<AsyncResolver>();epoll_event event{};event.events=EPOLLIN;event.data.u64=resolver_token;
   if(epoll_ctl(epoll.value,EPOLL_CTL_ADD,resolver->notification_fd(),&event)<0)throw std::runtime_error("resolver registration failed");
  }
  if(options.csv_files.size()>16)throw std::invalid_argument("too many CSV sinks");
  handshake_log=std::make_unique<CsvSink>(options.work_directory+"/handshakes.log",1024*1024,2,true);
  for(const auto& path:options.csv_files)csv_sinks.push_back(std::make_unique<CsvSink>(path));
  CoordinatorHooks hooks;
  hooks.request_traffic=[this](ReceiverModel& r,const auto& wanted){if(auto* c=find_receiver(r.id)){stamp(*c);c->session->request_traffic(wanted);}};
  hooks.client_stats=[this](ReceiverModel& r,const auto& value){if(auto* c=find_receiver(r.id)){stamp(*c);c->session->send_stats(value);}};
  hooks.status=[this](const auto& snapshot){
   publish_status(snapshot,options.work_directory,options.prometheus_file);
   // Pinned Python checks logging on its 15s state-write cadence. Logging
   // configuration must never change those writes or clock-counter resets.
   if(now.wall>next_status_log){
    next_status_log=now.wall+options.status_interval*.95;
    if(options.status_logger)options.status_logger(snapshot.title);
   }
  };
  hooks.output=[this](const MlatResult& result){
   result_cache={};++stats.outputs;
   if(!csv_sinks.empty())try{
    auto ac=co->interests().find(result.address);if(!ac)throw std::runtime_error("output aircraft absent");
    auto line=csv_result(result,*ac);for(auto& sink:csv_sinks)if(!sink->write(line))++stats.output_errors;
   }catch(...){++stats.output_errors;}
   for(auto id:sbs_clients)if(auto* c=find(id);c&&!c->closing&&!c->connecting){
    try{
     auto ac=co->interests().find(result.address);if(!ac)throw std::runtime_error("output aircraft absent");
     if(auto line=basestation_result(result,*ac,c->filtered,now.wall)){sbs_queue(*c,*line);++stats.sbs_records;}
    }catch(...){++stats.output_errors;}
   }
  };
  hooks.client_result=[this](ReceiverModel& r,const MlatResult& result){if(auto* c=find_receiver(r.id)){auto ac=co->interests().find(result.address);if(ac){stamp(*c);c->session->send_result(result,*ac,&result_cache);}}};
  co=std::make_unique<Coordinator>(random,std::move(hooks),now.wall,now.mono,options.interest);co->set_time(now.wall,now.mono,now.cpu);
  for(const auto& address:options.listeners)listen(address);
  for(const auto& address:options.basestation_listeners)listen(address.endpoint,true,address.filtered,address.queue_limit);
  co->start();
 }
 ~Impl(){stop();}
 Connection* find(std::uint64_t id){auto it=clients.find(id);return it==clients.end()?nullptr:it->second.get();}
 Connection* find_receiver(clock::StationId id){auto it=receiver_tokens.find(id);return it==receiver_tokens.end()?nullptr:find(it->second);}
 void stamp(Connection& c){if(c.session)c.session->set_time(now.wall,now.mono);}
 void close(Connection& c)noexcept{if(c.session)c.session->close();else{c.closing=true;c.close_due=now.mono;}}
 void sbs_queue(Connection& c,std::string_view line){
  try{queue(c.token,std::span(reinterpret_cast<const std::uint8_t*>(line.data()),line.size()));c.last_output=now.wall;}
  catch(...){close(c);throw;}
 }
 void refresh(){now=clock();co->set_time(now.wall,now.mono,now.cpu);}
 void interests(Connection& c){
  epoll_event event{};event.data.u64=c.token;event.events=EPOLLRDHUP;
  if(!c.closing&&!c.connecting)event.events|=EPOLLIN;
  if(c.queued||c.connecting)event.events|=EPOLLOUT;
  if(epoll_ctl(epoll.value,EPOLL_CTL_MOD,c.fd.value,&event)<0)throw std::runtime_error("epoll interest update failed");
 }
 void listen(const Endpoint& wanted,bool sbs=false,bool filtered=false,std::size_t queue_limit=0){
  if(sbs&&(queue_limit<1||queue_limit>4*1024*1024))throw std::invalid_argument("SBS queue bound");
  addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_PASSIVE|AI_NUMERICSERV;
  addrinfo* raw=nullptr;auto service=std::to_string(wanted.port);
  if(getaddrinfo(wanted.host.empty()?nullptr:wanted.host.c_str(),service.c_str(),&hints,&raw))throw std::runtime_error("listener address lookup failed");
  std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> addresses(raw,freeaddrinfo);
  for(auto* address=raw;address;address=address->ai_next){
   Fd fd(socket(address->ai_family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0));if(fd.value<0)continue;
   int yes=1;(void)setsockopt(fd.value,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
   if(address->ai_family==AF_INET6)(void)setsockopt(fd.value,IPPROTO_IPV6,IPV6_V6ONLY,&yes,sizeof yes);
   if(bind(fd.value,address->ai_addr,address->ai_addrlen)<0||::listen(fd.value,128)<0)continue;
   sockaddr_storage actual{};socklen_t length=sizeof actual;if(getsockname(fd.value,reinterpret_cast<sockaddr*>(&actual),&length)<0)throw std::runtime_error("listener endpoint unavailable");
   auto bound=endpoint(reinterpret_cast<sockaddr*>(&actual),length);epoll_event event{};event.events=EPOLLIN;event.data.u64=listener_bit|listeners.size();
   if(epoll_ctl(epoll.value,EPOLL_CTL_ADD,fd.value,&event)<0)throw std::runtime_error("listener registration failed");
   listeners.push_back({std::move(fd),std::move(bound),sbs,filtered,queue_limit});return;
  }
  throw std::runtime_error("unable to bind listener");
 }
 void queue(std::uint64_t id,std::span<const std::uint8_t> bytes){
  auto* c=find(id);if(!c||c->closing)throw std::runtime_error("closed output transport");
  if(bytes.size()>c->queue_limit-c->queued||bytes.size()>options.global_socket_queue-stats.queued_bytes){++stats.backpressure_closes;throw std::length_error("socket output budget");}
  bool empty=c->queued==0;c->writes.emplace_back(bytes.begin(),bytes.end());c->queued+=bytes.size();stats.queued_bytes+=bytes.size();stats.peak_queued_bytes=std::max(stats.peak_queued_bytes,stats.queued_bytes);
  if(empty)interests(*c);
 }
 void mark_closed(std::uint64_t id){if(auto* c=find(id)){c->closing=true;c->close_due=now.mono+1;interests(*c);}}
 void accept(Listener& listener){
  for(int accepted=0;accepted<64;++accepted){
   sockaddr_storage address{};socklen_t length=sizeof address;
   int fd=accept4(listener.fd.value,reinterpret_cast<sockaddr*>(&address),&length,SOCK_NONBLOCK|SOCK_CLOEXEC);
   if(fd<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)++stats.io_errors;break;}
   if((listener.sbs?sbs_clients.size()>=options.max_sbs_clients:clients.size()-sbs_clients.size()>=options.max_clients)||token+1>=listener_bit){::close(fd);++stats.refused;continue;}
   auto id=++token;auto item=std::make_unique<Connection>(fd,id);clients.emplace(id,std::move(item));auto& c=*clients.at(id);c.queue_limit=listener.sbs?listener.queue_limit:options.per_socket_queue;
   try{
    int yes=1;(void)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof yes);
    epoll_event event{};event.events=EPOLLIN|EPOLLRDHUP;event.data.u64=id;if(epoll_ctl(epoll.value,EPOLL_CTL_ADD,fd,&event)<0)throw std::runtime_error("peer registration failed");
    if(listener.sbs){c.sbs=true;c.filtered=listener.filtered;c.last_output=now.wall;c.heartbeat_due=now.mono+30;sbs_clients.push_back(id);++stats.sbs_accepted;continue;}
    auto peer=endpoint(reinterpret_cast<sockaddr*>(&address),length);ReceiverInfo info;info.source_ip=peer.host;info.source_port=peer.port;
    wire::SessionHooks hooks;hooks.write=[this,id](auto bytes){queue(id,bytes);};hooks.closed=[this,id](){mark_closed(id);};
    hooks.handshake_observed=[this](){if(!handshake_log->write("{\"event\":\"handshake\",\"redacted\":true}"))++stats.output_errors;};
    hooks.handshake.random_integer=[this](int low,int high){return std::uniform_int_distribution<int>(low,high-1)(handshake_random);};
    hooks.handshake.random_unit=[this](){return std::generate_canonical<double,53>(handshake_random);};
    hooks.handshake.close_existing=[this](auto receiver){if(auto* existing=find_receiver(receiver))existing->session->close();};
    c.session=std::make_unique<wire::Session>(*co,std::move(info),options.session,std::move(hooks),now.wall,now.mono);++stats.accepted;
   }catch(...){++stats.io_errors;remove(id);}
  }
 }
 void remove(std::uint64_t id)noexcept{
  auto* c=find(id);if(!c)return;
  if(c->session){c->session->close();stats.protocol_errors+=c->session->failures();}
  if(c->receiver)receiver_tokens.erase(*c->receiver);
  sbs_clients.remove(id);
  if(c->connector&&!stopped){
   auto& connector=connectors.at(*c->connector);connector.peer=0;
   if(c->connecting&&connector.next<connector.addresses.size()){connector.phase=Connector::Phase::trying;connector.retry_due=now.mono;}
   else{connector.phase=Connector::Phase::idle;connector.retry_due=now.mono+30;if(c->connecting)++stats.connector_failures;}
  }
  stats.queued_bytes-=c->queued;(void)epoll_ctl(epoll.value,EPOLL_CTL_DEL,c->fd.value,nullptr);clients.erase(id);++stats.closed;
 }
 void activated(Connection& c){
  c.connecting=false;c.last_output=now.wall;c.heartbeat_due=now.mono+30;
  sbs_clients.remove(c.token);sbs_clients.push_back(c.token); // registration order, not DNS request order
  if(c.connector)connectors.at(*c.connector).phase=Connector::Phase::connected;
  interests(c);++stats.sbs_accepted;
 }
 void try_connect(std::size_t index){
  auto& connector=connectors[index];
  if(sbs_clients.size()>=options.max_sbs_clients){connector.phase=Connector::Phase::idle;connector.retry_due=now.mono+30;++stats.connector_failures;return;}
  while(connector.next<connector.addresses.size()){
   const auto& address=connector.addresses[connector.next++];++stats.connector_attempts;
   Fd fd(socket(address.family,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0));if(fd.value<0)continue;
   int yes=1;(void)setsockopt(fd.value,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof yes);
   int connected=::connect(fd.value,reinterpret_cast<const sockaddr*>(&address.address),address.length);
   if(connected<0&&errno!=EINPROGRESS)continue;
   if(token+1>=listener_bit)throw std::runtime_error("transport token exhausted");
   auto id=++token;auto item=std::make_unique<Connection>(std::exchange(fd.value,-1),id);
   item->sbs=true;item->filtered=connector.options.filtered;item->queue_limit=connector.options.queue_limit;item->connector=index;
   item->connecting=true;item->connect_due=now.mono+30;
   clients.emplace(id,std::move(item));auto& c=*clients.at(id);sbs_clients.push_back(id);connector.peer=id;connector.phase=Connector::Phase::connecting;
   epoll_event event{};event.events=EPOLLOUT|EPOLLRDHUP;event.data.u64=id;
   if(epoll_ctl(epoll.value,EPOLL_CTL_ADD,c.fd.value,&event)<0){++stats.io_errors;remove(id);return;}
   if(connected==0)activated(c);
   return;
  }
  connector.phase=Connector::Phase::idle;connector.retry_due=now.mono+30;++stats.connector_failures;
 }
 void pump_connectors(){
  for(std::size_t index=0;index<connectors.size();++index){auto& c=connectors[index];
   if(c.phase==Connector::Phase::idle&&now.mono>=c.retry_due){
    c.query=++query_sequence;
    if(resolver->submit(c.query,c.options.endpoint.host,c.options.endpoint.port))c.phase=Connector::Phase::resolving;
    else{c.retry_due=now.mono+30;++stats.connector_failures;}
   }
   if(c.phase==Connector::Phase::resolving)if(auto result=resolver->take(c.query)){c.addresses=std::move(*result);c.next=0;c.phase=Connector::Phase::trying;}
   if(c.phase==Connector::Phase::trying)try_connect(index);
   if(c.phase==Connector::Phase::connecting)if(auto* peer=find(c.peer);peer&&now.mono>=peer->connect_due)close(*peer);
  }
 }
 void read(Connection& c){
  std::array<std::uint8_t,65536> buffer{};auto count=recv(c.fd.value,buffer.data(),c.sbs?1024:buffer.size(),0);
  if(c.sbs){if(count>0)stats.read_bytes+=static_cast<std::uint64_t>(count);else if(count==0)close(c);else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){++stats.io_errors;close(c);}return;}
  if(count>0){stats.read_bytes+=static_cast<std::uint64_t>(count);auto before=c.session->receiver()?c.session->receiver()->message_counter:0;
   c.session->receive(std::span(buffer).first(static_cast<std::size_t>(count)),now.wall,now.mono);
   // Status resets the source-compatible per-peer interval counter, but never
   // during receive(). Keep an independent cumulative throughput measurement.
   if(auto r=c.session->receiver())stats.protocol_messages+=r->message_counter-before;
   if(auto r=c.session->receiver();r&&!c.receiver){c.receiver=r->id;receiver_tokens[r->id]=c.token;}
  }else if(count==0){auto before=c.session->receiver()?c.session->receiver()->message_counter:0;c.session->eof();if(auto r=c.session->receiver())stats.protocol_messages+=r->message_counter-before;}
  else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){++stats.io_errors;close(c);c.close_due=now.mono;}
 }
 void write(Connection& c){
  std::size_t budget=65536;
  while(c.queued&&budget){auto& bytes=c.writes.front();auto size=std::min(budget,bytes.size()-c.offset);
   auto count=::send(c.fd.value,bytes.data()+c.offset,size,MSG_NOSIGNAL);
   if(count<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){++stats.io_errors;close(c);c.close_due=now.mono;}break;}
   if(count==0)break;
   auto n=static_cast<std::size_t>(count);c.offset+=n;c.queued-=n;stats.queued_bytes-=n;stats.written_bytes+=n;budget-=n;
   if(c.offset==bytes.size()){c.writes.pop_front();c.offset=0;}
  }
  if(!c.queued)interests(c);
 }
 void reap(){std::vector<std::uint64_t> retired;for(const auto& [id,c]:clients)if(c->closing&&(!c->queued||now.mono>=c->close_due))retired.push_back(id);for(auto id:retired)remove(id);}
 void pump(){
  std::vector<std::uint64_t> ready;for(const auto& [id,c]:clients)if(c->session){if(auto due=c->session->next_deadline();due&&*due<=now.mono)ready.push_back(id);}
  if(auto due=co->next_deadline();due&&*due<=now.mono)co->pump(now.wall,now.mono,now.cpu);
  for(auto id:ready)if(auto* c=find(id))c->session->advance(now.wall,now.mono);
  for(auto id:sbs_clients)if(auto* c=find(id);c&&!c->closing&&!c->connecting&&now.mono>=c->heartbeat_due){
   double delay=c->last_output+30-now.wall;
   if(delay>0.1)c->heartbeat_due=now.mono+delay;
   else try{sbs_queue(*c,"\n");c->heartbeat_due=now.mono+30;}catch(...){++stats.output_errors;}
  }
 }
 void session_budget(){
  std::size_t total=0;for(const auto& [id,c]:clients){(void)id;if(c->session)total+=c->session->queued_bytes();}
  if(total<=options.global_session_queue)return;
  for(auto& [id,c]:clients){(void)id;if(!c->session)continue;auto size=c->session->queued_bytes();if(size){total-=size;++stats.backpressure_closes;c->session->close();}if(total<=options.global_session_queue)break;}
 }
 int wait_time(int maximum){
  double deadline=now.mono+static_cast<double>(maximum)/1000;
  if(auto due=co->next_deadline())deadline=std::min(deadline,*due);
  for(const auto& [id,c]:clients){(void)id;if(c->session){if(auto due=c->session->next_deadline())deadline=std::min(deadline,*due);}else if(!c->closing)deadline=std::min(deadline,c->connecting?c->connect_due:c->heartbeat_due);if(c->closing)deadline=std::min(deadline,c->close_due);}
  for(const auto& c:connectors)if(c.phase==Connector::Phase::idle||c.phase==Connector::Phase::trying)deadline=std::min(deadline,c.retry_due);
  return static_cast<int>(std::clamp(std::ceil((deadline-now.mono)*1000),0.,static_cast<double>(maximum)));
 }
 std::optional<double> next_deadline()const{
  if(stopped)return {};
  auto deadline=co->next_deadline();
  auto consider=[&](double due){if(!deadline||due<*deadline)deadline=due;};
  for(const auto& [id,c]:clients){
   (void)id;
   if(c->session){if(auto due=c->session->next_deadline())consider(*due);}
   else if(!c->closing)consider(c->connecting?c->connect_due:c->heartbeat_due);
   if(c->closing)consider(c->close_due);
  }
  for(const auto& c:connectors)if(c.phase==Connector::Phase::idle||c.phase==Connector::Phase::trying)consider(c.retry_due);
  return deadline;
 }
 void tick(int maximum){
  if(stopped)return;
  if(maximum<0||maximum>1000)throw std::invalid_argument("poll wait bound");
  refresh();pump_connectors();pump();session_budget();reap();std::array<epoll_event,256> events{};
  int count=epoll_wait(epoll.value,events.data(),static_cast<int>(events.size()),wait_time(maximum));
  if(count<0){if(errno==EINTR)return;throw std::runtime_error("epoll wait failed");}
  refresh();++stats.iterations;
  for(int i=0;i<count;++i){const auto& event=events[static_cast<std::size_t>(i)];auto id=event.data.u64;
   if(id==resolver_token){resolver->drain_notifications();continue;}
   if(id&listener_bit){auto index=static_cast<std::size_t>(id&~listener_bit);if(index<listeners.size())accept(listeners[index]);continue;}
   auto* c=find(id);if(!c)continue;
   try{
    if(c->connecting){
     int error=0;socklen_t length=sizeof error;
     if(getsockopt(c->fd.value,SOL_SOCKET,SO_ERROR,&error,&length)<0||error){close(*c);continue;}
     activated(*c);
    }
    stamp(*c);if(event.events&EPOLLERR){++stats.io_errors;close(*c);c->close_due=now.mono;continue;}
    if(!c->closing&&(event.events&(EPOLLIN|EPOLLRDHUP|EPOLLHUP)))read(*c);
    if(event.events&EPOLLOUT)write(*c);
   }catch(...){++stats.io_errors;close(*c);c->close_due=now.mono;}
  }
  session_budget();reap();pump_connectors();
 }
 void stop()noexcept{if(stopped)return;stopped=true;while(!clients.empty())remove(clients.begin()->first);listeners.clear();resolver.reset();if(co)co->close();}
};
ServerTime Server::system_time(){auto seconds=[](clockid_t id){timespec time{};if(clock_gettime(id,&time))throw std::runtime_error("server clock failure");return static_cast<double>(time.tv_sec)+static_cast<double>(time.tv_nsec)*1e-9;};return {seconds(CLOCK_REALTIME),seconds(CLOCK_MONOTONIC),seconds(CLOCK_PROCESS_CPUTIME_ID)};}
Server::Server(ServerOptions options,Clock clock):impl_(std::make_unique<Impl>(std::move(options),std::move(clock))){}
Server::~Server()=default;
void Server::tick(int maximum){impl_->tick(maximum);}void Server::stop()noexcept{impl_->stop();}
std::optional<double> Server::next_deadline()const{return impl_->next_deadline();}
std::vector<Endpoint> Server::endpoints()const{std::vector<Endpoint> result;for(const auto& listener:impl_->listeners)if(!listener.sbs)result.push_back(listener.address);return result;}
std::size_t Server::connections()const{return impl_->clients.size()-impl_->sbs_clients.size();}
std::vector<SbsEndpoint> Server::basestation_endpoints()const{std::vector<SbsEndpoint> result;for(const auto& listener:impl_->listeners)if(listener.sbs)result.push_back({listener.address,listener.filtered,listener.queue_limit});return result;}
std::size_t Server::basestation_connections()const{std::size_t count=0;for(auto id:impl_->sbs_clients)if(auto* c=impl_->find(id);c&&!c->connecting&&!c->closing)++count;return count;}
const ServerStats& Server::stats()const{return impl_->stats;}
const Coordinator& Server::coordinator()const{return *impl_->co;}
}
