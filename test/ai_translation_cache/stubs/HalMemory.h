#pragma once
#include <cstddef>
struct TestHeapStats {
  size_t freeBytes = 1024 * 1024;
  size_t largestBlockBytes = 1024 * 1024;
};
class HalMemory {
 public:
  static TestHeapStats getDefaultHeap() { return {}; }
  static TestHeapStats getInternalHeap() { return {}; }
  static TestHeapStats getPsramHeap() { return {}; }
};
