add_executable(ai_prefetch_cache
  AiPrefetchCacheTest.cpp
  ${REPO_ROOT}/src/util/AiPrefetchCache.cpp
)
target_include_directories(ai_prefetch_cache BEFORE PRIVATE
  ${REPO_ROOT}/test/ai_translation_cache/stubs
  ${REPO_ROOT}/src/util
)
target_link_libraries(ai_prefetch_cache PRIVATE GTest::gtest_main)
gtest_discover_tests(ai_prefetch_cache)

add_executable(ai_prefetch_stream AiPrefetchStreamTest.cpp)
target_include_directories(ai_prefetch_stream PRIVATE ${REPO_ROOT}/src/util)
target_link_libraries(ai_prefetch_stream PRIVATE GTest::gtest_main)
gtest_discover_tests(ai_prefetch_stream)

add_executable(ai_prefetch_writer AiBoundedJsonWriterTest.cpp)
target_include_directories(ai_prefetch_writer PRIVATE ${REPO_ROOT}/src/util)
target_link_libraries(ai_prefetch_writer PRIVATE GTest::gtest_main)
gtest_discover_tests(ai_prefetch_writer)
