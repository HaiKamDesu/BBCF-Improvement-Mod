#pragma once
#include "WindowType.h"
#include "Overlay/Window/IWindow.h"

#include <map>

typedef std::map<WindowType_, IWindow*> WindowMap;

class WindowContainer
{
public:
	WindowContainer();
	~WindowContainer()
	{
		for (const auto& window : m_windows)
		{
			delete window.second;
		}
	}

	template <class T>
	T* GetWindow(WindowType_ type)
	{
		const auto it = m_windows.find(type);
		return it != m_windows.end() ? static_cast<T*>(it->second) : nullptr;
	}
	IWindow* GetWindow(WindowType_ type)
	{
		const auto it = m_windows.find(type);
		return it != m_windows.end() ? it->second : nullptr;
	}
	const WindowMap& GetWindows() const { return m_windows; }

	// True when a higher-priority exclusive modal is already claiming ImGui's popup slot, so
	// `self` has to wait its turn instead of drawing this frame.
	//
	// ImGui keeps exactly ONE open popup per level of the popup stack, and every one of these
	// windows calls OpenPopup() at level 0 on every frame it is open. Two of them open at
	// once therefore do not queue - they thrash: OpenPopupEx sees a different id in slot 0,
	// runs ClosePopupToLevel(0) to evict the other, and pushes its own, every frame, in both
	// directions. Neither prompt stays open long enough to click, the wrapper window of
	// whichever one lost is left on screen as a small empty box, and a modal is open at
	// end-of-frame regardless - so io.WantCaptureKeyboard stays set and the game reads no
	// keyboard at all.
	//
	// That is the "a window appears and closes in a split second and I'm stuck on the title
	// screen on keyboard" report. It needs two prompts pending at the same time, which is why
	// it only happens to people upgrading from an old install: a settings.ini predating these
	// settings leaves them at -1 (v3.110's file has no AllowPaletteDownloads line at all) and
	// an out-of-date install also has the update notifier queued behind it.
	//
	// Three of the four already deferred to each other ad hoc; the update notifier was never
	// part of the arrangement, in either direction. Order below is the order they are offered
	// in, and the consent prompts come first because until one is answered it keeps freezing
	// the keyboard on every launch.
	bool ShouldDeferExclusivePopup(WindowType_ self)
	{
		static const WindowType_ kExclusiveOrder[] = {
			WindowType_ReplayDBPopup,
			WindowType_WinePopup,
			WindowType_PaletteSharePopup,
			WindowType_UpdateNotifier,
		};

		for (const WindowType_ type : kExclusiveOrder)
		{
			if (type == self)
			{
				return false; // nothing ahead of us is open
			}
			const IWindow* const window = GetWindow(type);
			if (window != nullptr && window->IsOpen())
			{
				return true;
			}
		}
		return false; // not an exclusive popup
	}

private:
	void AddWindow(WindowType_ type, IWindow* pWindow) { m_windows[type] = pWindow; }

	WindowMap m_windows;
};
