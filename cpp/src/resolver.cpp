// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlat/resolver.hpp"
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <netdb.h>
#include <stdexcept>
#include <thread>
#include <sys/eventfd.h>
#include <unistd.h>
namespace mlat {
struct AsyncResolver::Impl {
 struct Job {std::string host,service;std::optional<std::vector<ResolvedAddress>> result;};
 struct State {
  int fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);bool stop=false;
  std::mutex mutex;std::condition_variable changed;std::map<std::uint64_t,Job> jobs;std::deque<std::uint64_t> ready;
  State(){if(fd<0)throw std::runtime_error("resolver notification creation failed");}
  ~State(){::close(fd);}
 };
 std::shared_ptr<State> state=std::make_shared<State>();std::thread worker;
 Impl():worker([s=state](){
  while(true){
   std::uint64_t id;std::string host,service;
   {
    std::unique_lock lock(s->mutex);s->changed.wait(lock,[&](){return s->stop||!s->ready.empty();});
    if(s->stop)return;
    id=s->ready.front();s->ready.pop_front();const auto& job=s->jobs.at(id);host=job.host;service=job.service;
   }
   std::vector<ResolvedAddress> result;addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_NUMERICSERV;
   addrinfo* addresses=nullptr;
   if(getaddrinfo(host.c_str(),service.c_str(),&hints,&addresses)==0){
    std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owner(addresses,freeaddrinfo);
    for(auto* address=addresses;address&&result.size()<16;address=address->ai_next){
     if((address->ai_family!=AF_INET&&address->ai_family!=AF_INET6)||address->ai_addrlen>sizeof(sockaddr_storage))continue;
     ResolvedAddress copy;copy.family=address->ai_family;copy.length=address->ai_addrlen;std::memcpy(&copy.address,address->ai_addr,address->ai_addrlen);result.push_back(copy);
    }
   }
   {
    std::lock_guard lock(s->mutex);if(s->stop)return;s->jobs.at(id).result=std::move(result);
   }
   std::uint64_t one=1;ssize_t written;
   do{written=::write(s->fd,&one,sizeof one);}while(written<0&&errno==EINTR);
   // EAGAIN means an unread notification already exists. Completed jobs are
   // also polled at each bounded host tick; no host pointer is used here.
  }
 }){}
 ~Impl(){
  {std::lock_guard lock(state->mutex);state->stop=true;state->ready.clear();}
  state->changed.notify_one();
  // libc DNS is not cancellable safely. Detach at shutdown: worker owns State,
  // returns after its current lookup, and cannot touch a destroyed host. Exactly
  // one worker per configured server, never one per request or retry.
  worker.detach();
 }
};
AsyncResolver::AsyncResolver():impl_(std::make_unique<Impl>()){}
AsyncResolver::~AsyncResolver()=default;
int AsyncResolver::notification_fd()const{return impl_->state->fd;}
bool AsyncResolver::submit(std::uint64_t id,const std::string& host,std::uint16_t port){
 if(host.empty()||host.size()>255||!port)throw std::invalid_argument("resolver query bounds");
 auto& s=*impl_->state;std::lock_guard lock(s.mutex);
 if(s.stop||s.jobs.size()>=16||s.jobs.contains(id))return false;
 s.jobs.emplace(id,Impl::Job{host,std::to_string(port),{}});
 try{s.ready.push_back(id);}catch(...){s.jobs.erase(id);throw;}
 s.changed.notify_one();return true;
}
std::optional<std::vector<ResolvedAddress>> AsyncResolver::take(std::uint64_t id){
 auto& s=*impl_->state;std::lock_guard lock(s.mutex);auto it=s.jobs.find(id);
 if(it==s.jobs.end()||!it->second.result)return {};
 auto result=std::move(it->second.result);s.jobs.erase(it);return result;
}
void AsyncResolver::drain_notifications(){std::uint64_t value;while(::read(notification_fd(),&value,sizeof value)==sizeof value){};}
}
