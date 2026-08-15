#pragma once

#include <string>
#include <utility>

namespace kv {

class Status {
 public:
  Status() : code_(Code::kOk) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string message) {
    return Status(Code::kNotFound, std::move(message));
  }
  static Status IOError(std::string message) {
    return Status(Code::kIOError, std::move(message));
  }
  static Status Corruption(std::string message) {
    return Status(Code::kCorruption, std::move(message));
  }
  static Status InvalidArgument(std::string message) {
    return Status(Code::kInvalidArgument, std::move(message));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  std::string ToString() const;

 private:
  enum class Code {
    kOk,
    kNotFound,
    kIOError,
    kCorruption,
    kInvalidArgument,
  };

  Status(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  Code code_;
  std::string message_;
};

}  // namespace kv
