#include "vector_db.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>
using Clock = std::chrono::steady_clock;
template<class T> void Check(const T& result) { if (!result.ok()) { std::cerr << result.ToString(); std::abort(); } }
int main(int argc, char** argv) {
  if (argc != 2) return 1;
  kv::VectorOptions options; options.kv_options.db_path = argv[1];
  options.vector_dimension = 8; options.kv_options.memtable_entries_limit = 2000;
  std::filesystem::remove_all(options.kv_options.db_path);
  kv::VectorDB db(options); Check(db.Open());
  kv::VectorWriteBatch batch;
  for (int i=0; i<1000; ++i) {
    std::vector<float> vector(8); for (int d=0;d<8;++d) vector[d]=std::sin((i+1)*(d+1)*0.17);
    Check(batch.PutVector("k"+std::to_string(i), vector));
  }
  Check(db.Write(batch)); Check(db.Close());
  for (int run=1; run<=3; ++run) {
    auto start = Clock::now(); Check(db.Open());
    auto open_ms = std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    const auto* snapshot = db.GetSnapshot();
    Check(db.PutVector("k0", {1,2,3,4,5,6,7,8}));
    std::vector<std::vector<float>> queries(8, std::vector<float>(8,0.5));
    std::vector<std::vector<kv::VectorResult>> results;
    start=Clock::now(); Check(db.SearchBatch(queries,10,64,&results,kv::ReadOptions{snapshot}));
    auto historical_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    if (results.size()!=8 || results[0].size()!=10) std::abort();
    start=Clock::now(); Check(db.SearchBatch(queries,10,64,&results));
    auto current_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    std::cout << "run="<<run<<", vectors=1000, dimension=8, queries=8, open_ms="<<open_ms
              <<", historical_batch_ms="<<historical_ms<<", current_batch_ms="<<current_ms<<'\n';
    db.ReleaseSnapshot(snapshot); Check(db.Close());
  }
  std::filesystem::copy(options.kv_options.db_path, options.kv_options.db_path.string()+"_reverse",
                        std::filesystem::copy_options::recursive);
  std::filesystem::remove_all(options.kv_options.db_path);
}
