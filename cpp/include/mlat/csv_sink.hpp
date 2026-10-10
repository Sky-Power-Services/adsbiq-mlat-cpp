// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace mlat {
// LocalCSVWriter's append/flush/rotation policy. Owns only the named file and
// its numbered backups; paths must be in a private, trusted output directory.
class CsvSink {
public:
 explicit CsvSink(std::string filename,std::uint64_t max_bytes=1024*1024,unsigned backups=10,bool private_only=false);
 ~CsvSink();CsvSink(const CsvSink&)=delete;CsvSink& operator=(const CsvSink&)=delete;
 // Record excludes the framing newline. A sink failure never stops MLAT.
 bool write(std::string_view record) noexcept;
 std::uint64_t errors()const noexcept{return errors_;}
private:
 void open();void rotate();
 std::string filename_;std::uint64_t max_bytes_,errors_=0;unsigned backups_;bool private_only_;int fd_=-1;
};
}
