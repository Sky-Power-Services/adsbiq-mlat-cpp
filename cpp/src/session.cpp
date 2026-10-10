// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of jsonclient.py, Copyright (C) 2015 Oliver Jowett and contributors.
#include "mlat/session.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <arpa/inet.h>
namespace mlat::wire {
Session::Session(Coordinator& co,ReceiverInfo peer,std::shared_ptr<const SessionConfig> config,SessionHooks hooks,double wall,double mono):
 co_(co),peer_(std::move(peer)),config_(std::move(config)),hooks_(std::move(hooks)),wall_(wall),mono_(mono),last_read_(wall),handshake_due_(mono+15){
 if(!config_||!hooks_.write||config_->source_url.empty()||config_->max_queued_bytes<32768||config_->max_queued_bytes>1024*1024)throw std::invalid_argument("invalid session configuration");
 set_time(wall,mono);
}
Session::~Session(){close();}
void Session::set_time(double wall,double mono){
 if(!std::isfinite(wall)||!std::isfinite(mono)||mono<mono_)throw std::invalid_argument("invalid session clocks");
 wall_=wall;mono_=mono;
}
void Session::close()noexcept{
 if(closed_)return;
 closed_=true;heartbeat_due_.reset();flush_due_.reset();traffic_pending_=false;input_.clear();output_.clear();queued_bytes_=0;wanted_.clear();requested_.clear();
 if(connection_.receiver&&co_.receivers().contains(connection_.receiver->id))try{co_.disconnect(connection_.receiver->id);}catch(...){++failures_;}
 if(hooks_.closed)try{hooks_.closed();}catch(...){++failures_;}
}
std::optional<double> Session::next_deadline()const noexcept{
 if(closed_)return {};
 if(!connection_.receiver)return handshake_due_;
 if(traffic_pending_)return mono_;
 auto next=heartbeat_due_;if(flush_due_&&(!next||*flush_due_<*next))next=flush_due_;return next;
}
void Session::proxy(std::string_view value){
 if(!config_->trusted_proxy||proxy_seen_)throw std::invalid_argument("untrusted/repeated proxy header");
 std::istringstream in{std::string(value)};std::string tag,protocol,source,destination,sport,dport,extra;
 if(!(in>>tag>>protocol>>source>>destination>>sport>>dport)||in>>extra||tag!="PROXY")throw std::invalid_argument("invalid proxy header");
 int family=protocol=="TCP4"?AF_INET:protocol=="TCP6"?AF_INET6:0;std::array<unsigned char,16> address{};
 if(!family||inet_pton(family,source.c_str(),address.data())!=1||inet_pton(family,destination.c_str(),address.data())!=1)throw std::invalid_argument("invalid proxy address");
 auto port=[](const std::string& text){int number=0;auto p=std::from_chars(text.data(),text.data()+text.size(),number);if(p.ec!=std::errc{}||p.ptr!=text.data()+text.size()||number<1||number>65535)throw std::invalid_argument("invalid proxy port");return number;};
 peer_.source_port=port(sport);(void)port(dport);peer_.source_ip=source;proxy_seen_=true;handshake_due_=mono_+15;
}
void Session::raw(const Json& value){
 auto text=value.dump(-1,' ',true)+'\n';if(text.size()>1024*1024)throw std::length_error("raw output bound");
 hooks_.write(std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
void Session::line(std::string_view value){
 if(closed_)return;
 if(!connection_.receiver){
  if(value.starts_with("PROXY ")){proxy(value);return;}
  if(hooks_.handshake_observed)try{hooks_.handshake_observed();}catch(...){/* logging cannot break feeder service */}
  connection_=handshake(co_,value,peer_,config_->motd,config_->source_url,hooks_.handshake);
  raw(connection_.response);
  if(!connection_.receiver){close();return;}
  if(closed_)return;
  if(connection_.compression!="none")decoder_=std::make_unique<Decoder>();
  if(connection_.compression=="zlib2")encoder_=std::make_unique<Encoder>();
  last_read_=wall_;heartbeat_due_=mono_+30;heartbeat_sequence_=++sequence_;return;
 }
 last_read_=wall_;
 auto message=dispatch_message(co_,connection_.receiver->id,value,wall_);
 if(message.kind==MessageKind::quine)send(quine_reply(message.query,config_->public_sources));
}
void Session::receive(std::span<const std::uint8_t> bytes,double wall,double mono){
 if(closed_)return;
 try{
  set_time(wall,mono);if(bytes.size()>1024*1024)throw std::length_error("session read bound");
  while(!bytes.empty()&&!closed_){
   if(decoder_){auto before=decoder_->completed_frames();decoder_->feed(bytes,[&](auto text){line(text);});if(decoder_->completed_frames()!=before)last_read_=wall_;break;}
   auto end=std::find(bytes.begin(),bytes.end(),static_cast<std::uint8_t>('\n'));auto size=static_cast<std::size_t>(end-bytes.begin());
   if(input_.size()+size>65536)throw std::length_error("plain line bound");
   for(std::size_t i=0;i<size;++i)if(bytes[i]>127)throw std::invalid_argument("non-ASCII plain input");
   input_.append(reinterpret_cast<const char*>(bytes.data()),size);
   if(end==bytes.end())break;
   bytes=bytes.subspan(size+1);auto complete=std::move(input_);input_.clear();line(complete);
  }
 }catch(...){failed();}
}
void Session::eof(){
 if(closed_)return;
 try{if(decoder_)decoder_->finish();else if(!input_.empty()){auto final=std::move(input_);input_.clear();line(final);}}catch(...){++failures_;}
 close();
}
void Session::send(const Json& value){
 if(!active())return;
 if(!encoder_){raw(value);return;}
 auto text=value.dump(-1,' ',true)+'\n';if(text.size()>config_->max_queued_bytes||queued_bytes_>config_->max_queued_bytes-text.size())throw std::length_error("session output queue bound");
 queued_bytes_+=text.size();output_.push_back(std::move(text));
 if(!flush_due_){flush_due_=mono_+1;flush_sequence_=++sequence_;}
}
void Session::flush(){
 flush_due_.reset();auto lines=std::move(output_);output_.clear();queued_bytes_=0;std::string batch;
 auto emit=[&](){if(batch.empty())return;auto bytes=encoder_->encode(batch,1024*1024);hooks_.write(bytes);batch.clear();};
 for(const auto& text:lines){if(batch.size()+text.size()>32768)emit();batch+=text;if(batch.size()>=32768)emit();}
 emit();
}
void Session::request_traffic(const std::set<std::uint32_t>& wanted){
 if(!active())return;
 if(wanted.size()>100000||(!wanted.empty()&&*wanted.rbegin()>0xffffff)){failed();throw std::length_error("session traffic bound");}
 wanted_=wanted;traffic_pending_=true;
}
void Session::traffic(){
 traffic_pending_=false;
 auto send_difference=[&](const auto& a,const auto& b,const char* key){Json addresses=Json::array();
  for(auto address:a)if(!b.contains(address)){
   std::array<char,8> buffer{};auto p=std::to_chars(buffer.data(),buffer.data()+buffer.size(),address,16);std::string text(buffer.data(),p.ptr);text.insert(0,6-text.size(),'0');addresses.push_back(text);
   if(addresses.size()==2048){send({{key,addresses}});addresses=Json::array();}
  }
  if(!addresses.empty())send({{key,addresses}});
 };
 send_difference(wanted_,requested_,"start_sending");send_difference(requested_,wanted_,"stop_sending");requested_=wanted_;
}
void Session::send_stats(const Json& value){try{if(active()&&connection_.return_stats)send({{"stats",value}});}catch(...){failed();throw;}}
void Session::send_result(const MlatResult& result,const AircraftModel& aircraft,ResultCache* cache){
 if(!active()||!connection_.return_results)return;
 try{ResultCache local;if(!cache)cache=&local;auto& value=connection_.result_format=="old"?cache->old:cache->ecef;
  if(!value)value=connection_.result_format=="old"?client_result_old(result,aircraft):client_result_ecef(result,aircraft);
  send({{"result",*value}});
 }catch(...){failed();throw;}
}
void Session::advance(double wall,double mono){
 if(closed_)return;
 try{
  set_time(wall,mono);if(!active()){if(mono_>=handshake_due_)close();return;}
  // Snapshot timers before ready work, preserving timer registration tie order.
  std::vector<std::tuple<double,std::uint64_t,bool>> due;
  if(heartbeat_due_&&*heartbeat_due_<=mono_)due.emplace_back(*heartbeat_due_,heartbeat_sequence_,true);
  if(flush_due_&&*flush_due_<=mono_)due.emplace_back(*flush_due_,flush_sequence_,false);
  std::sort(due.begin(),due.end());if(traffic_pending_)traffic();
  for(auto [when,sequence,heartbeat]:due){(void)when;(void)sequence;if(closed_)break;
   if(!heartbeat){flush();continue;}
   if(wall_-last_read_>150){close();break;}
   send({{"heartbeat",{{"server_time",python_round(wall_,3)}}}});heartbeat_due_=mono_+30;heartbeat_sequence_=++sequence_;
  }
 }catch(...){failed();}
}
}
