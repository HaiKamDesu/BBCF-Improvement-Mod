#include "ImGuiLogger.h"

#include "Core/logger.h"

#include <atltime.h>
#include <sstream>

Logger* g_imGuiLogger = new ImGuiLogger();

void ImGuiLogger::Clear()
{
	m_textBuffer.clear();
	m_lineOffsets.clear();
}

const std::string ImGuiLogger::GetTime()
{
	std::ostringstream oss;

	CTime time = CTime::GetCurrentTime();
	oss << time.GetHour()
		<< ":"
		<< time.GetMinute()
		<< ":"
		<< time.GetSecond();

	return oss.str();
}

const std::string ImGuiLogger::GetDate()
{
	std::ostringstream oss;

	CTime time = CTime::GetCurrentTime();
	oss << time.GetYear()
		<< "/"
		<< time.GetMonth()
		<< "/"
		<< time.GetDay();

	return oss.str();
}

void ImGuiLogger::Log(LogLevel_ logLevel, const char* fmt, ...)
{
	if (!m_loggingEnabled)
	{
		return;
	}

	//va_list args;
	//va_start(args, fmt);
	////
	//va_end(args);
}

void ImGuiLogger::Log(const char * fmt, ...)
{
	if (!m_loggingEnabled)
	{
		return;
	}

	va_list args, args_copy;
	va_start(args, fmt);
	va_copy(args_copy, args);

	const int size = std::vsnprintf(nullptr, 0, fmt, args) + 1;
	std::string result(size, ' ');
	std::vsnprintf(&result.front(), size, fmt, args_copy);

	va_end(args_copy);
	va_end(args);

	// Mirror every overlay log message into DEBUG.txt so the log window and
	// the on-disk log always show the same information.
	LOG(1, "[Overlay] %s", result.c_str());

	std::string fullMessage = GetTime() + " " + result;

	int oldSize = m_textBuffer.size();
	m_textBuffer.appendfv(fullMessage.c_str(), nullptr);

	for (int newSize = m_textBuffer.size(); oldSize < newSize; oldSize++)
	{
		if (m_textBuffer[oldSize] == '\n')
		{
			m_lineOffsets.push_back(oldSize);
		}
	}
}

void ImGuiLogger::LogSeparator()
{
	Log("------------------------------------------------------------------\n");
}
