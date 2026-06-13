#pragma once

#include <string>

#include "status.h"
#include "types.h"

namespace kv {

class InternalIterator {
 public:
  virtual ~InternalIterator() = default;

  virtual void SeekToFirst() = 0;
  virtual void Seek(const std::string& target) = 0;
  virtual void Next() = 0;
  virtual bool Valid() const = 0;
  virtual const std::string& key() const = 0;
  virtual const std::string& value() const = 0;
  virtual SequenceNumber sequence() const = 0;
  virtual bool deleted() const = 0;
  virtual Status status() const = 0;
};

}  // namespace kv
