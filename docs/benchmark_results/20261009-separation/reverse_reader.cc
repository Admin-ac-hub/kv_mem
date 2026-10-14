#include "db.h"
#include <cstdlib>
#include <iostream>
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  kv::Options options; options.db_path = argv[1]; options.vector_dimension = 8;
  kv::DB db(options); auto status = db.Open();
  if (!status.ok()) { std::cerr << status.ToString(); return 1; }
  kv::VectorRecord record;
  status=db.GetVector("k0", &record);
  if (!status.ok() || record.vector != std::vector<float>({1,2,3,4,5,6,7,8})) return 1;
  std::vector<kv::VectorResult> results; status=db.Search({1,2,3,4,5,6,7,8},1,&results);
  if (!status.ok() || results.size()!=1 || results[0].key!="k0") return 1;
  std::cout << "pre-separation binary read new ordinary-Put/SSTable/Manifest database successfully\n";
  return db.Close().ok()?0:1;
}
