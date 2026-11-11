#pragma once

#include <cstdio>
#include <cstdarg>
#include <mutex>

namespace Core
{
    enum class LogLevel
    {
        Debug,
        Info,
        Warn,
        Error
    };

    class Logger
    {
    public:
        static Logger& Instance();

        void SetLevel(LogLevel level);
        void Log(LogLevel level, const char* fmt, ...);

        void Debug(const char* fmt, ...);
        void Info(const char* fmt, ...);
        void Warn(const char* fmt, ...);
        void Error(const char* fmt, ...);

    private:
        Logger() = default;
        void VLog(LogLevel level, const char* fmt, va_list args);

        LogLevel m_level = LogLevel::Info;
        std::mutex m_mutex;
    };
} // namespace Core
