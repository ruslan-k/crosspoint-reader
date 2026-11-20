#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

// Split HTTP SSE data lines and frame model-emitted JSON objects independently
// of TCP packet boundaries, SSE chunk boundaries, braces inside JSON strings,
// and arbitrary Markdown fences. Callbacks run synchronously on the Translate
// modal task; the caller owns both bounded buffers (PSRAM on Picco).
namespace ai_prefetch {

class StreamFramer {
 public:
  StreamFramer(char* eventBuffer, size_t eventCapacity, char* objectBuffer, size_t objectCapacity)
      : event_(eventBuffer), eventCap_(eventCapacity), object_(objectBuffer), objectCap_(objectCapacity) {}

  template <typename OnEvent>
  bool feedSse(const char* bytes, size_t count, OnEvent&& onEvent) {
    if (!event_ || eventCap_ < 64 || !bytes) return false;
    for (size_t i = 0; i < count; ++i) {
      const char ch = bytes[i];
      if (ch == '\n') {
        size_t length = eventUsed_;
        if (length > 0 && event_[length - 1] == '\r') --length;
        const std::string_view line(event_, length);
        if (line.size() >= 5 && line.substr(0, 5) == "data:") {
          size_t offset = 5;
          if (offset < line.size() && line[offset] == ' ') ++offset;
          const auto value = line.substr(offset);
          if (!value.empty() && value != "[DONE]") onEvent(value);
          if (value == "[DONE]") finished_ = true;
        }
        eventUsed_ = 0;
      } else {
        if (eventUsed_ >= eventCap_) return false;
        event_[eventUsed_++] = ch;
      }
    }
    return true;
  }

  template <typename OnObject>
  bool feedModel(const char* bytes, size_t count, OnObject&& onObject) {
    if (!object_ || objectCap_ < 64 || !bytes) return false;
    for (size_t i = 0; i < count; ++i) {
      const char ch = bytes[i];
      if (!depth_) {
        if (ch != '{') continue;
        objectUsed_ = 0;
        depth_ = 1;
        insideString_ = false;
        escaped_ = false;
      } else if (insideString_) {
        if (escaped_) {
          escaped_ = false;
        } else if (ch == '\\') {
          escaped_ = true;
        } else if (ch == '"') {
          insideString_ = false;
        }
      } else if (ch == '"') {
        insideString_ = true;
      } else if (ch == '{') {
        ++depth_;
        if (depth_ > 32) return false;
      } else if (ch == '}') {
        --depth_;
      }
      if (objectUsed_ >= objectCap_) return false;
      object_[objectUsed_++] = ch;
      if (!depth_) {
        onObject(std::string_view(object_, objectUsed_));
        objectUsed_ = 0;
      }
    }
    return true;
  }

  bool finished() const { return finished_; }
  bool hasIncompleteObject() const { return depth_ != 0; }

 private:
  char* event_ = nullptr;
  size_t eventCap_ = 0;
  size_t eventUsed_ = 0;
  char* object_ = nullptr;
  size_t objectCap_ = 0;
  size_t objectUsed_ = 0;
  unsigned depth_ = 0;
  bool insideString_ = false;
  bool escaped_ = false;
  bool finished_ = false;
};

}  // namespace ai_prefetch
