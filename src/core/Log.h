#pragma once
// ---------------------------------------------------------------------------
// Log.h - 线程安全的文件 / 控制台日志 + 简单任务日志
// ---------------------------------------------------------------------------
#include <string>
#include <mutex>
#include <fstream>
#include <functional>
#include <vector>

namespace dirmv
{

    enum class LogLevel
    {
        Trace = 0,
        Info,
        Warn,
        Err
    };

    class Logger
    {
    public:
        static Logger &Instance();

        // 设置日志文件路径（追加模式）。传空字符串则关闭文件日志。
        void SetFile(const std::wstring &path);
        void SetLevel(LogLevel lv) { level_ = lv; }
        void EnableConsole(bool on) { console_ = on; }

        void Write(LogLevel lv, const std::wstring &msg);

        // 日志回调（用于把日志同步到 GUI 列表）
        using Sink = std::function<void(LogLevel, const std::wstring &)>;
        void AddSink(Sink sink);

        // 便捷函数
        void Trace(const std::wstring &m) { Write(LogLevel::Trace, m); }
        void Info(const std::wstring &m) { Write(LogLevel::Info, m); }
        void Warn(const std::wstring &m) { Write(LogLevel::Warn, m); }
        void Error(const std::wstring &m) { Write(LogLevel::Err, m); }

    private:
        Logger() = default;
        std::mutex mtx_;
        std::wstring filePath_;
        bool fileEnabled_ = false;
        LogLevel level_ = LogLevel::Info;
        bool console_ = false;
        std::vector<Sink> sinks_;
    };

// 便捷宏
#define LOGI(msg) ::dirmv::Logger::Instance().Info(msg)
#define LOGW(msg) ::dirmv::Logger::Instance().Warn(msg)
#define LOGE(msg) ::dirmv::Logger::Instance().Error(msg)
#define LOGT(msg) ::dirmv::Logger::Instance().Trace(msg)

} // namespace dirmv
