#include "gui/I18n.h"
#include "gui/LangEn.h"

#include <cwctype>
#include <string>
#include <unordered_map>

namespace
{
    std::wstring g_lang = L"zh";
    std::unordered_map<std::wstring, std::wstring> g_zh2en;
    std::unordered_map<std::wstring, std::wstring> g_en2zh;
    bool g_inited = false;

    void EnsureInit()
    {
        if (g_inited)
            return;
        g_inited = true;
        const size_t n = sizeof(kUiStrings) / sizeof(kUiStrings[0]);
        g_zh2en.reserve(n * 2);
        g_en2zh.reserve(n * 2);
        for (size_t i = 0; i < n; ++i)
        {
            const std::wstring zh = kUiStrings[i].zh;
            const std::wstring en = kUiStrings[i].en;
            g_zh2en.emplace(zh, en);
            g_en2zh.emplace(en, zh);
        }
    }
}

namespace i18n
{
    void SetLanguage(const std::wstring &lang)
    {
        EnsureInit();
        std::wstring v = lang;
        for (auto &c : v)
            c = (wchar_t)towlower(c);
        g_lang = (v == L"en") ? L"en" : L"zh";
    }

    const std::wstring &Language()
    {
        return g_lang;
    }

    bool IsEnglish()
    {
        return g_lang == L"en";
    }

    const wchar_t *T(const wchar_t *zh)
    {
        if (!zh)
            return L"";
        EnsureInit();
        if (g_lang != L"en")
            return zh;
        auto it = g_zh2en.find(zh);
        return (it == g_zh2en.end()) ? zh : it->second.c_str();
    }

    std::wstring TranslateText(const std::wstring &text)
    {
        if (text.empty())
            return text;
        EnsureInit();
        if (g_lang == L"en")
        {
            auto it = g_zh2en.find(text);
            if (it != g_zh2en.end())
                return it->second;
            return text;
        }
        auto it = g_en2zh.find(text);
        if (it != g_en2zh.end())
            return it->second;
        return text;
    }

    void InitFromConfig(const std::wstring &cfgLang)
    {
        SetLanguage(cfgLang);
    }
}
