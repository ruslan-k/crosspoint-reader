add_executable(ai_translation_cache
  AiTranslationCacheTest.cpp
  ${REPO_ROOT}/src/util/AiTranslationCache.cpp
)
target_include_directories(ai_translation_cache BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/util
)
target_link_libraries(ai_translation_cache PRIVATE GTest::gtest_main)
gtest_discover_tests(ai_translation_cache)
