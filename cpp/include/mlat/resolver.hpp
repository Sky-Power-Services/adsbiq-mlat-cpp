// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/socket.h>
namespace mlat {
struct ResolvedAddress {sockaddr_storage address{};socklen_t length=0;int family=0;};
// One bounded worker, <=16 queued/inflight/results, <=16 addresses per result.
// Worker only owns shared private state; never calls back into the server.
class AsyncResolver {
public:
 AsyncResolver();~AsyncResolver();
 AsyncResolver(const AsyncResolver&)=delete;AsyncResolver& operator=(const AsyncResolver&)=delete;
 int notification_fd()const;
 bool submit(std::uint64_t id,const std::string& host,std::uint16_t port);
 std::optional<std::vector<ResolvedAddress>> take(std::uint64_t id);
 void drain_notifications();
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
