#pragma once

#include <stddef.h>
#include <stdint.h>

namespace MsgPack {

struct Bytes {
  const uint8_t* data = nullptr;
  size_t len = 0;
};

class Reader {
public:
  Reader(const uint8_t* data, size_t len) : cur_(data), end_(data ? data + len : nullptr) {}

  size_t remaining() const;
  bool atEnd() const;

  bool readNil();
  bool readBool(bool& out);
  bool readUInt(uint64_t& out);
  bool readInt(int64_t& out);
  bool readArray(uint32_t& out);
  bool readMap(uint32_t& out);
  bool readBytes(Bytes& out);
  bool readString(char* out, size_t cap);
  bool skip(uint8_t depth = 0);

private:
  bool readU8(uint8_t& out);
  bool readU16(uint16_t& out);
  bool readU32(uint32_t& out);
  bool readU64(uint64_t& out);
  bool skipBytes(size_t len);

  const uint8_t* cur_ = nullptr;
  const uint8_t* end_ = nullptr;
};

}  // namespace MsgPack
