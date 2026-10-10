// SPDX-License-Identifier: AGPL-3.0-or-later
// Protocol derived from jsonclient.py, Copyright (C) 2015 Oliver Jowett.
#include "mlat/framing.hpp"
#include <algorithm>
#include <stdexcept>

namespace mlat::wire {
namespace {
[[noreturn]] void fail(const char* reason) {throw std::runtime_error(reason);}
constexpr std::array<std::uint8_t,4> trailer={0,0,255,255};
}
Decoder::Decoder(std::size_t maximum): max_expanded_(maximum) {
    if (!maximum || maximum>4*1024*1024) fail("invalid decompression budget");
    if (inflateInit(&stream_)!=Z_OK) fail("inflate initialization failed");
}
Decoder::~Decoder() {inflateEnd(&stream_);}
void Decoder::feed(std::span<const std::uint8_t> input, const LineHandler& handler) {
    if (failed_) fail("decoder unusable after protocol failure");
    try {
        while (!input.empty()) {
            if (header_used_<2) {
                header_[header_used_++]=input.front(); input=input.subspan(1);
                if (header_used_<2) continue;
                expected_=(static_cast<std::size_t>(header_[0])<<8)|header_[1];
                payload_.clear(); payload_.reserve(expected_+4);
                if (expected_==0) fail("empty compressed frame");
            }
            const auto n=std::min(expected_-payload_.size(),input.size());
            payload_.insert(payload_.end(),input.begin(),input.begin()+static_cast<std::ptrdiff_t>(n));
            input=input.subspan(n);
            if (payload_.size()==expected_) {
                frame(handler);
                ++completed_frames_;
                header_used_=0; expected_=0; payload_.clear();
            }
        }
    } catch (...) {failed_=true; throw;}
}
void Decoder::finish() const {
    if (failed_ || header_used_ || !payload_.empty()) fail("truncated compressed frame");
}
void Decoder::frame(const LineHandler& handler) {
    payload_.insert(payload_.end(),trailer.begin(),trailer.end());
    stream_.next_in=payload_.data(); stream_.avail_in=static_cast<uInt>(payload_.size());
    std::array<unsigned char,65536> output{};
    std::string pending;
    std::size_t expanded=0;
    do {
        stream_.next_out=output.data(); stream_.avail_out=static_cast<uInt>(output.size());
        const auto before=stream_.avail_in;
        const int status=inflate(&stream_,Z_SYNC_FLUSH);
        if (status!=Z_OK && status!=Z_BUF_ERROR) fail("invalid or terminated compressed stream");
        const auto produced=output.size()-stream_.avail_out;
        if (expanded+produced>max_expanded_) fail("decompression budget exceeded");
        expanded+=produced;
        if (!produced && before==stream_.avail_in && stream_.avail_in) fail("decompressor made no progress");
        for (std::size_t i=0;i<produced;++i)
            if (output[i]>127) fail("non-ASCII protocol data");
        pending.append(reinterpret_cast<const char*>(output.data()),produced);
        std::size_t start=0;
        while (true) {
            const auto end=pending.find('\n',start);
            if (end==std::string::npos) break;
            handler(std::string_view(pending).substr(start,end-start));
            start=end+1;
        }
        pending.erase(0,start);
        if (pending.size()>1024) fail("unfinished line exceeds protocol bound");
        // If all input was consumed and output filled exactly, drain any
        // remaining decompressor output before accepting this frame.
        if (!stream_.avail_in && stream_.avail_out) break;
    } while (true);
    if (!pending.empty()) fail("frame is not newline terminated");
}
Encoder::Encoder() {
    if (deflateInit(&stream_,1)!=Z_OK) fail("deflate initialization failed");
}
Encoder::~Encoder() {deflateEnd(&stream_);}
std::vector<std::uint8_t> Encoder::encode(std::string_view lines,std::size_t max_input) {
    if (failed_) fail("encoder unusable after failure");
    if (lines.empty()) return {};
    if (!max_input || max_input>1024*1024 || lines.size()>max_input || lines.back()!='\n') fail("encoding requires bounded complete lines");
    for (const unsigned char c:lines) if (c>127) fail("non-ASCII protocol data");
    try {
        stream_.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(lines.data()));
        stream_.avail_in=static_cast<uInt>(lines.size());
        std::vector<std::uint8_t> result(65538);
        stream_.next_out=result.data()+2;
        stream_.avail_out=65536;
        if (deflate(&stream_,Z_SYNC_FLUSH)!=Z_OK || stream_.avail_in || !stream_.avail_out)
            fail("compressed output bound exceeded");
        const auto written=65536-stream_.avail_out;
        if (written<4 || !std::equal(trailer.begin(),trailer.end(),result.begin()+2+written-4))
            fail("missing zlib flush marker");
        const auto length=written-4;
        result.resize(length+2);
        result[0]=static_cast<std::uint8_t>(length>>8);
        result[1]=static_cast<std::uint8_t>(length&255);
        return result;
    } catch (...) {failed_=true; throw;}
}
}
