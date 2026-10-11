#include "core/Integrity.h"

#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <cctype>

#pragma comment(lib, "bcrypt.lib")

namespace dirmv
{

    // 内嵌密钥（用于 HMAC）。防篡改目标：检出普通用户/工具的随手改动，
    // 而非对抗逆向工程。可采用多段拼接略微增加难度。
    static const char *kKeyParts[] = {
        "dirmv-2026-",
        "cfg-integrity-",
        "5f3a9c1e7b2d4806"};
    static std::string Key()
    {
        std::string k;
        for (auto p : kKeyParts)
            k += p;
        return k;
    }

    static std::string ToHex(const unsigned char *data, size_t len)
    {
        static const char *hex = "0123456789abcdef";
        std::string s;
        s.reserve(len * 2);
        for (size_t i = 0; i < len; ++i)
        {
            s.push_back(hex[data[i] >> 4]);
            s.push_back(hex[data[i] & 0x0F]);
        }
        return s;
    }

    std::string Integrity::HmacSha256Hex(const std::string &payload)
    {
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                        BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
            return "";

        unsigned char hash[32] = {0};
        std::string key = Key();

        // 计算 HMAC
        NTSTATUS st = 0;
        // 使用一次性接口所需的缓冲区
        DWORD objLen = 0, cb = 0;
        BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0);
        std::vector<unsigned char> obj(objLen);

        BCRYPT_HASH_HANDLE hHash = nullptr;
        st = BCryptCreateHash(hAlg, &hHash, obj.data(), objLen,
                              (PUCHAR)key.data(), (ULONG)key.size(), 0);
        if (st == 0)
        {
            BCryptHashData(hHash, (PUCHAR)payload.data(), (ULONG)payload.size(), 0);
            BCryptFinishHash(hHash, hash, sizeof(hash), 0);
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return ToHex(hash, sizeof(hash));
    }

    std::wstring Integrity::Sign(const std::wstring &configText)
    {
        // 规范化：签名正文始终以换行结尾，签名行紧随其后。
        // 这样 Verify 只要「取 #SIG: 之前的全部文本」即可与这里一致。
        std::wstring payload = configText;
        while (!payload.empty() && (payload.back() == L'\n' || payload.back() == L'\r'))
            payload.pop_back();
        payload += L"\r\n";

        // 以 UTF-8 计算签名
        int len = WideCharToMultiByte(CP_UTF8, 0, payload.c_str(), (int)payload.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string utf8((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, payload.c_str(), (int)payload.size(),
                            &utf8[0], len, nullptr, nullptr);

        std::string sig = HmacSha256Hex(utf8);
        std::string sigLine = "#SIG:" + sig;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, sigLine.c_str(), (int)sigLine.size(), nullptr, 0);
        std::wstring wsig((size_t)wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, sigLine.c_str(), (int)sigLine.size(), &wsig[0], wlen);

        std::wstring out = payload;
        out += wsig;
        out += L"\r\n";
        return out;
    }

    static const std::wstring kSigPrefix = L"#SIG:";

    bool Integrity::HasSignature(const std::wstring &signedText)
    {
        return signedText.find(kSigPrefix) != std::wstring::npos;
    }

    int Integrity::Verify(const std::wstring &signedText, std::wstring &out_payload)
    {
        // 找出签名行（可能位于末尾）
        size_t pos = signedText.rfind(kSigPrefix);
        if (pos == std::wstring::npos)
        {
            out_payload = signedText;
            return 1; // 无签名
        }
        // 签名行结尾
        size_t eol = signedText.find(L'\n', pos);
        std::wstring sig = signedText.substr(pos + kSigPrefix.size(),
                                             (eol == std::wstring::npos)
                                                 ? std::wstring::npos
                                                 : eol - pos - kSigPrefix.size());
        // 去尾部 \r/空白
        while (!sig.empty() && (sig.back() == L'\r' || sig.back() == L' ' || sig.back() == L'\t'))
            sig.pop_back();

        // 去掉签名行本身及其前面的换行，得到正文
        size_t lineStart = pos;
        while (lineStart > 0 && (signedText[lineStart - 1] == L'\n' || signedText[lineStart - 1] == L'\r'))
            --lineStart;
        std::wstring base = signedText.substr(0, lineStart);
        // 新格式：正文规范化后以单个 \r\n 结尾（与 Sign 一致）
        std::wstring payload = base + L"\r\n";

        auto hmacOf = [](const std::wstring &text) -> std::string
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                          nullptr, 0, nullptr, nullptr);
            std::string utf8((size_t)len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                &utf8[0], len, nullptr, nullptr);
            return HmacSha256Hex(utf8);
        };

        // 签名(宽) 转 UTF-8 小写
        std::string sigUtf8;
        {
            int l2 = WideCharToMultiByte(CP_UTF8, 0, sig.c_str(), (int)sig.size(),
                                         nullptr, 0, nullptr, nullptr);
            sigUtf8.resize((size_t)l2);
            WideCharToMultiByte(CP_UTF8, 0, sig.c_str(), (int)sig.size(),
                                &sigUtf8[0], l2, nullptr, nullptr);
        }
        for (auto &c : sigUtf8)
            c = (char)tolower((unsigned char)c);

        out_payload = payload;
        // 新格式校验；旧格式（历史签名：正文不含结尾换行）兼容
        if (sigUtf8 == hmacOf(payload))
            return 0;
        if (sigUtf8 == hmacOf(base))
            return 0;
        return 2;
    }

} // namespace dirmv
