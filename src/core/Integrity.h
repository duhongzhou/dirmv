#pragma once
// ---------------------------------------------------------------------------
// Integrity.h - 配置防篡改（HMAC-SHA256 签名）
//   配置文本 + "\n#SIG:<hex>" 行；加载时校验签名，任何手改都会导致不匹配
//   使用 Windows CNG (bcrypt) 实现，无第三方依赖
// ---------------------------------------------------------------------------
#include <string>

namespace dirmv
{

    class Integrity
    {
    public:
        // 对 payload 计算 HMAC-SHA256，返回 64 字符十六进制小写
        static std::string HmacSha256Hex(const std::string &payload);

        // 给配置文本追加签名行；返回带签名的完整文本
        static std::wstring Sign(const std::wstring &configText);

        // 校验带签名的配置文本。out_payload 返回去掉签名行的原文。
        // 返回：0=签名有效；1=无签名行；2=签名不匹配
        static int Verify(const std::wstring &signedText, std::wstring &out_payload);

        // 是否包含签名行
        static bool HasSignature(const std::wstring &signedText);
    };

} // namespace dirmv
