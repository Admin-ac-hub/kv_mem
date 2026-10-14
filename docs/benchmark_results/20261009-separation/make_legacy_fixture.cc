#include "db.h"
#include "format.h"
#include <cstdlib>
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  kv::Options o; o.db_path = argv[1]; o.vector_dimension = 3;
  kv::DB db(o);
  auto check = [](kv::Status s) { if (!s.ok()) std::abort(); };
  check(db.Open());
  check(db.PutVector("a", {1,0,0}, "old-a"));
  check(db.PutVector("b", {0,1,0}, "old-b"));
  check(db.Put("plain", "old-plain"));
  check(db.Close());
  kv::Manifest m(o.db_path); check(m.Load());
  kv::WriteBatch b;
  check(b.PutVector("a", {0,0,2}, "new-a"));
  b.Delete("b"); b.Put("plain", "new-plain");
  check(b.PutVector("c", {3,0,0}, "temporary"));
  b.Delete("c"); check(b.PutVector("c", {0,3,0}, "new-c"));
  kv::WALWriter w(o.db_path / kv::WALFileName(m.WALFileNumber()));
  check(w.Open()); check(w.AppendBatch(b, m.LastSequence()+1));
}
