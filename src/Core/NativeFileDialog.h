#pragma once

#include <string>
#include <vector>

/*
	One shared way to put a Windows file picker on screen.

	Doing this wrong is what makes the game hang or die, and the same block of OPENFILENAMEA
	setup had been copy-pasted into four windows, three of them getting it right and one not.
	The rules the copies had to remember, now in one place:

	  - Never call GetOpenFileName/GetSaveFileName on the render thread. The common dialog
	    pumps its own message loop; doing that from inside the present/ImGui path freezes the
	    game for as long as the dialog is up and can deadlock outright. The dialog runs on a
	    worker thread and the answer is picked up later, from the UI, via Consume().
	  - Always pass OFN_NOCHANGEDIR and restore the working directory afterwards. Without it
	    the picker silently changes the process CWD, and every relative path the mod uses
	    after that - settings, palettes, TAS movie files - resolves somewhere else.
	  - Never set hwndOwner to the game window. An owned modal ties the dialog's message loop
	    to a window the render thread is driving.
	  - Only one dialog at a time.

	Usage: call Open() with a token identifying the caller, then poll Consume() with the same
	token every frame until it returns true.
*/
namespace NativeFileDialog
{
	struct Filter
	{
		std::string description; // "TAS Movie (*.txt)"
		std::string pattern;     // "*.txt"
	};

	struct Request
	{
		bool save = false;                 // save picker instead of open picker
		std::string title;
		std::vector<Filter> filters;       // an "All Files" entry is appended automatically
		std::string defaultExtension;      // without the dot
		std::string initialPath;           // a file or a folder to start in; may be empty
		int contextId = -1;                // opaque, handed back untouched with the result
	};

	struct Result
	{
		bool accepted = false;
		std::string path;
		int contextId = -1;
	};

	// Starts the picker on a worker thread. Returns false when one is already on screen.
	// Any completed-but-unclaimed result is discarded, so a window that closes without
	// consuming its answer cannot wedge the next caller.
	bool Open(const char* ownerToken, const Request& request);

	// True while a picker is on screen. Use it to disable the button that opened it.
	bool IsOpen();

	// Returns true exactly once, on the first call after the picker this token opened has
	// closed. Poll from the UI thread.
	bool Consume(const char* ownerToken, Result* outResult);
}
