#include "compression.h"

namespace kv {

namespace {

class CopyCodec : public CompressionCodec {
 public:
  explicit CopyCodec(CompressionType type) : type_(type) {}

  CompressionType Type() const override {
    return type_;
  }

  Status Compress(std::string_view input, std::string* output) const override {
    output->assign(input.data(), input.size());
    return Status::OK();
  }

  Status Uncompress(std::string_view input, std::string* output) const override {
    output->assign(input.data(), input.size());
    return Status::OK();
  }

 private:
  CompressionType type_;
};

}  // namespace

std::unique_ptr<CompressionCodec> NewCompressionCodec(CompressionType type) {
  switch (type) {
    case CompressionType::kNone:
    case CompressionType::kFake:
      return std::make_unique<CopyCodec>(type);
  }
  return nullptr;
}

}  // namespace kv
