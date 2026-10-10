// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "mlat/session.hpp"
namespace mlat {
struct Endpoint {std::string host="127.0.0.1";std::uint16_t port=0;};
struct SbsEndpoint {Endpoint endpoint;bool filtered=false;std::size_t queue_limit=262144;};
struct ServerTime {double wall,mono,cpu;};
struct ServerOptions {
 std::vector<Endpoint> listeners{{}};
 std::shared_ptr<const wire::SessionConfig> session;
 std::string work_directory;
 std::optional<std::string> prometheus_file;
 std::vector<std::string> csv_files;
 std::vector<SbsEndpoint> basestation_listeners,basestation_connectors;
 std::size_t max_sbs_clients=128;
 std::size_t max_clients=1024,per_socket_queue=1024*1024,global_socket_queue=64*1024*1024,global_session_queue=64*1024*1024;
 std::uint64_t random_seed=1;InterestOptions interest;
 double status_interval=15;
 std::function<void(const std::string&)> status_logger;
};
struct ServerStats {
 std::uint64_t accepted=0,refused=0,closed=0,read_bytes=0,written_bytes=0,protocol_messages=0,io_errors=0,protocol_errors=0,backpressure_closes=0,outputs=0,output_errors=0,sbs_accepted=0,sbs_records=0,connector_attempts=0,connector_failures=0,iterations=0;
 std::size_t queued_bytes=0,peak_queued_bytes=0;
};
class Server {
public:
 using Clock=std::function<ServerTime()>;
 explicit Server(ServerOptions options,Clock clock={});
 ~Server();Server(const Server&)=delete;Server& operator=(const Server&)=delete;
 void tick(int maximum_wait_ms=250);
 // Earliest scheduled timer in the injected monotonic clock domain. This
 // does not imply socket quiescence or acknowledge pending input processing.
 std::optional<double> next_deadline()const;
 void stop()noexcept;
 std::vector<Endpoint> endpoints()const;
 std::size_t connections()const;
 std::vector<SbsEndpoint> basestation_endpoints()const;
 std::size_t basestation_connections()const;
 const ServerStats& stats()const;
 const Coordinator& coordinator()const;
 static ServerTime system_time();
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
