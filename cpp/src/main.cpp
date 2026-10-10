// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlat/server.hpp"
#include <charconv>
#include <csignal>
#include <iostream>
#include <sys/prctl.h>
namespace {
volatile std::sig_atomic_t stopping=0;
void stop_signal(int){stopping=1;}
unsigned long long integer(std::string_view text){unsigned long long value=0;auto p=std::from_chars(text.data(),text.data()+text.size(),value);if(p.ec!=std::errc{}||p.ptr!=text.data()+text.size())throw std::invalid_argument("invalid integer argument");return value;}
bool boolean(std::string_view text){if(text=="true")return true;if(text=="false")return false;throw std::invalid_argument("invalid boolean argument");}
double real(std::string_view text){double value=0;auto p=std::from_chars(text.data(),text.data()+text.size(),value);if(p.ec!=std::errc{}||p.ptr!=text.data()+text.size()||!std::isfinite(value))throw std::invalid_argument("invalid real argument");return value;}
mlat::Endpoint endpoint(std::string_view text){
 std::string_view host="127.0.0.1",port=text;
 if(text.starts_with('[')){auto end=text.find(']');if(end==text.npos||end+1>=text.size()||text[end+1]!=':')throw std::invalid_argument("invalid bracketed endpoint");host=text.substr(1,end-1);port=text.substr(end+2);}
 else if(auto colon=text.find(':');colon!=text.npos){host=text.substr(0,colon);port=text.substr(colon+1);}
 auto number=integer(port);if(number>65535||host.empty())throw std::invalid_argument("invalid listen endpoint");return {std::string(host),static_cast<std::uint16_t>(number)};
}
}
int main(int argc,char**argv){
 try{
  mlat::ServerOptions options;options.listeners.clear();auto session=std::make_shared<mlat::wire::SessionConfig>();std::optional<double> run_seconds;std::string tag="mlat-server";
  for(int i=1;i<argc;++i){std::string_view arg=argv[i];auto value=[&]()->std::string_view{if(i+1>=argc)throw std::invalid_argument("missing option value");return argv[++i];};
   if(arg=="--help"){std::cout<<"MLAT C++ experimental server (no production cutover approved)\n"
     "Required: --client-listen [host:]port --work-dir EXISTING_DIR --source-url URL\n"
     "Options: --motd TEXT --seed N --max-clients N --partition I/N\n"
     "         --min-tracking N (default 3) --mark-adsb-time=true|false (default true)\n"
     "         --prometheus-file PATH --write-csv PATH --trusted-proxy --run-seconds N\n"
     "         --basestation-listen [host:]port --filtered-basestation-listen [host:]port\n"
     "         --basestation-connect host:port --filtered-basestation-connect host:port\n"
     "         --status-interval SECONDS (-1 disables logs) --tag NAME\n"
     "Port-only listeners bind loopback. UDP and pseudorange output are not implemented.\n"
     "SIGHUP preserves connections (the pinned Python blacklist has no filtering effect).\n"
     "Unknown/unused options are ignored with a fixed warning.\n";return 0;}
   if(arg=="--client-listen")options.listeners.push_back(endpoint(value()));
   else if(arg=="--basestation-listen"||arg=="--filtered-basestation-listen")options.basestation_listeners.push_back({endpoint(value()),arg=="--filtered-basestation-listen"});
   else if(arg=="--basestation-connect"||arg=="--filtered-basestation-connect")options.basestation_connectors.push_back({endpoint(value()),arg=="--filtered-basestation-connect"});
   else if(arg=="--work-dir")options.work_directory=value();
   else if(arg=="--source-url")session->source_url=value();
   else if(arg=="--motd")session->motd=value();
   else if(arg=="--status-interval")options.status_interval=real(value());
   else if(arg.starts_with("--status-interval="))options.status_interval=real(arg.substr(18));
   else if(arg=="--tag")tag=value();
   else if(arg.starts_with("--tag="))tag=arg.substr(6);
   else if(arg=="--seed")options.random_seed=integer(value());
   else if(arg=="--max-clients")options.max_clients=integer(value());
   else if(arg=="--min-tracking")options.interest.min_tracking=integer(value());
   else if(arg.starts_with("--min-tracking="))options.interest.min_tracking=integer(arg.substr(15));
   else if(arg=="--mark-adsb-time")options.interest.mark_adsb_time=boolean(value());
   else if(arg.starts_with("--mark-adsb-time="))options.interest.mark_adsb_time=boolean(arg.substr(17));
   else if(arg=="--prometheus-file")options.prometheus_file=value();
   else if(arg=="--write-csv")options.csv_files.emplace_back(value());
   else if(arg=="--trusted-proxy")session->trusted_proxy=true;
   else if(arg=="--run-seconds"){auto text=value();double seconds=0;auto p=std::from_chars(text.data(),text.data()+text.size(),seconds);if(p.ec!=std::errc{}||p.ptr!=text.data()+text.size()||!std::isfinite(seconds)||seconds<=0||seconds>86400)throw std::invalid_argument("invalid run duration");run_seconds=seconds;}
   else if(arg=="--partition"){auto text=value();auto slash=text.find('/');if(slash==text.npos)throw std::invalid_argument("invalid partition");auto first=integer(text.substr(0,slash)),count=integer(text.substr(slash+1));if(!first||first>count||count>65536)throw std::invalid_argument("invalid partition");options.interest.partition=static_cast<std::uint32_t>(first);options.interest.partitions=static_cast<std::uint32_t>(count);}
   else {
    // Do not echo unknown option names or values: either can contain secrets.
    std::cerr<<"Warning: unrecognized or unused CLI argument ignored\n";
    if(arg.starts_with("--")&&arg.find('=')==arg.npos&&i+1<argc&&!std::string_view(argv[i+1]).starts_with("--"))++i;
   }
  }
  if(options.work_directory.empty()||options.listeners.empty()||session->source_url.empty())throw std::invalid_argument("work directory, listener and actual source URL are required");
  if(session->trusted_proxy)for(const auto& e:options.listeners)if(e.host!="127.0.0.1"&&e.host!="::1")throw std::invalid_argument("trusted proxy requires an explicit loopback-only listener");
  const auto selected_interest=options.interest;
  // Linux comm is limited to 15 bytes. Do not rewrite argv or retain/print
  // arbitrary tag content in logs; aggregate status titles remain unchanged.
  if(prctl(PR_SET_NAME,tag.substr(0,15).c_str(),0UL,0UL,0UL)!=0)throw std::runtime_error("process tag failed");
  const double selected_status_interval=options.status_interval;
  options.status_logger=[](const std::string& title){std::cerr<<title<<'\n';};
  options.session=session;std::signal(SIGINT,stop_signal);std::signal(SIGTERM,stop_signal);std::signal(SIGPIPE,SIG_IGN);
  // Pinned Python always handles HUP to reread an otherwise unused blacklist.
  // There is no filtering state to reload here; never terminate live feeders.
  // Pseudorange logging/reopening remains explicitly unsupported.
  std::signal(SIGHUP,SIG_IGN);
  mlat::Server server(std::move(options));nlohmann::json endpoints=nlohmann::json::array();for(const auto& e:server.endpoints())endpoints.push_back({{"host",e.host},{"port",e.port}});
  nlohmann::json sbs=nlohmann::json::array();for(const auto& e:server.basestation_endpoints())sbs.push_back({{"host",e.endpoint.host},{"port",e.endpoint.port},{"filtered",e.filtered}});
  std::cout<<nlohmann::json{{"ready",true},{"listeners",endpoints},{"basestation_listeners",sbs},{"min_tracking",selected_interest.min_tracking},{"mark_adsb_time",selected_interest.mark_adsb_time},{"status_interval",selected_status_interval}}.dump()<<std::endl;
  double started=mlat::Server::system_time().mono;
  while(!stopping&&(!run_seconds||mlat::Server::system_time().mono-started<*run_seconds))server.tick(100);
  server.stop();const auto& s=server.stats();
  std::cout<<nlohmann::json{{"stopped",true},{"accepted",s.accepted},{"refused",s.refused},{"closed",s.closed},{"read_bytes",s.read_bytes},{"written_bytes",s.written_bytes},
    {"protocol_messages",s.protocol_messages},{"io_errors",s.io_errors},{"protocol_errors",s.protocol_errors},{"backpressure_closes",s.backpressure_closes},{"coordinator_errors",server.coordinator().errors()},
    {"outputs",s.outputs},{"output_errors",s.output_errors},{"sbs_accepted",s.sbs_accepted},{"sbs_records",s.sbs_records},{"connector_attempts",s.connector_attempts},{"connector_failures",s.connector_failures},{"queued_bytes",s.queued_bytes},{"peak_queued_bytes",s.peak_queued_bytes}}.dump()<<std::endl;return 0;
 }catch(const std::exception&){std::cerr<<"MLAT server startup/runtime error (use --help; sensitive arguments are not logged)\n";return 2;}
}
