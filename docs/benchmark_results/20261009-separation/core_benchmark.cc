#include "db.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
using Clock = std::chrono::steady_clock;
void Check(kv::Status status) { if (!status.ok()) { std::cerr << status.ToString(); std::abort(); } }
int main(int argc,char**argv) {
  if(argc!=2) return 1;
  kv::Options options; options.db_path=argv[1]; options.memtable_entries_limit=256;
  options.level0_sstable_limit=4; options.level1_size_limit_bytes=65536;
  options.block_cache_capacity=64;
  std::filesystem::remove_all(options.db_path);
  kv::DB db(options); Check(db.Open());
  const std::string payload(128,'x');
  auto start=Clock::now();
  for(int first=0;first<10000;first+=32) {
    kv::WriteBatch batch;
    for(int i=first;i<first+32 && i<10000;++i) batch.Put("bench:"+std::to_string(i),payload);
    Check(db.Write(batch));
  }
  Check(db.Close());
  double write_s=std::chrono::duration<double>(Clock::now()-start).count();
  auto stats=db.Stats();
  Check(db.Open()); std::mt19937 random(20261009);
  start=Clock::now();
  for(int i=0;i<10000;++i) { std::string value; Check(db.Get("bench:"+std::to_string(random()%10000),&value)); if(value!=payload) std::abort(); }
  double read_s=std::chrono::duration<double>(Clock::now()-start).count();
  start=Clock::now(); size_t count=0;
  auto iterator=db.NewIterator();
  for(iterator->SeekToFirst();iterator->Valid();iterator->Next()) ++count;
  Check(iterator->status()); if(count!=10000) std::abort();
  double scan_s=std::chrono::duration<double>(Clock::now()-start).count();
  iterator.reset(); Check(db.Close());
  std::cout << "write_sec="<<write_s<<", read_sec="<<read_s<<", scan_sec="<<scan_s
            <<", flush_count="<<stats.flush_count<<", compaction_count="<<stats.compaction_count<<'\n';
  std::filesystem::remove_all(options.db_path);
}
