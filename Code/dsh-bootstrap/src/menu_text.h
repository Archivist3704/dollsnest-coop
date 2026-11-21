#pragma once

namespace dsh::menu_text {

enum class Language { English, SimplifiedChinese, TraditionalChinese, Japanese };

struct Text {
    const wchar_t* caption;
    const wchar_t* subtitle;
    const wchar_t* host;
    const wchar_t* join;
    const wchar_t* quit;
    const wchar_t* keep;
    const wchar_t* end;
};

inline constexpr Text kEnglish{
    L"Co-op Menu", L"", L"Host a session", L"Join a session", L"Quit", L"Continue", L"End the session"};

inline constexpr Text kSimplifiedChinese{
    L"联机菜单", L"Co-op Menu", L"担任主机", L"加入房间", L"退出", L"继续", L"结束联机"};

inline constexpr Text kTraditionalChinese{
    L"連線選單", L"Co-op Menu", L"擔任主機", L"加入房間", L"退出", L"繼續", L"結束連線"};

inline constexpr Text kJapanese{
    L"マルチプレイメニュー", L"Co-op Menu", L"ホストになる", L"セッションに参加", L"やめる", L"続ける",
    L"セッションを終了"};

inline const Text& for_language(Language language) {
    switch (language) {
        case Language::SimplifiedChinese: return kSimplifiedChinese;
        case Language::TraditionalChinese: return kTraditionalChinese;
        case Language::Japanese: return kJapanese;
        default: return kEnglish;
    }
}

inline const char* tag(Language language) {
    switch (language) {
        case Language::SimplifiedChinese: return "zh-Hans";
        case Language::TraditionalChinese: return "zh-Hant";
        case Language::Japanese: return "ja";
        default: return "en";
    }
}

}
