#pragma once

#include <atomic>
#include <WiFi.h>

// Exactly one AI HTTP worker (prefetch or whole-book cache) at a time.
// The foreground translation checks this lease before allocating TLS.
namespace ai_http_gate {
inline std::atomic<bool> active{false};
inline std::atomic<bool> shutdownWifiAfterRelease{false};

inline bool busy() { return active.load(std::memory_order_acquire); }
inline bool acquire() {
  bool expected = false;
  return active.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
}
inline void requestWifiShutdown() {
  shutdownWifiAfterRelease.store(true, std::memory_order_release);
}
inline void release() {
  if (shutdownWifiAfterRelease.exchange(false, std::memory_order_acq_rel)) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
  active.store(false, std::memory_order_release);
}
}  // namespace ai_http_gate
