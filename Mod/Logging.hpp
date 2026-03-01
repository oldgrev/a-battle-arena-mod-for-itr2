#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iostream>
#include <Windows.h>

namespace Mod
{
    class Logger
    {
    public:
        static Logger &Get()
        {
            static Logger instance;
            return instance;
        }

        void Initialize()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!file_.is_open())
            {
                char tempPath[MAX_PATH] = {};
                DWORD len = GetTempPathA(MAX_PATH, tempPath);
                if (len == 0 || len >= MAX_PATH)
                {
                    path_ = "C:\\itr2_mod_log.txt";
                }
                else
                {
                    path_ = std::string(tempPath);
                    if (!path_.empty() && path_.back() != '\\')
                    {
                        path_.push_back('\\');
                    }
                    path_ += "itr2_mod_log.txt";
                }

                file_.open(path_, std::ios::out | std::ios::app);
            }
        }

        // current time as string for log entries
        std::string currentTimeString() const
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char buffer[64] = {};
            snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            return std::string(buffer);
        }

        // Log a message to the file with timestamp
        void Log(const std::string &message)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!file_.is_open())
            {
                Initialize();
            }

            if (file_.is_open())
            {
                file_ << currentTimeString() << " " << message << "\n";
                file_.flush();
            }

            // Also output to debug console
            OutputDebugStringA((currentTimeString() + " " + message + "\n").c_str());
        }

        std::string GetPath() const
        {
            return path_;
        }

    private:
        Logger() = default;
        ~Logger()
        {
            if (file_.is_open())
            {
                file_.close();
            }
        }

        // Prevent copying
        Logger(const Logger &) = delete;
        Logger &operator=(const Logger &) = delete;

        std::mutex mutex_;
        std::ofstream file_;
        std::string path_;
    };

// Convenience macros
#define LOG_INFO(msg)                       \
    do                                      \
    {                                       \
        std::ostringstream _oss;            \
        _oss << "[mod] " << msg;            \
        Mod::Logger::Get().Log(_oss.str()); \
    } while (0)
    //do {} while (0)

#define LOG_ERROR(msg)                      \
    do                                      \
    {                                       \
        std::ostringstream _oss;            \
        _oss << "[ERROR] " << msg;          \
        Mod::Logger::Get().Log(_oss.str()); \
    } while (0)

#define LOG_WARN(msg)                       \
    do                                      \
    {                                       \
        std::ostringstream _oss;            \
        _oss << "[WARN] " << msg;           \
        Mod::Logger::Get().Log(_oss.str()); \
    } while (0)

#define LOG_DEBUG(msg)                      \
    do                                      \
    {                                       \
        std::ostringstream _oss;            \
        _oss << "[DEBUG] " << msg;          \
        Mod::Logger::Get().Log(_oss.str()); \
    } while (0)
}
