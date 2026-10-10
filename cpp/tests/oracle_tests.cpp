// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlat/geodesy.hpp"
#include "mlat/modes.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using nlohmann::json;

namespace {
std::size_t checks=0;
void require(bool condition, const std::string& label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
void near(double actual, double expected, double tolerance, const std::string& label) {
    require(std::isfinite(actual) && std::abs(actual-expected)<=tolerance, label);
}
template<class T> json nullable(const std::optional<T>& v) {return v ? json(*v) : json(nullptr);}
json message_json(const mlat::modes::Message& m) {
    json j={{"DF",m.df},{"address",m.address},{"altitude",nullable(m.altitude)},
            {"callsign",nullable(m.callsign)},{"squawk",nullable(m.squawk)},{"crc_ok",nullable(m.crc_ok)}};
    if (m.estype) {j["estype"]=static_cast<int>(*m.estype); j["nuc"]=nullable(m.nuc);}
#define FIELD(name,field) if(m.field) j[name]=*m.field
    FIELD("F",f); FIELD("LAT",lat); FIELD("LON",lon); FIELD("SS",ss); FIELD("SAF",saf);
    FIELD("AC12",ac12); FIELD("T",t); FIELD("CATEGORY",category); FIELD("CA",ca); FIELD("CF",cf);
    FIELD("AA",aa); FIELD("VS",vs); FIELD("CC",cc); FIELD("SL",sl); FIELD("RI",ri);
    FIELD("FS",fs); FIELD("DR",dr); FIELD("UM",um); FIELD("AC",ac); FIELD("ID",id);
#undef FIELD
    return j;
}
}
int main(int argc, char** argv) {
    try {
        require(argc==2,"oracle path required");
        std::ifstream input(argv[1]); require(input.good(),"oracle missing");
        const auto oracle=json::parse(input);
        for (unsigned i=0;i<8192;++i) {
            require(nullable(mlat::modes::decode_ac13(i))==oracle["ac13"][i],"AC13 "+std::to_string(i));
            require(mlat::modes::decode_id13(i)==oracle["id13"][i].get<std::string>(),"ID13 "+std::to_string(i));
        }
        for (unsigned i=0;i<4096;++i)
            require(nullable(mlat::modes::decode_ac12(i))==oracle["ac12"][i],"AC12 "+std::to_string(i));
        std::size_t index=0;
        for (const auto& row:oracle["messages"]) {
            auto data=mlat::modes::from_hex(row["hex"].get<std::string>());
            require(mlat::modes::crc_residual(data)==row["residual"],"CRC "+std::to_string(index));
            const auto m=mlat::modes::decode(data);
            const json actual=m ? message_json(*m) : json(nullptr);
            if (actual!=row["decoded"]) {
                std::cerr<<"case "<<index<<" actual="<<actual<<" expected="<<row["decoded"]<<'\n';
            }
            require(actual==row["decoded"],"decoded field parity "+std::to_string(index++));
        }
        for (const auto& row:oracle["geodesy"]) {
            const auto llh=row["llh"].get<mlat::Vec3>();
            const auto ecef=mlat::llh_to_ecef(llh);
            const auto reference=row["ecef"].get<mlat::Vec3>();
            const auto inverse=mlat::ecef_to_llh(reference);
            for (int i=0;i<3;++i) {
                near(ecef[static_cast<std::size_t>(i)],reference[static_cast<std::size_t>(i)],1e-7,"LLH/ECEF parity");
                near(inverse[static_cast<std::size_t>(i)],row["inverse"][i],i==2 ? 1e-6 : 1e-10,"ECEF/LLH parity");
            }
            near(mlat::ecef_distance(reference,{6378137,0,0}),row["distance"],1e-7,"ECEF distance parity");
        }
        for (const auto& row:oracle["cpr"]) {
            const auto v=row["input"].get<std::array<int,4>>();
            bool rejected=false;
            std::array<double,4> actual{};
            try { actual=mlat::modes::decode_cpr(v[0],v[1],v[2],v[3]); }
            catch (const std::invalid_argument&) { rejected=true; }
            require(rejected==row["result"].is_null(),"CPR acceptance parity");
            if (!rejected) for (int i=0;i<4;++i)
                near(actual[static_cast<std::size_t>(i)],row["result"][i],1e-10,"CPR coordinate parity");
        }
        // Malformed input is rejected before any unchecked field reads.
        const auto valid=mlat::modes::from_hex("8d40621d58c382d690c8ac2863a7");
        for (std::size_t n=0;n<valid.size();++n)
            require(!mlat::modes::decode(std::span(valid).first(n)),"truncated decode");
        auto oversized=valid; oversized.push_back(0);
        require(!mlat::modes::decode(oversized),"oversized decode");
        bool invalid_hex=false;
        try {(void)mlat::modes::from_hex("zzzzzzzzzzzzzz");}
        catch (const std::invalid_argument&) {invalid_hex=true;}
        require(invalid_hex,"malformed hex");
        std::cout<<"PASS "<<checks<<" checks against deployed Python/Cython oracle; not a server benchmark\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;}
}
