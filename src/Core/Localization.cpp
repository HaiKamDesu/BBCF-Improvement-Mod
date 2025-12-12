#include "Localization.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>

#ifndef _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
#endif

#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;

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

extern "C" IMAGE_DOS_HEADER __ImageBase;

const std::unordered_map<std::string, std::string> kEmptyLanguage = {};

// Compile-time fallback dictionaries to guarantee at least the shipping languages
// are available even if embedded resources fail to load (e.g., resource section
// stripped or build misconfigured). These mirror the on-disk .resx contents so
// format strings remain stable.
const char* kBuiltInEnglishResx = R"RESX(<?xml version="1.0" encoding="utf-8"?>
<root>
  <resheader name="resmimetype">
    <value>text/microsoft-resx</value>
  </resheader>
  <resheader name="version">
    <value>2.0</value>
  </resheader>
  <resheader name="reader">
    <value>System.Resources.ResXResourceReader, System.Windows.Forms, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>
  </resheader>
  <resheader name="writer">
    <value>System.Resources.ResXResourceWriter, System.Windows.Forms, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>
  </resheader>
  <data name="_DisplayName" xml:space="preserve">
    <value>English</value>
  </data>
  <data name="_LanguageCode" xml:space="preserve">
    <value>en</value>
  </data>
  <data name="Toggle UI" xml:space="preserve">
    <value>Toggle UI</value>
  </data>
  <data name="Only show menus in training" xml:space="preserve">
    <value>Only show menus in training mode</value>
  </data>
  <data name="Toggle Netplay UI" xml:space="preserve">
    <value>Toggle Netplay UI</value>
  </data>
  <data name="Hide HUD" xml:space="preserve">
    <value>Hide HUD</value>
  </data>
  <data name="Toggle visible" xml:space="preserve">
    <value>Toggle visible</value>
  </data>
  <data name="Minimize" xml:space="preserve">
    <value>Minimize</value>
  </data>
  <data name="Menu size" xml:space="preserve">
    <value>Menu size</value>
  </data>
  <data name="Rendering settings" xml:space="preserve">
    <value>Rendering settings</value>
  </data>
  <data name="Enable multisampling" xml:space="preserve">
    <value>Enable multisampling</value>
  </data>
  <data name="Anti-aliasing samples" xml:space="preserve">
    <value>Anti-aliasing samples</value>
  </data>
  <data name="V-Sync" xml:space="preserve">
    <value>V-Sync</value>
  </data>
  <data name="Hide menu fullscreen" xml:space="preserve">
    <value>Hide menu in fullscreen</value>
  </data>
  <data name="Menu size help" xml:space="preserve">
    <value>Adjust the size of the menus in the overlay window.</value>
  </data>
  <data name="Multisampling help" xml:space="preserve">
    <value>Enable or disable multisampling for improved rendering quality.</value>
  </data>
  <data name="Anti-aliasing help" xml:space="preserve">
    <value>Select the number of samples used for anti-aliasing.</value>
  </data>
  <data name="V-Sync help" xml:space="preserve">
    <value>Toggle vertical synchronization to reduce screen tearing.</value>
  </data>
  <data name="Hide menu fullscreen help" xml:space="preserve">
    <value>Hide the overlay menu when the game is in fullscreen mode.</value>
  </data>
  <data name="Custom palettes" xml:space="preserve">
    <value>Custom palettes</value>
  </data>
  <data name="Enable custom palettes" xml:space="preserve">
    <value>Enable custom palettes</value>
  </data>
  <data name="Save palettes" xml:space="preserve">
    <value>Save palettes</value>
  </data>
  <data name="Load palettes" xml:space="preserve">
    <value>Load palettes</value>
  </data>
  <data name="Load external palettes" xml:space="preserve">
    <value>Load external palettes by default</value>
  </data>
  <data name="Swap controller" xml:space="preserve">
    <value>Swap controller positions</value>
  </data>
  <data name="Hide palette info" xml:space="preserve">
    <value>Hide palette info during online matches</value>
  </data>
  <data name="Palettes online notice" xml:space="preserve">
    <value>Palettes may desync online unless both players match.</value>
  </data>
  <data name="Controller settings" xml:space="preserve">
    <value>Controller settings</value>
  </data>
  <data name="Primary keyboard" xml:space="preserve">
    <value>Primary keyboard device</value>
  </data>
  <data name="Gamepad input" xml:space="preserve">
    <value>Gamepad input</value>
  </data>
  <data name="Ignore devices help" xml:space="preserve">
    <value>Filter out devices that should not be used by the game or overlay.</value>
  </data>
  <data name="Toggle button" xml:space="preserve">
    <value>Toggle menu button</value>
  </data>
  <data name="Toggle online button" xml:space="preserve">
    <value>Toggle netplay menu button</value>
  </data>
  <data name="Toggle HUD button" xml:space="preserve">
    <value>Hide HUD button</value>
  </data>
  <data name="Save state keybind" xml:space="preserve">
    <value>Save state keybind</value>
  </data>
  <data name="Load state keybind" xml:space="preserve">
    <value>Load state keybind</value>
  </data>
  <data name="Load replay state keybind" xml:space="preserve">
    <value>Load replay state keybind</value>
  </data>
  <data name="Freeze frame keybind" xml:space="preserve">
    <value>Freeze frame keybind</value>
  </data>
  <data name="Step frames keybind" xml:space="preserve">
    <value>Step frames keybind</value>
  </data>
  <data name="Auto archive" xml:space="preserve">
    <value>Auto archive replays</value>
  </data>
  <data name="Upload replay" xml:space="preserve">
    <value>Upload replay data</value>
  </data>
  <data name="Upload replay host" xml:space="preserve">
    <value>Upload host</value>
  </data>
  <data name="Upload replay port" xml:space="preserve">
    <value>Upload port</value>
  </data>
  <data name="Upload replay endpoint" xml:space="preserve">
    <value>Upload endpoint</value>
  </data>
  <data name="Logging" xml:space="preserve">
    <value>Logging</value>
  </data>
  <data name="Enable update checks" xml:space="preserve">
    <value>Enable update checks</value>
  </data>
  <data name="Updates help" xml:space="preserve">
    <value>Enable or disable automatic update checks for the mod.</value>
  </data>
  <data name="Frame history settings" xml:space="preserve">
    <value>Frame history settings</value>
  </data>
  <data name="Frame history width" xml:space="preserve">
    <value>Frame history width</value>
  </data>
  <data name="Frame history height" xml:space="preserve">
    <value>Frame history height</value>
  </data>
  <data name="Frame history spacing" xml:space="preserve">
    <value>Frame history spacing</value>
  </data>
  <data name="Frame advantage settings" xml:space="preserve">
    <value>Frame advantage settings</value>
  </data>
  <data name="Frame advantage toggle" xml:space="preserve">
    <value>Enable frame advantage overlay</value>
  </data>
  <data name="Frame advantage width" xml:space="preserve">
    <value>Frame advantage width</value>
  </data>
  <data name="Frame advantage height" xml:space="preserve">
    <value>Frame advantage height</value>
  </data>
  <data name="Frame advantage spacing" xml:space="preserve">
    <value>Frame advantage spacing</value>
  </data>
  <data name="Avatar settings" xml:space="preserve">
    <value>Avatar settings</value>
  </data>
  <data name="Show avatar" xml:space="preserve">
    <value>Show avatar</value>
  </data>
  <data name="Notifications" xml:space="preserve">
    <value>Notifications</value>
  </data>
  <data name="Language" xml:space="preserve">
    <value>Language</value>
  </data>
  <data name="Language incomplete label" xml:space="preserve">
    <value>%s - Incomplete (missing %zu keys)</value>
  </data>
  <data name="Language selection help" xml:space="preserve">
    <value>Select the UI language. Incomplete languages cannot be chosen.</value>
  </data>
  <data name="Log" xml:space="preserve">
    <value>Log</value>
  </data>
  <data name="States" xml:space="preserve">
    <value>States</value>
  </data>
  <data name="Player count" xml:space="preserve">
    <value>Current players</value>
  </data>
  <data name="Github" xml:space="preserve">
    <value>Github</value>
  </data>
  <data name="Discord" xml:space="preserve">
    <value>Discord</value>
  </data>
</root>)RESX";

const char* kBuiltInSpanishResx = R"RESX(<?xml version="1.0" encoding="utf-8"?>
<root>
  <resheader name="resmimetype">
    <value>text/microsoft-resx</value>
  </resheader>
  <resheader name="version">
    <value>2.0</value>
  </resheader>
  <resheader name="reader">
    <value>System.Resources.ResXResourceReader, System.Windows.Forms, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>
  </resheader>
  <resheader name="writer">
    <value>System.Resources.ResXResourceWriter, System.Windows.Forms, Version=2.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value>
  </resheader>
  <data name="_DisplayName" xml:space="preserve">
    <value>Español</value>
  </data>
  <data name="_LanguageCode" xml:space="preserve">
    <value>es</value>
  </data>
  <data name="Toggle UI" xml:space="preserve">
    <value>Alternar interfaz</value>
  </data>
  <data name="Only show menus in training" xml:space="preserve">
    <value>Mostrar menús solo en entrenamiento</value>
  </data>
  <data name="Toggle Netplay UI" xml:space="preserve">
    <value>Alternar interfaz en línea</value>
  </data>
  <data name="Hide HUD" xml:space="preserve">
    <value>Ocultar HUD</value>
  </data>
  <data name="Toggle visible" xml:space="preserve">
    <value>Alternar visibilidad</value>
  </data>
  <data name="Minimize" xml:space="preserve">
    <value>Minimizar</value>
  </data>
  <data name="Menu size" xml:space="preserve">
    <value>Tamaño del menú</value>
  </data>
  <data name="Rendering settings" xml:space="preserve">
    <value>Configuración de renderizado</value>
  </data>
  <data name="Enable multisampling" xml:space="preserve">
    <value>Habilitar multisampling</value>
  </data>
  <data name="Anti-aliasing samples" xml:space="preserve">
    <value>Antialiasing (muestras)</value>
  </data>
  <data name="V-Sync" xml:space="preserve">
    <value>V-Sync</value>
  </data>
  <data name="Hide menu fullscreen" xml:space="preserve">
    <value>Ocultar menú en pantalla completa</value>
  </data>
  <data name="Menu size help" xml:space="preserve">
    <value>Ajusta el tamaño de los menús en la ventana de la superposición.</value>
  </data>
  <data name="Multisampling help" xml:space="preserve">
    <value>Activa o desactiva el multisampling para mejorar la calidad de renderizado.</value>
  </data>
  <data name="Anti-aliasing help" xml:space="preserve">
    <value>Selecciona el número de muestras usadas para antialiasing.</value>
  </data>
  <data name="V-Sync help" xml:space="preserve">
    <value>Alterna la sincronización vertical para reducir el tearing.</value>
  </data>
  <data name="Hide menu fullscreen help" xml:space="preserve">
    <value>Oculta el menú de la superposición cuando el juego está en pantalla completa.</value>
  </data>
  <data name="Custom palettes" xml:space="preserve">
    <value>Paletas personalizadas</value>
  </data>
  <data name="Enable custom palettes" xml:space="preserve">
    <value>Habilitar paletas personalizadas</value>
  </data>
  <data name="Save palettes" xml:space="preserve">
    <value>Guardar paletas</value>
  </data>
  <data name="Load palettes" xml:space="preserve">
    <value>Cargar paletas</value>
  </data>
  <data name="Load external palettes" xml:space="preserve">
    <value>Cargar paletas externas por defecto</value>
  </data>
  <data name="Swap controller" xml:space="preserve">
    <value>Intercambiar posiciones de mando</value>
  </data>
  <data name="Hide palette info" xml:space="preserve">
    <value>Ocultar info de paleta en partidas online</value>
  </data>
  <data name="Palettes online notice" xml:space="preserve">
    <value>Las paletas pueden desincronizarse online si los jugadores no coinciden.</value>
  </data>
  <data name="Controller settings" xml:space="preserve">
    <value>Configuración de mandos</value>
  </data>
  <data name="Primary keyboard" xml:space="preserve">
    <value>Teclado principal</value>
  </data>
  <data name="Gamepad input" xml:space="preserve">
    <value>Entrada de gamepad</value>
  </data>
  <data name="Ignore devices help" xml:space="preserve">
    <value>Filtra dispositivos que no deben usar el juego o la superposición.</value>
  </data>
  <data name="Toggle button" xml:space="preserve">
    <value>Botón para mostrar menú</value>
  </data>
  <data name="Toggle online button" xml:space="preserve">
    <value>Botón para menú online</value>
  </data>
  <data name="Toggle HUD button" xml:space="preserve">
    <value>Botón para ocultar HUD</value>
  </data>
  <data name="Save state keybind" xml:space="preserve">
    <value>Tecla para guardar estado</value>
  </data>
  <data name="Load state keybind" xml:space="preserve">
    <value>Tecla para cargar estado</value>
  </data>
  <data name="Load replay state keybind" xml:space="preserve">
    <value>Tecla para cargar estado de repetición</value>
  </data>
  <data name="Freeze frame keybind" xml:space="preserve">
    <value>Tecla para congelar fotograma</value>
  </data>
  <data name="Step frames keybind" xml:space="preserve">
    <value>Tecla para avanzar fotogramas</value>
  </data>
  <data name="Auto archive" xml:space="preserve">
    <value>Archivar repeticiones automáticamente</value>
  </data>
  <data name="Upload replay" xml:space="preserve">
    <value>Subir datos de repetición</value>
  </data>
  <data name="Upload replay host" xml:space="preserve">
    <value>Host de subida</value>
  </data>
  <data name="Upload replay port" xml:space="preserve">
    <value>Puerto de subida</value>
  </data>
  <data name="Upload replay endpoint" xml:space="preserve">
    <value>Endpoint de subida</value>
  </data>
  <data name="Logging" xml:space="preserve">
    <value>Registro</value>
  </data>
  <data name="Enable update checks" xml:space="preserve">
    <value>Habilitar comprobación de actualizaciones</value>
  </data>
  <data name="Updates help" xml:space="preserve">
    <value>Activa o desactiva la comprobación automática de actualizaciones del mod.</value>
  </data>
  <data name="Frame history settings" xml:space="preserve">
    <value>Historial de fotogramas</value>
  </data>
  <data name="Frame history width" xml:space="preserve">
    <value>Ancho del historial</value>
  </data>
  <data name="Frame history height" xml:space="preserve">
    <value>Alto del historial</value>
  </data>
  <data name="Frame history spacing" xml:space="preserve">
    <value>Espaciado del historial</value>
  </data>
  <data name="Frame advantage settings" xml:space="preserve">
    <value>Ventaja de fotogramas</value>
  </data>
  <data name="Frame advantage toggle" xml:space="preserve">
    <value>Habilitar superposición de ventaja de fotogramas</value>
  </data>
  <data name="Frame advantage width" xml:space="preserve">
    <value>Ancho de ventaja</value>
  </data>
  <data name="Frame advantage height" xml:space="preserve">
    <value>Alto de ventaja</value>
  </data>
  <data name="Frame advantage spacing" xml:space="preserve">
    <value>Espaciado de ventaja</value>
  </data>
  <data name="Avatar settings" xml:space="preserve">
    <value>Configuración de avatar</value>
  </data>
  <data name="Show avatar" xml:space="preserve">
    <value>Mostrar avatar</value>
  </data>
  <data name="Notifications" xml:space="preserve">
    <value>Notificaciones</value>
  </data>
  <data name="Language" xml:space="preserve">
    <value>Idioma</value>
  </data>
  <data name="Language incomplete label" xml:space="preserve">
    <value>%s - Incompleto (faltan %zu claves)</value>
  </data>
  <data name="Language selection help" xml:space="preserve">
    <value>Selecciona el idioma de la interfaz. Los idiomas incompletos no pueden elegirse.</value>
  </data>
  <data name="Log" xml:space="preserve">
    <value>Registro</value>
  </data>
  <data name="States" xml:space="preserve">
    <value>Estados</value>
  </data>
  <data name="Player count" xml:space="preserve">
    <value>Jugadores actuales</value>
  </data>
  <data name="Github" xml:space="preserve">
    <value>Github</value>
  </data>
  <data name="Discord" xml:space="preserve">
    <value>Discord</value>
  </data>
</root>)RESX";

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
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &output[0], length, nullptr, nullptr);
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

bool ParseResxFile(const fs::path& path, LanguageDefinition& outDefinition)
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

        HMODULE moduleHandle = reinterpret_cast<HMODULE>(&__ImageBase);
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&__ImageBase), &moduleHandle))
        {
                moduleHandle = reinterpret_cast<HMODULE>(&__ImageBase);
        }

        struct EnumContext
        {
                HMODULE module;
                std::vector<ResxEntry>* entries;
        } context{ moduleHandle, &entries };

        const BOOL enumResult = EnumResourceNamesW(moduleHandle, RT_RCDATA,
                [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) -> BOOL
                {
                        if (IS_INTRESOURCE(name))
                        {
                                return TRUE;
                        }

                        auto* ctx = reinterpret_cast<EnumContext*>(param);
                        auto* out = ctx->entries;
                        std::string resourceName = WideToUtf8(name);
                        if (resourceName.find(".resx") == std::string::npos)
                        {
                                return TRUE;
                        }

                        HRSRC resource = FindResourceW(ctx->module, name, RT_RCDATA);
                        if (!resource)
                        {
                                return TRUE;
                        }

                        HGLOBAL handle = LoadResource(ctx->module, resource);
                        if (!handle)
                        {
                                return TRUE;
                        }

                        const DWORD size = SizeofResource(ctx->module, resource);
                        const void* data = LockResource(handle);
                        if (!data || size == 0)
                        {
                                return TRUE;
                        }

                        std::string content(static_cast<const char*>(data), static_cast<const char*>(data) + size);
                        AddResxEntry(resourceName, std::move(content), true, *out);
                        return TRUE;
                },
                reinterpret_cast<LONG_PTR>(&context));

        if (!enumResult)
        {
                LOG(1, "%s EnumResourceNamesW failed while scanning embedded localization resources (err=%lu).", kLanguageLogTag,
                        GetLastError());
        }

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
    fs::path localizationDir;

    const std::vector<fs::path> candidates = {
            fs::path(kLocalizationDirectory),
            fs::path("localization"),
    };

    for (const auto& candidate : candidates)
    {
        if (fs::exists(candidate) && fs::is_directory(candidate))
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

    for (const auto& entry : fs::directory_iterator(localizationDir))
    {
        if (!fs::is_regular_file(entry.status()) || entry.path().extension() != ".resx")
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


std::vector<ResxEntry> LoadBuiltInResxEntries()
{
        std::vector<ResxEntry> entries;
        AddResxEntry("Localization.resx", kBuiltInEnglishResx, true, entries);
        AddResxEntry("Localization.es.resx", kBuiltInSpanishResx, true, entries);

        if (!entries.empty())
        {
                LOG(1, "%s Falling back to compiled-in localization data (%zu resource(s)).", kLanguageLogTag, entries.size());
        }

        return entries;
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

        if (entries.empty())
        {
                entries = LoadBuiltInResxEntries();
        }

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
