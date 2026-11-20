#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Zero-heap JSON construction into a caller-owned fixed-capacity buffer.
// Used with SPIRAM-backed request/prompt buffers on Picco: no large temporary
// std::string or ArduinoJson document is required during TLS preflight.
namespace ai_prefetch {
class BoundedJsonWriter {
 public:
  BoundedJsonWriter(char* data, size_t capacity) : data_(data), capacity_(capacity) {}

  bool append(std::string_view text) {
    if (!data_ || used_ > capacity_ || text.size() > capacity_ - used_) return false;
    if (!text.empty()) memcpy(data_ + used_, text.data(), text.size());
    used_ += text.size();
    return true;
  }

  bool integer(unsigned value) {
    char digits[12];
    size_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value && n < sizeof(digits));
    if (value) return false;
    while (n) {
      if (!append(std::string_view(digits + --n, 1))) return false;
    }
    return true;
  }

  bool quoted(std::string_view text) {
    if (!append("\"")) return false;
    static constexpr char HEX[] = "0123456789abcdef";
    for (unsigned char byte : text) {
      switch (byte) {
        case '"':
          if (!append("\\\"")) return false;
          break;
        case '\\':
          if (!append("\\\\")) return false;
          break;
        case '\b':
          if (!append("\\b")) return false;
          break;
        case '\f':
          if (!append("\\f")) return false;
          break;
        case '\n':
          if (!append("\\n")) return false;
          break;
        case '\r':
          if (!append("\\r")) return false;
          break;
        case '\t':
          if (!append("\\t")) return false;
          break;
        default:
          if (byte < 0x20) {
            const char escape[6] = {'\\', 'u', '0', '0', HEX[(byte >> 4) & 15], HEX[byte & 15]};
            if (!append(std::string_view(escape, sizeof(escape)))) return false;
          } else if (!append(std::string_view(reinterpret_cast<const char*>(&byte), 1))) {
            return false;
          }
      }
    }
    return append("\"");
  }

  size_t size() const { return used_; }
  std::string_view view() const { return std::string_view(data_, used_); }

 private:
  char* data_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
};
}  // namespace ai_prefetch
