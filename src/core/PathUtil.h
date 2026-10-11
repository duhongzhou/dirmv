#pragma once
// ---------------------------------------------------------------------------
// PathUtil.h - 路径、文件、字符串工具（全部 Unicode）
// ---------------------------------------------------------------------------
#include <string>
#include <vector>
#include <cstdint>

namespace dirmv::Path
{

    // --- 字符串 ---
    std::wstring Trim(const std::wstring &s);
    std::wstring ToLower(const std::wstring &s);
    bool Equals(const std::wstring &a, const std::wstring &b); // 不区分大小写
    std::wstring FormatBytes(uint64_t bytes);

    // --- 拼接 / 规范化 ---
    std::wstring Combine(const std::wstring &a, const std::wstring &b);
    // 去掉尾部斜杠，统一为反斜杠
    std::wstring Normalize(const std::wstring &p);
    std::wstring FileName(const std::wstring &p);  // 末段
    std::wstring ParentDir(const std::wstring &p); // 父目录
    std::wstring ExePath();
    std::wstring ExeDir();

    // --- 判定 ---
    bool IsAbsolute(const std::wstring &p);
    bool IsDriveRoot(const std::wstring &p); // C:\ 或 C: 或无
    // b 是否在 a 内部（不含相等）
    bool IsSubPath(const std::wstring &a, const std::wstring &b);
    bool DirectoryExists(const std::wstring &p);
    bool FileExists(const std::wstring &p);
    // 是否为符号链接/联接（重解析点）
    bool IsReparsePoint(const std::wstring &p);
    // 读取链接/联接的目标路径（失败返回空串）。junction=true 表示是目录联接
    std::wstring ReadLinkTarget(const std::wstring &p, bool *junction = nullptr);

    // --- 文本文件 ---
    std::wstring ReadFileUtf8(const std::wstring &path);
    bool WriteFileUtf8(const std::wstring &path, const std::wstring &text);
    bool AppendFileUtf8(const std::wstring &path, const std::wstring &text);

    // --- 目录枚举 ---
    std::vector<std::wstring> ListSubDirectories(const std::wstring &dir);

} // namespace dirmv::Path
