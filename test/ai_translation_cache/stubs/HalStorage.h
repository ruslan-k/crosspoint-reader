#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

class HalFile {
 public:
  bool open(const std::filesystem::path& path, std::ios::openmode mode) {
    file_.open(path, mode | std::ios::binary);
    return file_.is_open();
  }
  int read(void* destination, size_t count) {
    file_.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
    return static_cast<int>(file_.gcount());
  }
  size_t write(const void* bytes, size_t size) {
    file_.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    return file_.good() ? size : 0;
  }
  size_t fileSize() {
    const auto pos = file_.tellg();
    file_.seekg(0, std::ios::end);
    const auto end = file_.tellg();
    file_.seekg(pos);
    return static_cast<size_t>(end);
  }
  void flush() { file_.flush(); }
  bool close() { file_.close(); return true; }
 private:
  std::fstream file_;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  static const std::filesystem::path& root() {
    static const std::filesystem::path path = std::filesystem::temp_directory_path() / "crosspoint_ai_cache_unit";
    return path;
  }
  static std::filesystem::path hostPath(const std::string& path) {
    return root() / path.substr(path.find_first_not_of('/'));
  }
  void reset() {
    std::filesystem::remove_all(root());
    std::filesystem::create_directories(root());
  }
  bool exists(const char* path) { return std::filesystem::exists(hostPath(path)); }
  bool ensureDirectoryExists(const char* path) {
    std::error_code error;
    std::filesystem::create_directories(hostPath(path), error);
    return !error && std::filesystem::is_directory(hostPath(path));
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& f) {
    return f.open(hostPath(path), std::ios::in);
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& f) {
    return f.open(hostPath(path), std::ios::out | std::ios::trunc);
  }
  bool replaceFile(const char* temp, const char* destination) {
    std::error_code error;
    std::filesystem::rename(hostPath(temp), hostPath(destination), error);
    return !error;
  }
  bool remove(const char* path) {
    std::error_code error;
    return std::filesystem::remove(hostPath(path), error);
  }
};

#define Storage HalStorage::getInstance()
