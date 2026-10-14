#include "status.h"

namespace kv {

std::string Status::ToString() const {
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      return "NotFound: " + message_;
    case Code::kIOError:
      return "IOError: " + message_;
    case Code::kCorruption:
      return "Corruption: " + message_;
    case Code::kInvalidArgument:
      return "InvalidArgument: " + message_;
  }
  return "Unknown";
}

}  // namespace kv
