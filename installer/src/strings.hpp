// Aurora Browser Setup — installer UI strings.
//
// The language page changes the wizard immediately and is also handed to a
// fresh Aurora profile on first launch. Only languages with a complete
// translation are listed: a selector that silently does nothing would be
// worse than a short list.
#pragma once

#include <string>

namespace aurora::setup {

enum class Language { En, Ro, De, Fr, Es };

struct LanguageInfo {
    Language id;
    const wchar_t* native_name;
    const wchar_t* tag;
    unsigned short primary_language;  // PRIMARYLANGID of the Windows locale
};

struct Strings {
    const wchar_t* app_title;
    const wchar_t* tagline;
    const wchar_t* lang_title;
    const wchar_t* lang_subtitle;
    const wchar_t* welcome_title;
    const wchar_t* welcome_body;
    const wchar_t* welcome_hint;
    const wchar_t* location_title;
    const wchar_t* location_subtitle;
    const wchar_t* location_browse;
    const wchar_t* space_required;
    const wchar_t* space_available;
    const wchar_t* location_choose;
    const wchar_t* components_title;
    const wchar_t* components_subtitle;
    const wchar_t* components_required;
    const wchar_t* components_desktop;
    const wchar_t* components_start_menu;
    const wchar_t* components_description;
    const wchar_t* installing_title;
    const wchar_t* installing_subtitle;
    const wchar_t* installing_preparing;
    const wchar_t* installing_files;
    const wchar_t* complete_title;
    const wchar_t* complete_subtitle;
    const wchar_t* complete_updated;
    const wchar_t* complete_launch;
    const wchar_t* complete_shortcut;
    const wchar_t* failed_title;
    const wchar_t* failed_subtitle;
    const wchar_t* failed_code;
    const wchar_t* failed_log;
    const wchar_t* thanks_title;
    const wchar_t* thanks_subtitle;
    const wchar_t* btn_back;
    const wchar_t* btn_next;
    const wchar_t* btn_cancel;
    const wchar_t* btn_finish;
    const wchar_t* btn_close;
    const wchar_t* btn_launch;
    const wchar_t* cancel_title;
    const wchar_t* cancel_text;
    const wchar_t* running_title;
    const wchar_t* running_text;
    const wchar_t* elevate_title;
    const wchar_t* elevate_text;
    const wchar_t* invalid_dir_title;
    const wchar_t* invalid_dir_text;
    const wchar_t* shortcut_error_title;
    const wchar_t* launch_error_title;
    const wchar_t* update_title;
    const wchar_t* update_subtitle;
    const wchar_t* install_canceled;
};

const LanguageInfo* language_list();
int language_count();
const Strings& strings(Language language);
int language_index(Language language);
Language language_from_tag(const std::wstring& tag);
Language system_language();

// Replaces the first "%s" in `pattern` with `value`.
std::wstring format_string(const wchar_t* pattern, const std::wstring& value);

} // namespace aurora::setup
