#include "mlat/resolver.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <chrono>
#include <iostream>
#include <map>
#include <thread>
using namespace mlat;
std::size_t checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
int main(){try{
 AsyncResolver resolver;
 for(std::uint64_t id=1;id<=16;++id)require(resolver.submit(id,"127.0.0.1",54321),"bounded query accepted");
 require(!resolver.submit(17,"127.0.0.1",54321),"pending/result budget enforced");
 require(!resolver.submit(1,"127.0.0.1",54321),"duplicate request rejected");
 std::map<std::uint64_t,std::vector<ResolvedAddress>> results;
 auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
 while(results.size()<16&&std::chrono::steady_clock::now()<deadline){
  pollfd fd{resolver.notification_fd(),POLLIN,0};(void)poll(&fd,1,20);resolver.drain_notifications();
  for(std::uint64_t id=1;id<=16;++id)if(!results.contains(id))if(auto result=resolver.take(id))results[id]=std::move(*result);
 }
 require(results.size()==16,"all bounded jobs completed without external network");
 for(const auto& [id,addresses]:results){
  (void)id;require(!addresses.empty()&&addresses.size()<=16,"resolved result bound");
  for(const auto& address:addresses){
   require(address.family==AF_INET&&address.length==sizeof(sockaddr_in),"numeric IPv4 family");
   const auto* numeric=reinterpret_cast<const sockaddr_in*>(&address.address);
   require(ntohs(numeric->sin_port)==54321&&ntohl(numeric->sin_addr.s_addr)==0x7f000001,"address and port preserved");
  }
 }
 require(resolver.submit(17,"::1",12345),"drained result releases capacity");
 std::optional<std::vector<ResolvedAddress>> ipv6;
 while(!ipv6&&std::chrono::steady_clock::now()<deadline){pollfd fd{resolver.notification_fd(),POLLIN,0};(void)poll(&fd,1,20);resolver.drain_notifications();ipv6=resolver.take(17);}
 require(ipv6&&!ipv6->empty()&&ipv6->front().family==AF_INET6,"numeric IPv6 resolution");
 for(const auto& host:{std::string(),std::string(256,'x')}){bool rejected=false;try{resolver.submit(18,host,1);}catch(const std::invalid_argument&){rejected=true;}require(rejected,"hostname size bounds");}
 bool rejected=false;try{resolver.submit(18,"localhost",0);}catch(const std::invalid_argument&){rejected=true;}require(rejected,"zero destination port rejected");
 {AsyncResolver pending;require(pending.submit(1,"127.0.0.1",12345),"shutdown with pending query");}
 std::this_thread::sleep_for(std::chrono::milliseconds(20));
 std::cout<<"PASS "<<checks<<" bounded async resolver checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
