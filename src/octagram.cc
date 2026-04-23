#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"
#include <algorithm>
#include <filesystem>
#include <rime/config.h>
#include <rime/resource.h>
#include <rime/service.h>
#include <utf8.h>

namespace rime {

namespace {

bool IsRelativePathUnderRoot(const path& full_path, const path& root_path) {
  if (full_path.empty() || root_path.empty()) {
    return false;
  }
  const auto rel = std::filesystem::absolute(full_path).lexically_relative(
      std::filesystem::absolute(root_path));
  if (rel.empty()) {
    return false;
  }
  const auto rel_text = rel.generic_u8string();
  return rel_text != ".." && rel_text.rfind("../", 0) != 0;
}

string SchemaNamespace(Config* config) {
  if (!config) {
    return string();
  }
  string schema_id;
  if (config->GetString("schema/schema_id", &schema_id)) {
    auto ns = path(schema_id).parent_path();
    if (!ns.empty()) {
      return ns.generic_u8string();
    }
  }

  const auto& config_path = config->file_path();
  auto parent_path = config_path.parent_path();
  if (parent_path.empty()) {
    return string();
  }

  auto& deployer = Service::instance().deployer();
  const vector<path> roots = {deployer.staging_dir, deployer.prebuilt_data_dir,
                              deployer.user_data_dir, deployer.shared_data_dir};
  for (const auto& root : roots) {
    if (!IsRelativePathUnderRoot(config_path, root)) {
      continue;
    }
    auto relative_parent =
        std::filesystem::absolute(parent_path).lexically_relative(
            std::filesystem::absolute(root));
    if (!relative_parent.empty()) {
      auto relative_parent_text = relative_parent.generic_u8string();
      if (relative_parent_text != ".." &&
          relative_parent_text.rfind("../", 0) != 0) {
        return relative_parent_text;
      }
    }
  }
  return string();
}

bool NamespaceResourcesOnly(Config* config) {
  if (!config) {
    return false;
  }
  bool namespace_resources_only = false;
  return config->GetBool("schema/namespace_resources_only",
                         &namespace_resources_only) &&
         namespace_resources_only;
}

bool ExistsInResolver(ResourceResolver* resolver, const string& resource_id) {
  return resolver && std::filesystem::exists(resolver->ResolvePath(resource_id));
}

}  // namespace

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
  path language_path(language);
  if (language.empty() || language_path.is_absolute() ||
      language_path.has_parent_path()) {
    return language;
  }

  const auto schema_namespace = SchemaNamespace(config);
  if (schema_namespace.empty()) {
    return language;
  }

  const bool namespace_only = NamespaceResourcesOnly(config);
  the<ResourceResolver> deployed_resolver(
      Service::instance().CreateDeployedResourceResolver(kGramDbType));
  the<ResourceResolver> source_resolver(
      Service::instance().CreateResourceResolver(kGramDbType));

  vector<string> candidates;
  candidates.emplace_back(
      (path(schema_namespace) / language_path).generic_u8string());
  if (!namespace_only) {
    candidates.push_back(language);
  }
  for (const auto& candidate : candidates) {
    if (ExistsInResolver(deployed_resolver.get(), candidate) ||
        ExistsInResolver(source_resolver.get(), candidate)) {
      return candidate;
    }
  }
  return candidates.front();
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
