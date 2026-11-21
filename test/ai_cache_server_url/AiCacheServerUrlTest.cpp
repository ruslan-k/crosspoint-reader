#include "util/AiCacheServerUrl.h"

#include <cassert>
#include <string>

using ai_cache_server::buildRequestUrl;

int main() {
  const std::string origin = "https://minisforum.tail36c157.ts.net";
  assert(buildRequestUrl(origin, "/status", "abc", "") ==
         origin + "/api/cache/status?sha=abc");
  assert(buildRequestUrl(origin, "/status", "abc", "a+b&=?% #") ==
         origin + "/api/cache/status?sha=abc&token=a%2Bb%26%3D%3F%25%20%23");
  assert(buildRequestUrl(origin, "/job", "", "tok") ==
         origin + "/api/cache/job?token=tok");
  assert(buildRequestUrl(origin + "?mode=fast", "/download", "abc", "t") ==
         origin + "/api/cache/download?sha=abc&mode=fast&token=t");
  assert(buildRequestUrl(origin + "/api/cache/?mode=fast", "/job", "", "t") ==
         origin + "/api/cache/job?mode=fast&token=t");
  assert(buildRequestUrl(origin + "/api/cache", "/download", "abc", "t") ==
         origin + "/api/cache/download?sha=abc&token=t");
  assert(buildRequestUrl(origin + "/?mode=fast", "/status", "abc", "") ==
         origin + "/api/cache/status?sha=abc&mode=fast");
}
