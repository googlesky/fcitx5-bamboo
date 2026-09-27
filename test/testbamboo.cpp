/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/environ.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/action.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/userinterfacemanager.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace fcitx;

namespace {

// Stands in for an application with the cursor at the end of its text:
// applies what fcitx sends, handles the keys fcitx lets through and reports
// its text as surrounding text, unless told not to like a Wayland terminal.
class FakeEditor : public InputContext {
public:
    FakeEditor(Instance *instance, const std::string &program,
               CapabilityFlags caps, bool reportSurrounding = true,
               const char *frontend = "bambootest")
        : InputContext(instance->inputContextManager(), program),
          reportSurrounding_(reportSurrounding), frontend_(frontend) {
        created();
        setCapabilityFlags(caps);
        syncSurrounding();
        focusIn();
        instance->setCurrentInputMethod(this, "bamboo", true);
    }
    ~FakeEditor() override { destroy(); }

    const char *frontend() const override { return frontend_; }

    // Returns whether fcitx filtered the key.
    bool press(const Key &key) {
        KeyEvent event(this, key);
        if (keyEvent(event)) {
            return true;
        }
        if (key.check(FcitxKey_BackSpace)) {
            if (!text_.empty()) {
                text_.pop_back();
            }
        } else if (key.check(FcitxKey_Return)) {
            text_.push_back('\n');
        } else if (!key.states().testAny(KeyStates{
                       KeyState::Ctrl, KeyState::Alt, KeyState::Super})) {
            // Control characters like Escape's are not typed.
            if (auto chr = Key::keySymToUnicode(key.sym()); chr >= 0x20) {
                text_.push_back(chr);
            }
        }
        syncSurrounding();
        return false;
    }

    // The application edits its text on its own, like an autocorrection.
    void replaceText(const std::string &text) {
        text_.clear();
        commitStringImpl(text);
    }

    // Like an application reporting its text late, if at all.
    void setReportSurrounding(bool report) { reportSurrounding_ = report; }
    void reportText(const std::string &text) {
        surroundingText().setText(text, utf8::length(text), utf8::length(text));
        updateSurroundingText();
    }

    // Types ASCII keys one by one.
    void type(std::string_view keys) {
        for (char c : keys) {
            press(Key(static_cast<KeySym>(c)));
        }
    }

    std::string text() const {
        std::string result;
        for (auto c : text_) {
            result += utf8::UCS4ToUTF8(c);
        }
        return result;
    }
    std::string preedit() { return inputPanel().clientPreedit().toString(); }

protected:
    void commitStringImpl(const std::string &str) override {
        for (auto c : utf8::MakeUTF8CharRange(str)) {
            text_.push_back(c);
        }
        syncSurrounding();
    }
    void deleteSurroundingTextImpl(int offset, unsigned int size) override {
        FCITX_ASSERT(offset == -static_cast<int>(size) && size <= text_.size())
            << "bad delete " << offset << " " << size << " on " << text();
        text_.resize(text_.size() - size);
        syncSurrounding();
    }
    void forwardKeyImpl(const ForwardKeyEvent & /*event*/) override {}
    void updatePreeditImpl() override {}

private:
    void syncSurrounding() {
        if (reportSurrounding_) {
            surroundingText().setText(text(), text_.size(), text_.size());
            updateSurroundingText();
        }
    }

    std::vector<uint32_t> text_;
    bool reportSurrounding_;
    const char *frontend_;
};

const CapabilityFlags PreeditCaps{CapabilityFlag::Preedit,
                                  CapabilityFlag::SurroundingText};

// Sub configs load partially: a missing list node keeps the old list.
void clearList(AddonInstance *bamboo, const std::string &path,
               const std::string &list) {
    RawConfig config;
    config.get(list, true);
    bamboo->setSubConfig(path, config);
}

void setup(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo", true);
    FCITX_ASSERT(bamboo);
    auto group = instance->inputMethodManager().currentGroup();
    group.inputMethodList().clear();
    group.inputMethodList().push_back(InputMethodGroupItem("keyboard-us"));
    group.inputMethodList().push_back(InputMethodGroupItem("bamboo"));
    group.setDefaultInputMethod("");
    instance->inputMethodManager().setGroup(group);
}

void testPreedit(Instance *instance) {
    FakeEditor editor(instance, "testapp", PreeditCaps);
    editor.type("tieengs");
    FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    FCITX_ASSERT(editor.text().empty()) << editor.text();
    editor.type(" vieetj");
    FCITX_ASSERT(editor.text() == "tiếng ") << editor.text();
    FCITX_ASSERT(editor.preedit() == "việt") << editor.preedit();
    editor.press(Key(FcitxKey_Return));
    FCITX_ASSERT(editor.text() == "tiếng việt\n") << editor.text();
    FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    // Switching input method commits the word being typed once.
    editor.type("nam");
    instance->setCurrentInputMethod(&editor, "keyboard-us", true);
    FCITX_ASSERT(editor.text() == "tiếng việt\nnam") << editor.text();
    FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
}

void testRestoreKeyStroke(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("RestoreKeyStroke/0", "Shift+space");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        // Nothing to restore: the key belongs to the application.
        FCITX_ASSERT(!editor.press(Key("Shift+space")));
        editor.type("tooi");
        FCITX_ASSERT(editor.preedit() == "tôi") << editor.preedit();
        FCITX_ASSERT(editor.press(Key("Shift+space")));
        FCITX_ASSERT(editor.preedit() == "tooi") << editor.preedit();
        // The next key must not be swallowed.
        editor.type("a");
        FCITX_ASSERT(editor.preedit() == "tooia") << editor.preedit();
        editor.type(" ");
        FCITX_ASSERT(editor.text() == " tooia ") << editor.text();
    }
    // Lock keys are ignored, unless they are the restore key.
    config.setValueByPath("RestoreKeyStroke/0", "Shift+Caps_Lock");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("tooi");
        FCITX_ASSERT(editor.press(Key(FcitxKey_Caps_Lock, KeyState::Shift)));
        FCITX_ASSERT(editor.preedit() == "tooi") << editor.preedit();
    }
    RawConfig reset;
    reset.get("RestoreKeyStroke", true);
    bamboo->setConfig(reset);
}

void testLockKeysKeepWord(Instance *instance) {
    FakeEditor editor(instance, "testapp", PreeditCaps);
    editor.type("tie");
    FCITX_ASSERT(!editor.press(Key(FcitxKey_Caps_Lock)));
    FCITX_ASSERT(!editor.press(Key(FcitxKey_Shift_L)));
    FCITX_ASSERT(editor.text().empty()) << editor.text();
    FCITX_ASSERT(editor.preedit() == "tie") << editor.preedit();
    editor.press(Key(FcitxKey_Return));
    FCITX_ASSERT(editor.text() == "tie\n") << editor.text();
}

// The tray toggle is ibus-bamboo's "spell check": restoring invalid words.
void testSpellCheckAction(Instance *instance) {
    auto *action =
        instance->userInterfaceManager().lookupAction("bamboo-spell-check");
    FCITX_ASSERT(action);
    FakeEditor editor(instance, "testapp", PreeditCaps);
    editor.type("text ");
    action->activate(&editor);
    editor.type("text ");
    action->activate(&editor);
    editor.type("text ");
    FCITX_ASSERT(editor.text() == "text tẽt text ") << editor.text();
}

void testInputModes(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    appModes.setValueByPath("AppMode/1/Program", "excluded");
    appModes.setValueByPath("AppMode/1/Mode", "Exclude");
    bamboo->setSubConfig("app_modes", appModes);
    RawConfig config;
    config.setValueByPath("RestoreKeyStroke/0", "Shift+space");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("tieengs vieetj hoafn");
        FCITX_ASSERT(editor.text() == "tiếng việt hoàn") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        // The tone moves back, the application must not get this backspace.
        FCITX_ASSERT(editor.press(Key(FcitxKey_BackSpace)));
        FCITX_ASSERT(editor.text() == "tiếng việt hòa") << editor.text();
        editor.type(" tooi");
        FCITX_ASSERT(editor.press(Key("Shift+space")));
        FCITX_ASSERT(editor.text() == "tiếng việt hòa tooi") << editor.text();
        // Leaving the input method must not commit the word a second time.
        instance->setCurrentInputMethod(&editor, "keyboard-us", true);
        FCITX_ASSERT(editor.text() == "tiếng việt hòa tooi") << editor.text();
    }
    {
        // Without surrounding text support, fall back to the preedit.
        FakeEditor editor(instance, "surrounding",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
        FCITX_ASSERT(editor.text().empty()) << editor.text();
    }
    {
        // Wayland frontends claim surrounding text for every client, a
        // client that sends none must get the preedit.
        FakeEditor editor(instance, "surrounding", PreeditCaps, false);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
        FCITX_ASSERT(editor.text().empty()) << editor.text();
    }
    {
        // Text changed behind the engine's back must not be deleted.
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("to");
        editor.replaceText("tx");
        editor.type("o");
        FCITX_ASSERT(editor.text() == "txo") << editor.text();
    }
    {
        // A word ends in the mode it started in.
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("to");
        editor.setCapabilityFlags(CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("o");
        FCITX_ASSERT(editor.preedit() == "o") << editor.preedit();
        editor.press(Key(FcitxKey_Return));
        FCITX_ASSERT(editor.text() == "too\n") << editor.text();
    }
    {
        FakeEditor editor(instance, "surrounding",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("to");
        editor.setCapabilityFlags(PreeditCaps);
        editor.type("o");
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        FCITX_ASSERT(editor.text() == "too") << editor.text();
    }
    {
        FakeEditor editor(instance, "excluded", PreeditCaps);
        FCITX_ASSERT(!editor.press(Key(FcitxKey_t)));
        editor.type("ieengs");
        FCITX_ASSERT(editor.text() == "tieengs") << editor.text();
    }
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    }
    config.setValueByPath("DefaultInputMode", "Surrounding Text");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("tieengs");
        FCITX_ASSERT(editor.text() == "tiếng") << editor.text();
    }
    RawConfig reset;
    reset.setValueByPath("DefaultInputMode", "Preedit");
    reset.get("RestoreKeyStroke", true);
    bamboo->setConfig(reset);
    clearList(bamboo, "app_modes", "AppMode");
}

// A key right after a word edits it like one being typed, once the
// application reported the word after a key it handled itself.
void testEditWordBeforeCursor(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    bamboo->setSubConfig("app_modes", appModes);
    for (const auto *program : {"testapp", "surrounding"}) {
        FakeEditor editor(instance, program, PreeditCaps);
        editor.type("vieet ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("j ");
        FCITX_ASSERT(editor.text() == "việt ") << program << editor.text();
        // A click resets the input method.
        editor.replaceText("xin chao");
        editor.reset();
        editor.type("f ");
        FCITX_ASSERT(editor.text() == "xin chào ") << program << editor.text();
        // The application did not report the last BackSpace yet.
        editor.type("tooi ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.setReportSurrounding(false);
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("s ");
        FCITX_ASSERT(editor.text() == "xin chào tôs ")
            << program << editor.text();
    }
    {
        // Typing fast, a report of the word may come after the space.
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        editor.type("toi ");
        editor.reportText("toi");
        editor.type("s");
        FCITX_ASSERT(editor.text() == "toi s") << editor.text();
    }
    {
        // Wayland frontends answer from a copy of the text.
        FakeEditor editor(instance, "testapp", PreeditCaps, true, "wayland_v2");
        editor.type("vieet ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("j ");
        FCITX_ASSERT(editor.text() == "viêtj ") << editor.text();
    }
    RawConfig config;
    config.setValueByPath("EditWordBeforeCursor", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("vieet ");
        editor.press(Key(FcitxKey_BackSpace));
        editor.type("j ");
        FCITX_ASSERT(editor.text() == "viêtj ") << editor.text();
    }
    config.setValueByPath("EditWordBeforeCursor", "True");
    bamboo->setConfig(config);
    clearList(bamboo, "app_modes", "AppMode");
}

void testStandaloneW(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("QuickTyping/StandaloneW", "Ư, but W at word start");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("w");
        FCITX_ASSERT(editor.preedit() == "w") << editor.preedit();
        editor.type(" nhwng ");
        FCITX_ASSERT(editor.text() == "w nhưng ") << editor.text();
    }
    config.setValueByPath("QuickTyping/StandaloneW",
                          "As the input method does");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("nhwng ");
        FCITX_ASSERT(editor.text() == "nhwng ") << editor.text();
    }
}

void testQuickTyping(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("QuickTyping/EndConsonants", "True");
    config.setValueByPath("QuickTyping/DoubleConsonants", "True");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("dog ccaf happy ");
        FCITX_ASSERT(editor.text() == "dong chà happy ") << editor.text();
    }
    config.setValueByPath("QuickTyping/EndConsonants", "False");
    config.setValueByPath("QuickTyping/DoubleConsonants", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("dog ");
        FCITX_ASSERT(editor.text() == "dog ") << editor.text();
    }
}

void testCapitalizeSentences(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("CapitalizeSentences", "True");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("xin chaof. tooi ddi");
        editor.press(Key(FcitxKey_Return));
        editor.type("abc! hey? ghi ");
        FCITX_ASSERT(editor.text() == "Xin chào. Tôi đi\nAbc! Hey? Ghi ")
            << editor.text();
        editor.replaceText("");
        editor.type("vnexpress.net ");
        FCITX_ASSERT(editor.text() == "Vnexpress.net ") << editor.text();
    }
    {
        // Without its text, keys tell a sentence start, not a field start.
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("abc. hey ghi");
        FCITX_ASSERT(editor.text() + editor.preedit() == "abc. Hey ghi")
            << editor.text();
        // A click may have moved the cursor anywhere.
        editor.type(". ");
        editor.reset();
        editor.type("jkl ");
        FCITX_ASSERT(editor.text() == "abc. Hey ghi. jkl ") << editor.text();
    }
    {
        // A text reported late is not trusted.
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("abc. ");
        editor.setReportSurrounding(false);
        editor.type("hey ghi ");
        FCITX_ASSERT(editor.text() == "Abc. Hey ghi ") << editor.text();
    }
    for (const auto flag :
         {CapabilityFlag::Terminal, CapabilityFlag::NoAutoUpperCase}) {
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit,
                                          CapabilityFlag::SurroundingText,
                                          flag});
        editor.type("abc. hey ");
        FCITX_ASSERT(editor.text() == "abc. hey ") << editor.text();
    }
    config.setValueByPath("InputMethod", "VIQR");
    bamboo->setConfig(config);
    {
        // VIQR types tones with '.' and '?'.
        FakeEditor editor(instance, "testapp",
                          CapabilityFlags{CapabilityFlag::Preedit});
        editor.type("ma. ba? ca");
        FCITX_ASSERT(editor.text() + editor.preedit() == "mạ bả ca")
            << editor.text();
    }
    config.setValueByPath("InputMethod", "Telex");
    config.setValueByPath("CapitalizeSentences", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("abc. hey ");
        FCITX_ASSERT(editor.text() == "abc. hey ") << editor.text();
    }
}

bool hasImportAction(Instance *instance, InputContext *ic) {
    auto *action =
        instance->userInterfaceManager().lookupAction("bamboo-import-macro");
    return std::ranges::count(
               ic->statusArea().actions(StatusGroup::InputMethod), action) == 1;
}

// Moving from ibus-bamboo: its macro file merges into the current table.
void testImportMacros(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    const auto home = std::filesystem::temp_directory_path() /
                      ("testbamboo-home-" + std::to_string(getpid()));
    std::filesystem::create_directories(home / ".config/ibus-bamboo");
    const std::string oldHome = getEnvironmentOrEmpty("HOME");
    setEnvironment("HOME", home.c_str());
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        FCITX_ASSERT(!hasImportAction(instance, &editor));
    }
    {
        std::ofstream file(home / ".config/ibus-bamboo/ibus-bamboo.macro.text");
        file << "# DO NOT DELETE THIS LINE*** version=1 ***\n#vn:Việt\n"
                "vn:Việt Nam\nhn:Hải Nam\nlnk:http://x\n";
    }
    RawConfig macros;
    macros.setValueByPath("Macro/0/Key", "hn");
    macros.setValueByPath("Macro/0/Value", "Hà Nội");
    bamboo->setSubConfig("macro/Telex", macros);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        FCITX_ASSERT(hasImportAction(instance, &editor));
        auto *action = instance->userInterfaceManager().lookupAction(
            "bamboo-import-macro");
        action->activate(&editor);
        // Importing again adds nothing.
        action->activate(&editor);
        editor.type("vn hn lnk ");
        FCITX_ASSERT(editor.text() == "việt nam hà nội http://x ")
            << editor.text();
    }
    RawConfig saved;
    bamboo->getSubConfig("macro/Telex")->save(saved);
    FCITX_ASSERT(saved.valueByPath("Macro/2/Key") &&
                 !saved.valueByPath("Macro/3/Key"))
        << saved;
    setEnvironment("HOME", oldHome.c_str());
    std::filesystem::remove_all(home);
    clearList(bamboo, "macro/Telex", "Macro");
    RawConfig config;
    config.setValueByPath("Macro", "False");
    bamboo->setConfig(config);
}

void testSpellCheckExceptions(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig config;
    config.setValueByPath("SpellCheckExceptions/0", "Krông");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("Kroong text ");
        FCITX_ASSERT(editor.text() == "Krông text ") << editor.text();
    }
    RawConfig reset;
    reset.get("SpellCheckExceptions", true);
    bamboo->setConfig(reset);
}

// In terminals and code editors Escape leaves Vietnamese, like VNIKey's vim
// mode: vim's normal mode needs plain keys.
void testTerminalEscape(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "vimapp");
    appModes.setValueByPath("AppMode/0/Mode", "Preedit");
    appModes.setValueByPath("AppMode/0/Terminal", "True");
    bamboo->setSubConfig("app_modes", appModes);
    {
        FakeEditor editor(instance, "vimapp", PreeditCaps);
        editor.type("vieetj");
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
        FCITX_ASSERT(instance->inputMethod(&editor) == "keyboard-us");
        editor.type("dd");
        FCITX_ASSERT(editor.text() == "việtdd") << editor.text();
    }
    {
        FakeEditor editor(instance, "vimapp", PreeditCaps);
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(instance->inputMethod(&editor) == "keyboard-us");
    }
    {
        // Terminals reported by the application itself.
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Terminal);
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(instance->inputMethod(&editor) == "keyboard-us");
    }
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("vieetj");
        FCITX_ASSERT(!editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        FCITX_ASSERT(instance->inputMethod(&editor) == "bamboo");
    }
    clearList(bamboo, "app_modes", "AppMode");
}

// The panel shows EN whenever keys go straight to the application.
void testModeLabel(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    auto *engine = instance->inputMethodEngine("bamboo");
    const auto *entry = instance->inputMethodManager().entry("bamboo");
    FCITX_ASSERT(engine && entry);
    RawConfig appModes;
    appModes.setValueByPath("AppMode/0/Program", "surrounding");
    appModes.setValueByPath("AppMode/0/Mode", "Surrounding Text");
    appModes.setValueByPath("AppMode/1/Program", "excluded");
    appModes.setValueByPath("AppMode/1/Mode", "Exclude");
    bamboo->setSubConfig("app_modes", appModes);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "VI");
        FCITX_ASSERT(engine->subMode(*entry, editor) == "Telex")
            << engine->subMode(*entry, editor);
    }
    {
        FakeEditor editor(instance, "surrounding", PreeditCaps);
        FCITX_ASSERT(engine->subMode(*entry, editor) ==
                     "Telex (Surrounding Text)")
            << engine->subMode(*entry, editor);
    }
    {
        FakeEditor editor(instance, "excluded", PreeditCaps);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
    }
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Email);
        FCITX_ASSERT(engine->subModeLabel(*entry, editor) == "EN");
    }
    clearList(bamboo, "app_modes", "AppMode");
}

// Addresses and numbers are never Vietnamese, unlike browsers' URL fields
// where people search.
void testFieldHints(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Email);
        editor.type("tuanf@gmail.com");
        FCITX_ASSERT(editor.text() == "tuanf@gmail.com") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    }
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Dialable);
        editor.type("0912 dd");
        FCITX_ASSERT(editor.text() == "0912 dd") << editor.text();
    }
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Url);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    }
    {
        // Becoming a number field mid-word ends the word first.
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("vieetj");
        editor.setCapabilityFlags(PreeditCaps | CapabilityFlag::Number);
        editor.type("1");
        FCITX_ASSERT(editor.text() == "việt1") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();
    }
    RawConfig config;
    config.setValueByPath("AutoExcludeFields", "False");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp",
                          PreeditCaps | CapabilityFlag::Email);
        editor.type("tieengs");
        FCITX_ASSERT(editor.preedit() == "tiếng") << editor.preedit();
    }
    config.setValueByPath("AutoExcludeFields", "True");
    bamboo->setConfig(config);
}

// ibus-bamboo's Shift+~ table choosing the typing mode of the application.
void testInputModePicker(Instance *instance) {
    const Key tilde(FcitxKey_asciitilde, KeyState::Shift);
    auto *bamboo = instance->addonManager().addon("bamboo");
    {
        FakeEditor editor(instance, "pickerapp", PreeditCaps);
        editor.type("vieetj");
        // Opening it ends the word.
        FCITX_ASSERT(editor.press(tilde));
        FCITX_ASSERT(editor.text() == "việt") << editor.text();
        auto candidates = editor.inputPanel().candidateList();
        FCITX_ASSERT(candidates && candidates->size() == 3);
        FCITX_ASSERT(candidates->label(0).toString() == "*. ");
        // Pressed again it closes and types '~'.
        FCITX_ASSERT(!editor.press(tilde));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        FCITX_ASSERT(editor.text() == "việt~") << editor.text();

        editor.press(tilde);
        FCITX_ASSERT(editor.press(Key(FcitxKey_2)));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        editor.type(" tieengs");
        FCITX_ASSERT(editor.text() == "việt~ tiếng") << editor.text();
        FCITX_ASSERT(editor.preedit().empty()) << editor.preedit();

        editor.press(tilde);
        editor.press(Key(FcitxKey_Down));
        FCITX_ASSERT(editor.press(Key(FcitxKey_Return)));
        editor.type(" aa");
        FCITX_ASSERT(editor.text() == "việt~ tiếng aa") << editor.text();

        // An excluded application can still get Vietnamese back.
        FCITX_ASSERT(editor.press(tilde));
        FCITX_ASSERT(editor.press(Key(FcitxKey_Escape)));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        editor.press(tilde);
        editor.press(Key(FcitxKey_1));
        editor.type(" aa");
        FCITX_ASSERT(editor.preedit() == "â") << editor.preedit();
        // Any other key closes it and types as usual.
        editor.press(tilde);
        editor.type("s");
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        FCITX_ASSERT(editor.preedit() == "s") << editor.preedit();
    }
    auto *appModes = bamboo->getSubConfig("app_modes");
    RawConfig saved;
    appModes->save(saved);
    FCITX_ASSERT(saved.valueByPath("AppMode/0/Program") &&
                 *saved.valueByPath("AppMode/0/Program") == "pickerapp");
    FCITX_ASSERT(*saved.valueByPath("AppMode/0/Mode") == "Preedit");
    FCITX_ASSERT(!saved.valueByPath("AppMode/1/Program"));
    {
        // Without a program name there is nothing to remember a mode for.
        FakeEditor editor(instance, "", PreeditCaps);
        FCITX_ASSERT(!editor.press(tilde));
        FCITX_ASSERT(editor.text() == "~") << editor.text();
    }
    RawConfig config;
    config.setValueByPath("InputMethod", "VIQR");
    bamboo->setConfig(config);
    {
        // '~' is VIQR's tone key, it must not open the table mid-word.
        FakeEditor editor(instance, "pickerapp", PreeditCaps);
        editor.type("a");
        FCITX_ASSERT(editor.press(tilde));
        FCITX_ASSERT(!editor.inputPanel().candidateList());
        FCITX_ASSERT(editor.preedit() == "ã") << editor.preedit();
    }
    config.setValueByPath("InputMethod", "Telex");
    bamboo->setConfig(config);
    clearList(bamboo, "app_modes", "AppMode");
}

// bamboo-core panics on this entry (3 targets, 2 results); fcitx5 must live.
void testBrokenCustomKeymap(Instance *instance) {
    auto *bamboo = instance->addonManager().addon("bamboo");
    RawConfig keymap;
    keymap.setValueByPath("CustomKeymap/0/Key", "w");
    keymap.setValueByPath("CustomKeymap/0/Value", "UOA_ƯƠ");
    bamboo->setSubConfig("custom_keymap", keymap);
    RawConfig config;
    config.setValueByPath("InputMethod", "Custom");
    bamboo->setConfig(config);
    {
        FakeEditor editor(instance, "testapp", PreeditCaps);
        editor.type("aw");
        FCITX_ASSERT(editor.text() == "aw") << editor.text();
    }
    config.setValueByPath("InputMethod", "Telex");
    bamboo->setConfig(config);
    clearList(bamboo, "custom_keymap", "CustomKeymap");
}

} // namespace

int main() {
    setupTestingEnvironmentPath(TESTING_BINARY_DIR, {"src"}, {"test"});
    char arg0[] = "testbamboo";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testim,bamboo";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=5,bamboo=5");
    Instance instance(FCITX_ARRAY_SIZE(argv), argv);
    instance.addonManager().registerDefaultLoader(nullptr);
    instance.eventDispatcher().schedule([&instance]() {
        setup(&instance);
        testPreedit(&instance);
        testRestoreKeyStroke(&instance);
        testLockKeysKeepWord(&instance);
        testBrokenCustomKeymap(&instance);
        testSpellCheckAction(&instance);
        testInputModes(&instance);
        testInputModePicker(&instance);
        testFieldHints(&instance);
        testModeLabel(&instance);
        testTerminalEscape(&instance);
        testSpellCheckExceptions(&instance);
        testImportMacros(&instance);
        testEditWordBeforeCursor(&instance);
        testStandaloneW(&instance);
        testQuickTyping(&instance);
        testCapitalizeSentences(&instance);
        instance.eventDispatcher().detach();
        instance.exit();
    });
    instance.exec();
    return 0;
}
