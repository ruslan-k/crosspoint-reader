#pragma once

#include <string>

// Picco only. Best-effort whole-book cache service; never performs HTTP on
// the UI task. The reader calls tick() on idle passes, including while reading.
namespace ai_cache_server {
void watch(const std::string& bookPath, const std::string& baseUrl, const std::string& token);
void tick(bool otherWifiUser = false);
void pauseForForeground();
void adoptWifi();
bool active();
bool workerPending();
bool takeUpdated(const std::string& bookPath);
void cancel();
bool validBaseUrl(const std::string& url);
}  // namespace ai_cache_server
