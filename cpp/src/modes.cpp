// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of modes_cython/message.pyx, Copyright (C) 2015 Oliver Jowett.
// Source pin: bcfacbf67a8d592101b975f7529f78294019c2bd.
#include "mlat/modes.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mlat::modes {
namespace {
constexpr int bad_alt = -999999;
constexpr int altitude(unsigned v) {
    if (!v || (v & 0x40)) return bad_alt;
    if (v & 0x10) {
        const auto n = ((v & 0x1f80) >> 2) | ((v & 0x20) >> 1) | (v & 15);
        return static_cast<int>(n) * 25 - 1000;
    }
    if (!(v & 0x1500)) return bad_alt;
    int h = 0;
    if (v & 0x1000) h ^= 7;
    if (v & 0x0400) h ^= 3;
    if (v & 0x0100) h ^= 1;
    if (h & 5) h ^= 5;
    if (h > 5) return bad_alt;
    int f = 0;
    if (v & 0x0010) f ^= 0x1ff;
    if (v & 0x0004) f ^= 0x0ff;
    if (v & 0x0001) f ^= 0x07f;
    if (v & 0x0800) f ^= 0x03f;
    if (v & 0x0200) f ^= 0x01f;
    if (v & 0x0080) f ^= 0x00f;
    if (v & 0x0020) f ^= 0x007;
    if (v & 0x0008) f ^= 0x003;
    if (v & 0x0002) f ^= 0x001;
    if (f & 1) h = 6 - h;
    const int a = 500*f + 100*h - 1300;
    return a < -1200 ? bad_alt : a;
}
constexpr auto alt_table = [] {
    std::array<int,8192> a{};
    for (unsigned i=0; i<a.size(); ++i) a[i]=altitude(i);
    return a;
}();
constexpr auto crc_table = [] {
    std::array<std::uint32_t,256> a{};
    for (std::uint32_t i=0; i<a.size(); ++i) {
        auto c = i << 16;
        for (int j=0; j<8; ++j) c = (c << 1) ^ ((c & 0x800000) ? 0xfff409u : 0u);
        a[i] = c & 0xffffff;
    }
    return a;
}();
constexpr std::string_view charset = " ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
std::string callsign(std::span<const std::uint8_t> b) {
    std::string result(8, ' ');
    result[0]=charset[(b[5]&0xfc)>>2];
    result[1]=charset[((b[5]&3)<<4)|((b[6]&0xf0)>>4)];
    result[2]=charset[((b[6]&15)<<2)|((b[7]&0xc0)>>6)];
    result[3]=charset[b[7]&63];
    result[4]=charset[(b[8]&0xfc)>>2];
    result[5]=charset[((b[8]&3)<<4)|((b[9]&0xf0)>>4)];
    result[6]=charset[((b[9]&15)<<2)|((b[10]&0xc0)>>6)];
    result[7]=charset[b[10]&63];
    return result;
}
void extended(Message& m, std::span<const std::uint8_t> b) {
    const int type = (b[4]&0xf8)>>3;
    if (type >= 1 && type <= 4) m.estype=ESType::id_and_category;
    else if (type >= 5 && type <= 8) {m.estype=ESType::surface_position; m.nuc=14-type;}
    else if (type == 0 || (type >= 9 && type <= 18) || (type >= 20 && type <= 22)) {
        m.estype=ESType::airborne_position;
        m.nuc=type==0 || type==22 ? 0 : (type>=20 ? 29-type : 18-type);
    } else if (type == 19) m.estype=ESType::airborne_velocity;
    else m.estype=ESType::other;
    if (m.estype == ESType::airborne_position) {
        m.ss=(b[4]&6)>>1; m.saf=b[4]&1;
        m.ac12=(b[5]<<4)|((b[6]&0xf0)>>4);
        m.t=(b[6]&8)>>3; m.f=(b[6]&4)>>2;
        m.lat=((b[6]&3)<<15)|(b[7]<<7)|((b[8]&0xfe)>>1);
        m.lon=((b[8]&1)<<16)|(b[9]<<8)|b[10];
        m.altitude=decode_ac12(static_cast<unsigned>(*m.ac12));
    } else if (m.estype == ESType::id_and_category) {
        m.category=b[4]&7; m.callsign=callsign(b);
    }
}
constexpr std::array<double,58> nl_thresholds = {
    10.47047130,14.82817437,18.18626357,21.02939493,23.54504487,25.82924707,
    27.93898710,29.91135686,31.77209708,33.53993436,35.22899598,36.85025108,
    38.41241892,39.92256684,41.38651832,42.80914012,44.19454951,45.54626723,
    46.86733252,48.16039128,49.42776439,50.67150166,51.89342469,53.09516153,
    54.27817472,55.44378444,56.59318756,57.72747354,58.84763776,59.95459277,
    61.04917774,62.13216659,63.20427479,64.26616523,65.31845310,66.36171008,
    67.39646774,68.42322022,69.44242631,70.45451075,71.45986473,72.45884545,
    73.45177442,74.43893416,75.42056257,76.39684391,77.36789461,78.33374083,
    79.29428225,80.24923213,81.19801349,82.13956981,83.07199445,83.99173563,
    84.89166191,85.75541621,86.53536998,87.00000000};
int nl(double lat) {
    return 59-static_cast<int>(std::upper_bound(nl_thresholds.begin(),nl_thresholds.end(),
                                               std::abs(lat))-nl_thresholds.begin());
}
int mod(int a, int b) { const int r=a%b; return r<0 ? r+b : r; }
int nibble(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    throw std::invalid_argument("invalid Mode S hex");
}
}
std::vector<std::uint8_t> from_hex(std::string_view text) {
    if (text.size()!=14 && text.size()!=28) throw std::invalid_argument("invalid Mode S length");
    std::vector<std::uint8_t> out(text.size()/2);
    for (std::size_t i=0; i<out.size(); ++i)
        out[i]=static_cast<std::uint8_t>((nibble(text[2*i])<<4)|nibble(text[2*i+1]));
    return out;
}
std::optional<int> decode_ac13(unsigned value) noexcept {
    const int alt=alt_table[value & 0x1fff];
    return alt==bad_alt ? std::nullopt : std::optional<int>{alt};
}
std::optional<int> decode_ac12(unsigned value) noexcept {
    return decode_ac13(((value & 0xfc0)<<1)|(value&0x3f));
}
std::string decode_id13(unsigned v) {
    unsigned a=0,b=0,c=0,d=0;
    if (v&0x1000) c|=1;
    if (v&0x0800) a|=1;
    if (v&0x0400) c|=2;
    if (v&0x0200) a|=2;
    if (v&0x0100) c|=4;
    if (v&0x0080) a|=4;
    if (v&0x0020) b|=1;
    if (v&0x0010) d|=1;
    if (v&0x0008) b|=2;
    if (v&0x0004) d|=2;
    if (v&0x0002) b|=4;
    if (v&0x0001) d|=4;
    return {static_cast<char>('0'+a),static_cast<char>('0'+b),
            static_cast<char>('0'+c),static_cast<char>('0'+d)};
}
std::uint32_t crc_residual(std::span<const std::uint8_t> data) {
    if (data.empty()) throw std::invalid_argument("empty Mode S message");
    const std::size_t n=data[0]>>3 >15 ? 14 : 7;
    if (data.size()!=n) throw std::invalid_argument("Mode S DF/length mismatch");
    auto rem=crc_table[data[0]];
    for (std::size_t i=1; i<n-3; ++i)
        rem=((rem&0xffff)<<8)^crc_table[data[i]^(rem>>16)];
    return rem^(static_cast<std::uint32_t>(data[n-3])<<16)^
               (static_cast<std::uint32_t>(data[n-2])<<8)^data[n-1];
}
std::optional<Message> decode(std::span<const std::uint8_t> b) {
    if (b.empty()) return std::nullopt;
    const int df=b[0]>>3;
    const std::size_t size=df>15 ? 14 : 7;
    if (b.size()!=size) return std::nullopt;
    if (df!=0 && df!=4 && df!=5 && df!=11 && df!=16 && df!=17 &&
        df!=18 && df!=20 && df!=21) return std::nullopt;
    Message m; m.df=df;
    const auto residual=crc_residual(b);
    if (df==11 || df==17 || df==18) {
        m.aa=(b[1]<<16)|(b[2]<<8)|b[3]; m.address=static_cast<std::uint32_t>(*m.aa);
        if (df==18) m.cf=b[0]&7; else m.ca=b[0]&7;
        if (df==11) {
            if (residual==0) m.crc_ok=true;
            else if (residual & ~0x7fu) m.crc_ok=false;
        } else {m.crc_ok=residual==0; extended(m,b);}
    } else {
        m.address=residual;
        const auto code=((static_cast<unsigned>(b[2])&31)<<8)|b[3];
        if (df==5 || df==21) {m.id=static_cast<int>(code); m.squawk=decode_id13(code);}
        else {m.ac=static_cast<int>(code); m.altitude=decode_ac13(code);}
        if (df==0 || df==16) {
            m.vs=(b[0]&4)>>2; m.sl=(b[1]&0xe0)>>5;
            m.ri=((b[1]&3)<<1)|((b[2]&0x80)>>7);
            if (df==0) m.cc=(b[0]&2)>>1;
        } else {
            m.fs=b[0]&7; m.dr=(b[1]&0xf8)>>3; m.um=((b[1]&7)<<3)|((b[2]&0xe0)>>5);
        }
        if ((df==20 || df==21) && b[4]==0x20) {
            auto c=callsign(b);
            if (c!="        " && c.find('?')==std::string::npos) m.callsign=std::move(c);
        }
    }
    return m;
}
std::array<double,4> decode_cpr(int lat_even, int lon_even, int lat_odd, int lon_odd) {
    for (int v : {lat_even,lon_even,lat_odd,lon_odd})
        if (v<0 || v>=131072) throw std::invalid_argument("CPR field out of range");
    const double le=lat_even, lo=lat_odd, oe=lon_even, oo=lon_odd;
    const int j=static_cast<int>(std::floor((59*le-60*lo)/131072.0+0.5));
    double rle=6.0*(mod(j,60)+le/131072.0);
    double rlo=(360.0/59.0)*(mod(j,59)+lo/131072.0);
    if (rle>=270) rle-=360;
    if (rlo>=270) rlo-=360;
    if (rle < -90 || rle>90 || rlo < -90 || rlo>90)
        throw std::invalid_argument("CPR latitude out of range");
    const int ne=nl(rle), no=std::max(1,ne-1);
    if (ne!=nl(rlo)) throw std::invalid_argument("CPR latitude zone mismatch");
    const int m=static_cast<int>(std::floor((oe*(ne-1)-oo*ne)/131072.0+0.5));
    double roe=(360.0/ne)*(mod(m,ne)+oe/131072.0);
    double roo=(360.0/no)*(mod(m,no)+oo/131072.0);
    roe-=std::floor((roe+180)/360)*360;
    roo-=std::floor((roo+180)/360)*360;
    return {rle,roe,rlo,roo};
}
}
