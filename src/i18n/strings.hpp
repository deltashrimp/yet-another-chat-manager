#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace yacm::i18n {

// Message catalog. Translations can be loaded from JSON files
// (i18n/<lang>.json) next to the binary; built-in en/ru defaults are
// always available. Missing keys fall back to English, then to the key
// itself.
class Strings {
public:
    explicit Strings(std::string default_lang);

    // Loads translations from <dir>/<lang>.json for every supported lang.
    // Missing files are skipped; existing keys override built-ins.
    void load_dir(const std::string& dir);

    [[nodiscard]] std::string t(const std::string& lang, const std::string& key) const;
    [[nodiscard]] std::string t(const std::string& key) const;  // default lang

    [[nodiscard]] bool is_supported(const std::string& lang) const;
    [[nodiscard]] std::vector<std::string> supported_langs() const;
    [[nodiscard]] const std::string& default_lang() const { return default_lang_; }

private:
    void add_builtin_en();
    void add_builtin_ru();

    std::string default_lang_;
    std::unordered_map<std::string, std::map<std::string, std::string>> data_;
    std::vector<std::string> supported_langs_;
};

}  // namespace yacm::i18n