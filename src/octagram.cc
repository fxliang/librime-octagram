#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"
#include <algorithm>
#include <filesystem>
#include <rime/config.h>
#if __has_include(<rime/namespace_resource_utils.h>)
#include <rime/namespace_resource_utils.h>
#define RIME_HAS_NAMESPACE_RESOURCE_UTILS 1
#else
#define RIME_HAS_NAMESPACE_RESOURCE_UTILS 0
#endif
#include <rime/resource.h>
#include <rime/service.h>
#include <utf8.h>

namespace rime {

struct GrammarConfig {
  int collocation_max_length = 4;
  int collocation_min_length = 3;
  double collocation_penalty = -12;
  double non_collocation_penalty = -12;
  double weak_collocation_penalty = -24;
  double rear_penalty = -18;
};

const ResourceType kGramDbType = {"gram_db", "", ".gram"};
const string kGrammarDefaultLanguage = "zh-hant";

Octagram::Octagram(Config* config, OctagramComponent* component)
    : config_(std::make_unique<GrammarConfig>()) {
  string language;
  if (config) {
    if (config->GetString("grammar/language", &language)) {
      LOG(INFO) << "use grammar: " << language;
    } else {
      return;
    }
    config->GetInt("grammar/collocation_max_length",
                  &config_->collocation_max_length);
    config->GetInt("grammar/collocation_min_length",
                  &config_->collocation_min_length);
    config->GetDouble("grammar/collocation_penalty",
                     &config_->collocation_penalty);
    config->GetDouble("grammar/non_collocation_penalty",
                     &config_->non_collocation_penalty);
    config->GetDouble("grammar/weak_collocation_penalty",
                     &config_->weak_collocation_penalty);
    config->GetDouble("grammar/rear_penalty",
                     &config_->rear_penalty);
  }
  if (!language.empty()) {
    db_ = component->GetDb(component->ResolveLanguageResourceId(config, language));
  }
}

Octagram::~Octagram() {}

inline static double scale_value(int value) {
  return value >= 0 ? double(value) / GramDb::kValueScale : -1;
}

inline static bool update_result(double& result, double new_value) {
  if (new_value > result) {
    result = new_value;
    return true;
  }
  return false;
}

inline static const char* str_begin(const string& str) {
  return str.c_str();
}

inline static const char* str_end(const string& str) {
  return str.c_str() + str.length();
}

inline static const char* last_n_unicode(const string& str,
                                         int max,
                                         int& out_count) {
  const char* begin = str_begin(str);
  const char* p = str_end(str);
  out_count = 0;
  while (p != begin && out_count < max) {
    utf8::unchecked::prior(p);
    ++out_count;
  }
  return p;
}

inline static const char* first_n_unicode(const string& str,
                                          int max,
                                          int& out_count) {
  const char* p = str_begin(str);
  const char* end = str_end(str);
  out_count = 0;
  while (p != end && out_count < max) {
    utf8::unchecked::next(p);
    ++out_count;
  }
  return p;
}

inline static bool matches_whole_query(const char* context_ptr,
                                       const string& context_query,
                                       size_t match_length,
                                       const string& word_query) {
  return context_ptr == str_begin(context_query) &&
      match_length == word_query.length();
}

double Octagram::Query(const string& context,
                       const string& word,
                       bool is_rear) {
  if (!db_ || context.empty()) {
    return config_->non_collocation_penalty;
  }
  double result = config_->non_collocation_penalty;
  GramDb::Match matches[GramDb::kMaxResults];
  int n = (std::min)(grammar::kMaxEncodedUnicode,
                     config_->collocation_max_length - 1);
  int context_len = 0;
  string context_query = grammar::encode(
      last_n_unicode(context, n, context_len),
      str_end(context));
  int word_query_len = 0;
  string word_query = grammar::encode(
      str_begin(word),
      first_n_unicode(word, n, word_query_len));
  for (const char* context_ptr = str_begin(context_query);
       context_len > 0;
       --context_len, context_ptr = grammar::next_unicode(context_ptr)) {
    int num_results = db_->Lookup(context_ptr, word_query, matches);
    DLOG(INFO) << "Lookup(" << context_ptr << " + " << word_query << ") returns "
               << num_results << " results";
    for (auto i = 0; i < num_results; ++i) {
      const auto& match(matches[i]);
      const int match_len = grammar::unicode_length(word_query, match.length);
      DLOG(INFO) << "match[" << match.length << "] = "
                 << scale_value(match.value);
      const int collocation_len = context_len + match_len;
      if (update_result(result,
                        scale_value(match.value) +
                        (collocation_len >= config_->collocation_min_length ||
                         matches_whole_query(context_ptr, context_query,
                                             match.length, word_query)
                         ? config_->collocation_penalty
                         : config_->weak_collocation_penalty))) {
        DLOG(INFO) << "update: " << context << "[" << context_len << "] + "
                   << word << "[" << match_len << "] = " << result;
      }
    }
  }
  if (is_rear) {
    int word_len = utf8::unchecked::distance(word.c_str(),
                                             word.c_str() + word.length());
    if (word_query_len == word_len &&
        db_->Lookup(word_query, "$", matches) > 0 &&
        update_result(result,
                      scale_value(matches[0].value) + config_->rear_penalty)) {
      DLOG(INFO) << "update: " << word << "$ / " << result;
    }
  }
  DLOG(INFO) << "context = " << context << ", word = " << word
             << " / " << result;
  return result;
}

OctagramComponent::OctagramComponent() {}

OctagramComponent::~OctagramComponent() {}

Octagram* OctagramComponent::Create(Config* config) {
  return new Octagram(config, this);
}

string OctagramComponent::ResolveLanguageResourceId(
    Config* config,
    const string& language) const {
#if !RIME_HAS_NAMESPACE_RESOURCE_UTILS
  return language;
#else
  string schema_id;
  if (config) {
    config->GetString("schema/schema_id", &schema_id);
  }
  const bool namespace_only = NamespaceResourcesOnlyFromConfig(config);
  the<ResourceResolver> deployed_resolver(
      Service::instance().CreateDeployedResourceResolver(kGramDbType));
  the<ResourceResolver> source_resolver(
      Service::instance().CreateResourceResolver(kGramDbType));
  const auto candidates = BuildSchemaScopedResourceCandidates(
      language, schema_id, config, namespace_only,
      /*allow_default_namespace_fallback=*/true,
      /*allow_parent_path_namespace_candidates=*/false);
  string resolved_language;
  if (ResolveFirstExistingResourcePath(candidates, deployed_resolver.get(),
                                       source_resolver.get(),
                                       &resolved_language)) {
    return resolved_language;
  }
  return candidates.empty() ? language : candidates.front();
#endif
}

GramDb* OctagramComponent::GetDb(const string& language) {
  if (unavailable_languages_.find(language) != unavailable_languages_.end()) {
    return nullptr;
  }
  auto& loaded = db_by_language_[language];
  if (!loaded) {
    the<ResourceResolver> deployed_resolver(
        Service::instance().CreateDeployedResourceResolver(kGramDbType));
    the<ResourceResolver> source_resolver(
        Service::instance().CreateResourceResolver(kGramDbType));
    const auto deployed_path = deployed_resolver->ResolvePath(language);
    const auto source_path = source_resolver->ResolvePath(language);
    path file_path;
    if (std::filesystem::exists(deployed_path)) {
      file_path = deployed_path;
    } else if (std::filesystem::exists(source_path)) {
      file_path = source_path;
    } else {
      unavailable_languages_.insert(language);
      LOG(ERROR) << "failed to find grammar database: " << language
                 << "; searched '" << deployed_path << "' and '"
                 << source_path << "'.";
      return nullptr;
    }
    the<GramDb> db = std::make_unique<GramDb>(file_path);
    if (!db->Load()) {
      unavailable_languages_.insert(language);
      LOG(ERROR) << "failed to load grammar database: " << language;
      return nullptr;
    }
    loaded = std::move(db);
  }
  return loaded.get();
}

}  // namespace rime
