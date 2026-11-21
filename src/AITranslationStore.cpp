#include "AITranslationStore.h"

#include <ObfuscationUtils.h>

#include <cstring>

void AITranslationStore::toJson(JsonDocument& doc) const {
  doc["url"] = url_;
  doc["model"] = model_;
  doc["reasoningEffort"] = reasoningEffort_;
  // Device-MAC XOR obfuscation is only protection from casual SD inspection,
  // not encryption. The web API never returns this value.
  doc["key_obf"] = obfuscation::obfuscateToBase64(key_);
}

bool AITranslationStore::fromJson(JsonVariantConst doc) {
  const char* url = doc["url"] | "";
  const char* model = doc["model"] | "";
  const char* effort = doc["reasoningEffort"] | "default";
  const char* encoded = doc["key_obf"] | "";
  if (strlen(url) > 255 || strlen(model) > 96) return false;
  url_ = url;
  model_ = model;
  // Old files have no effort field. Invalid stored values safely revert to provider default.
  reasoningEffort_ = validReasoningEffort(effort) ? effort : "default";
  key_.clear();
  if (encoded[0]) {
    bool valid = false;
    bool tooLong = false;
    key_ = obfuscation::deobfuscateFromBase64(encoded, 512, &valid, &tooLong);
    if (!valid || tooLong) key_.clear();
  }
  return true;
}

bool AITranslationStore::configure(const std::string& url, const std::string& model, const std::string& key,
                                   const bool replaceKey, const std::string& reasoningEffort) {
  if (!validReasoningEffort(reasoningEffort)) return false;
  url_ = url;
  model_ = model;
  reasoningEffort_ = reasoningEffort;
  if (replaceKey) key_ = key;
  return saveToFile();
}
