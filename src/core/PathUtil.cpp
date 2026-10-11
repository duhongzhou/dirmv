#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "core/PathUtil.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201)
#endif
// 自包含定义，避免依赖 SDK 的 _WIN32_WINNT 门槛
typedef struct _dirmv_REPARSE_DATA_BUFFER
{
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union
    {
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct
        {
            USHORT SubstituteNameOffset;
            USHORT SubstituteNameLength;
            USHORT PrintNameOffset;
            USHORT PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
    };
} dirmv_REPARSE_DATA_BUFFER;
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace dirmv::Path
{

    std::wstring Trim(const std::wstring &s)
    {
        size_t b = 0, e = s.size();
        auto sp = [](wchar_t c)
        { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        while (b < e && sp(s[b]))
            ++b;
        while (e > b && sp(s[e - 1]))
            --e;
        return s.substr(b, e - b);
    }

    std::wstring ToLower(const std::wstring &s)
    {
        std::wstring r = s;
        std::transform(r.begin(), r.end(), r.begin(),
                       [](wchar_t c)
                       { return (wchar_t)std::towlower(c); });
        return r;
    }

    bool Equals(const std::wstring &a, const std::wstring &b)
    {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    std::wstring FormatBytes(uint64_t bytes)
    {
        const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
        double v = (double)bytes;
        int i = 0;
        while (v >= 1024.0 && i < 5)
        {
            v /= 1024.0;
            ++i;
        }
        wchar_t buf[64];
        swprintf_s(buf, L"%.2f %s", v, units[i]);
        return buf;
    }

    std::wstring Combine(const std::wstring &a, const std::wstring &b)
    {
        if (a.empty())
            return b;
        std::wstring r = a;
        if (r.back() != L'\\' && r.back() != L'/')
            r += L'\\';
        size_t i = 0;
        while (i < b.size() && (b[i] == L'\\' || b[i] == L'/'))
            ++i;
        r += b.substr(i);
        return r;
    }

    std::wstring Normalize(const std::wstring &p)
    {
        if (p.empty())
            return p;
        std::wstring r = p;
        std::replace(r.begin(), r.end(), L'/', L'\\');
        while (r.size() > 3 && (r.back() == L'\\'))
            r.pop_back();
        // "C:" -> "C:\"? 保留原样，IsDriveRoot 会处理
        return r;
    }

    std::wstring FileName(const std::wstring &p)
    {
        std::wstring n = Normalize(p);
        size_t pos = n.find_last_of(L'\\');
        return (pos == std::wstring::npos) ? n : n.substr(pos + 1);
    }

    std::wstring ParentDir(const std::wstring &p)
    {
        std::wstring n = Normalize(p);
        size_t pos = n.find_last_of(L'\\');
        if (pos == std::wstring::npos)
            return L"";
        if (pos == 2 && n[1] == L':')
            return n.substr(0, 3); // "C:\"
        return n.substr(0, pos);
    }

    std::wstring ExePath()
    {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        return std::wstring(buf, n);
    }

    std::wstring ExeDir()
    {
        return ParentDir(ExePath());
    }

    bool IsAbsolute(const std::wstring &p)
    {
        if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'))
            return true;
        if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
            return true; // UNC
        return false;
    }

    bool IsDriveRoot(const std::wstring &p)
    {
        std::wstring n = Normalize(p);
        if (n.empty())
            return true;
        if (n.size() == 2 && n[1] == L':')
            return true; // "C:"
        if (n.size() == 3 && n[1] == L':' && n[2] == L'\\')
            return true; // "C:\"
        return false;
    }

    bool IsSubPath(const std::wstring &a, const std::wstring &b)
    {
        std::wstring pa = ToLower(Normalize(a));
        std::wstring pb = ToLower(Normalize(b));
        if (pa.empty() || pb.empty())
            return false;
        if (pa.back() != L'\\')
            pa += L'\\';
        if (pb.size() <= pa.size())
            return false;
        return pb.compare(0, pa.size(), pa) == 0;
    }

    bool DirectoryExists(const std::wstring &p)
    {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool FileExists(const std::wstring &p)
    {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool IsReparsePoint(const std::wstring &p)
    {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    std::wstring ReadLinkTarget(const std::wstring &p, bool *junction)
    {
        if (junction)
            *junction = false;
        HANDLE h = CreateFileW(p.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return L"";

        std::vector<BYTE> buf(16 * 1024); // MAXIMUM_REPARSE_DATA_BUFFER_SIZE
        DWORD bytes = 0;
        std::wstring result;
        if (DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                            buf.data(), (DWORD)buf.size(), &bytes, nullptr))
        {
            auto *rdb = reinterpret_cast<dirmv_REPARSE_DATA_BUFFER *>(buf.data());
            if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT)
            {
                // 目录联接：PrintName 是显示名
                if (junction)
                    *junction = true;
                USHORT off = rdb->MountPointReparseBuffer.PrintNameOffset;
                USHORT len = rdb->MountPointReparseBuffer.PrintNameLength;
                if (len > 0)
                {
                    const wchar_t *name =
                        (const wchar_t *)((BYTE *)rdb->MountPointReparseBuffer.PathBuffer + off);
                    result.assign(name, len / sizeof(wchar_t));
                }
                else
                {
                    off = rdb->MountPointReparseBuffer.SubstituteNameOffset;
                    len = rdb->MountPointReparseBuffer.SubstituteNameLength;
                    const wchar_t *name =
                        (const wchar_t *)((BYTE *)rdb->MountPointReparseBuffer.PathBuffer + off);
                    result.assign(name, len / sizeof(wchar_t));
                }
            }
            else if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK)
            {
                if (junction)
                    *junction = false;
                USHORT off = rdb->SymbolicLinkReparseBuffer.PrintNameOffset;
                USHORT len = rdb->SymbolicLinkReparseBuffer.PrintNameLength;
                const wchar_t *name =
                    (const wchar_t *)((BYTE *)rdb->SymbolicLinkReparseBuffer.PathBuffer + off);
                result.assign(name, len / sizeof(wchar_t));
            }
        }
        CloseHandle(h);

        // 去掉前置 \\?\ 或 \??\ 前缀
        if (result.rfind(L"\\\\?\\", 0) == 0)
            result = result.substr(4);
        else if (result.rfind(L"\\??\\", 0) == 0)
            result = result.substr(4);
        return result;
    }

    std::wstring ReadFileUtf8(const std::wstring &path)
    {
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
            return L"";
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::string data;
        if (sz > 0)
        {
            data.resize((size_t)sz);
            fread(&data[0], 1, (size_t)sz, f);
        }
        fclose(f);
        if (data.empty())
            return L"";
        int wlen = MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), &w[0], wlen);
        return w;
    }

    bool WriteFileUtf8(const std::wstring &path, const std::wstring &text)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string data((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                            &data[0], len, nullptr, nullptr);
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
            return false;
        if (!data.empty())
            fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        return true;
    }

    bool AppendFileUtf8(const std::wstring &path, const std::wstring &text)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string data((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                            &data[0], len, nullptr, nullptr);
        FILE *f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f)
            return false;
        if (!data.empty())
            fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        return true;
    }

    std::vector<std::wstring> ListSubDirectories(const std::wstring &dir)
    {
        std::vector<std::wstring> out;
        std::wstring pattern = Combine(dir, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchLimitToDirectories, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE)
            return out;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            out.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return out;
    }

} // namespace dirmv::Path
