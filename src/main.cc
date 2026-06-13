#include <iostream>

#include "db.h"

int main() {
  kv::DB db("demo_db");
  kv::Status status = db.Open();
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }

  status = db.Put("name", "kv");
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }

  std::string value;
  status = db.Get("name", &value);
  if (!status.ok()) {
    std::cerr << status.ToString() << '\n';
    return 1;
  }

  std::cout << "name=" << value << '\n';
  return 0;
}
