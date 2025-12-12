#include "Localization.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "logger.h"

namespace
{
const char* kLocalizationDirectory = "resource/localization";
const char* kDisplayNameKey = "_DisplayName";
const char* kDefaultLanguageCode = "en";

const std::unordered_map<std::string, std::string> kEmptyLanguage = {};

struct LanguageDefinition
{
        std::string code;
        std::string displayName;
        std::string explicitCode;
        std::unordered_map<std::string, std::string> strings;
        bool isFallback = false;
};

std::string Trim(const std::string& value)
{
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
                return "";
        }

        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
}

std::string DecodeXmlEntities(std::string value)
{
        const std::pair<std::string, std::string> replacements[] = {
                {"&lt;", "<"},
                {"&gt;", ">"},
                {"&amp;", "&"},
                {"&quot;", "\""},
                {"&apos;", "'"},
        };

        for (const auto& replacement : replacements)
        {
                size_t position = 0;
                while ((position = value.find(replacement.first, position)) != std::string::npos)
                {
                        value.replace(position, replacement.first.length(), replacement.second);
                        position += replacement.second.length();
                }
        }

        return value;
}

bool ParseResxFile(const std::filesystem::path& path, LanguageDefinition& outDefinition)
{
        std::ifstream file(path);
        if (!file.is_open())
        {
                LOG(1, "Failed to open localization file: %s", path.string().c_str());
                return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        // Use a character class that matches any character (including newlines) instead of dotall,
        // which is unsupported in the MSVC STL implementation used by this project.
        std::regex dataRegex(R"(<data\s+name=\"([^\"]+)\"[^>]*>\s*<value>([\s\S]*?)</value>)",
                std::regex_constants::icase);

        auto begin = std::sregex_iterator(content.begin(), content.end(), dataRegex);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it)
        {
            std::string key = Trim((*it)[1].str());
            std::string value = (*it)[2].str();

            value = DecodeXmlEntities(value);
            value = Trim(value);

            if (key == kDisplayNameKey)
            {
                    outDefinition.displayName = value;
                    continue;
            }

            if (key == "_LanguageCode")
            {
                    outDefinition.explicitCode = value;
                    continue;
            }

            outDefinition.strings.emplace(std::move(key), std::move(value));
        }

        if (outDefinition.displayName.empty())
        {
                outDefinition.displayName = outDefinition.code;
        }

        if (outDefinition.strings.empty())
        {
                LOG(1, "Localization file %s did not contain any <data> entries with values.", path.string().c_str());
                return false;
        }

        return true;
}

std::vector<LanguageDefinition> LoadResxLanguages()
{
        std::vector<LanguageDefinition> languages;
        std::filesystem::path localizationDir;

        const std::vector<std::filesystem::path> candidates = {
                std::filesystem::path(kLocalizationDirectory),
                std::filesystem::path("localization"),
        };

        for (const auto& candidate : candidates)
        {
                if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate))
                {
                        localizationDir = candidate;
                        break;
                }
        }

        if (localizationDir.empty())
        {
                LOG(1, "Localization directory not found: %s", std::filesystem::path(kLocalizationDirectory).string().c_str());
                return languages;
        }

        struct ResxEntry
        {
                std::filesystem::path path;
                std::string baseName;
                std::string culture;
                bool hasCulture;
        };

        std::vector<ResxEntry> files;
        files.reserve(8);

        const std::regex nameRegex(R"((.*?)(?:\.([A-Za-z0-9_-]+))?\.resx$)");

        for (const auto& entry : std::filesystem::directory_iterator(localizationDir))
        {
                if (!entry.is_regular_file() || entry.path().extension() != ".resx")
                {
                        continue;
                }

                const std::string filename = entry.path().filename().string();
                std::smatch match;
                if (!std::regex_match(filename, match, nameRegex))
                {
                        continue;
                }

                ResxEntry info;
                info.path = entry.path();
                info.baseName = match[1].str();
                info.hasCulture = match[2].matched;
                info.culture = info.hasCulture ? match[2].str() : std::string();
                files.push_back(std::move(info));
        }

        if (files.empty())
        {
                return languages;
        }

        std::string targetBaseName;
        for (const auto& file : files)
        {
                if (!file.hasCulture)
                {
                        targetBaseName = file.baseName;
                        break;
                }
        }

        if (targetBaseName.empty())
        {
                targetBaseName = files.front().baseName;
        }

        for (const auto& file : files)
        {
                if (file.baseName != targetBaseName)
                {
                        continue;
                }

                LanguageDefinition definition;
                definition.code = file.hasCulture ? file.culture : kDefaultLanguageCode;
                definition.isFallback = !file.hasCulture;

                if (ParseResxFile(file.path, definition))
                {
                        if (!definition.explicitCode.empty())
                        {
                                definition.code = definition.explicitCode;
                        }

                        languages.push_back(std::move(definition));
                }
        }

        std::sort(languages.begin(), languages.end(),
                [](const LanguageDefinition& a, const LanguageDefinition& b)
                {
                        return a.displayName < b.displayName;
                });

        return languages;
}
} // namespace

std::unordered_map<std::string, std::unordered_map<std::string, std::string>> Localization::m_languageStrings = {};
std::vector<LanguageOption> Localization::m_languageOptions = {};
std::string Localization::m_currentLanguage = kDefaultLanguageCode;
std::string Localization::m_fallbackLanguage = kDefaultLanguageCode;
bool Localization::m_initialized = false;

void Localization::Initialize(const std::string& requestedLanguage)
{
        if (!m_initialized)
        {
                LoadLanguageData();
                m_initialized = true;
        }

        RefreshLanguageStatuses();

        if (IsLanguageComplete(requestedLanguage))
        {
                m_currentLanguage = requestedLanguage;
        }
        else
        {
                m_currentLanguage = m_fallbackLanguage;
        }
}

const std::string& Localization::Translate(const std::string& key)
{
        const auto& languageMap = GetLanguageMap(m_currentLanguage);
        const auto& fallbackMap = GetLanguageMap(m_fallbackLanguage);

        auto it = languageMap.find(key);
        if (it != languageMap.end())
        {
                return it->second;
        }

        auto fallbackIt = fallbackMap.find(key);
        if (fallbackIt != fallbackMap.end())
        {
                return fallbackIt->second;
        }

        auto insertion = m_languageStrings[m_fallbackLanguage].emplace(key, key);
        return insertion.first->second;
}

const std::vector<LanguageOption>& Localization::GetAvailableLanguages()
{
        return m_languageOptions;
}

bool Localization::SetCurrentLanguage(const std::string& languageCode)
{
        if (!IsLanguageComplete(languageCode))
        {
                return false;
        }

        m_currentLanguage = languageCode;
        return true;
}

const std::string& Localization::GetCurrentLanguage()
{
        return m_currentLanguage;
}

bool Localization::IsLanguageComplete(const std::string& languageCode)
{
        return GetMissingKeyCount(languageCode) == 0;
}

size_t Localization::GetMissingKeyCount(const std::string& languageCode)
{
        const auto& fallbackMap = GetLanguageMap(m_fallbackLanguage);
        const auto& languageMap = GetLanguageMap(languageCode);

        if (fallbackMap.empty())
        {
                return 0;
        }

        size_t missingKeys = 0;
        for (const auto& required : fallbackMap)
        {
                if (languageMap.find(required.first) == languageMap.end())
                {
                        ++missingKeys;
                }
        }
        return missingKeys;
}

void Localization::LoadLanguageData()
{
        if (!m_languageStrings.empty())
        {
                return;
        }

        auto definitions = LoadResxLanguages();
        for (const auto& language : definitions)
        {
                m_languageStrings.emplace(language.code, language.strings);

                if (language.isFallback)
                {
                        m_fallbackLanguage = language.code;
                }

                LanguageOption option{};
                option.code = language.code;
                option.displayName = language.displayName;
                option.complete = false;
                option.missingKeys = 0;
                m_languageOptions.push_back(option);
        }

        if (m_languageStrings.empty())
        {
                m_languageStrings.emplace(m_fallbackLanguage, kEmptyLanguage);

                LanguageOption option{ m_fallbackLanguage, "English", true, 0 };
                m_languageOptions.push_back(option);
        }

        if (m_languageStrings.find(m_fallbackLanguage) == m_languageStrings.end() && !m_languageOptions.empty())
        {
                m_fallbackLanguage = m_languageOptions.front().code;
        }
}

void Localization::RefreshLanguageStatuses()
{
        for (auto& option : m_languageOptions)
        {
                option.missingKeys = GetMissingKeyCount(option.code);
                option.complete = option.missingKeys == 0;
        }
}

const std::unordered_map<std::string, std::string>& Localization::GetLanguageMap(const std::string& languageCode)
{
        auto languageIt = m_languageStrings.find(languageCode);
        if (languageIt == m_languageStrings.end())
        {
                auto fallbackIt = m_languageStrings.find(m_fallbackLanguage);
                if (fallbackIt != m_languageStrings.end())
                {
                        return fallbackIt->second;
                }

                return kEmptyLanguage;
        }

        return languageIt->second;
}

const std::string& L(const std::string& key)
{
        return Localization::Translate(key);
}
