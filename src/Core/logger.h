#pragma once
#include "Settings.h"

#include "D3D9EXWrapper/d3d9.h"

#include <string>

#define DEBUG_LOG_LEVEL 5 //0 = highest, 7 = lowest priority

#define LOG(_level, ...)                                                                                                  \
        {                                                                                                                 \
                if (IsLoggingEnabled() && DEBUG_LOG_LEVEL >= _level)                                                      \
                {                                                                                                         \
                        logger_with_level(_level, __VA_ARGS__);                                                           \
                }                                                                                                         \
        }

//Use this to log in naked asm functions
#define LOG_ASM(_level, ...)                                                                                              \
        {                                                                                                                 \
                __asm { pushad }                                                                                          \
                if (IsLoggingEnabled() && DEBUG_LOG_LEVEL >= _level)                                                      \
                {                                                                                                         \
                        logger_with_level(_level, __VA_ARGS__);                                                           \
                }                                                                                                         \
                __asm { popad }                                                                                           \
        }

#define RELOG(_level, ...)                                                                                                 \
        {                                                                                                                  \
                if (IsReTraceLoggingEnabled() && GetReTraceLogLevel() >= _level)                                          \
                {                                                                                                          \
                        relog_with_level(_level, __VA_ARGS__);                                                            \
                }                                                                                                          \
        }

void logger_with_level(int level, const char* message, ...);
void ForceLog(const char* message, ...);

// Writes an already-formatted line for a process that is in the middle of dying.
//
// ForceLog is not safe on that path: it builds a std::string and then pushes a copy into
// the in-memory ring under the log mutex, so it allocates twice. A heap-corruption crash
// dies inside that, which cost a real crash report - the line reached disk and the crash
// handler never got another instruction. This one allocates nothing, takes no lock it is
// willing to wait for, and never touches the ring.
void ForceLogRaw(const char* line);
void openLogger();
void closeLogger();
void SetLoggingEnabled(bool enabled);

// Removes DEBUG.txt after the fact. The log is opened before settings are read so
// the early-startup window is captured; this is how an install with
// GenerateDebugLogs=0 still ends up with no log file.
void DeleteDebugLogFile();

// One-shot record of how the process was started: game dir, working directory,
// resolved log path, command line, exe path.
void LogStartupEnvironment();
bool IsLoggingEnabled();
void relog_with_level(int level, const char* message, ...);
void ConfigureReTraceLogging(bool enabled, int level, int maxFileMb, int maxBackups);
bool IsReTraceLoggingEnabled();
int GetReTraceLogLevel();
//free it after usage!!
char* getFullDate();
void logSettingsIni();
bool hookSucceeded(PBYTE addr, const char* funcName);
void logD3DPParams(D3DPRESENT_PARAMETERS* pPresentationParameters, bool isOriginalSettings = true);

// Returns a snapshot of the in-memory log ring buffer (chronological order).
std::string GetRecentLogs();
