#pragma once

#include <string>

// 轻量界面文案本地化：中文为源语言，英文为译文。
// 仅影响图形界面（GUI）的显示文字；日志、错误详情等仍保持中文。
namespace i18n
{
    // 语言取值："zh"（默认）/ "en"
    void SetLanguage(const std::wstring &lang);
    const std::wstring &Language();
    bool IsEnglish();

    // 取中文源串对应的当前语言文本；没有译文时原样返回 zh。
    const wchar_t *T(const wchar_t *zh);

    // 在「中文源串 ↔ 英文译文」之间互相查找：当前语言为 en 时把 zh 译成 en，
    // 当前语言为 zh 时把 en 还原成 zh；找不到则原样返回。
    std::wstring TranslateText(const std::wstring &text);

    // 从配置读取语言（zh/en），非法值回退 zh。
    void InitFromConfig(const std::wstring &cfgLang);
}
