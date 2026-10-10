// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of coordinator.py, Oliver Jowett and contributors.
#include "mlat/coordinator.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace mlat {
Coordinator::Coordinator(InterestRandom& rng,CoordinatorHooks hooks,double wall,double mono,InterestOptions options):
 random_(rng),hooks_(std::move(hooks)),options_(options),wall_(wall),mono_(mono),cleanup_{mono+1,++timer_sequence_},
 interests_(rng,[this](auto r){if(ready_.size()>=1000000)throw std::length_error("coordinator ready bound");ready_.push_back({Work::refresh,r});},wall,options),
 clocks_([this](auto address){return interests_.find(address);},[this](ReceiverModel& r,double timestamp,const SyncMessage& m,double now,bool decoded){mlat_.receive(receiver(r.id),timestamp,m,now,mono_,decoded);}),
 mlat_([this](auto address){return interests_.find(address);},[this](auto ids){return clocks_.normalization_models(ids);},[this](const auto& value){result(value);},wall,mono,[this]{return ++timer_sequence_;}){
 if(!std::isfinite(wall)||!std::isfinite(mono))throw std::invalid_argument("invalid coordinator time");
}
Coordinator::~Coordinator(){try{close();}catch(...){}}
void Coordinator::start(){if(closed_||started_)throw std::logic_error("coordinator already started/closed");started_=true;ready_.push_back({Work::maintenance,{}});maintenance_={std::numeric_limits<double>::infinity(),0};}
void Coordinator::close(){if(closed_)return;while(!receivers_.empty())disconnect(receivers_.begin()->first);ready_.clear();closed_=true;started_=false;}
void Coordinator::set_time(double wall,double mono,double cpu){
 if(!std::isfinite(wall)||!std::isfinite(mono)||!std::isfinite(cpu)||mono<mono_||cpu<cpu_)throw std::invalid_argument("invalid/nonmonotonic coordinator clocks");
 wall_=wall;mono_=mono;cpu_=cpu;
}
std::shared_ptr<ReceiverModel> Coordinator::receiver(clock::StationId id)const{auto it=receivers_.find(id);if(it==receivers_.end()||it->second->dead)throw std::invalid_argument("inactive receiver");return it->second;}
std::shared_ptr<ReceiverModel> Coordinator::connect(const ReceiverInfo& info,std::function<void(ReceiverModel&)> authenticate){
 if(closed_||receivers_.size()>=4096)throw std::length_error("receiver bound/closed coordinator");
 if(info.user.empty()||info.user.size()>8192||info.connection_info.size()>16384||info.source_ip.size()>256||(info.uuid&&info.uuid->size()>8192)||usernames_.contains(info.user))throw std::invalid_argument("invalid/duplicate receiver identity");
 if(public_uid_>4611686018427387904ULL)public_uid_=0;
 auto occupied=[&](auto uid){return std::any_of(receivers_.begin(),receivers_.end(),[&](const auto& entry){return entry.second->public_id==uid;});};
 while(occupied(public_uid_))++public_uid_;
 auto r=std::make_shared<ReceiverModel>(++generation_,info.clock_type,info.llh,wall_);r->public_id=public_uid_;r->user=info.user;r->uuid=info.uuid;
 r->privacy=info.privacy;r->connection_info=info.connection_info;r->source_ip=info.source_ip;r->source_port=info.source_port;
 double x=0,y=0;if(!r->privacy){x=random_.uniform();y=random_.uniform();}StatusEngine::location(*r,info.llh,x,y);
 if(authenticate)authenticate(*r);
 if(r->dead||usernames_.contains(r->user))throw std::invalid_argument("authentication produced invalid receiver");
 interests_.attach(r);clocks_.attach(r);receivers_.emplace(r->id,r);usernames_.emplace(r->user,r->id);return r;
}
void Coordinator::disconnect(clock::StationId id){auto r=receiver(id);r->dead=true;interests_.disconnect(*r);clocks_.disconnect(*r);receivers_.erase(id);usernames_.erase(r->user);}
void Coordinator::location(clock::StationId id,Vec3 llh){auto r=receiver(id);double x=0,y=0;if(!r->privacy){x=random_.uniform();y=random_.uniform();}StatusEngine::location(*r,llh,x,y);}
void Coordinator::seen(clock::StationId id,std::span<const std::uint32_t> addresses){auto r=receiver(id);interests_.add(*r,addresses,wall_);if(!r->last_rate_report)interests_.update_interest(*r,wall_);}
void Coordinator::lost(clock::StationId id,std::span<const std::uint32_t> addresses){auto r=receiver(id);interests_.remove(*r,addresses);if(!r->last_rate_report)interests_.update_interest(*r,wall_);}
void Coordinator::rates(clock::StationId id,const std::map<std::uint32_t,double>& report){
 if(report.size()>100000)throw std::length_error("rate report bound");
 for(const auto& [address,rate]:report)if(address>0xffffff||!std::isfinite(rate)||rate<0||rate>1000000)throw std::invalid_argument("invalid rate report");
 auto r=receiver(id);r->last_rate_report=report;interests_.update_interest(*r,wall_);
}
void Coordinator::reset(clock::StationId id,bool input_connected){auto r=receiver(id);clocks_.clock_reset(*r,wall_);if(input_connected)r->clock_reset_counter=0;}
void Coordinator::sync(clock::StationId id,double even,double odd,const SyncMessage& em,const SyncMessage& om){clocks_.sync(receiver(id),even,odd,em,om,wall_);}
void Coordinator::mlat(clock::StationId id,double timestamp,std::span<const std::uint8_t> message){mlat_.receive(receiver(id),timestamp,message,wall_,mono_);}
void Coordinator::message_received(clock::StationId id){++receiver(id)->message_counter;}
IntervalStats Coordinator::interval_stats()const{
 const auto& s=clocks_.stats();const auto& m=mlat_.stats();
 return {s.points-sync_checkpoint_.points,s.messages-sync_checkpoint_.messages,m.messages-mlat_checkpoint_.messages,m.valid_groups-mlat_checkpoint_.valid_groups,
  m.normalize-mlat_checkpoint_.normalize,m.solve_attempt-mlat_checkpoint_.solve_attempt,m.solve_success-mlat_checkpoint_.solve_success,m.solve_used-mlat_checkpoint_.solve_used};
}
void Coordinator::maintenance(){
 maintenance_={mono_+15,++timer_sequence_};auto pairs=clocks_.status_pairs();
 double previous_cpu=status_.cpu_checkpoint();
 try{
 auto snapshot=status_.update(receivers_,interests_.aircraft(),interests_.wanted().size(),pairs,interval_stats(),wall_,cpu_,options_.partition,options_.partitions);
 clocks_.decay_status_counters();
 if(hooks_.client_stats)for(const auto& [id,value]:snapshot.client_statistics)hooks_.client_stats(*receiver(id),value);
 if(hooks_.status)hooks_.status(snapshot);
 }catch(...){status_.restore_cpu_checkpoint(previous_cpu);throw;}
 sync_checkpoint_=clocks_.stats();mlat_checkpoint_=mlat_.stats();clocks_.clear_sync_points();
}
void Coordinator::result(const MlatResult& output){
 if(hooks_.output)hooks_.output(output);
 if(!forward_to_clients(output.filter.valid,output.dof))return;
 if(hooks_.client_result)for(const auto& r:output.all_receivers)try{hooks_.client_result(*r,output);}catch(...){report_error("client result callback");}
}
void Coordinator::report_error(std::string_view where)noexcept{++errors_;if(hooks_.error)try{hooks_.error(where);}catch(...){}}
void Coordinator::pump(double wall,double mono,double cpu){
 if(!started_||closed_||pumping_)throw std::logic_error("inactive/reentrant coordinator pump");
 set_time(wall,mono,cpu);
 pumping_=true;struct Reset {bool& flag;~Reset(){flag=false;}} reset{pumping_};
 auto ready=std::move(ready_);ready_.clear();std::vector<std::pair<Timer,Work>> timers;
 if(cleanup_.due<=mono_)timers.push_back({cleanup_,Work::cleanup});
 if(maintenance_.due<=mono_)timers.push_back({maintenance_,Work::maintenance});
 for(auto [due,sequence]:mlat_.ready_timers(mono_))timers.push_back({{due,sequence},Work::cohort});
 std::sort(timers.begin(),timers.end(),[](const auto& a,const auto& b){return std::tie(a.first.due,a.first.sequence)<std::tie(b.first.due,b.first.sequence);});
 for(const auto& t:timers)ready.push_back({t.second,{}});
 for(const auto& work:ready)try{
  if(work.kind==Work::cleanup){cleanup_={mono_+10,++timer_sequence_};clocks_.cleanup(wall_);}
  else if(work.kind==Work::maintenance)maintenance();
  else if(work.kind==Work::cohort)mlat_.advance_one(mono_,wall_);
  else {InterestTracker::refresh_requests(*work.receiver);if(hooks_.request_traffic){std::set<std::uint32_t> ids;for(const auto& ac:work.receiver->requested)ids.insert(ac->address);hooks_.request_traffic(*work.receiver,ids);}}
 }catch(...){report_error("coordinator callback");}
}
std::optional<double> Coordinator::next_deadline()const{
 if(!started_||closed_)return std::nullopt;
 if(!ready_.empty())return mono_;
 double next=std::min(cleanup_.due,maintenance_.due);if(auto due=mlat_.next_deadline())next=std::min(next,*due);return next;
}
void publish_status(const StatusSnapshot& snapshot,const std::string& directory,const std::optional<std::string>& prometheus){
 namespace fs=std::filesystem;fs::path root(directory);
 if(!fs::is_directory(root))throw std::invalid_argument("status work directory missing");
 auto write=[](const fs::path& path,const std::string& data){if(fs::is_symlink(path))throw std::invalid_argument("status symlink refused");std::ofstream out(path,std::ios::binary|std::ios::trunc);out.exceptions(std::ios::badbit|std::ios::failbit);out.write(data.data(),static_cast<std::streamsize>(data.size()));out.close();};
 auto atomic=[&](std::string_view name,const nlohmann::json& data){auto target=root/name;auto temp=target;temp+=".tmp";write(temp,data.dump());fs::rename(temp,target);};
 atomic("sync.json",snapshot.sync);atomic("clients.json",snapshot.clients);atomic("aircraft.json",snapshot.aircraft);
 if(prometheus)try{write(*prometheus,snapshot.prometheus);}catch(const std::exception&){} // optional exporter, as in Python
}
}
