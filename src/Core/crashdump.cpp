#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "crashdump.h"

#include "ExitDiagnostics.h"

#include "Settings.h"
#include "info.h"
#include "logger.h"

#include <algorithm>
#include <codecvt>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <dbghelp.h>
#include <intrin.h>
#include <limits>
#include <locale>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <vector>
#include <tchar.h>
#include <windows.h>

#ifndef MiniDumpWithFullMemoryInfo
#define MiniDumpWithFullMemoryInfo (0x00000800)
#endif

#ifndef MiniDumpWithThreadInfo
#define MiniDumpWithThreadInfo (0x00001000)
#endif

#ifndef MiniDumpWithUnloadedModules
#define MiniDumpWithUnloadedModules (0x00002000)
#endif

#ifndef STATUS_HEAP_CORRUPTION
#define STATUS_HEAP_CORRUPTION static_cast<DWORD>(0xC0000374L)
#endif

#ifndef STATUS_FATAL_APP_EXIT
#define STATUS_FATAL_APP_EXIT static_cast<DWORD>(0x40000015L)
#endif

namespace
{
        using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, const MINIDUMP_EXCEPTION_INFORMATION*, const MINIDUMP_USER_STREAM_INFORMATION*, const MINIDUMP_CALLBACK_INFORMATION*);

        std::atomic<bool> g_crashBundleWritten{ false };
        PVOID g_vectoredHandler = nullptr;

        // The thread that actually faulted. The bundle may be written by a different one
        // (see the reporter thread below), and MiniDumpWriteDump must still be told which
        // thread crashed or the dump names the reporter instead.
        std::atomic<DWORD> g_faultingThreadId{ 0 };

        // Every log call in this file can run while the process is dying, so none of them
        // use ForceLog: it allocates twice and blocks on the log mutex, which deadlocks if
        // the fault happened inside the logger. This formats into a stack buffer and hands
        // the finished line to ForceLogRaw, which does neither.
        void CrashProgress(const char* format, ...)
        {
                char buffer[1024];
                va_list args;
                va_start(args, format);
                const int written = _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
                va_end(args);
                if (written <= 0)
                {
                        return;
                }

                ForceLogRaw(buffer);
        }


        std::string ToUtf8(const std::wstring& value)
        {
                if (value.empty())
                {
                        return std::string();
                }

                std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
                return converter.to_bytes(value);
        }

        std::wstring BuildTimestamp()
        {
                SYSTEMTIME st;
                GetLocalTime(&st);

                wchar_t buffer[32];
                swprintf_s(buffer, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                return std::wstring(buffer);
        }

        std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
        {
                if (left.empty())
                {
                        return right;
                }

                if (left.back() == L'\\')
                {
                        return left + right;
                }

                return left + L"\\" + right;
        }

        std::wstring GetExecutableDirectory()
        {
                wchar_t modulePath[MAX_PATH] = {};
                const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
                if (length == 0 || length >= MAX_PATH)
                {
                        CrashProgress("[Crash] GetModuleFileNameW failed (len=%lu err=%lu)\n", length, GetLastError());
                        return std::wstring();
                }

                std::wstring path(modulePath, length);
                const size_t lastSlash = path.find_last_of(L"\\/");
                if (lastSlash == std::wstring::npos)
                {
                        CrashProgress("[Crash] Failed to locate executable directory for %s\n", ToUtf8(path).c_str());
                        return std::wstring();
                }

                return path.substr(0, lastSlash);
        }

        std::wstring GetCrashRootDirectory()
        {
                const std::wstring exeDir = GetExecutableDirectory();
                if (exeDir.empty())
                {
                        CrashProgress("[Crash] Falling back to relative CrashReports path.\n");
                        return L"BBCF_IM\\CrashReports";
                }

                return JoinPath(exeDir, L"BBCF_IM\\CrashReports");
        }

        // Plain CreateDirectoryW, one level at a time. SHCreateDirectoryExW initialises COM,
        // and a bundle is often written while the main thread is stuck holding COM's lock
        // (2026-10-09: the close-hang watchdog waited on it forever inside CoUninitialize,
        // so the process it was meant to end stayed alive).
        void EnsureDirectory(const std::wstring& path)
        {
                for (size_t i = 0; i <= path.size(); ++i)
                {
                        if (i != path.size() && path[i] != L'\\' && path[i] != L'/')
                        {
                                continue;
                        }
                        if (i == 0 || (i == 2 && path[1] == L':'))
                        {
                                continue; // root of a drive or of a UNC/relative path
                        }
                        const std::wstring prefix = path.substr(0, i);
                        if (!CreateDirectoryW(prefix.c_str(), nullptr))
                        {
                                const DWORD error = GetLastError();
                                if (error != ERROR_ALREADY_EXISTS && error != ERROR_ACCESS_DENIED && i == path.size())
                                {
                                        CrashProgress("[Crash] Failed to ensure directory %s (err=%lu)\n", ToUtf8(path).c_str(), error);
                                }
                        }
                }
        }

        void WriteTextFile(const std::wstring& path, const std::string& content)
        {
                HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                        return;
                }

                DWORD bytesToWrite = static_cast<DWORD>(content.size());
                DWORD written = 0;
                if (bytesToWrite > 0)
                {
                        WriteFile(file, content.data(), bytesToWrite, &written, nullptr);
                }

                CloseHandle(file);
        }

        // ---------------------------------------------------------------------
        // Address / register / stack detail.
        //
        // Everything below exists because a crash_context.txt carrying only the
        // exception code and a bare address cannot distinguish the two failure
        // families we actually ship: a null-ish data dereference, and control
        // flow landing on a garbage address. Telling those apart used to require
        // shipping the reporter's whole 900 MB dump across three zip parts. A
        // module+offset, the register set and a walked stack answer it in the
        // text file, and cost nothing when nothing crashes.
        // ---------------------------------------------------------------------

        std::string Hex32(uint32_t value)
        {
                char buffer[16];
                sprintf_s(buffer, "%08X", value);
                return std::string(buffer);
        }

        const char* ExceptionCodeName(DWORD code)
        {
                switch (code)
                {
                case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
                case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
                case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
                case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
                case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
                case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
                case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
                case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
                case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
                case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
                case STATUS_HEAP_CORRUPTION:             return "STATUS_HEAP_CORRUPTION";
                case STATUS_FATAL_APP_EXIT:              return "STATUS_FATAL_APP_EXIT";
                case 0xC0000409:                         return "STATUS_STACK_BUFFER_OVERRUN (CRT fastfail)";
                case 0xE06D7363:                         return "C++ exception (MSVC)";
                default:                                 return "<unknown>";
                }
        }

        // SizeOfImage read straight out of the mapped PE headers, so no psapi
        // dependency is added to the crash path.
        uint32_t ImageSizeOf(HMODULE module)
        {
                if (module == nullptr)
                {
                        return 0;
                }

                const uint8_t* const base = reinterpret_cast<const uint8_t*>(module);
                const IMAGE_DOS_HEADER* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
                {
                        return 0;
                }

                const IMAGE_NT_HEADERS32* const nt =
                        reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
                if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
                {
                        return 0;
                }

                return nt->OptionalHeader.SizeOfImage;
        }

        std::string ModuleBaseName(HMODULE module)
        {
                wchar_t path[MAX_PATH] = {};
                const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
                if (length == 0 || length >= MAX_PATH)
                {
                        return std::string("<unknown module>");
                }

                std::wstring full(path, length);
                const size_t lastSlash = full.find_last_of(L"\\/");
                return ToUtf8(lastSlash == std::wstring::npos ? full : full.substr(lastSlash + 1));
        }

        // "BBCF.exe+0x4f350", or "<unmapped>" when the address is not inside any
        // loaded module - which is itself the single most diagnostic thing this
        // file can say, so it is never silently omitted.
        std::string DescribeAddress(uintptr_t address)
        {
                HMODULE module = nullptr;
                if (address == 0)
                {
                        return "<null>";
                }

                if (!GetModuleHandleExW(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address),
                            &module) ||
                    module == nullptr)
                {
                        return "<unmapped>";
                }

                std::ostringstream oss;
                oss << ModuleBaseName(module) << "+0x" << std::hex
                    << (address - reinterpret_cast<uintptr_t>(module));
                return oss.str();
        }

        struct SymbolApi
        {
                using SymSetOptions_t = DWORD(WINAPI*)(DWORD);
                using SymInitializeW_t = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
                using SymCleanup_t = BOOL(WINAPI*)(HANDLE);
                using SymFromAddr_t = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
                using SymFunctionTableAccess64_t = PVOID(WINAPI*)(HANDLE, DWORD64);
                using SymGetModuleBase64_t = DWORD64(WINAPI*)(HANDLE, DWORD64);
                using StackWalk64_t = BOOL(WINAPI*)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
                                                    PREAD_PROCESS_MEMORY_ROUTINE64,
                                                    PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                                    PGET_MODULE_BASE_ROUTINE64,
                                                    PTRANSLATE_ADDRESS_ROUTINE64);

                SymSetOptions_t SetOptions = nullptr;
                SymInitializeW_t Initialize = nullptr;
                SymCleanup_t Cleanup = nullptr;
                SymFromAddr_t FromAddr = nullptr;
                SymFunctionTableAccess64_t FunctionTableAccess = nullptr;
                SymGetModuleBase64_t GetModuleBase = nullptr;
                StackWalk64_t StackWalk = nullptr;
                bool initialized = false;

                bool CanWalk() const
                {
                        return StackWalk != nullptr && FunctionTableAccess != nullptr && GetModuleBase != nullptr;
                }
        };

        void LoadSymbolApi(HMODULE dbghelp, SymbolApi& api)
        {
                if (dbghelp == nullptr)
                {
                        return;
                }

                api.SetOptions = reinterpret_cast<SymbolApi::SymSetOptions_t>(GetProcAddress(dbghelp, "SymSetOptions"));
                api.Initialize = reinterpret_cast<SymbolApi::SymInitializeW_t>(GetProcAddress(dbghelp, "SymInitializeW"));
                api.Cleanup = reinterpret_cast<SymbolApi::SymCleanup_t>(GetProcAddress(dbghelp, "SymCleanup"));
                api.FromAddr = reinterpret_cast<SymbolApi::SymFromAddr_t>(GetProcAddress(dbghelp, "SymFromAddr"));
                api.FunctionTableAccess = reinterpret_cast<SymbolApi::SymFunctionTableAccess64_t>(
                        GetProcAddress(dbghelp, "SymFunctionTableAccess64"));
                api.GetModuleBase = reinterpret_cast<SymbolApi::SymGetModuleBase64_t>(
                        GetProcAddress(dbghelp, "SymGetModuleBase64"));
                api.StackWalk = reinterpret_cast<SymbolApi::StackWalk64_t>(GetProcAddress(dbghelp, "StackWalk64"));

                if (api.SetOptions)
                {
                        // No SYMOPT_EXACT_SYMBOLS and no symbol server: an explicit
                        // search path overrides any _NT_SYMBOL_PATH the user happens
                        // to have set, so a crashing game never stalls on a network
                        // symbol fetch. Export-only resolution is enough to make a
                        // system-DLL frame readable, and the mod's own PDB (when it
                        // sits next to the DLL) names our frames outright.
                        api.SetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS |
                                       SYMOPT_FAIL_CRITICAL_ERRORS);
                }

                if (api.Initialize)
                {
                        std::wstring searchPath = GetExecutableDirectory();

                        HMODULE self = nullptr;
                        if (GetModuleHandleExW(
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(&LoadSymbolApi),
                                    &self) &&
                            self != nullptr)
                        {
                                wchar_t selfPath[MAX_PATH] = {};
                                const DWORD length = GetModuleFileNameW(self, selfPath, MAX_PATH);
                                if (length != 0 && length < MAX_PATH)
                                {
                                        std::wstring full(selfPath, length);
                                        const size_t lastSlash = full.find_last_of(L"\\/");
                                        if (lastSlash != std::wstring::npos)
                                        {
                                                const std::wstring selfDir = full.substr(0, lastSlash);
                                                if (selfDir != searchPath)
                                                {
                                                        if (!searchPath.empty())
                                                        {
                                                                searchPath += L";";
                                                        }
                                                        searchPath += selfDir;
                                                }
                                        }
                                }
                        }

                        api.initialized = api.Initialize(GetCurrentProcess(),
                                                         searchPath.empty() ? nullptr : searchPath.c_str(),
                                                         TRUE) != FALSE;
                }
        }

        struct WalkedFrame
        {
                uint64_t pc;
                uint64_t frame;
        };

        // POD-only and SEH-guarded on purpose: dbghelp walking a corrupt stack in
        // a dying process is allowed to fault, and it must never take the bundle
        // down with it. No C++ object with a destructor may live in this scope.
        int WalkStackGuarded(const SymbolApi& api, const CONTEXT& source, WalkedFrame* frames, int maxFrames)
        {
                int count = 0;

                __try
                {
                        CONTEXT context = source;

                        STACKFRAME64 stackFrame{};
                        stackFrame.AddrPC.Mode = AddrModeFlat;
                        stackFrame.AddrFrame.Mode = AddrModeFlat;
                        stackFrame.AddrStack.Mode = AddrModeFlat;
#if defined(_M_IX86)
                        stackFrame.AddrPC.Offset = context.Eip;
                        stackFrame.AddrFrame.Offset = context.Ebp;
                        stackFrame.AddrStack.Offset = context.Esp;
                        const DWORD machine = IMAGE_FILE_MACHINE_I386;
#elif defined(_M_X64)
                        stackFrame.AddrPC.Offset = context.Rip;
                        stackFrame.AddrFrame.Offset = context.Rbp;
                        stackFrame.AddrStack.Offset = context.Rsp;
                        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
                        return 0;
#endif

                        while (count < maxFrames)
                        {
                                if (!api.StackWalk(machine, GetCurrentProcess(), GetCurrentThread(), &stackFrame,
                                                   &context, nullptr, api.FunctionTableAccess, api.GetModuleBase, nullptr))
                                {
                                        break;
                                }

                                if (stackFrame.AddrPC.Offset == 0)
                                {
                                        break;
                                }

                                frames[count].pc = stackFrame.AddrPC.Offset;
                                frames[count].frame = stackFrame.AddrFrame.Offset;
                                ++count;
                        }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                        // Keep whatever frames were collected before the fault.
                }

                return count;
        }

        std::string DescribeSymbol(const SymbolApi& api, uint64_t address)
        {
                if (!api.initialized || api.FromAddr == nullptr)
                {
                        return std::string();
                }

                alignas(SYMBOL_INFO) uint8_t storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
                SYMBOL_INFO* const symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = MAX_SYM_NAME;

                DWORD64 displacement = 0;
                if (!api.FromAddr(GetCurrentProcess(), static_cast<DWORD64>(address), &displacement, symbol))
                {
                        return std::string();
                }

                // With no PDBs, dbghelp falls back to the nearest export, which for
                // an address deep inside a private function names something it has
                // nothing to do with. Past a sane function size that guess is worse
                // than silence - the module+offset next to it is always exact.
                if (displacement > 0x4000)
                {
                        return std::string();
                }

                std::ostringstream oss;
                oss << " " << symbol->Name;
                if (displacement != 0)
                {
                        oss << "+0x" << std::hex << displacement;
                }
                return oss.str();
        }

        void AppendModuleLine(std::ostringstream& oss, const char* label, HMODULE module)
        {
                if (module == nullptr)
                {
                        return;
                }

                const uintptr_t base = reinterpret_cast<uintptr_t>(module);
                const uint32_t size = ImageSizeOf(module);
                oss << "  " << ModuleBaseName(module) << "  base=0x" << Hex32(static_cast<uint32_t>(base))
                    << " size=0x" << std::hex << size << std::dec;
                if (label != nullptr)
                {
                        oss << "  (" << label << ")";
                }
                oss << "\n";
        }

        // ntdll raises STATUS_HEAP_CORRUPTION with exactly one parameter: a pointer to the
        // heap manager's own failure record (ntdll!_HEAP_FAILURE_INFORMATION). It is the
        // record `!heap -s` prints, and it carries the only facts a heap crash can be
        // triaged from - which heap, which block, and the two blocks either side that were
        // still intact. None of that is anywhere else in a dump, and reconstructing it by
        // hand means decoding ntdll's LFH checks instruction by instruction.
        //
        // Layout verified field-by-field against a real 0xC0000374 dump (Win11 26100 x86)
        // and against ntdll's public symbols; Version/StructureSize are checked before any
        // other field is read, so a future layout change degrades to "not decoded" rather
        // than printing nonsense.
        struct HeapFailureRecord
        {
                ULONG Version;                   // 0x00
                ULONG StructureSize;             // 0x04
                ULONG FailureType;               // 0x08
                PVOID HeapAddress;               // 0x0C
                PVOID Address;                   // 0x10
                PVOID Param1;                    // 0x14
                PVOID Param2;                    // 0x18
                PVOID Param3;                    // 0x1C
                PVOID PreviousBlock;             // 0x20
                PVOID NextBlock;                 // 0x24
                ULONG_PTR ExpectedDecodedEntry[2]; // 0x28
                PVOID StackTrace[32];            // 0x30
        };

        const char* HeapFailureTypeName(ULONG type)
        {
                // ntdll!_HEAP_FAILURE_TYPE.
                static const char* const kNames[] = {
                        "internal", "unknown", "generic", "entry_corruption",
                        "multiple_entries_corruption", "virtual_block_corruption",
                        "buffer_overrun", "buffer_underrun", "block_not_busy",
                        "invalid_argument", "invalid_allocation_type", "usage_after_free",
                        "cross_heap_operation", "freelists_corruption", "listentry_corruption",
                        "lfh_bitmap_mismatch", "segment_lfh_bitmap_corruption",
                        "segment_lfh_double_free", "vs_subsegment_corruption", "null_heap",
                        "allocation_limit", "commit_limit", "invalid_va_mgr_query",
                        "segment_lfh_delay_free_corruption",
                };
                return (type < _countof(kNames)) ? kNames[type] : "<unknown>";
        }

        // No C++ objects with destructors here: the __try makes them illegal (C2712).
        bool ReadHeapFailureRecord(uintptr_t address, HeapFailureRecord* out)
        {
                __try
                {
                        const HeapFailureRecord* src = reinterpret_cast<const HeapFailureRecord*>(address);
                        if (src->Version > 8 ||
                            src->StructureSize < sizeof(HeapFailureRecord) ||
                            src->StructureSize > 0x4000)
                        {
                                return false;
                        }
                        *out = *src;
                        return true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                        return false;
                }
        }

        void AppendHeapFailureDetail(std::ostringstream& oss, PEXCEPTION_POINTERS ExPtr)
        {
                if (ExPtr == nullptr || ExPtr->ExceptionRecord == nullptr ||
                    ExPtr->ExceptionRecord->ExceptionCode != STATUS_HEAP_CORRUPTION ||
                    ExPtr->ExceptionRecord->NumberParameters < 1)
                {
                        return;
                }

                HeapFailureRecord record{};
                const uintptr_t recordAddress =
                        static_cast<uintptr_t>(ExPtr->ExceptionRecord->ExceptionInformation[0]);
                if (!ReadHeapFailureRecord(recordAddress, &record))
                {
                        oss << "\nHeap failure record at 0x" << Hex32(static_cast<uint32_t>(recordAddress))
                            << " could not be decoded.\n";
                        return;
                }

                oss << "\nHeap failure record (ntdll): type " << record.FailureType
                    << " heap_failure_" << HeapFailureTypeName(record.FailureType) << "\n";
                oss << "  Heap:          0x" << Hex32(reinterpret_cast<uint32_t>(record.HeapAddress));
                if (record.HeapAddress == GetProcessHeap())
                {
                        oss << "  (the process default heap - the game's CRT and the mod's CRT\n"
                               "                              both allocate from it, so either side can be the culprit)";
                }
                oss << "\n";
                oss << "  Block:         0x" << Hex32(reinterpret_cast<uint32_t>(record.Address)) << "\n";
                oss << "  Intact before: 0x" << Hex32(reinterpret_cast<uint32_t>(record.PreviousBlock)) << "\n";
                oss << "  Intact after:  0x" << Hex32(reinterpret_cast<uint32_t>(record.NextBlock)) << "\n";

                oss << "  Heap manager stack:\n";
                for (size_t i = 0; i < _countof(record.StackTrace); ++i)
                {
                        const uintptr_t frame = reinterpret_cast<uintptr_t>(record.StackTrace[i]);
                        if (frame == 0)
                        {
                                break;
                        }
                        oss << "    0x" << Hex32(static_cast<uint32_t>(frame)) << "  "
                            << DescribeAddress(frame) << "\n";
                }

                oss << "  NOTE: this is corruption *detected* while the heap was being used, not the\n"
                       "        write that caused it. The culprit ran earlier and left no trace in this\n"
                       "        dump. To catch the writer itself, reproduce once under Page Heap:\n"
                       "          gflags /p /enable BBCF.exe /full   (elevated, then reproduce)\n"
                       "          gflags /p /disable BBCF.exe        (afterwards)\n";
        }

        void AppendExceptionDetail(std::ostringstream& oss, PEXCEPTION_POINTERS ExPtr)
        {
                if (ExPtr == nullptr || ExPtr->ExceptionRecord == nullptr)
                {
                        oss << "Exception record: <not available>\n";
                        return;
                }

                const EXCEPTION_RECORD& record = *ExPtr->ExceptionRecord;
                const uintptr_t faultPc = reinterpret_cast<uintptr_t>(record.ExceptionAddress);

                // Original three keys keep their exact names so anything already
                // reading this file keeps working; the detail is additive.
                oss << "Exception code: 0x" << Hex32(record.ExceptionCode)
                    << " (" << ExceptionCodeName(record.ExceptionCode) << ")\n";
                oss << "Exception flags: 0x" << Hex32(record.ExceptionFlags) << "\n";
                oss << "Exception address: 0x" << Hex32(static_cast<uint32_t>(faultPc))
                    << "  " << DescribeAddress(faultPc) << "\n";

                if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                     record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
                    record.NumberParameters >= 2)
                {
                        const ULONG_PTR kind = record.ExceptionInformation[0];
                        const uintptr_t target = static_cast<uintptr_t>(record.ExceptionInformation[1]);
                        const char* kindText = kind == 0 ? "read" : (kind == 1 ? "write" : (kind == 8 ? "execute (DEP)" : "<unknown>"));
                        oss << "Access type: " << kindText << " at 0x" << Hex32(static_cast<uint32_t>(target))
                            << "  " << DescribeAddress(target) << "\n";
                }

                // The distinction that matters when triaging: a fault *at* an
                // address inside a module is ordinary bad-pointer code, but a
                // fault whose instruction pointer is nowhere means control flow
                // itself was corrupted, and the culprit is almost always a hook.
                if (DescribeAddress(faultPc) == "<unmapped>" || faultPc < 0x10000)
                {
                        oss << "Note: the instruction pointer is not inside any loaded module, so control flow\n"
                               "      was transferred to a bad address - an indirect call/jmp through a corrupt\n"
                               "      pointer, or a 'ret' that popped the wrong value. Suspect a JMP hook first;\n"
                               "      see docs/HookEpilogueContract.md.\n";
                }
        }

        void AppendRegisters(std::ostringstream& oss, PEXCEPTION_POINTERS ExPtr)
        {
                if (ExPtr == nullptr || ExPtr->ContextRecord == nullptr)
                {
                        return;
                }

                const CONTEXT& c = *ExPtr->ContextRecord;
                // The report is usually written by the reporter thread, so the current
                // thread is not the one that faulted. Printing GetCurrentThreadId() here named
                // the reporter as "the faulting thread" in every off-thread report.
                const DWORD faultingThread = g_faultingThreadId.load();
                oss << "\nRegisters (faulting thread "
                    << (faultingThread != 0 ? faultingThread : GetCurrentThreadId())
                    << (faultingThread != 0 && faultingThread == ExitDiagnostics::MainThreadId() ? ", the main thread" : "")
                    << "):\n";
#if defined(_M_IX86)
                oss << "  eax=" << Hex32(c.Eax) << " ebx=" << Hex32(c.Ebx)
                    << " ecx=" << Hex32(c.Ecx) << " edx=" << Hex32(c.Edx) << "\n";
                oss << "  esi=" << Hex32(c.Esi) << " edi=" << Hex32(c.Edi)
                    << " ebp=" << Hex32(c.Ebp) << " esp=" << Hex32(c.Esp) << "\n";
                oss << "  eip=" << Hex32(c.Eip) << " eflags=" << Hex32(c.EFlags)
                    << " cs=" << Hex32(c.SegCs) << " ss=" << Hex32(c.SegSs) << "\n";

                // Inside a live frame esp is at or below ebp. Above it means the
                // frame was already torn down when the fault happened, which is
                // what a 'ret' that consumed a saved register instead of the
                // return address leaves behind. Worth naming, because that
                // shipped twice.
                // A 32-bit process always runs with cs=0x23 (0x1B on native 32-bit Windows).
                // Anything else means the record itself is not a real register snapshot -
                // seen from a Windows-on-ARM machine, where BBCF runs under x86 emulation and
                // the context came back with cs=ss=0x3A64 and every GPR zero. The stack
                // below is walked from this record, so it has to be read with that in mind.
                if ((c.SegCs & 0xFFFF) != 0x23 && (c.SegCs & 0xFFFF) != 0x1B)
                {
                        oss << "  Note: this register block is not a valid x86 context (cs should be 0x23), so\n"
                               "        treat the registers and the walked stack below as unreliable. Seen under\n"
                               "        x86 emulation on ARM64 - check the Host line. The dump's own thread list\n"
                               "        is still accurate.\n";
                }

                if (c.Ebp != 0 && c.Esp > c.Ebp)
                {
                        oss << "  Note: esp is above ebp, so the frame was already unwound at the point of the\n"
                               "        fault - consistent with a 'ret' that consumed a saved register instead of\n"
                               "        the return address. See docs/HookEpilogueContract.md.\n";
                }
#else
                oss << "  <register dump not implemented for this architecture>\n";
#endif
        }

        void AppendStackTrace(std::ostringstream& oss, PEXCEPTION_POINTERS ExPtr, const SymbolApi& api)
        {
                if (ExPtr == nullptr || ExPtr->ContextRecord == nullptr)
                {
                        return;
                }

                if (!api.CanWalk())
                {
                        oss << "\nStack: <dbghelp StackWalk64 unavailable>\n";
                        return;
                }

                constexpr int kMaxFrames = 64;
                WalkedFrame frames[kMaxFrames]{};
                const int count = WalkStackGuarded(api, *ExPtr->ContextRecord, frames, kMaxFrames);

                oss << "\nStack (faulting thread, innermost first):\n";
                if (count == 0)
                {
                        oss << "  <stack walk produced no frames>\n";
                        return;
                }

                for (int i = 0; i < count; ++i)
                {
                        char index[8];
                        sprintf_s(index, "%02d", i);
                        oss << "  #" << index << " 0x" << Hex32(static_cast<uint32_t>(frames[i].pc))
                            << "  " << DescribeAddress(static_cast<uintptr_t>(frames[i].pc))
                            << DescribeSymbol(api, frames[i].pc) << "\n";
                }
        }

        // What the machine really is. BBCF is a 32-bit process, so IsWow64Process alone
        // cannot tell x64 from ARM64, and on ARM64 the game runs under x86 emulation -
        // which changes what a garbled register block or an odd timing race means.
        std::string DescribeHost()
        {
                using IsWow64Process2_t = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
                const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
                const auto isWow64Process2 = kernel32
                        ? reinterpret_cast<IsWow64Process2_t>(GetProcAddress(kernel32, "IsWow64Process2"))
                        : nullptr;
                if (isWow64Process2 != nullptr)
                {
                        USHORT processMachine = 0;
                        USHORT nativeMachine = 0;
                        if (isWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine))
                        {
                                switch (nativeMachine)
                                {
                                case 0xAA64: return "ARM64 Windows - BBCF runs under x86 emulation";
                                case 0x8664: return "x64 Windows (WOW64)";
                                case 0x014C: return "32-bit x86 Windows";
                                default:
                                {
                                        char text[64];
                                        sprintf_s(text, "native machine 0x%04X", nativeMachine);
                                        return text;
                                }
                                }
                        }
                }

                BOOL isWow64 = FALSE;
                IsWow64Process(GetCurrentProcess(), &isWow64);
                return isWow64 ? "64-bit Windows (WOW64; IsWow64Process2 unavailable)" : "32-bit Windows";
        }

        // The main thread at the moment of the report, captured from another thread.
        //
        // This is the single most useful thing a crash on a worker thread can show, and it
        // is what cracked Randomtrr2's report: the worker's stack only said "a BBCF job
        // touched freed memory", while the main thread was sitting in exit() behind a
        // "Thread Delete Time Out." box - i.e. the game was already closing.
        //
        // POD-only and SEH-guarded: the main thread is SUSPENDED while this runs, so
        // nothing here may allocate or take a lock (it could be holding the heap lock),
        // and its stack is read raw rather than through dbghelp. Two views: the EBP chain,
        // which is exact where frame pointers exist, and a scan for values that point into
        // BBCF.exe or the mod, which survives frame-pointer-omitted system DLLs.
        struct ThreadStackCapture
        {
                bool suspended;
                bool haveContext;
                DWORD error;
                DWORD eip;
                DWORD esp;
                DWORD ebp;
                uintptr_t chain[40];
                int chainCount;
                uintptr_t scan[32];
                int scanCount;
        };

        bool InRange(uintptr_t value, uintptr_t begin, uintptr_t end)
        {
                return value >= begin && value < end;
        }

        void CaptureThreadStackGuarded(HANDLE thread, uintptr_t gameBegin, uintptr_t gameEnd,
                                       uintptr_t modBegin, uintptr_t modEnd, ThreadStackCapture& out)
        {
                if (SuspendThread(thread) == static_cast<DWORD>(-1))
                {
                        out.error = GetLastError();
                        return;
                }
                out.suspended = true;

                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (!GetThreadContext(thread, &context))
                {
                        out.error = GetLastError();
                        ResumeThread(thread);
                        return;
                }
                out.haveContext = true;
#if defined(_M_IX86)
                out.eip = context.Eip;
                out.esp = context.Esp;
                out.ebp = context.Ebp;

                // Copy the stack while the thread is suspended, analyse it after resuming.
                // Nothing here may fault: dispatching an exception walks the loaded-module
                // list, and a thread suspended inside a DLL load or unload holds that lock,
                // so the fault never finishes and the thread is never resumed (2026-10-09,
                // the frame-stall sampler hung the game's close exactly that way).
                static unsigned char stackCopy[0x8000];
                size_t stackBytes = 0;
                MEMORY_BASIC_INFORMATION region{};
                if (VirtualQuery(reinterpret_cast<const void*>(context.Esp), &region, sizeof(region)) != 0 &&
                    region.State == MEM_COMMIT && (region.Protect & PAGE_GUARD) == 0)
                {
                        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
                        size_t want = regionEnd - context.Esp;
                        if (want > sizeof(stackCopy))
                        {
                                want = sizeof(stackCopy);
                        }
                        SIZE_T read = 0;
                        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(context.Esp),
                                              stackCopy, want, &read))
                        {
                                stackBytes = read & ~static_cast<size_t>(3);
                        }
                }
                ResumeThread(thread);

                const uintptr_t stackLow = context.Esp;
                const uintptr_t stackHigh = context.Esp + stackBytes;
                auto readStack = [&](uintptr_t address, uintptr_t& value) -> bool
                {
                        if (address < stackLow || address + sizeof(uintptr_t) > stackHigh)
                        {
                                return false;
                        }
                        memcpy(&value, stackCopy + (address - stackLow), sizeof(value));
                        return true;
                };

                out.chain[out.chainCount++] = context.Eip;
                uintptr_t frame = context.Ebp;
                while (out.chainCount < static_cast<int>(_countof(out.chain)))
                {
                        uintptr_t next = 0;
                        uintptr_t returnAddress = 0;
                        if ((frame & 3) != 0 || !readStack(frame, next) || !readStack(frame + 4, returnAddress) ||
                            returnAddress == 0)
                        {
                                break;
                        }
                        out.chain[out.chainCount++] = returnAddress;
                        if (next <= frame)
                        {
                                break;
                        }
                        frame = next;
                }

                for (size_t offset = 0; offset + sizeof(uintptr_t) <= stackBytes &&
                     out.scanCount < static_cast<int>(_countof(out.scan)); offset += sizeof(uintptr_t))
                {
                        uintptr_t value = 0;
                        memcpy(&value, stackCopy + offset, sizeof(value));
                        if (InRange(value, gameBegin, gameEnd) || InRange(value, modBegin, modEnd))
                        {
                                out.scan[out.scanCount++] = value;
                        }
                }
#else
                ResumeThread(thread);
#endif
        }

        // A stack slot is only worth printing if it is a return address, i.e. the bytes
        // just before it are a call: E8 rel32 (5 bytes), FF /2 with a disp32 or absolute
        // operand (6), FF /2 with a disp8 (3), or FF /2 through a register (2). Everything
        // else in a module range is a pointer to data or a string. Guarded: the candidate
        // can sit at the very start of a mapping.
        bool LooksLikeReturnAddressGuarded(uintptr_t value)
        {
                __try
                {
                        const uint8_t* const p = reinterpret_cast<const uint8_t*>(value);
                        if (p[-5] == 0xE8)
                        {
                                return true;
                        }
                        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)
                        {
                                return true;
                        }
                        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
                        {
                                return true;
                        }
                        if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0)
                        {
                                return true;
                        }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
                return false;
        }

        void AppendMainThreadStack(std::ostringstream& oss, const SymbolApi& api)
        {
                const DWORD mainThread = ExitDiagnostics::MainThreadId();
                const HANDLE handle = ExitDiagnostics::MainThreadHandle();
                const DWORD faultingThread = g_faultingThreadId.load();

                oss << "\nMain thread (" << mainThread << ") at the time of this report:\n";
                if (mainThread == 0 || handle == nullptr)
                {
                        oss << "  <main thread was not recorded at startup>\n";
                        return;
                }
                if (mainThread == faultingThread || mainThread == GetCurrentThreadId())
                {
                        oss << "  <the main thread is the faulting thread - see the stack above>\n";
                        return;
                }

                const HMODULE game = GetModuleHandleW(nullptr);
                HMODULE self = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&AppendMainThreadStack), &self);
                const uintptr_t gameBegin = reinterpret_cast<uintptr_t>(game);
                const uintptr_t modBegin = reinterpret_cast<uintptr_t>(self);

                ThreadStackCapture capture{};
                CaptureThreadStackGuarded(handle, gameBegin, gameBegin + ImageSizeOf(game),
                                          modBegin, modBegin + (self ? ImageSizeOf(self) : 0), capture);
                if (!capture.haveContext)
                {
                        oss << "  <could not " << (capture.suspended ? "read its context" : "suspend it")
                            << ", error " << capture.error << ">\n";
                        return;
                }

                oss << "  eip=" << Hex32(capture.eip) << " esp=" << Hex32(capture.esp)
                    << " ebp=" << Hex32(capture.ebp) << "\n";
                oss << "  Frame chain (innermost first):\n";
                for (int i = 0; i < capture.chainCount; ++i)
                {
                        char index[8];
                        sprintf_s(index, "%02d", i);
                        oss << "    #" << index << " 0x" << Hex32(static_cast<uint32_t>(capture.chain[i])) << "  "
                            << DescribeAddress(capture.chain[i]) << DescribeSymbol(api, capture.chain[i]) << "\n";
                }
                oss << "  Return addresses into BBCF.exe / the mod found on its stack (innermost first;\n"
                       "  survives frames the chain above cannot follow):\n";
                int printed = 0;
                uintptr_t previous = 0;
                for (int i = 0; i < capture.scanCount; ++i)
                {
                        const uintptr_t value = capture.scan[i];
                        if (value == previous || !LooksLikeReturnAddressGuarded(value))
                        {
                                continue;
                        }
                        previous = value;
                        ++printed;
                        oss << "    " << DescribeAddress(value) << DescribeSymbol(api, value) << "\n";
                }
                if (printed == 0)
                {
                        oss << "    <none>\n";
                }
        }

        std::string BuildContextText(PEXCEPTION_POINTERS ExPtr, const std::wstring& dumpPath, const SymbolApi& api)
        {
                std::ostringstream oss;
                oss << "BBCF Improvement Mod crash report" << "\n";
                oss << "Mod version: " << MOD_VERSION_NUM << "\n";
                oss << "Dump file: " << ToUtf8(dumpPath) << "\n";

                oss << "Host: " << DescribeHost() << "\n";
                if (ExitDiagnostics::IsExiting())
                {
                        oss << "DURING SHUTDOWN: the game was already closing when this happened - see\n"
                               "\"Process phase\" and \"Lifecycle events\" below for what closed it.\n";
                }

                AppendExceptionDetail(oss, ExPtr);
                AppendHeapFailureDetail(oss, ExPtr);
                AppendRegisters(oss, ExPtr);
                AppendStackTrace(oss, ExPtr, api);
                AppendMainThreadStack(oss, api);
                ExitDiagnostics::AppendReport(oss);

                // Just the two bases every triage starts from: without them a
                // "BBCF.exe+0x4f350" cannot be turned back into the static address
                // the disassembly uses.
                oss << "\nModules:\n";
                const HMODULE host = GetModuleHandleW(nullptr);
                AppendModuleLine(oss, "game", host);

                HMODULE self = nullptr;
                if (GetModuleHandleExW(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&BuildContextText),
                            &self) &&
                    self != host)
                {
                        AppendModuleLine(oss, "the mod", self);
                }

                oss << "\n";
                oss << "GenerateDebugLogs: " << Settings::settingsIni.generateDebugLogs << "\n";
                oss << "Language: " << Settings::settingsIni.language << "\n";

                return oss.str();
        }

        std::string BuildUserStreamPayload(const std::string& context, const std::string& logs)
        {
                std::ostringstream oss;
                oss << context << "\n";
                oss << "================== Recent Logs (in-memory ring) ==================\n";
                if (logs.empty())
                {
                        oss << "<no log entries captured>\n";
                }
                else
                {
                        oss << logs;
                }

                return oss.str();
        }

        // ---------------------------------------------------------------------------
        // Targeted extra memory
        // ---------------------------------------------------------------------------
        //
        // A medium dump captures thread stacks and whatever they point at, which leaves
        // exactly the memory a corruption bug is diagnosed from out of the file: the
        // faulting heap region is not referenced by any stack, so it arrived as a 1 KB
        // sliver that ended mid-structure. These ranges are named explicitly instead, and
        // cost tens of KB against a dump already measured in tens of MB.
        struct DumpRange
        {
                ULONG64 base;
                ULONG size;
        };

        DumpRange g_extraRanges[96] = {};
        int g_extraRangeCount = 0;
        int g_extraRangeCursor = 0;

        // Heap handles, snapshotted while the process is healthy. A heap-corruption crash
        // usually faults *inside* the heap lock, and the faulting thread is parked holding
        // it while the reporter thread writes the bundle - so calling GetProcessHeaps here
        // would deadlock the report. Reading a plain array cannot.
        HANDLE g_processHeaps[64] = {};
        std::atomic<DWORD> g_processHeapCount{ 0 };

        // Refreshed on a slow cadence from the watchdog thread, because a heap created
        // after startup (the game loads codecs and D3D drivers long after DllMain) would
        // otherwise never be in the list. Entries are written before the count, so a crash
        // landing mid-refresh reads handles that are stale at worst - never past the end.
        void RefreshProcessHeapSnapshot()
        {
                HANDLE heaps[_countof(g_processHeaps)] = {};
                DWORD count = GetProcessHeaps(static_cast<DWORD>(_countof(heaps)), heaps);
                if (count == 0)
                {
                        return;
                }
                if (count > _countof(heaps))
                {
                        count = static_cast<DWORD>(_countof(heaps));
                }
                memcpy(g_processHeaps, heaps, count * sizeof(HANDLE));
                g_processHeapCount.store(count);
        }

        void AddDumpRange(uintptr_t base, SIZE_T size)
        {
                if (base == 0 || size == 0 || g_extraRangeCount >= static_cast<int>(_countof(g_extraRanges)))
                {
                        return;
                }
                g_extraRanges[g_extraRangeCount].base = static_cast<ULONG64>(base);
                g_extraRanges[g_extraRangeCount].size = static_cast<ULONG>(size);
                ++g_extraRangeCount;
        }

        // Captures the committed region an address lives in, so a structure is whole in the
        // dump instead of clipped. VirtualQuery takes no heap lock, so it is safe on this
        // path; a region larger than the cap is windowed around the address instead.
        void AddDumpRegionAround(uintptr_t address, SIZE_T cap)
        {
                if (address == 0)
                {
                        return;
                }

                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi) ||
                    mbi.State != MEM_COMMIT)
                {
                        return;
                }

                uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                SIZE_T size = mbi.RegionSize;
                if (size > cap)
                {
                        const SIZE_T half = cap / 2;
                        uintptr_t start = (address > base + half) ? (address - half) : base;
                        if (start + cap > base + size)
                        {
                                start = base + size - cap;
                        }
                        base = start;
                        size = cap;
                }
                AddDumpRange(base, size);
        }

        void PrepareExtraDumpRanges(PEXCEPTION_POINTERS ExPtr)
        {
                g_extraRangeCount = 0;
                g_extraRangeCursor = 0;

                // The heap headers. `!heap` needs them for the encoding key and the segment
                // list, and without them every heap extension in the debugger refuses to
                // run ("Failed to read heap key"). One page each, so <256 KB in total.
                const DWORD heapCount = g_processHeapCount.load();
                for (DWORD i = 0; i < heapCount; ++i)
                {
                        AddDumpRange(reinterpret_cast<uintptr_t>(g_processHeaps[i]), 0x1000);
                }

                if (ExPtr == nullptr || ExPtr->ExceptionRecord == nullptr)
                {
                        return;
                }

                const EXCEPTION_RECORD& record = *ExPtr->ExceptionRecord;

                if (record.ExceptionCode == STATUS_HEAP_CORRUPTION && record.NumberParameters >= 1)
                {
                        const uintptr_t infoAddress = static_cast<uintptr_t>(record.ExceptionInformation[0]);
                        AddDumpRange(infoAddress, 0x400); // the failure record itself

                        HeapFailureRecord failure{};
                        if (ReadHeapFailureRecord(infoAddress, &failure))
                        {
                                // The corrupted block and its neighbours, whole. 1 MB covers
                                // an LFH subsegment and everything around it with room to
                                // spare; the region is usually far smaller than the cap.
                                AddDumpRegionAround(reinterpret_cast<uintptr_t>(failure.Address), 1u << 20);
                                AddDumpRegionAround(reinterpret_cast<uintptr_t>(failure.PreviousBlock), 1u << 20);
                                AddDumpRegionAround(reinterpret_cast<uintptr_t>(failure.NextBlock), 1u << 20);
                                AddDumpRange(reinterpret_cast<uintptr_t>(failure.HeapAddress), 0x1000);
                        }
                }
                else if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                          record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
                         record.NumberParameters >= 2)
                {
                        // The object that was being read or written, when it is mapped at
                        // all. A null or otherwise wild address simply yields nothing.
                        AddDumpRegionAround(static_cast<uintptr_t>(record.ExceptionInformation[1]), 256u * 1024u);
                }
        }

        BOOL CALLBACK MinidumpCallback(PVOID, const PMINIDUMP_CALLBACK_INPUT input, PMINIDUMP_CALLBACK_OUTPUT output)
        {
                if (input == nullptr || output == nullptr)
                {
                        return TRUE;
                }
                if (input->CallbackType != MemoryCallback)
                {
                        return TRUE; // every other decision keeps dbghelp's default
                }

                if (g_extraRangeCursor < g_extraRangeCount)
                {
                        const DumpRange& range = g_extraRanges[g_extraRangeCursor++];
                        output->MemoryBase = range.base;
                        output->MemorySize = range.size;
                        return TRUE;
                }

                // A zero-sized range is how the enumeration ends; returning FALSE here
                // would fail the whole dump.
                output->MemoryBase = 0;
                output->MemorySize = 0;
                return TRUE;
        }

        // enum: 0 = small, 1 = medium, 2 = full. Same order as CrashDumpDetail in
        // settings.def and kCrashDumpDetailOptions in SettingsIniWindow.cpp.
        MINIDUMP_TYPE DumpFlagsForDetail(int detail)
        {
                switch (detail)
                {
                case 0:
                        // Thread stacks and the module list. Enough to identify a crash
                        // site and read a call stack; not enough to inspect an object.
                        return static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                                                          MiniDumpWithUnloadedModules |
                                                          MiniDumpWithFullMemoryInfo |
                                                          MiniDumpScanMemory);

                case 2:
                        // The old unconditional behaviour: the entire committed address
                        // space. ~500 MB per dump for this game, which is how
                        // CrashReports reached 14 GB. Kept for the rare case that needs
                        // the whole heap.
                        return static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory |
                                                          MiniDumpWithFullMemoryInfo |
                                                          MiniDumpWithHandleData |
                                                          MiniDumpWithThreadInfo |
                                                          MiniDumpWithUnloadedModules |
                                                          MiniDumpWithDataSegs);

                case 1:
                default:
                        // Measured on a process holding 300 MB of committed private
                        // memory: this set produced a 29 MB dump and full memory produced
                        // 362 MB, i.e. the game's own bulk allocations are excluded
                        // entirely, which is what made dumps ~500 MB. The ~29 MB that
                        // remains is the code sections of loaded system modules; if that
                        // still needs to come down, the lever is a
                        // MINIDUMP_CALLBACK_INFORMATION that clears ModuleWriteModule for
                        // third-party modules, not a different flag - flag combinations
                        // were measured and none of them move it.
                        //
                        // MiniDumpWithFullMemoryInfo is the address-space map (the VAD
                        // list), not memory: a few hundred KB that make !address, !vprot
                        // and the heap extensions work instead of guessing from whatever
                        // slices happened to be captured.
                        //
                        // Everything the crash context and a stack walk actually consume:
                        // stacks, the memory those stacks point at (so the faulting
                        // object is inspectable), module data segments, handles and thread
                        // state. Excludes the bulk texture and audio memory that made up
                        // almost all of a full dump.
                        return static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                          MiniDumpWithDataSegs |
                                                          MiniDumpWithHandleData |
                                                          MiniDumpWithThreadInfo |
                                                          MiniDumpWithProcessThreadData |
                                                          MiniDumpWithUnloadedModules |
                                                          MiniDumpWithFullMemoryInfo |
                                                          MiniDumpScanMemory);
                }
        }

        // Oldest-first prune of previous bundles. Nothing capped this before, and with
        // every dump being full-memory that is how the folder reached 14 GB.
        void PruneOldCrashBundles(const std::wstring& crashRoot, int keep)
        {
                if (keep <= 0)
                {
                        return;
                }

                std::vector<std::wstring> bundles;

                WIN32_FIND_DATAW find{};
                const std::wstring pattern = JoinPath(crashRoot, L"Crash_*");
                HANDLE handle = FindFirstFileW(pattern.c_str(), &find);
                if (handle == INVALID_HANDLE_VALUE)
                {
                        return;
                }

                do
                {
                        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        {
                                bundles.push_back(find.cFileName);
                        }
                } while (FindNextFileW(handle, &find));
                FindClose(handle);

                // Names are Crash_YYYYMMDD_HHMMSS, so lexicographic order is chronological.
                std::sort(bundles.begin(), bundles.end());

                // keep - 1, because the caller is about to add one more.
                const size_t target = static_cast<size_t>(keep > 1 ? keep - 1 : 0);
                if (bundles.size() <= target)
                {
                        return;
                }

                const size_t toDelete = bundles.size() - target;
                for (size_t i = 0; i < toDelete; ++i)
                {
                        const std::wstring dir = JoinPath(crashRoot, bundles[i]);

                        // Only ever the files this bundle writes, never a recursive wipe:
                        // this runs unattended against a directory under the user's game
                        // install, so it deletes exactly what WriteCrashBundle created and
                        // leaves anything else (and the directory) alone if it cannot.
                        static const wchar_t* const kBundleFiles[] = { L"crash.dmp", L"logs.txt", L"crash_context.txt" };
                        for (const wchar_t* name : kBundleFiles)
                        {
                                DeleteFileW(JoinPath(dir, name).c_str());
                        }

                        if (!RemoveDirectoryW(dir.c_str()))
                        {
                                CrashProgress("[Crash] Kept %s (not empty or in use, err=%lu)\n",
                                         ToUtf8(dir).c_str(), GetLastError());
                                continue;
                        }

                        CrashProgress("[Crash] Pruned old crash bundle %s\n", ToUtf8(dir).c_str());
                }
        }
}

// ---------------------------------------------------------------------------------------
// Crash-path plumbing.
//
// A real crash on 2026-09-03 produced "[Crash] UnhandledExFilter invoked." and then
// nothing at all: no bundle, and - because the filter ends in ExitProcess - no WER report
// and no LocalDump either. The line reached disk, so the write succeeded and ForceLog never
// returned. ForceLog builds a std::string and then pushes a copy into the in-memory ring
// under the log mutex, which is two heap operations and a second lock acquisition on a
// path where the heap may be the thing that is broken.
//
// So the crash path no longer uses it. Everything below formats into a stack buffer and
// writes through ForceLogRaw, which allocates nothing and will not block on the log mutex.
// ---------------------------------------------------------------------------------------

// Set once the bundle has actually been written, so the handlers can decide whether they
// are allowed to swallow the crash or have to hand it to Windows.
static std::atomic<bool> g_crashBundleOk{ false };


// GetRecentLogs walks the ring and builds one big string, so it allocates. It is the least
// valuable artifact in the bundle and the most likely to fault on a corrupt heap, so it is
// both isolated and written last.
static void AssignRecentLogs(std::string* out)
{
        *out = GetRecentLogs();
}

static bool TryGetRecentLogs(std::string* out)
{
        __try
        {
                AssignRecentLogs(out);
                return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
                return false;
        }
}

static void WriteCrashBundleImpl(const char* reason, PEXCEPTION_POINTERS ExPtr, bool showDialog);

// Deliberately holds no object with a destructor: __try is not allowed in a function that
// needs unwinding, and this wrapper exists so that a fault anywhere in the bundle writer
// still leaves a decision the caller can act on instead of taking the process down.
void WriteCrashBundle(const char* reason, PEXCEPTION_POINTERS ExPtr, bool showDialog)
{
        if (g_crashBundleWritten.exchange(true))
        {
                CrashProgress("[Crash] WriteCrashBundle re-entered; a bundle was already "
                              "written or attempted this process.\n");
                return;
        }

        CrashProgress("[Crash] WriteCrashBundle invoked (reason=%s, showDialog=%d)\n",
                      reason ? reason : "<null>", showDialog ? 1 : 0);

        __try
        {
                WriteCrashBundleImpl(reason, ExPtr, showDialog);
        }
        __except (CrashProgress("[Crash] Faulted while writing the crash bundle "
                                "(code 0x%08X). Handing the crash to Windows so it is not "
                                "lost.\n", GetExceptionCode()),
                  EXCEPTION_EXECUTE_HANDLER)
        {
        }
}

static void WriteCrashBundleImpl(const char* reason, PEXCEPTION_POINTERS ExPtr, bool showDialog)
{
        MiniDumpWriteDump_t pMiniDumpWriteDump = nullptr;

        HMODULE hLib = LoadLibrary(_T("dbghelp"));
        if (hLib)
        {
                pMiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(hLib, "MiniDumpWriteDump"));
        }
        else
        {
                CrashProgress("[Crash] LoadLibrary(dbghelp) failed err=%lu\n", GetLastError());
        }

        const std::wstring timestamp = BuildTimestamp();
        const std::wstring crashRoot = GetCrashRootDirectory();
        const std::wstring crashDir = JoinPath(crashRoot, L"Crash_" + timestamp);
        const std::wstring dumpPath = JoinPath(crashDir, L"crash.dmp");
        const std::wstring logsPath = JoinPath(crashDir, L"logs.txt");
        const std::wstring contextPath = JoinPath(crashDir, L"crash_context.txt");

        CrashProgress("[Crash] Crash bundle paths: root=%s dir=%s dump=%s\n",
                 ToUtf8(crashRoot).c_str(),
                 ToUtf8(crashDir).c_str(),
                 ToUtf8(dumpPath).c_str());

        EnsureDirectory(crashRoot);
        PruneOldCrashBundles(crashRoot, Settings::settingsIni.crashReportsToKeep);
        EnsureDirectory(crashDir);

        // Order is deliberate: the context file is the cheapest artifact and the one that
        // usually answers the report on its own, so it lands before anything that can fail.
        // The log snapshot is the least valuable and the most likely to fault on a broken
        // heap, so it goes last.
        SymbolApi symbolApi;
        LoadSymbolApi(hLib, symbolApi);

        std::string context = BuildContextText(ExPtr, dumpPath, symbolApi);

        // Release symbols BEFORE writing the dump, not after. Walking the stack and
        // resolving names makes dbghelp map the images it needed to read, and those
        // mappings are then committed memory that MiniDumpWriteDump faithfully captures -
        // measured at 28 MB of loaded system-module code in a hello-world process, none of
        // which is worth a single byte since those images exist on every machine. Nothing
        // below this point needs symbols.
        if (symbolApi.initialized && symbolApi.Cleanup != nullptr)
        {
                symbolApi.Cleanup(GetCurrentProcess());
                symbolApi.initialized = false;
        }
        if (reason)
        {
                context.append("Reason: ");
                context.append(reason);
                context.push_back('\n');
        }
        WriteTextFile(contextPath, context);
        CrashProgress("[Crash] Wrote crash context (%zu bytes) to %s\n",
                      context.size(), ToUtf8(contextPath).c_str());

        std::string recentLogs;
        if (TryGetRecentLogs(&recentLogs))
        {
                WriteTextFile(logsPath, recentLogs);
                CrashProgress("[Crash] Wrote logs snapshot (%zu bytes) to %s\n",
                              recentLogs.size(), ToUtf8(logsPath).c_str());
        }
        else
        {
                CrashProgress("[Crash] Could not read the in-memory log ring; continuing "
                              "without logs.txt.\n");
        }

        const std::string payload = BuildUserStreamPayload(context, recentLogs);
        const ULONG payloadSize = static_cast<ULONG>(std::min<size_t>(payload.size(), std::numeric_limits<ULONG>::max()));
        MINIDUMP_USER_STREAM userStream{};
        userStream.Type = CommentStreamA;
        userStream.BufferSize = payloadSize;
        userStream.Buffer = const_cast<char*>(payload.data());

        MINIDUMP_USER_STREAM_INFORMATION userStreams{};
        userStreams.UserStreamCount = 1;
        userStreams.UserStreamArray = &userStream;

        wchar_t messageBuffer[MAX_PATH * 2] = {};
        if (pMiniDumpWriteDump)
        {
                HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

                if (hFile != INVALID_HANDLE_VALUE)
                {
                        MINIDUMP_EXCEPTION_INFORMATION md{};
                        const DWORD faultingThread = g_faultingThreadId.load();
                        md.ThreadId = (faultingThread != 0) ? faultingThread : GetCurrentThreadId();
                        md.ExceptionPointers = ExPtr;
                        md.ClientPointers = FALSE;
                        PrepareExtraDumpRanges(ExPtr);
                        MINIDUMP_CALLBACK_INFORMATION callbackInfo{};
                        callbackInfo.CallbackRoutine = MinidumpCallback;
                        callbackInfo.CallbackParam = nullptr;
                        CrashProgress("[Crash] Naming %d extra memory range(s) for the dump.\n", g_extraRangeCount);

                        const BOOL win = pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, DumpFlagsForDetail(Settings::settingsIni.crashDumpDetail), &md, &userStreams, &callbackInfo);

                        if (!win)
                        {
                                CrashProgress("[Crash] MiniDumpWriteDump failed err=%lu\n", GetLastError());
                                wsprintf(messageBuffer, _T("MiniDumpWriteDump failed. Error: %u\n%ls"), GetLastError(), dumpPath.c_str());
                        }
                        else
                        {
                                CrashProgress("[Crash] MiniDumpWriteDump succeeded for %s\n", ToUtf8(dumpPath).c_str());
                                // The dump is what makes a crash solvable, so this is what
                                // decides whether the handlers may swallow the crash.
                                g_crashBundleOk.store(true);
                                wsprintf(messageBuffer, _T("Crash bundle created:\n%ls"), crashDir.c_str());
                        }
                        CloseHandle(hFile);
                }
                else
                {
                        const DWORD err = GetLastError();
                        CrashProgress("[Crash] CreateFileW failed for dump path %s (err=%lu)\n", ToUtf8(dumpPath).c_str(), err);
                        wsprintf(messageBuffer, _T("Could not create dump file at:\n%ls\nError: %lu"), dumpPath.c_str(), err);
                }
        }
        else
        {
                wsprintf(messageBuffer, _T("Could not load dbghelp; crash context saved at:\n%ls"), crashDir.c_str());
        }

        CrashProgress("[Crash] Bundle written to %s (dump ok=%d)\n",
                      ToUtf8(crashDir).c_str(), g_crashBundleOk.load() ? 1 : 0);

        if (showDialog)
        {
                // The bundle is on disk by now, so the watchdog's job is done - and what
                // follows is a modal dialog paced by a human. Leaving it armed would
                // force-fail the process out from under someone who simply had not clicked
                // OK within a minute, which a test caught it doing exactly.
                DisarmCrashWatchdog();

                // Name the crash in the dialog itself: "a bundle was written" alone sends the
                // user digging through folders for the one line that matters.
                wchar_t dialogText[1024] = {};
                if (ExPtr != nullptr && ExPtr->ExceptionRecord != nullptr)
                {
                        const EXCEPTION_RECORD& record = *ExPtr->ExceptionRecord;
                        const std::string where = DescribeAddress(reinterpret_cast<uintptr_t>(record.ExceptionAddress));
                        wsprintfW(dialogText, L"BBCF crashed: 0x%08lX %hs at %hs\nReason: %hs\n\n%ls",
                                  record.ExceptionCode, ExceptionCodeName(record.ExceptionCode), where.c_str(),
                                  reason ? reason : "unknown", messageBuffer);
                }
                else
                {
                        wsprintfW(dialogText, L"BBCF crashed.\nReason: %hs\n\n%ls",
                                  reason ? reason : "unknown", messageBuffer);
                }

                // Topmost and foreground: an ownerless box otherwise opens behind the game
                // window, and the crash looks like the game silently closing.
                MessageBoxW(NULL, dialogText, L"BBCF Improvement Mod - crash", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
        }
}

// Decides what to do once the bundle writer has had its turn.
//
// ExitProcess used to be unconditional, and that was safe only by accident: the CRT kept
// unregistering our filter, so this rarely ran and crashes fell through to Windows. Now
// that the filter reliably survives, exiting here also suppresses WER and any configured
// LocalDumps - so a failure inside the bundle writer meant the crash was lost completely,
// which is exactly what happened on 2026-09-03.
//
// So: exit quietly only when we actually have a dump. Otherwise hand the exception back to
// Windows, which is worse for the user (they get the OS crash dialog) and much better for
// the goal, because WER then captures what we could not.
static LONG FinishCrashHandling(const char* who)
{
        if (g_crashBundleOk.load())
        {
                ExitProcess(0);
                return EXCEPTION_EXECUTE_HANDLER; // not reached
        }

        CrashProgress("[Crash] %s produced no dump; handing the exception to Windows so WER "
                      "and LocalDumps can still capture it.\n", who);
        return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------------
// Reporter thread.
//
// A stack overflow cannot be reported from the thread that overflowed - there is no stack
// left to report with. Measured on a real crash on 2026-09-03: a thread consumed its whole
// 1 MB stack, UnhandledExceptionFilter called our filter, and our filter died in __chkstk's
// probe loop before it could write a single line. No log, no bundle. SetThreadStackGuarantee
// does not help either: it is per-thread, and we only ever set it on the init thread, never
// on whichever thread eventually overflows.
//
// So the faulting thread hands the work to a thread that has its own untouched stack and
// waits. Created up front, while the process is still healthy, because creating a thread is
// exactly the kind of thing that fails when things have gone wrong.
// ---------------------------------------------------------------------------------------
namespace
{
        struct CrashReportRequest
        {
                const char*          reason = nullptr;
                PEXCEPTION_POINTERS  pointers = nullptr;
                bool                 showDialog = false;
        };

        CrashReportRequest g_reportRequest;
        HANDLE g_reportRequested = nullptr;
        HANDLE g_reportFinished = nullptr;

        DWORD WINAPI CrashReporterThread(LPVOID)
        {
                if (WaitForSingleObject(g_reportRequested, INFINITE) != WAIT_OBJECT_0)
                {
                        return 0;
                }

                WriteCrashBundle(g_reportRequest.reason, g_reportRequest.pointers,
                                 g_reportRequest.showDialog);

                SetEvent(g_reportFinished);
                return 0;
        }

        // Returns once the bundle has been written, or immediately writes it inline if the
        // reporter thread is not available - inline is what used to happen always, so the
        // fallback is no worse than the old behaviour.
        void ReportCrashOffThread(const char* reason, PEXCEPTION_POINTERS ExPtr, bool showDialog)
        {
                g_faultingThreadId.store(GetCurrentThreadId());

                if (g_reportRequested == nullptr || g_reportFinished == nullptr)
                {
                        CrashProgress("[Crash] No reporter thread; writing the bundle on the "
                                      "faulting thread instead.\n");
                        WriteCrashBundle(reason, ExPtr, showDialog);
                        return;
                }

                g_reportRequest.reason = reason;
                g_reportRequest.pointers = ExPtr;
                g_reportRequest.showDialog = showDialog;

                SetEvent(g_reportRequested);

                // No timeout here on purpose: the hang watchdog owns that decision, and it
                // force-fails the whole process after a minute if this never comes back.
                WaitForSingleObject(g_reportFinished, INFINITE);
        }
}

// ---------------------------------------------------------------------------------------
// Hang watchdog.
//
// Everything else here defends against the crash path FAULTING. Nothing defends against it
// HANGING, and SEH cannot: MiniDumpWriteDump takes the loader lock, so a crash caused by
// heap or loader corruption can wedge the code trying to report it. Because the handler has
// by then already decided not to hand the exception to Windows, the result is a process
// that sits there forever with no report from anyone.
//
// A minute is deliberately generous. Writing a full-memory dump of this game took 13
// seconds on a reporter's machine, so anything still running at 60s is genuinely stuck, not
// slow, and firing this can never be mistaken for impatience.
// ---------------------------------------------------------------------------------------
namespace
{
        constexpr DWORD kWatchdogDeadlineMs = 60 * 1000;
        constexpr DWORD kWatchdogStepMs = 250;
        constexpr DWORD kHeapRefreshIntervalMs = 30 * 1000;

        // Bumped by both arming and disarming, so a superseded countdown retires instead of
        // firing on a later, healthy state.
        std::atomic<unsigned int> g_watchdogGeneration{ 0 };
        char g_watchdogReason[128] = {};

        // The watchdog thread is created once, at install time, and parked on this event.
        //
        // It used to be created by ArmCrashWatchdog, i.e. from inside the crash. That works
        // for every crash except the one the watchdog matters most for: a heap corruption
        // raised while the heap lock is held. Starting a thread runs LdrInitializeThunk,
        // which allocates the new thread's CRT data - so the watchdog blocked on the very
        // lock the faulting thread was holding and never ran a single instruction of its
        // own body. Reproduced with build/crashctx_test mode 8: the process sat wedged past
        // 240 seconds with the reporter stuck and the 60-second deadline never firing.
        // Parking a thread that already exists costs one SetEvent and cannot allocate.
        HANDLE g_watchdogArm = nullptr;

        DWORD WINAPI CrashWatchdogThread(LPVOID)
        {
                for (;;)
                {
                        const DWORD wait = WaitForSingleObject(g_watchdogArm, kHeapRefreshIntervalMs);
                        if (wait == WAIT_TIMEOUT)
                        {
                                RefreshProcessHeapSnapshot();
                                continue;
                        }
                        if (wait != WAIT_OBJECT_0)
                        {
                                return 0;
                        }

                        const unsigned int myGeneration = g_watchdogGeneration.load();
                        bool superseded = false;
                        for (DWORD waited = 0; waited < kWatchdogDeadlineMs; waited += kWatchdogStepMs)
                        {
                                if (g_watchdogGeneration.load() != myGeneration)
                                {
                                        superseded = true;
                                        break;
                                }
                                Sleep(kWatchdogStepMs);
                        }

                        if (superseded || g_watchdogGeneration.load() != myGeneration)
                        {
                                continue;
                        }

                        CrashProgress("[Crash] %s has not finished after %lu seconds and is presumed "
                                      "deadlocked. Forcing termination so Windows Error Reporting and "
                                      "LocalDumps still capture a dump of the hang.\n",
                                      g_watchdogReason, kWatchdogDeadlineMs / 1000);
                        break;
                }

                // Not ExitProcess and not TerminateProcess: both end the process cleanly, and
                // WER would see nothing - which is the situation this exists to prevent.
                // __fastfail raises STATUS_STACK_BUFFER_OVERRUN, which cannot be swallowed by
                // any handler and which WER does capture, and the dump it writes contains
                // every thread including the stuck one.
                __fastfail(FAST_FAIL_FATAL_APP_EXIT);
                return 0; // not reached; __fastfail is not declared noreturn
        }
}

static void StartCrashWatchdogThread()
{
        if (g_watchdogArm != nullptr)
        {
                return;
        }

        g_watchdogArm = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_watchdogArm == nullptr)
        {
                CrashProgress("[Crash] Could not create the watchdog event (err=%lu); a hang "
                              "in the crash path will not be caught.\n", GetLastError());
                return;
        }

        const HANDLE thread = CreateThread(nullptr, 0, CrashWatchdogThread, nullptr, 0, nullptr);
        if (thread == nullptr)
        {
                CrashProgress("[Crash] Could not start the crash watchdog (err=%lu); a hang "
                              "in the crash path will not be caught.\n", GetLastError());
                CloseHandle(g_watchdogArm);
                g_watchdogArm = nullptr;
                return;
        }
        CloseHandle(thread);
        CrashProgress("[Crash] Crash watchdog thread parked and ready.\n");
}

void ArmCrashWatchdog(const char* reason)
{
        strncpy_s(g_watchdogReason, sizeof(g_watchdogReason),
                  reason ? reason : "Crash handling", _TRUNCATE);

        g_watchdogGeneration.fetch_add(1);

        if (g_watchdogArm == nullptr)
        {
                CrashProgress("[Crash] Watchdog is not available; a hang in the crash path "
                              "will not be caught.\n");
                return;
        }
        SetEvent(g_watchdogArm);

        CrashProgress("[Crash] Watchdog armed for \"%s\" (%lus).\n",
                      g_watchdogReason, kWatchdogDeadlineMs / 1000);
}

void DisarmCrashWatchdog()
{
        g_watchdogGeneration.fetch_add(1);
}

LONG WINAPI UnhandledExFilter(PEXCEPTION_POINTERS ExPtr)
{
        CrashProgress("[Crash] UnhandledExFilter invoked.\n");
        ArmCrashWatchdog("Unhandled-exception crash handling");
        ReportCrashOffThread("Unhandled exception", ExPtr, true);
        return FinishCrashHandling("UnhandledExFilter");
}

// ---------------------------------------------------------------------------------------
// C++ throw tracking.
//
// BBCF's WinMain wraps the whole game in catch(...) and returns -1 (funclet at static
// 0x0044EDD3). The SE translator it installs (0x0044E740, where our game-filter hook sits)
// only sees SEH faults - the CRT never runs a translator for a C++ throw - so a throw that
// escapes the main loop exits the game through the normal path: no WER report, no bundle,
// a clean-shutdown log and "BBCF exit(-1): WinMain RETURNED". Seen 2026-09-29 on a ranked
// loading screen. By the time that catch runs the exception object is out of reach, so the
// vectored handler notes every first-chance C++ throw here and the catch hook reads the
// last one on its thread.
//
// Runs on every C++ throw in the process, including the ones the thrower catches itself,
// so recording allocates nothing, takes no lock and never logs.
// ---------------------------------------------------------------------------------------
namespace
{
        constexpr DWORD kMsvcCxxExceptionCode = 0xE06D7363;
        constexpr int kCxxThrowRingSize = 16;
        constexpr int kCxxThrowFrames = 24;

        struct CxxThrowRecord
        {
                LONG sequence = 0; // 0 = never written
                DWORD threadId = 0;
                DWORD tick = 0;
                uintptr_t throwInfo = 0;
                char typeName[96] = {};
                int frameCount = 0;
                uintptr_t frames[kCxxThrowFrames] = {};
        };

        CxxThrowRecord g_cxxThrows[kCxxThrowRingSize];
        std::atomic<LONG> g_cxxThrowCount{ 0 };

        // ThrowInfo -> CatchableTypeArray -> first CatchableType -> TypeDescriptor name.
        // On x86 these are absolute pointers. The first entry is the thrown type itself.
        void ReadCxxTypeName(uintptr_t throwInfo, char* out, size_t size)
        {
                out[0] = '\0';
                if (throwInfo == 0)
                {
                        strncpy_s(out, size, "<rethrow>", _TRUNCATE);
                        return;
                }

                __try
                {
                        const uintptr_t catchableTypes = *reinterpret_cast<const uintptr_t*>(throwInfo + 0x0C);
                        if (catchableTypes == 0 || *reinterpret_cast<const int*>(catchableTypes) <= 0)
                        {
                                return;
                        }
                        const uintptr_t catchableType = *reinterpret_cast<const uintptr_t*>(catchableTypes + 4);
                        const uintptr_t typeDescriptor = *reinterpret_cast<const uintptr_t*>(catchableType + 4);
                        const char* name = reinterpret_cast<const char*>(typeDescriptor + 8);
                        size_t i = 0;
                        for (; i + 1 < size && name[i] != '\0'; ++i)
                        {
                                out[i] = name[i];
                        }
                        out[i] = '\0';
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                        strncpy_s(out, size, "<unreadable>", _TRUNCATE);
                }
        }

        // Frame-pointer walk from the raise context, bounded by this thread's stack. BBCF
        // and the mod both keep frame pointers, so this reaches the thrower and its callers.
        int WalkFramesFromContext(const CONTEXT* context, uintptr_t* frames, int maxFrames)
        {
                const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
                const uintptr_t stackLow = reinterpret_cast<uintptr_t>(tib->StackLimit);
                const uintptr_t stackHigh = reinterpret_cast<uintptr_t>(tib->StackBase);

                int count = 0;
                frames[count++] = static_cast<uintptr_t>(context->Eip);
                uintptr_t ebp = static_cast<uintptr_t>(context->Ebp);
                while (count < maxFrames &&
                       ebp >= stackLow && ebp + 8 <= stackHigh && (ebp & 3) == 0)
                {
                        const uintptr_t next = *reinterpret_cast<const uintptr_t*>(ebp);
                        const uintptr_t ret = *reinterpret_cast<const uintptr_t*>(ebp + 4);
                        if (ret == 0)
                        {
                                break;
                        }
                        frames[count++] = ret;
                        if (next <= ebp)
                        {
                                break;
                        }
                        ebp = next;
                }
                return count;
        }

        void RecordCxxThrow(PEXCEPTION_POINTERS ExPtr)
        {
                const EXCEPTION_RECORD* record = ExPtr->ExceptionRecord;
                if (record->NumberParameters < 3)
                {
                        return;
                }

                const LONG sequence = g_cxxThrowCount.fetch_add(1) + 1;
                CxxThrowRecord& slot = g_cxxThrows[(sequence - 1) % kCxxThrowRingSize];
                slot.sequence = 0;
                slot.threadId = GetCurrentThreadId();
                slot.tick = GetTickCount();
                slot.throwInfo = static_cast<uintptr_t>(record->ExceptionInformation[2]);
                ReadCxxTypeName(slot.throwInfo, slot.typeName, sizeof(slot.typeName));
                slot.frameCount = ExPtr->ContextRecord
                        ? WalkFramesFromContext(ExPtr->ContextRecord, slot.frames, kCxxThrowFrames)
                        : 0;
                slot.sequence = sequence;
        }

        bool FindLastCxxThrowOnThread(DWORD threadId, CxxThrowRecord* out, LONG* totalThrows)
        {
                const LONG total = g_cxxThrowCount.load();
                *totalThrows = total;
                for (LONG seq = total; seq > 0 && seq > total - kCxxThrowRingSize; --seq)
                {
                        const CxxThrowRecord& slot = g_cxxThrows[(seq - 1) % kCxxThrowRingSize];
                        if (slot.sequence == seq && slot.threadId == threadId)
                        {
                                *out = slot;
                                return true;
                        }
                }
                return false;
        }

        // ".?AVbad_alloc@std@@" -> "std::bad_alloc". Templates and other decorations are
        // left as they are; the raw name is still unambiguous.
        std::string UndecorateTypeName(const char* raw)
        {
                std::string name(raw);
                if (name.size() < 7 ||
                    (name.compare(0, 4, ".?AV") != 0 && name.compare(0, 4, ".?AU") != 0) ||
                    name.find_first_of("?$", 4) != std::string::npos ||
                    name.compare(name.size() - 2, 2, "@@") != 0)
                {
                        return name;
                }

                std::vector<std::string> parts;
                std::string body = name.substr(4, name.size() - 6);
                size_t start = 0;
                for (;;)
                {
                        const size_t at = body.find('@', start);
                        parts.push_back(body.substr(start, at - start));
                        if (at == std::string::npos) break;
                        start = at + 1;
                }

                std::string result;
                for (auto it = parts.rbegin(); it != parts.rend(); ++it)
                {
                        if (!result.empty()) result += "::";
                        result += *it;
                }
                return result;
        }

        // The 32-bit game has 2 GB of address space, and a failed large allocation is the
        // prime suspect for a throw on a loading screen (see the char-select atlas OOM).
        // "largest free" is the number that decides whether a big allocation can succeed.
        void DescribeAddressSpace(char* out, size_t size)
        {
                SYSTEM_INFO info{};
                GetSystemInfo(&info);
                const uintptr_t limit = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);

                uintptr_t address = 0;
                SIZE_T freeTotal = 0, largestFree = 0, committed = 0, reserved = 0;
                MEMORY_BASIC_INFORMATION mbi{};
                while (address < limit &&
                       VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == sizeof(mbi))
                {
                        if (mbi.State == MEM_FREE)
                        {
                                freeTotal += mbi.RegionSize;
                                largestFree = std::max(largestFree, mbi.RegionSize);
                        }
                        else if (mbi.State == MEM_COMMIT)
                        {
                                committed += mbi.RegionSize;
                        }
                        else
                        {
                                reserved += mbi.RegionSize;
                        }

                        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                        if (next <= address) break;
                        address = next;
                }

                sprintf_s(out, size,
                          "address space: %u MB committed, %u MB reserved, %u MB free, largest free "
                          "block %.2f MB (user limit %u MB)",
                          static_cast<unsigned>(committed >> 20), static_cast<unsigned>(reserved >> 20),
                          static_cast<unsigned>(freeTotal >> 20), largestFree / (1024.0 * 1024.0),
                          static_cast<unsigned>((limit + 1) >> 20));
        }
}

static void ReportCxxExceptionSwallowedByGameImpl(const char* where);

// Called from inside the game's catch funclet. If the report itself throws (the likely
// culprit is running out of memory, and the report allocates), the throw must not leave
// this function: a second exception in flight inside a catch block ends in terminate().
void ReportCxxExceptionSwallowedByGame(const char* where)
{
        try
        {
                ReportCxxExceptionSwallowedByGameImpl(where);
        }
        catch (...)
        {
                ForceLog("[Crash] The report for a swallowed C++ exception threw; writing a bundle without it.\n");
                ArmCrashWatchdog("WinMain catch(...) crash handling");
                WriteCrashBundleForCurrentContext("Game's WinMain catch(...) swallowed a C++ exception "
                                                  "(details unavailable - the report itself failed)",
                                                  kMsvcCxxExceptionCode, true);
                DisarmCrashWatchdog();
        }
}

static void ReportCxxExceptionSwallowedByGameImpl(const char* where)
{
        const DWORD threadId = GetCurrentThreadId();
        CxxThrowRecord last;
        LONG totalThrows = 0;
        const bool found = FindLastCxxThrowOnThread(threadId, &last, &totalThrows);

        char addressSpace[192] = {};
        DescribeAddressSpace(addressSpace, sizeof(addressSpace));

        std::ostringstream reason;
        reason << where << " swallowed a C++ exception and the game is closing with exit code -1.\n";
        if (found)
        {
                reason << "Last C++ throw on this thread (" << (GetTickCount() - last.tick) << " ms ago, "
                       << totalThrows << " throws this session): type=" << UndecorateTypeName(last.typeName)
                       << " (" << last.typeName << "), type info in " << DescribeAddress(last.throwInfo) << "\n"
                       << "Throw stack (raise site first):\n";
                for (int i = 0; i < last.frameCount; ++i)
                {
                        reason << "  #" << i << " " << DescribeAddress(last.frames[i]) << "\n";
                }
        }
        else
        {
                reason << "No C++ throw on this thread was recorded (" << totalThrows
                       << " this session). It may have been older than the last " << kCxxThrowRingSize
                       << " throws, or thrown before the vectored handler was installed.\n";
        }
        reason << addressSpace << "\n";

        const std::string text = reason.str();
        LOG(0, "[Crash] %s", text.c_str());

        // The game exits on its own once the catch returns, so the watchdog only guards the
        // bundle write. The dialog is shown because this used to be a completely silent exit.
        ArmCrashWatchdog("WinMain catch(...) crash handling");
        WriteCrashBundleForCurrentContext(text.c_str(), kMsvcCxxExceptionCode, true);
        DisarmCrashWatchdog();
}

static LONG WINAPI VectoredCrashHandler(PEXCEPTION_POINTERS ExPtr)
{
        if (!ExPtr || !ExPtr->ExceptionRecord)
        {
                return EXCEPTION_CONTINUE_SEARCH;
        }

        const DWORD code = ExPtr->ExceptionRecord->ExceptionCode;

        if (code == kMsvcCxxExceptionCode)
        {
                RecordCxxThrow(ExPtr);
                return EXCEPTION_CONTINUE_SEARCH;
        }

        // Catch heap corruption and other fail-fast style exceptions that may bypass
        // the standard unhandled filter.
        if (code == STATUS_HEAP_CORRUPTION || code == STATUS_FATAL_APP_EXIT)
        {
                CrashProgress("[Crash] VectoredCrashHandler caught code=0x%08X\n", code);
                ArmCrashWatchdog("Vectored crash handling");
                ReportCrashOffThread("Vectored crash handler", ExPtr, true);
                return FinishCrashHandling("VectoredCrashHandler");
        }

        return EXCEPTION_CONTINUE_SEARCH;
}

void WriteCrashBundleForCurrentContext(const char* reason, DWORD pseudoExceptionCode, bool showDialog)
{
        // No exception has been raised, so synthesize the record from the caller's own
        // state. Eip points at the abort/fastfail site, which is what makes the walked
        // stack show who decided to die rather than just "the CRT".
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&context);

        EXCEPTION_RECORD record{};
        record.ExceptionCode = pseudoExceptionCode;
        record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_IX86)
        record.ExceptionAddress = reinterpret_cast<PVOID>(context.Eip);
#endif

        EXCEPTION_POINTERS pointers{};
        pointers.ExceptionRecord = &record;
        pointers.ContextRecord = &context;

        WriteCrashBundle(reason, &pointers, showDialog);
}

void InstallCrashHandlers()
{
        if (!g_vectoredHandler)
        {
                g_vectoredHandler = AddVectoredExceptionHandler(1, VectoredCrashHandler);
                CrashProgress("[Crash] Vectored exception handler installed (%p).\n", g_vectoredHandler);
        }

        // A stack overflow leaves too little room for a filter to run and write a dump,
        // which is why every 0xC00000FD on record produced nothing. Reserving a slice of
        // the guard region gives the handler somewhere to stand.
        ULONG_PTR guarantee = 64 * 1024;
        if (!SetThreadStackGuarantee(&guarantee))
        {
                CrashProgress("[Crash] SetThreadStackGuarantee failed err=%lu\n", GetLastError());
        }

        // Up front, while the heap and address space are still healthy. The heap list is
        // taken here for the same reason and one more: GetProcessHeaps takes the process
        // heap lock, which the faulting thread is usually holding by the time a heap
        // crash is being reported.
        RefreshProcessHeapSnapshot();
        CrashProgress("[Crash] Snapshotted %lu process heap(s) for dump capture.\n",
                      g_processHeapCount.load());

        StartCrashWatchdogThread();

        g_reportRequested = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_reportFinished = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_reportRequested != nullptr && g_reportFinished != nullptr)
        {
                const HANDLE reporter = CreateThread(nullptr, 0, CrashReporterThread, nullptr, 0, nullptr);
                if (reporter != nullptr)
                {
                        CloseHandle(reporter);
                }
                else
                {
                        CrashProgress("[Crash] Could not start the crash reporter thread "
                                      "(err=%lu); stack-overflow crashes will not be "
                                      "reportable.\n", GetLastError());
                }
        }

        SetUnhandledExceptionFilter(UnhandledExFilter);
        CrashProgress("[Crash] Unhandled exception filter installed.\n");
}
