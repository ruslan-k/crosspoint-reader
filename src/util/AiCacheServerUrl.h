#pragma once

#include <string>
#include <string_view>

namespace ai_cache_server {

// Construct URL without logging or exposing the token via the settings GET API.
// A base URL may be the server origin or the /api/cache Funnel prefix, with
// optional existing query parameters that must remain after the resource path.
inline std::string buildRequestUrl(std::string_view base, std::string_view resource,
                                   std::string_view sha, std::string_view token) {
  const size_t question = base.find('?');
  std::string result(base.substr(0, question));
  if (!result.empty() && result.back() == '/') result.pop_back();
  if (result.size() < 10 || result.compare(result.size() - 10, 10, "/api/cache") != 0)
    result += "/api/cache";
  result.append(resource.data(), resource.size());

  if (!sha.empty()) {
    result += "?sha=";
    result.append(sha.data(), sha.size());
  }
  if (question != std::string_view::npos && question + 1 < base.size()) {
    result += sha.empty() ? "?" : "&";
    result.append(base.data() + question + 1, base.size() - question - 1);
  }
  if (!token.empty()) {
    result += result.find('?') == std::string::npos ? "?" : "&";
    result += "token=";
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : token) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
        result += static_cast<char>(c);
      } else {
        result += '%';
        result += hex[c >> 4];
        result += hex[c & 15];
      }
    }
  }
  return result;
}
}  // namespace ai_cache_server
