// This deliberately includes only the two public core API headers.
#include "db.h"
#include "write_batch.h"

int main() {
  kv::Options options;
  options.db_path = std::filesystem::path(KV_TEST_DB_ROOT) / "core_consumer";
  std::filesystem::remove_all(options.db_path);
  kv::DB db(options);
  if (!db.Open().ok()) return 1;
  kv::WriteBatch batch;
  batch.Put("key", std::string("KVV1\0\xff", 6));
  if (!db.Write(batch).ok()) return 2;
  std::string value;
  if (!db.Get("key", &value).ok() || value != batch.Operations()[0].value) return 3;
  if (!db.Close().ok()) return 4;
  std::filesystem::remove_all(options.db_path);
}
