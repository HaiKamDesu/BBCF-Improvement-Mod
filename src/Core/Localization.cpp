#include "Localization.h"

#include <Windows.h>

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
const char* kLanguageLogTag = "[Language]";
const size_t kMissingPreviewLimit = 5;

const std::unordered_map<std::string, std::string> kEmptyLanguage = {};

struct LanguageDefinition
{
        std::string code;
        std::string displayName;
        std::string explicitCode;
        std::unordered_map<std::string, std::string> strings;
        bool isFallback = false;
};

struct ResxEntry
{
        std::string name;
        std::string baseName;
        std::string culture;
        bool hasCulture = false;
        std::string content;
        bool fromResource = false;
};

const std::regex kResxNameRegex(R"((.*?)(?:\.([A-Za-z0-9_-]+))?\.resx$)");

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

std::string WideToUtf8(LPCWSTR value)
{
        if (!value)
        {
                return std::string();
        }

        const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
                return std::string();
        }

        std::string output(static_cast<size_t>(length - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, output.data(), length, nullptr, nullptr);
        return output;
}

std::vector<std::string> CollectMissingKeysPreview(const std::unordered_map<std::string, std::string>& fallback,
        const std::unordered_map<std::string, std::string>& language)
{
        std::vector<std::string> preview;
        preview.reserve(kMissingPreviewLimit);

        for (const auto& required : fallback)
        {
                if (language.find(required.first) == language.end())
                {
                        preview.push_back(required.first);
                        if (preview.size() >= kMissingPreviewLimit)
                        {
                                break;
                        }
                }
        }

        return preview;
}

bool AddResxEntry(const std::string& name, std::string content, bool fromResource, std::vector<ResxEntry>& out)
{
        std::smatch match;
        if (!std::regex_match(name, match, kResxNameRegex))
        {
                LOG(2, "%s Skipping localization blob with unexpected name: %s", kLanguageLogTag, name.c_str());
                return false;
        }

        ResxEntry entry;
        entry.name = name;
        entry.baseName = match[1].str();
        entry.hasCulture = match[2].matched;
        entry.culture = entry.hasCulture ? match[2].str() : std::string();
        entry.content = std::move(content);
        entry.fromResource = fromResource;
        out.push_back(std::move(entry));
        return true;
}

bool ParseResxContent(const std::string& content, const std::string& sourceLabel, LanguageDefinition& outDefinition)
{
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
                LOG(1, "%s Localization blob %s did not contain any <data> entries with values.", kLanguageLogTag, sourceLabel.c_str());
                return false;
        }

        return true;
}

bool ParseResxFile(const std::filesystem::path& path, LanguageDefinition& outDefinition)
{
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
                LOG(1, "%s Failed to open localization file: %s", kLanguageLogTag, path.string().c_str());
                return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return ParseResxContent(buffer.str(), path.string(), outDefinition);
}

std::vector<ResxEntry> LoadEmbeddedResxEntries()
{
        std::vector<ResxEntry> entries;

        EnumResourceNamesW(nullptr, RT_RCDATA,
                [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) -> BOOL
                {
                        if (IS_INTRESOURCE(name))
                        {
                                return TRUE;
                        }

                        auto* out = reinterpret_cast<std::vector<ResxEntry>*>(param);
                        std::string resourceName = WideToUtf8(name);
                        if (resourceName.find(".resx") == std::string::npos)
                        {
                                return TRUE;
                        }

                        HRSRC resource = FindResourceW(nullptr, name, RT_RCDATA);
                        if (!resource)
                        {
                                return TRUE;
                        }

                        HGLOBAL handle = LoadResource(nullptr, resource);
                        if (!handle)
                        {
                                return TRUE;
                        }

                        const DWORD size = SizeofResource(nullptr, resource);
                        const void* data = LockResource(handle);
                        if (!data || size == 0)
                        {
                                return TRUE;
                        }

                        std::string content(static_cast<const char*>(data), static_cast<const char*>(data) + size);
                        AddResxEntry(resourceName, std::move(content), true, *out);
                        return TRUE;
                },
                reinterpret_cast<LONG_PTR>(&entries));

        if (entries.empty())
        {
                LOG(2, "%s No embedded localization resources detected.", kLanguageLogTag);
        }
        else
        {
                LOG(2, "%s Loaded %zu embedded localization resource(s).", kLanguageLogTag, entries.size());
        }

        return entries;
}

std::vector<ResxEntry> LoadFilesystemResxEntries()
{
        std::vector<ResxEntry> files;
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
                LOG(2, "%s Localization directory not found on disk; relying on embedded resources.", kLanguageLogTag);
                return files;
        }

        for (const auto& entry : std::filesystem::directory_iterator(localizationDir))
        {
                if (!entry.is_regular_file() || entry.path().extension() != ".resx")
                {
                        continue;
                }

                std::ifstream file(entry.path(), std::ios::binary);
                if (!file.is_open())
                {
                        LOG(1, "%s Failed to open localization file on disk: %s", kLanguageLogTag, entry.path().string().c_str());
                        continue;
                }

                std::stringstream buffer;
                buffer << file.rdbuf();
                AddResxEntry(entry.path().filename().string(), buffer.str(), false, files);
        }

        if (files.empty())
        {
                LOG(2, "%s No .resx files found in %s.", kLanguageLogTag, localizationDir.string().c_str());
        }

        return files;
}

std::string SelectBaseName(const std::vector<ResxEntry>& entries)
{
        for (const auto& entry : entries)
        {
                if (!entry.hasCulture)
                {
                        return entry.baseName;
                }
        }

        if (!entries.empty())
        {
                return entries.front().baseName;
        }

        return std::string();
}

std::vector<LanguageDefinition> LoadResxLanguages()
{
        auto entries = LoadEmbeddedResxEntries();
        auto fileEntries = LoadFilesystemResxEntries();
        entries.insert(entries.end(), fileEntries.begin(), fileEntries.end());

        std::vector<LanguageDefinition> languages;
        if (entries.empty())
        {
                return languages;
        }

        const std::string targetBaseName = SelectBaseName(entries);
        if (targetBaseName.empty())
        {
                LOG(1, "%s No valid localization base name found while loading resx data.", kLanguageLogTag);
                return languages;
        }

        std::unordered_map<std::string, size_t> languageIndex;

        for (const auto& entry : entries)
        {
                if (entry.baseName != targetBaseName)
                {
                        LOG(2, "%s Skipping resx '%s' because base name '%s' does not match '%s'.",
                                kLanguageLogTag, entry.name.c_str(), entry.baseName.c_str(), targetBaseName.c_str());
                        continue;
                }

                LanguageDefinition definition;
                definition.code = entry.hasCulture ? entry.culture : kDefaultLanguageCode;
                definition.isFallback = !entry.hasCulture;

                if (!ParseResxContent(entry.content, entry.name, definition))
                {
                        continue;
                }

                if (!definition.explicitCode.empty())
                {
                        definition.code = definition.explicitCode;
                }

                const auto existing = languageIndex.find(definition.code);
                if (existing != languageIndex.end())
                {
                        languages[existing->second] = definition;
                }
                else
                {
                        languageIndex.emplace(definition.code, languages.size());
                        languages.push_back(definition);
                }

                LOG(2, "%s Loaded language '%s' (%s) with %zu entries from %s.", kLanguageLogTag,
                        definition.code.c_str(), definition.displayName.c_str(), definition.strings.size(),
                        entry.fromResource ? "embedded resources" : "disk");
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
        const auto fallbackIt = m_languageStrings.find(m_fallbackLanguage);
        if (fallbackIt == m_languageStrings.end())
        {
                return 0;
        }

        const auto languageIt = m_languageStrings.find(languageCode);
        const auto& languageMap = languageIt != m_languageStrings.end() ? languageIt->second : kEmptyLanguage;
        const auto& fallbackMap = fallbackIt->second;

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
                LOG(1, "%s No localization resources found; using empty fallback language '%s'.", kLanguageLogTag, m_fallbackLanguage.c_str());
        }

        if (m_languageStrings.find(m_fallbackLanguage) == m_languageStrings.end() && !m_languageOptions.empty())
        {
                m_fallbackLanguage = m_languageOptions.front().code;
        }

        LOG(2, "%s Fallback language set to '%s'.", kLanguageLogTag, m_fallbackLanguage.c_str());
}

void Localization::RefreshLanguageStatuses()
{
        const auto fallbackIt = m_languageStrings.find(m_fallbackLanguage);
        const auto& fallbackMap = fallbackIt != m_languageStrings.end() ? fallbackIt->second : kEmptyLanguage;

        for (auto& option : m_languageOptions)
        {
                const auto languageIt = m_languageStrings.find(option.code);
                const auto& languageMap = languageIt != m_languageStrings.end() ? languageIt->second : kEmptyLanguage;

                option.missingKeys = GetMissingKeyCount(option.code);
                option.complete = option.missingKeys == 0;

                if (!option.complete && !fallbackMap.empty())
                {
                        auto preview = CollectMissingKeysPreview(fallbackMap, languageMap);
                        LOG(2, "%s Language '%s' missing %zu key(s)%s.", kLanguageLogTag, option.code.c_str(), option.missingKeys,
                                preview.size() < option.missingKeys ? " (preview)" : "");
                        for (const auto& key : preview)
                        {
                                LOG(2, "%s    missing: %s", kLanguageLogTag, key.c_str());
                        }
                }
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
