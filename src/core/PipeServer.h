#pragma once
// ---------------------------------------------------------------------------
// PipeServer.h - 命名管道控制通道
//   GUI 与后台服务之间传递控制命令（开始迁移/恢复/查询状态/停止）
//   协议：请求 "CMD\n"，响应为一行 JSON
// ---------------------------------------------------------------------------
#include <string>
#include <thread>
#include <atomic>
#include <functional>

namespace dirmv
{

    // 命令
    enum class PipeCommand
    {
        Status,  // 查询当前状态
        Migrate, // 立即迁移
        Restore, // 恢复
        Stop,    // 停止当前任务
        Reload,  // 重新加载配置
        Unknown,
    };

    class PipeServer
    {
    public:
        PipeServer() = default;
        ~PipeServer() { Stop(); }

        // handler 接收命令，返回一行 JSON 响应
        void Start(const std::wstring &pipeName,
                   std::function<std::wstring(PipeCommand, const std::wstring &)> handler);
        void Stop();

        static std::wstring DefaultPipeName();

    private:
        void Run();

        std::wstring pipeName_;
        std::function<std::wstring(PipeCommand, const std::wstring &)> handler_;
        std::thread thread_;
        std::atomic<bool> running_{false};
        void *stopEvent_ = nullptr; // HANDLE
    };

    // 客户端：向服务发送命令并等待响应（GUI / CLI 使用）
    class PipeClient
    {
    public:
        static std::wstring Send(const std::wstring &pipeName,
                                 PipeCommand cmd, const std::wstring &arg = L"");
    };

    const wchar_t *PipeCommandToString(PipeCommand c);
    PipeCommand PipeCommandFromString(const std::wstring &s);

} // namespace dirmv
