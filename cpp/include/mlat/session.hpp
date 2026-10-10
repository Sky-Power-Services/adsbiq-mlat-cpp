// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of jsonclient.py, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include "mlat/protocol.hpp"
#include "mlat/framing.hpp"
#include "mlat/output.hpp"
namespace mlat::wire {
struct SessionConfig {
 std::string motd,source_url;
 bool trusted_proxy=false; // Only on a listener restricted to explicitly trusted proxies.
 std::size_t max_queued_bytes=262144;
 std::map<std::string,std::string> public_sources;
};
struct SessionHooks {
 // Must consume/copy bytes now, with bounded transport backpressure.
 std::function<void(std::span<const std::uint8_t>)> write;
 // Notification only: owner MUST defer destruction until callbacks return.
 std::function<void()> closed;
 HandshakeHooks handshake;
 // No arguments by design: observers cannot persist raw private handshakes.
 std::function<void()> handshake_observed;
};
struct ResultCache {std::optional<Json> old,ecef;};
class Session {
public:
 Session(Coordinator& coordinator,ReceiverInfo peer,std::shared_ptr<const SessionConfig> config,
         SessionHooks hooks,double wall,double mono);
 ~Session();
 Session(const Session&)=delete;Session& operator=(const Session&)=delete;
 // Host also sets Coordinator clocks before input and before invoking hooks.
 void set_time(double wall,double mono);
 void receive(std::span<const std::uint8_t> input,double wall,double mono);
 void advance(double wall,double mono);
 void eof();void close()noexcept;
 void request_traffic(const std::set<std::uint32_t>& wanted);
 void send_stats(const Json& statistics);
 void send_result(const MlatResult& result,const AircraftModel& aircraft,ResultCache* cache=nullptr);
 bool closed()const noexcept{return closed_;}
 bool active()const noexcept{return connection_.receiver&&!closed_;}
 std::shared_ptr<ReceiverModel> receiver()const noexcept{return connection_.receiver;}
 std::optional<double> next_deadline()const noexcept;
 double last_read()const noexcept{return last_read_;}
 const auto& requested()const noexcept{return requested_;}
 std::size_t queued_bytes()const noexcept{return queued_bytes_;}
 std::uint64_t failures()const noexcept{return failures_;}
private:
 void line(std::string_view value);void proxy(std::string_view value);
 void raw(const Json& value);void send(const Json& value);void flush();void traffic();
 void failed()noexcept{++failures_;close();}
 Coordinator& co_;ReceiverInfo peer_;std::shared_ptr<const SessionConfig> config_;SessionHooks hooks_;
 HandshakeResult connection_;double wall_,mono_,last_read_,handshake_due_;
 std::optional<double> heartbeat_due_,flush_due_;
 std::uint64_t sequence_=0,heartbeat_sequence_=0,flush_sequence_=0,failures_=0;
 bool closed_=false,proxy_seen_=false,traffic_pending_=false;
 std::string input_;std::vector<std::string> output_;std::size_t queued_bytes_=0;
 std::set<std::uint32_t> wanted_,requested_;
 std::unique_ptr<Decoder> decoder_;std::unique_ptr<Encoder> encoder_;
};
}
