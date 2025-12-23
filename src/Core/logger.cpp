#include "logger.h"

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

#include <ShlObj.h>

namespace
{
        FILE* g_oFile = nullptr;
        bool g_isLoggingEnabled = false;

        struct CrashLogEntry
        {
                uint64_t timestampMs = 0;
                DWORD threadId = 0;
                int level = 0;
                std::string message;
        };

        constexpr size_t kCrashRingSize = 2048;
        std::array<CrashLogEntry, kCrashRingSize> g_crashLogRing{};
        size_t g_crashLogCount = 0;
        size_t g_crashLogWriteIndex = 0;
        std::mutex g_logMutex;

        std::string FormatLogMessage(const char* message, va_list args)
        {
                va_list argsCopy;
                va_copy(argsCopy, args);
                const int required = std::vsnprintf(nullptr, 0, message, argsCopy);
                va_end(argsCopy);

                if (required <= 0)
                {
                        return std::string();
                }

                const size_t bufferSize = static_cast<size_t>(required) + 1; // include null terminator
                std::string buffer(bufferSize, '\0');

                const int written = std::vsnprintf(&buffer[0], buffer.size(), message, args);
                if (written < 0)
                {
                        return std::string();
                }

                buffer.resize(static_cast<size_t>(written));
                if (buffer.empty() || buffer.back() != '\n')
                {
                        buffer.push_back('\n');
                }

                return buffer;
        }

        uint64_t GetTimestampMs()
        {
                using namespace std::chrono;
                return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        std::string FormatTimestamp(uint64_t timestampMs)
        {
                using namespace std::chrono;
                const auto secondsSinceEpoch = duration_cast<seconds>(milliseconds(timestampMs));
                const auto msRemainder = timestampMs % 1000;

                const std::time_t timeT = secondsSinceEpoch.count();
                std::tm localTime{};
                localtime_s(&localTime, &timeT);

                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03llu",
                              localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                              localTime.tm_hour, localTime.tm_min, localTime.tm_sec,
                              static_cast<unsigned long long>(msRemainder));

                return buffer;
        }

        std::string FormatLogPrefix(int level, uint64_t timestampMs, DWORD threadId)
        {
                std::ostringstream oss;
                oss << "[" << FormatTimestamp(timestampMs) << "][T" << threadId << "][L" << level << "] ";
                return oss.str();
        }

        std::string BuildLogLine(int level, const char* message, va_list args, uint64_t timestampMs, DWORD threadId)
        {
                const std::string body = FormatLogMessage(message, args);
                if (body.empty())
                {
                        return std::string();
                }

                const std::string prefix = FormatLogPrefix(level, timestampMs, threadId);
                return prefix + body;
        }

        void AppendCrashLogEntry(int level, const std::string& message, uint64_t timestampMs, DWORD threadId)
        {
                CrashLogEntry entry;
                entry.timestampMs = timestampMs;
                entry.threadId = threadId;
                entry.level = level;
                entry.message = message;

                const std::lock_guard<std::mutex> lock(g_logMutex);
                g_crashLogRing[g_crashLogWriteIndex] = std::move(entry);
                g_crashLogWriteIndex = (g_crashLogWriteIndex + 1) % kCrashRingSize;
                if (g_crashLogCount < kCrashRingSize)
                {
                        ++g_crashLogCount;
                }
        }

        void EnsureLogDirectory()
        {
                SHCreateDirectoryExW(nullptr, L"BBCF_IM", nullptr);
        }
}

bool IsLoggingEnabled()
{
        return g_isLoggingEnabled && g_oFile;
}

bool hookSucceeded(PBYTE addr, const char* funcName)
{
        if (!addr)
        {
                LOG(2, "FAILED to hook %s\n", funcName);
                return false;
        }

        LOG(2, "Successfully hooked %s at 0x%p\n", funcName, addr);
        return true;
}

char* getFullDate()
{
        time_t timer;
        char* buffer = (char*)malloc(sizeof(char) * 26);
        if (!buffer)
        {
                return NULL;
        }

        struct tm* tm_info;

        time(&timer);
        tm_info = localtime(&timer);

        strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
        return buffer;
}

void logger_with_level(int level, const char* message, ...)
{
        if (!message || !g_oFile) { return; }

        va_list args;
        va_start(args, message);

        const uint64_t timestamp = GetTimestampMs();
        const DWORD threadId = GetCurrentThreadId();
        const std::string line = BuildLogLine(level, message, args, timestamp, threadId);
        va_end(args);

        if (line.empty())
        {
                return;
        }

        {
                const std::lock_guard<std::mutex> lock(g_logMutex);
                fputs(line.c_str(), g_oFile);
                fflush(g_oFile);
        }

        AppendCrashLogEntry(level, line, timestamp, threadId);
}

void ForceLog(const char* message, ...)
{
        if (!message)
        {
                return;
        }

        if (!g_oFile)
        {
                openLogger();
        }

        if (!g_oFile)
        {
                return;
        }

        va_list args;
        va_start(args, message);

        const uint64_t timestamp = GetTimestampMs();
        const DWORD threadId = GetCurrentThreadId();
        const std::string line = BuildLogLine(0, message, args, timestamp, threadId);
        va_end(args);

        if (line.empty())
        {
                return;
        }

        {
                const std::lock_guard<std::mutex> lock(g_logMutex);
                fputs(line.c_str(), g_oFile);
                fflush(g_oFile);
        }

        AppendCrashLogEntry(0, line, timestamp, threadId);
}

void openLogger()
{
        if (g_oFile)
        {
                return;
        }

        EnsureLogDirectory();
        g_oFile = _wfopen(L"BBCF_IM\\DEBUG.txt", L"w, ccs=UTF-8");
        if (!g_oFile)
        {
                g_isLoggingEnabled = false;
                return;
        }

        char* time = getFullDate();

        fprintf(g_oFile, "\n\n\n\n");

        if (time)
        {
                fprintf(g_oFile, "BBCF_FIX START - %s\n", time);
                free(time);
        }
        else
        {
                fprintf(g_oFile, "BBCF_FIX START - {Couldn't get the current time}\n");
        }

        fprintf(g_oFile, "/////////////////////////////////////\n");
        fprintf(g_oFile, "/////////////////////////////////////\n\n");
        fflush(g_oFile);
}

void closeLogger()
{
        if (!g_oFile)
        {
                g_isLoggingEnabled = false;
                return;
        }

        char* time = getFullDate();
        if (time)
        {
                fprintf(g_oFile, "BBCF_FIX STOP - %s\n", time);
                free(time);
        }
        else
        {
                fprintf(g_oFile, "BBCF_FIX STOP - {Couldn't get the current time}\n");
        }

        fclose(g_oFile);
        g_oFile = nullptr;
        g_isLoggingEnabled = false;
}

void SetLoggingEnabled(bool enabled)
{
        if (enabled)
        {
                openLogger();
                g_isLoggingEnabled = g_oFile != nullptr;
        }
        else
        {
                closeLogger();
        }
}

void logSettingsIni()
{
        LOG(1, "settings.ini config:\n");

        std::ostringstream oss;

        //X-Macro
#define SETTING(_type, _var, _inistring, _defaultval) \
        oss << "\t- " << _inistring << " = " << Settings::settingsIni.##_var << "\n";
#include "settings.def"
#undef SETTING

        LOG(1, oss.str().c_str());
}

void logD3DPParams(D3DPRESENT_PARAMETERS* pPresentationParameters, bool isOriginalSettings)
{
        if (isOriginalSettings)
        {
                LOG(1, "Original D3D PresentationParameters:\n");
        }
        else
        {
                LOG(1, "Modified D3D PresentationParameters:\n");
        }

        LOG(1, "\t- BackBufferWidth: %u\n", pPresentationParameters->BackBufferWidth);
        LOG(1, "\t- BackBufferHeight: %u\n", pPresentationParameters->BackBufferHeight);
        LOG(1, "\t- BackBufferFormat: %u\n", pPresentationParameters->BackBufferFormat);
        LOG(1, "\t- BackBufferCount: %u\n", pPresentationParameters->BackBufferCount);
        LOG(1, "\t- SwapEffect: %u\n", pPresentationParameters->SwapEffect);
        LOG(1, "\t- MultiSampleType: %u\n", pPresentationParameters->MultiSampleType);
        LOG(1, "\t- MultiSampleQuality: %d\n", pPresentationParameters->MultiSampleQuality);
        LOG(1, "\t- EnableAutoDepthStencil: %d\n", pPresentationParameters->EnableAutoDepthStencil);
        LOG(1, "\t- FullScreen_RefreshRateInHz: %u\n", pPresentationParameters->FullScreen_RefreshRateInHz);
        LOG(1, "\t- hDeviceWindow: 0x%p\n", pPresentationParameters->hDeviceWindow);
        LOG(1, "\t- Windowed: %d\n", pPresentationParameters->Windowed);
        LOG(1, "\t- Flags: 0x%p\n", pPresentationParameters->Flags);
        LOG(1, "\t- PresentationInterval: 0x%p\n", pPresentationParameters->PresentationInterval);
}

std::string GetRecentLogs()
{
        const std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_crashLogCount == 0)
        {
                return std::string();
        }

        std::ostringstream oss;
        const size_t startIndex = (g_crashLogCount == kCrashRingSize) ? g_crashLogWriteIndex : 0;
        for (size_t i = 0; i < g_crashLogCount; ++i)
        {
                const size_t index = (startIndex + i) % kCrashRingSize;
                oss << g_crashLogRing[index].message;
        }

        return oss.str();
}
