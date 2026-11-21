#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <string_view>

// Single user-configured translation provider. Credentials never enter the
// generic /api/settings response, and the backing file is a protected path.
class AITranslationStore : public PersistableStore<AITranslationStore> {
 public:
  static const char* getFilePath() { return "/.crosspoint/ai-translation.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool configure(const std::string& url, const std::string& model, const std::string& key, bool replaceKey,
                 const std::string& reasoningEffort, const std::string& cacheServerUrl);
  static constexpr bool validReasoningEffort(std::string_view effort) {
    return effort == "default" || effort == "low" || effort == "medium" || effort == "high";
  }
  const std::string& getUrl() const { return url_; }
  const std::string& getModel() const { return model_; }
  const std::string& getReasoningEffort() const { return reasoningEffort_; }
  const std::string& getCacheServerUrl() const { return cacheServerUrl_; }
  const std::string& getKey() const { return key_; }
  bool hasKey() const { return !key_.empty(); }
  bool configured() const { return !url_.empty() && !model_.empty() && hasKey(); }

 private:
  AITranslationStore() = default;
  friend class PersistableStore<AITranslationStore>;
  std::string url_;
  std::string model_;
  std::string reasoningEffort_ = "default";
  std::string cacheServerUrl_;
  std::string key_;
};

#define AI_TRANSLATION_STORE AITranslationStore::getInstance()
