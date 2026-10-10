// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlat/framing.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
using nlohmann::json;
namespace {
std::size_t checks=0;
void require(bool b,const char* reason) {++checks; if(!b) throw std::runtime_error(reason);}
template<class F> void rejects(F&& f) {
    bool rejected=false;
    try {f();} catch (const std::runtime_error&) {rejected=true;}
    require(rejected,"expected protocol rejection");
}
std::vector<std::uint8_t> unhex(const std::string& s) {
    std::vector<std::uint8_t> result;
    for(std::size_t i=0;i<s.size();i+=2)
        result.push_back(static_cast<std::uint8_t>(std::stoul(s.substr(i,2),nullptr,16)));
    return result;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"fixture required");
        std::ifstream file(argv[1]); require(file.good(),"fixture missing");
        auto fixture=json::parse(file);
        mlat::wire::Decoder decoder;
        mlat::wire::Encoder encoder;
        for(const auto& c:fixture["cases"]) {
            auto wire=unhex(c["wire_hex"].get<std::string>());
            std::vector<std::string> expected;
            std::string batch;
            for(const auto& line:c["lines"]) {
                auto s=line.get<std::string>(); batch+=s; s.pop_back(); expected.push_back(s);
            }
            std::vector<std::string> got;
            // Every byte delivered separately: header and payload fragmentation.
            for(auto b:wire) decoder.feed(std::span(&b,1),[&](auto line){got.emplace_back(line);});
            require(got==expected,"Python frame decode mismatch");
            require(encoder.encode(batch)==wire,"Python/C++ zlib encoder mismatch");
            decoder.finish();
        }
        rejects([] {mlat::wire::Decoder d; std::uint8_t b=0; d.feed(std::span(&b,1),[](auto){});d.finish();});
        rejects([] {mlat::wire::Decoder d; const std::array<std::uint8_t,2>b={0,0};d.feed(b,[](auto){});});
        rejects([] {mlat::wire::Encoder e;(void)e.encode("not newline terminated");});
        rejects([] {mlat::wire::Encoder e;(void)e.encode(std::string(32769,'\n'));});
        rejects([] {mlat::wire::Encoder e;(void)e.encode("\xff\n");});
        rejects([] {mlat::wire::Encoder e;mlat::wire::Decoder d(64);auto w=e.encode(std::string(100,'x')+"\n");d.feed(w,[](auto){});});
        // Reject concatenated independent zlib streams; dictionary is persistent.
        rejects([] {mlat::wire::Encoder e1,e2;mlat::wire::Decoder d;auto a=e1.encode("{}\n");
                    auto b=e2.encode("{}\n");d.feed(a,[](auto){});d.feed(b,[](auto){});});
        // Long complete lines are legal in the deployed framing routine.
        {mlat::wire::Encoder e;mlat::wire::Decoder d;auto line=std::string(2000,'x');
         auto w=e.encode(line+"\n");std::string out;d.feed(w,[&](auto s){out=s;});require(out==line,"long line");}
        // Incompressible bounded batch exercises the full output buffer path.
        {mlat::wire::Encoder e;mlat::wire::Decoder d;std::string line;unsigned x=1234;
         for(int i=0;i<30000;++i){x=1664525u*x+1013904223u;line.push_back(static_cast<char>(32+(x%95)));}
         auto w=e.encode(line+"\n");std::string out;d.feed(w,[&](auto s){out=s;});require(out==line,"large frame");}
        std::cout<<"PASS "<<checks<<" framing checks against deployed Python fixture\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
