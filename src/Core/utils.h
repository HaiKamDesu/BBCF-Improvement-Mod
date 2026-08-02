#pragma once
#include <string>
#include <Windows.h>

#define SAFE_RELEASE(x) if( x ) { (x)->Release(); (x) = NULL; }
#define SAFE_DELETE(x) if( x ) { delete(x); (x) = NULL; }
#define SAFE_DELETE_ARRAY(x) if( x ) { delete [] (x); (x) = NULL; }
#define SHOWERROR(s,f,l) char buf[1024]; sprintf( buf, "File: %s\nLine: %d\n%s",f,l,s); MessageBox( 0, buf, "Error", 0 );
#define SHOWERROR_W(s,f,l) wchar_t buf[1024]; wsprintf(buf, L"File: %s\nLine: %d\n%s",f,l,s); MessageBox(0, buf, L"Error", 0);

#define nameof(symbol) #symbol

char* GetBbcfBaseAdress();
void WriteToProtectedMemory(uintptr_t addressToWrite, char* valueToWrite, int byteNum);
char* RawMemoryArrayToString(unsigned char* srcBuf, int length);
DWORD FindPattern(LPCWSTR module, char *pattern, char *mask);
DWORD* GetInterfaceFuncPtr(DWORD* pDeviceInterface, const char *fmt, ...);
bool Hook(void* toHook, void* ourFunc, int len);

std::string FormatText(const char* message, ...);
unsigned int rgb(double hue);
DWORD QuickChecksum(DWORD *pData, int size);

bool utils_WriteFile(const char* path, void* inBuffer, unsigned long bufferSize, bool binaryFile = false, bool append = false);
bool utils_ReadFile(const char* path, void* outBuffer, unsigned long bufferSize, bool binaryFile = false);

/* True while an ImGui text field owns keyboard input (typing an entry name, a search
   filter, ...). Keyboard hotkeys must stay inert while this is true, otherwise typing a
   letter that happens to be bound -- "C" freezes the game by default -- fires the hotkey.
   Controller/gamepad bindings are unaffected and must not consult this. */
bool IsTypingInImGuiTextField();

/* True once WindowManager::Initialize has created the ImGui context (happens at the title
   screen, ~20s into launch while the intro videos play). Anything that touches ImGui from a
   hook that can fire earlier MUST check this first: since ImGui 1.60 the global context
   pointer is NULL until CreateContext(), so ImGui::GetIO() asserts and crashes. Under 1.53
   it pointed at a statically allocated default context, which is why early calls used to be
   silently harmless. */
bool IsImGuiContextReady();

/* ImGui's frame delta, or 0.0f when the context does not exist yet. See IsImGuiContextReady. */
float GetImGuiDeltaTime();

std::string utf16_to_utf8(const std::wstring & wstr);
std::wstring utf8_to_utf16(const std::string & utf8_str);
int SafeDereferencePtr(int* ptr);
