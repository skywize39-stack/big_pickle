#include "core/diagnostics/Logger.hpp"

#include <cstdio>
#include <cstdarg>

namespace Core
{
    Logger& Logger::Instance()
    {
        static Logger instance;
        return instance;
    }

    void Logger::SetLevel(LogLevel level)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_level = level;
    }

    void Logger::Log(LogLevel level, const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        VLog(level, fmt, args);
        va_end(args);
    }

    void Logger::Debug(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        VLog(LogLevel::Debug, fmt, args);
        va_end(args);
    }

    void Logger::Info(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        VLog(LogLevel::Info, fmt, args);
        va_end(args);
    }

    void Logger::Warn(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        VLog(LogLevel::Warn, fmt, args);
        va_end(args);
    }

    void Logger::Error(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        VLog(LogLevel::Error, fmt, args);
        va_end(args);
    }

    void Logger::VLog(LogLevel level, const char* fmt, va_list args)
    {
        if (level < m_level)
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        const char* prefix = "";
        switch (level)
        {
        case LogLevel::Debug: prefix = "[DEBUG] "; break;
        case LogLevel::Info:  prefix = "[INFO]  "; break;
        case LogLevel::Warn:  prefix = "[WARN]  "; break;
        case LogLevel::Error: prefix = "[ERROR] "; break;
        }

        std::printf("%s", prefix);
        std::vprintf(fmt, args);
        std::printf("\n");
        std::fflush(stdout);
    }
} // namespace Core
