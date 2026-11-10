#include "core/Log.h"
#include "core/PathUtil.h"
#include <windows.h>
#include <vector>
#include <cstdio>

namespace dirmv
{

    Logger &Logger::Instance()
    {
        static Logger inst;
        return inst;
    }

    void Logger::SetFile(const std::wstring &path)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        filePath_ = path;
        fileEnabled_ = !path.empty();
    }

    void Logger::AddSink(Sink sink)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        sinks_.push_back(std::move(sink));
    }

    // 日志单文件上限：超过后滚动为 <name>.1（覆盖旧备份），避免无限增长
    static constexpr uint64_t kMaxLogBytes = 5ull * 1024 * 1024;

    static void RotateIfNeeded(const std::wstring &path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            return;
        ULARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart < kMaxLogBytes)
            return;
        std::wstring bak = path + L".1";
        MoveFileExW(path.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    static const wchar_t *LevelName(LogLevel lv)    {
        switch (lv)
        {
        case LogLevel::Trace:
            return L"TRACE";
        case LogLevel::Info:
            return L"INFO ";
        case LogLevel::Warn:
            return L"WARN ";
        case LogLevel::Err:
            return L"ERROR";
        }
        return L"?????";
    }

    void Logger::Write(LogLevel lv, const std::wstring &msg)
    {
        if (lv < level_)
            return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t ts[64];
        swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::wstring line = std::wstring(ts) + L" [" + LevelName(lv) + L"] " + msg;

        std::vector<Sink> sinkCopy;
        bool toFile = false;
        bool toConsole = false;
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            toFile = fileEnabled_ && !filePath_.empty();
            path = filePath_;
            toConsole = console_;
            sinkCopy = sinks_;
        }
        // 文件按 UTF-8 追加写（避免 wofstream 在中文上失败），并做大小轮转
        if (toFile)
        {
            RotateIfNeeded(path);
            Path::AppendFileUtf8(path, line + L"\r\n");
        }
        // 控制台优先用 WriteConsoleW 输出宽字符，避免代码页乱码
        if (toConsole)
        {
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode = 0;
            std::wstring l = line + L"\r\n";
            if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
            {
                DWORD written = 0;
                WriteConsoleW(hOut, l.c_str(), (DWORD)l.size(), &written, nullptr);
            }
            else
            {
                int len = WideCharToMultiByte(CP_UTF8, 0, l.c_str(), (int)l.size(),
                                              nullptr, 0, nullptr, nullptr);
                std::string utf8((size_t)len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, l.c_str(), (int)l.size(),
                                    &utf8[0], len, nullptr, nullptr);
                fwrite(utf8.data(), 1, utf8.size(), stdout);
                fflush(stdout);
            }
        }
        for (auto &s : sinkCopy)
        {
            if (s)
                s(lv, line);
        }
    }

} // namespace dirmv
