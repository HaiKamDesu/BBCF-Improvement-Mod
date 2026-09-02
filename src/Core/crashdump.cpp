#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "crashdump.h"

#include "Settings.h"
#include "info.h"
#include "logger.h"

#include <algorithm>
#include <codecvt>
#include <atomic>
#include <cstdint>
#include <dbghelp.h>
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
                        ForceLog("[Crash] GetModuleFileNameW failed (len=%lu err=%lu)\n", length, GetLastError());
                        return std::wstring();
                }

                std::wstring path(modulePath, length);
                const size_t lastSlash = path.find_last_of(L"\\/");
                if (lastSlash == std::wstring::npos)
                {
                        ForceLog("[Crash] Failed to locate executable directory for %s\n", ToUtf8(path).c_str());
                        return std::wstring();
                }

                return path.substr(0, lastSlash);
        }

        std::wstring GetCrashRootDirectory()
        {
                const std::wstring exeDir = GetExecutableDirectory();
                if (exeDir.empty())
                {
                        ForceLog("[Crash] Falling back to relative CrashReports path.\n");
                        return L"BBCF_IM\\CrashReports";
                }

                return JoinPath(exeDir, L"BBCF_IM\\CrashReports");
        }

        void EnsureDirectory(const std::wstring& path)
        {
                const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
                if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS)
                {
                        ForceLog("[Crash] Failed to ensure directory %s (err=%d)\n", ToUtf8(path).c_str(), result);
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
                oss << "\nRegisters (faulting thread " << GetCurrentThreadId() << "):\n";
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

        std::string BuildContextText(PEXCEPTION_POINTERS ExPtr, const std::wstring& dumpPath, const SymbolApi& api)
        {
                std::ostringstream oss;
                oss << "BBCF Improvement Mod crash report" << "\n";
                oss << "Mod version: " << MOD_VERSION_NUM << "\n";
                oss << "Dump file: " << ToUtf8(dumpPath) << "\n";

                AppendExceptionDetail(oss, ExPtr);
                AppendRegisters(oss, ExPtr);
                AppendStackTrace(oss, ExPtr, api);

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
                                ForceLog("[Crash] Kept %s (not empty or in use, err=%lu)\n",
                                         ToUtf8(dir).c_str(), GetLastError());
                                continue;
                        }

                        ForceLog("[Crash] Pruned old crash bundle %s\n", ToUtf8(dir).c_str());
                }
        }
}

void WriteCrashBundle(const char* reason, PEXCEPTION_POINTERS ExPtr, bool showDialog)
{
        if (g_crashBundleWritten.exchange(true))
        {
                return;
        }

        ForceLog("[Crash] WriteCrashBundle invoked (reason=%s, showDialog=%d)\n",
                 reason ? reason : "<null>", showDialog ? 1 : 0);

        MiniDumpWriteDump_t pMiniDumpWriteDump = nullptr;

        HMODULE hLib = LoadLibrary(_T("dbghelp"));
        if (hLib)
        {
                pMiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDump_t>(GetProcAddress(hLib, "MiniDumpWriteDump"));
        }
        else
        {
                ForceLog("[Crash] LoadLibrary(dbghelp) failed err=%lu\n", GetLastError());
        }

        const std::wstring timestamp = BuildTimestamp();
        const std::wstring crashRoot = GetCrashRootDirectory();
        const std::wstring crashDir = JoinPath(crashRoot, L"Crash_" + timestamp);
        const std::wstring dumpPath = JoinPath(crashDir, L"crash.dmp");
        const std::wstring logsPath = JoinPath(crashDir, L"logs.txt");
        const std::wstring contextPath = JoinPath(crashDir, L"crash_context.txt");

        ForceLog("[Crash] Crash bundle paths: root=%s dir=%s dump=%s\n",
                 ToUtf8(crashRoot).c_str(),
                 ToUtf8(crashDir).c_str(),
                 ToUtf8(dumpPath).c_str());

        EnsureDirectory(crashRoot);
        PruneOldCrashBundles(crashRoot, Settings::settingsIni.crashReportsToKeep);
        EnsureDirectory(crashDir);

        const std::string recentLogs = GetRecentLogs();
        WriteTextFile(logsPath, recentLogs);
        ForceLog("[Crash] Wrote logs snapshot (%zu bytes) to %s\n", recentLogs.size(), ToUtf8(logsPath).c_str());

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
        ForceLog("[Crash] Wrote crash context (%zu bytes) to %s\n", context.size(), ToUtf8(contextPath).c_str());

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
                        md.ThreadId = GetCurrentThreadId();
                        md.ExceptionPointers = ExPtr;
                        md.ClientPointers = FALSE;
                        const BOOL win = pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, DumpFlagsForDetail(Settings::settingsIni.crashDumpDetail), &md, &userStreams, nullptr);

                        if (!win)
                        {
                                ForceLog("[Crash] MiniDumpWriteDump failed err=%lu\n", GetLastError());
                                wsprintf(messageBuffer, _T("MiniDumpWriteDump failed. Error: %u\n%ls"), GetLastError(), dumpPath.c_str());
                        }
                        else
                        {
                                ForceLog("[Crash] MiniDumpWriteDump succeeded for %s\n", ToUtf8(dumpPath).c_str());
                                wsprintf(messageBuffer, _T("Crash bundle created:\n%ls"), crashDir.c_str());
                        }
                        CloseHandle(hFile);
                }
                else
                {
                        const DWORD err = GetLastError();
                        ForceLog("[Crash] CreateFileW failed for dump path %s (err=%lu)\n", ToUtf8(dumpPath).c_str(), err);
                        wsprintf(messageBuffer, _T("Could not create dump file at:\n%ls\nError: %lu"), dumpPath.c_str(), err);
                }
        }
        else
        {
                wsprintf(messageBuffer, _T("Could not load dbghelp; crash context saved at:\n%ls"), crashDir.c_str());
        }

        ForceLog("[Crash] Bundle written to %s\n", ToUtf8(crashDir).c_str());

        if (showDialog)
        {
                MessageBox(NULL, messageBuffer, _T("Unhandled exception"), MB_OK | MB_ICONERROR);
        }
}

LONG WINAPI UnhandledExFilter(PEXCEPTION_POINTERS ExPtr)
{
        ForceLog("[Crash] UnhandledExFilter invoked.\n");
        WriteCrashBundle("Unhandled exception", ExPtr, true);

        ExitProcess(0);

        return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI VectoredCrashHandler(PEXCEPTION_POINTERS ExPtr)
{
        if (!ExPtr || !ExPtr->ExceptionRecord)
        {
                return EXCEPTION_CONTINUE_SEARCH;
        }

        const DWORD code = ExPtr->ExceptionRecord->ExceptionCode;

        // Catch heap corruption and other fail-fast style exceptions that may bypass
        // the standard unhandled filter.
        if (code == STATUS_HEAP_CORRUPTION || code == STATUS_FATAL_APP_EXIT)
        {
                ForceLog("[Crash] VectoredCrashHandler caught code=0x%08X\n", code);
                WriteCrashBundle("Vectored crash handler", ExPtr, true);
                ExitProcess(0);
                return EXCEPTION_CONTINUE_SEARCH;
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

void ReassertUnhandledExceptionFilter()
{
        SetUnhandledExceptionFilter(UnhandledExFilter);
}

void InstallCrashHandlers()
{
        if (!g_vectoredHandler)
        {
                g_vectoredHandler = AddVectoredExceptionHandler(1, VectoredCrashHandler);
                ForceLog("[Crash] Vectored exception handler installed (%p).\n", g_vectoredHandler);
        }

        // A stack overflow leaves too little room for a filter to run and write a dump,
        // which is why every 0xC00000FD on record produced nothing. Reserving a slice of
        // the guard region gives the handler somewhere to stand.
        ULONG_PTR guarantee = 64 * 1024;
        if (!SetThreadStackGuarantee(&guarantee))
        {
                ForceLog("[Crash] SetThreadStackGuarantee failed err=%lu\n", GetLastError());
        }

        SetUnhandledExceptionFilter(UnhandledExFilter);
        ForceLog("[Crash] Unhandled exception filter installed.\n");
}
