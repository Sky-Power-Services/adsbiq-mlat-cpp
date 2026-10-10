// SPDX-License-Identifier: AGPL-3.0-or-later
// Rotation policy from Python logging.handlers.RotatingFileHandler, used by
// output.py's LocalCSVWriter (Oliver Jowett and contributors).
#include "mlat/csv_sink.hpp"
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace mlat {
CsvSink::CsvSink(std::string filename,std::uint64_t max_bytes,unsigned backups,bool private_only)
 :filename_(std::move(filename)),max_bytes_(max_bytes),backups_(backups),private_only_(private_only){
 if(filename_.empty()||max_bytes_==0||backups_==0||backups_>100)throw std::invalid_argument("invalid CSV sink configuration");
 if(private_only_)for(unsigned i=0;i<=backups_;++i){
  auto path=filename_+(i?"."+std::to_string(i):"");struct stat info{};
  if(lstat(path.c_str(),&info)==0){
   if(!S_ISREG(info.st_mode)||(info.st_mode&077)||info.st_nlink!=1||info.st_uid!=geteuid())throw std::runtime_error("unsafe existing private log");
  }else if(errno!=ENOENT)throw std::runtime_error("private log inspection failed");
 }
 open();
}
CsvSink::~CsvSink(){if(fd_>=0)::close(fd_);}
void CsvSink::open(){
 // Safer than the baseline: refuse symlink/nonregular destinations. Existing
 // regular logs retain permissions; newly created files are private.
 int descriptor=::open(filename_.c_str(),O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
 if(descriptor<0)throw std::runtime_error("CSV open failed");
 struct stat info{};
 if(fstat(descriptor,&info)<0||!S_ISREG(info.st_mode)){::close(descriptor);throw std::runtime_error("CSV destination is not a regular file");}
 if(private_only_&&((info.st_mode&077)||info.st_nlink!=1||info.st_uid!=geteuid())){::close(descriptor);throw std::runtime_error("unsafe private log permissions");}
 fd_=descriptor;
}
void CsvSink::rotate(){
 ::close(fd_);fd_=-1;
 for(unsigned i=backups_;i>1;--i){
  auto from=filename_+'.'+std::to_string(i-1),to=filename_+'.'+std::to_string(i);
  if(std::filesystem::exists(from)){std::filesystem::remove(to);std::filesystem::rename(from,to);}
 }
 auto first=filename_+".1";std::filesystem::remove(first);std::filesystem::rename(filename_,first);open();
}
bool CsvSink::write(std::string_view record)noexcept{
 try{
  if(record.size()>1024*1024)throw std::length_error("CSV record exceeds safety bound");
  if(fd_<0)open();
  auto position=lseek(fd_,0,SEEK_END);if(position<0)throw std::runtime_error("CSV seek failed");
  // Python compares byte stream position with len(unicode_record + newline),
  // NOT encoded byte length. csv_result supplies valid UTF-8 strings.
  std::uint64_t characters=1;for(unsigned char c:record)if((c&0xc0)!=0x80)++characters;
  if(static_cast<std::uint64_t>(position)+characters>=max_bytes_)rotate();
  std::string line(record);line+='\n';std::string_view remaining=line;
  while(!remaining.empty()){
   auto n=::write(fd_,remaining.data(),remaining.size());
   if(n<0&&errno==EINTR)continue;
   if(n<=0)throw std::runtime_error("CSV write failed");
   remaining.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
 }catch(...){++errors_;return false;}
}
}
