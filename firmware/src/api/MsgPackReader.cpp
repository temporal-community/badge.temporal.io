#include "MsgPackReader.h"

#include <string.h>

namespace MsgPack {

size_t Reader::remaining() const {
  if (!cur_ || !end_ || cur_ > end_) return 0;
  return static_cast<size_t>(end_ - cur_);
}

bool Reader::atEnd() const {
  return remaining() == 0;
}

bool Reader::readU8(uint8_t& out) {
  if (remaining() < 1) return false;
  out = *cur_++;
  return true;
}

bool Reader::readU16(uint16_t& out) {
  if (remaining() < 2) return false;
  out = (static_cast<uint16_t>(cur_[0]) << 8) |
        static_cast<uint16_t>(cur_[1]);
  cur_ += 2;
  return true;
}

bool Reader::readU32(uint32_t& out) {
  if (remaining() < 4) return false;
  out = (static_cast<uint32_t>(cur_[0]) << 24) |
        (static_cast<uint32_t>(cur_[1]) << 16) |
        (static_cast<uint32_t>(cur_[2]) << 8) |
        static_cast<uint32_t>(cur_[3]);
  cur_ += 4;
  return true;
}

bool Reader::readU64(uint64_t& out) {
  if (remaining() < 8) return false;
  out = 0;
  for (uint8_t i = 0; i < 8; i++) {
    out = (out << 8) | cur_[i];
  }
  cur_ += 8;
  return true;
}

bool Reader::skipBytes(size_t len) {
  if (remaining() < len) return false;
  cur_ += len;
  return true;
}

bool Reader::readNil() {
  if (remaining() < 1 || *cur_ != 0xC0) return false;
  cur_++;
  return true;
}

bool Reader::readBool(bool& out) {
  uint8_t marker = 0;
  if (!readU8(marker)) return false;
  if (marker == 0xC2) {
    out = false;
    return true;
  }
  if (marker == 0xC3) {
    out = true;
    return true;
  }
  return false;
}

bool Reader::readUInt(uint64_t& out) {
  uint8_t marker = 0;
  if (!readU8(marker)) return false;
  if (marker <= 0x7F) {
    out = marker;
    return true;
  }
  if (marker == 0xCC) {
    uint8_t v = 0;
    if (!readU8(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xCD) {
    uint16_t v = 0;
    if (!readU16(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xCE) {
    uint32_t v = 0;
    if (!readU32(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xCF) {
    return readU64(out);
  }
  return false;
}

bool Reader::readInt(int64_t& out) {
  uint8_t marker = 0;
  if (!readU8(marker)) return false;
  if (marker <= 0x7F) {
    out = marker;
    return true;
  }
  if (marker >= 0xE0) {
    out = static_cast<int8_t>(marker);
    return true;
  }
  if (marker == 0xCC) {
    uint8_t v = 0;
    if (!readU8(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xCD) {
    uint16_t v = 0;
    if (!readU16(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xCE) {
    uint32_t v = 0;
    if (!readU32(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xD0) {
    uint8_t v = 0;
    if (!readU8(v)) return false;
    out = static_cast<int8_t>(v);
    return true;
  }
  if (marker == 0xD1) {
    uint16_t v = 0;
    if (!readU16(v)) return false;
    out = static_cast<int16_t>(v);
    return true;
  }
  if (marker == 0xD2) {
    uint32_t v = 0;
    if (!readU32(v)) return false;
    out = static_cast<int32_t>(v);
    return true;
  }
  if (marker == 0xD3) {
    uint64_t v = 0;
    if (!readU64(v)) return false;
    out = static_cast<int64_t>(v);
    return true;
  }
  if (marker == 0xCF) {
    uint64_t v = 0;
    if (!readU64(v)) return false;
    out = static_cast<int64_t>(v);
    return true;
  }
  return false;
}

bool Reader::readArray(uint32_t& out) {
  uint8_t marker = 0;
  if (!readU8(marker)) return false;
  if ((marker & 0xF0) == 0x90) {
    out = marker & 0x0F;
    return true;
  }
  if (marker == 0xDC) {
    uint16_t v = 0;
    if (!readU16(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xDD) {
    return readU32(out);
  }
  return false;
}

bool Reader::readMap(uint32_t& out) {
  uint8_t marker = 0;
  if (!readU8(marker)) return false;
  if ((marker & 0xF0) == 0x80) {
    out = marker & 0x0F;
    return true;
  }
  if (marker == 0xDE) {
    uint16_t v = 0;
    if (!readU16(v)) return false;
    out = v;
    return true;
  }
  if (marker == 0xDF) {
    return readU32(out);
  }
  return false;
}

bool Reader::readBytes(Bytes& out) {
  uint8_t marker = 0;
  if (!readU8(marker)) return false;

  uint32_t len = 0;
  if ((marker & 0xE0) == 0xA0) {
    len = marker & 0x1F;
  } else if (marker == 0xD9 || marker == 0xC4) {
    uint8_t v = 0;
    if (!readU8(v)) return false;
    len = v;
  } else if (marker == 0xDA || marker == 0xC5) {
    uint16_t v = 0;
    if (!readU16(v)) return false;
    len = v;
  } else if (marker == 0xDB || marker == 0xC6) {
    if (!readU32(len)) return false;
  } else {
    return false;
  }

  if (remaining() < len) return false;
  out.data = cur_;
  out.len = len;
  cur_ += len;
  return true;
}

bool Reader::readString(char* out, size_t cap) {
  Bytes bytes;
  if (!readBytes(bytes)) return false;
  if (out && cap > 0) {
    const size_t n = (bytes.len < cap - 1) ? bytes.len : cap - 1;
    if (n > 0) memcpy(out, bytes.data, n);
    out[n] = '\0';
  }
  return true;
}

bool Reader::skip(uint8_t depth) {
  if (depth > 16) return false;
  uint8_t marker = 0;
  if (!readU8(marker)) return false;

  if (marker <= 0x7F || marker >= 0xE0 || marker == 0xC0 ||
      marker == 0xC2 || marker == 0xC3) {
    return true;
  }
  if ((marker & 0xE0) == 0xA0) return skipBytes(marker & 0x1F);
  if ((marker & 0xF0) == 0x90) {
    const uint32_t n = marker & 0x0F;
    for (uint32_t i = 0; i < n; i++) {
      if (!skip(depth + 1)) return false;
    }
    return true;
  }
  if ((marker & 0xF0) == 0x80) {
    const uint32_t n = marker & 0x0F;
    for (uint32_t i = 0; i < n * 2; i++) {
      if (!skip(depth + 1)) return false;
    }
    return true;
  }

  uint8_t u8 = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  switch (marker) {
    case 0xC4:
    case 0xD9:
      return readU8(u8) && skipBytes(u8);
    case 0xC5:
    case 0xDA:
      return readU16(u16) && skipBytes(u16);
    case 0xC6:
    case 0xDB:
      return readU32(u32) && skipBytes(u32);
    case 0xCC:
    case 0xD0:
      return skipBytes(1);
    case 0xCD:
    case 0xD1:
      return skipBytes(2);
    case 0xCE:
    case 0xD2:
    case 0xCA:
      return skipBytes(4);
    case 0xCF:
    case 0xD3:
    case 0xCB:
      return skipBytes(8);
    case 0xDC:
      if (!readU16(u16)) return false;
      for (uint32_t i = 0; i < u16; i++) {
        if (!skip(depth + 1)) return false;
      }
      return true;
    case 0xDD:
      if (!readU32(u32)) return false;
      for (uint32_t i = 0; i < u32; i++) {
        if (!skip(depth + 1)) return false;
      }
      return true;
    case 0xDE:
      if (!readU16(u16)) return false;
      for (uint32_t i = 0; i < static_cast<uint32_t>(u16) * 2; i++) {
        if (!skip(depth + 1)) return false;
      }
      return true;
    case 0xDF:
      if (!readU32(u32)) return false;
      for (uint32_t i = 0; i < u32 * 2; i++) {
        if (!skip(depth + 1)) return false;
      }
      return true;
    default:
      return false;
  }
}

}  // namespace MsgPack
