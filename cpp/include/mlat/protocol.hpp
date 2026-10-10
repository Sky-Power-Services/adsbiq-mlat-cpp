// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of jsonclient.py, Copyright (C) 2015 Oliver Jowett and contributors.
#pragma once
#include "mlat/coordinator.hpp"
namespace mlat::wire {
using Json=nlohmann::json;
// Transport handles first-line / PROXY deadlines; no implicit proxy trust here.
struct HandshakeHooks {
 std::function<int(int,int)> random_integer; // lower inclusive, upper exclusive
 std::function<double()> random_unit;
 std::function<void(clock::StationId)> close_existing;
 std::function<void(ReceiverModel&,const Json&)> authenticate;
};
struct HandshakeResult {
 std::shared_ptr<ReceiverModel> receiver;
 std::string compression,result_format="old";
 bool return_results=false,return_stats=false;
 Json response;
};
HandshakeResult handshake(Coordinator& coordinator,std::string_view line,ReceiverInfo peer,
 std::string_view motd,std::string_view source_url,const HandshakeHooks& hooks);
enum class MessageKind {ignored,malformed,sync,mlat,seen,lost,input_connected,reset,heartbeat,rates,quine};
struct Message {
 MessageKind kind=MessageKind::ignored;
 double even=0,odd=0,timestamp=0;
 SyncMessage even_message{},odd_message{};
 std::vector<std::uint8_t> bytes;
 std::vector<std::uint32_t> addresses;
 std::map<std::uint32_t,double> rates;
 Json query;
};
// Invalid JSON is ignored without incrementing the message counter, as source.
// Structurally invalid/nonfinite/bounded input throws; transport closes the peer.
Message decode_message(std::string_view line,ReceiverModel& receiver,double wall_time);
Message dispatch_message(Coordinator& coordinator,clock::StationId receiver,std::string_view line,double wall_time);
// Restricted source manifest only: never inspect arbitrary modules/files.
Json quine_reply(const Json& request,const std::map<std::string,std::string>& public_sources);
}
