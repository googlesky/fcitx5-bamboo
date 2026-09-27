/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#ifndef _FCITX5_BAMBOO_BAMBOOCONFIG_H_
#define _FCITX5_BAMBOO_BAMBOOCONFIG_H_

#include <algorithm>
#include <cstddef>
#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/stringutils.h>
#include <string>
#include <utility>
#include <vector>

namespace fcitx {

struct InputMethodConstrain;
struct InputMethodAnnotation;
using InputMethodOption =
    Option<std::string, InputMethodConstrain, DefaultMarshaller<std::string>,
           InputMethodAnnotation>;

struct StringListAnnotation : public EnumAnnotation {
    void setList(std::vector<std::string> list) { list_ = std::move(list); }
    const auto &list() { return list_; }
    void dumpDescription(RawConfig &config) const {
        EnumAnnotation::dumpDescription(config);
        for (size_t i = 0; i < list_.size(); i++) {
            config.setValueByPath("Enum/" + std::to_string(i), list_[i]);
        }
    }

protected:
    std::vector<std::string> list_;
};

struct InputMethodAnnotation : public StringListAnnotation {
    void dumpDescription(RawConfig &config) const {
        StringListAnnotation::dumpDescription(config);
        config.setValueByPath("LaunchSubConfig", "True");
        for (size_t i = 0; i < list_.size(); i++) {
            config.setValueByPath(
                "SubConfigPath/" + std::to_string(i),
                stringutils::concat("fcitx://config/addon/bamboo/macro/",
                                    list_[i]));
        }
    }
};

struct InputMethodConstrain {
    using Type = std::string;

    InputMethodConstrain(const InputMethodOption *option) : option_(option) {}

    bool check(const std::string &name) const {
        // Avoid check during initialization
        const auto &list = option_->annotation().list();
        if (list.empty()) {
            return true;
        }
        return std::find(list.begin(), list.end(), name) != list.end();
    }
    void dumpDescription(RawConfig & /*unused*/) const {}

protected:
    const InputMethodOption *option_;
};

FCITX_CONFIGURATION(BambooKeymap,
                    Option<std::string> key{this, "Key", _("Key"), ""};
                    Option<std::string> value{this, "Value", _("Value"), ""};);

FCITX_CONFIGURATION(
    BambooMacroTable,
    OptionWithAnnotation<std::vector<BambooKeymap>, ListDisplayOptionAnnotation>
        macros{this,
               "Macro",
               _("Macro"),
               {},
               {},
               {},
               ListDisplayOptionAnnotation("Key")};);

FCITX_CONFIGURATION(
    BambooCustomKeymap,
    OptionWithAnnotation<std::vector<BambooKeymap>, ListDisplayOptionAnnotation>
        customKeymap{this,
                     "CustomKeymap",
                     _("Custom Keymap"),
                     {},
                     {},
                     {},
                     ListDisplayOptionAnnotation("Key")};);

using InputMethodOption =
    Option<std::string, InputMethodConstrain, DefaultMarshaller<std::string>,
           InputMethodAnnotation>;

// ibus-bamboo's typing modes ("chế độ gõ"). SurroundingText types without a
// preedit by editing the text before the cursor, Exclude leaves the
// application alone.
enum class BambooInputMode { Preedit, SurroundingText, Exclude };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(BambooInputMode, N_("Preedit"),
                                 N_("Surrounding Text"), N_("Exclude"));

FCITX_CONFIGURATION(
    BambooAppMode,
    Option<std::string> program{this, "Program", _("Program"), ""};
    OptionWithAnnotation<BambooInputMode, BambooInputModeI18NAnnotation> mode{
        this, "Mode", _("Typing Mode"), BambooInputMode::Preedit};
    OptionWithAnnotation<bool, ToolTipAnnotation> terminal{
        this,
        "Terminal",
        _("Terminal or code editor"),
        false,
        {},
        {},
        ToolTipAnnotation(_("Escape switches to English, as vim needs"))};);

FCITX_CONFIGURATION(BambooAppModeList,
                    OptionWithAnnotation<std::vector<BambooAppMode>,
                                         ListDisplayOptionAnnotation>
                        appModes{this,
                                 "AppMode",
                                 _("Typing Mode per Application"),
                                 {},
                                 {},
                                 {},
                                 ListDisplayOptionAnnotation("Program")};);

// What a W with nothing to mark types. Telex W and Telex 2 type Ư, Always
// gives it to every Telex, NotAtWordStart types W at word start like UniKey
// without "Process W at word begin".
enum class BambooStandaloneW { Default, Always, NotAtWordStart };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(BambooStandaloneW,
                                 N_("As the input method does"), N_("Ư"),
                                 N_("Ư, but W at word start"));

FCITX_CONFIGURATION(
    BambooQuickTyping,
    OptionWithAnnotation<BambooStandaloneW, BambooStandaloneWI18NAnnotation>
        standaloneW{this, "StandaloneW", _("W with nothing to mark types"),
                    BambooStandaloneW::Default};);

FCITX_CONFIGURATION(
    BambooConfig, KeyListOption restoreKeyStroke{this,
                                                 "RestoreKeyStroke",
                                                 _("Restore Key Stroke"),
                                                 {},
                                                 KeyListConstrain()};
    Option<std::string, InputMethodConstrain, DefaultMarshaller<std::string>,
           InputMethodAnnotation>
        inputMethod{this, "InputMethod", _("Input Method"), "Telex",
                    InputMethodConstrain(&inputMethod)};
    OptionWithAnnotation<std::string, StringListAnnotation> outputCharset{
        this, "OutputCharset", _("Output Charset"), "Unicode"};
    OptionWithAnnotation<BambooInputMode, BambooInputModeI18NAnnotation>
        inputMode{this, "DefaultInputMode", _("Default Typing Mode"),
                  BambooInputMode::Preedit};
    SubConfigOption appModes{this, "AppModes", _("Typing Mode per Application"),
                             "fcitx://config/addon/bamboo/app_modes"};
    Option<bool> autoExcludeFields{
        this, "AutoExcludeFields",
        _("Disable Vietnamese in email, number and phone fields"), true};
    OptionWithAnnotation<bool, ToolTipAnnotation> editWordBeforeCursor{
        this,
        "EditWordBeforeCursor",
        _("Edit the word before the cursor"),
        true,
        {},
        {},
        ToolTipAnnotation(_("After BackSpace, arrows or a click, a key right "
                            "after a word edits it like one being typed"))};
    KeyListOption inputModeSwitchKey{
        this,
        "InputModeSwitchKey",
        _("Choose Typing Mode for Application"),
        {Key(FcitxKey_asciitilde)},
        KeyListConstrain(KeyConstrainFlag::AllowModifierLess)};
    // Defaults follow ibus-bamboo. Spell check means restoring the keys of
    // invalid words, SpellCheck only picks the dictionary over the rules.
    Option<bool> autoNonVnRestore{this, "AutoNonVnRestore",
                                  _("Enable spell check"), true};
    Option<bool> spellCheck{this, "SpellCheck",
                            _("Use dictionary for spell check"), false};
    Option<std::vector<std::string>> spellCheckExceptions{
        this, "SpellCheckExceptions", _("Words kept by spell check"), {}};
    Option<bool> macro{this, "Macro", _("Enable Macro"), false};
    Option<bool> capitalizeMacro{this, "CapitalizeMacro", _("Capitalize Macro"),
                                 true};
    Option<bool> modernStyle{this, "ModernStyle",
                             _("Use oà, _uý (instead of òa, úy)"), false};
    Option<bool> freeMarking{this, "FreeMarking",
                             _("Allow type with more freedom"), true};
    Option<BambooQuickTyping> quickTyping{this, "QuickTyping",
                                          _("Quick Typing")};
    Option<bool> displayUnderline{this, "DisplayUnderline",
                                  _("Underline the preedit text"), false};
    SubConfigOption custumKeymap{this, "CustomKeymap", _("Custom Keymap"),
                                 "fcitx://config/addon/bamboo/custom_keymap"};);
} // namespace fcitx

#endif
