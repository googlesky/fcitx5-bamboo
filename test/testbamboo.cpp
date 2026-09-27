/*
 * SPDX-FileCopyrightText: 2026 googlesky
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#include <cstdint>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
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
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/userinterfacemanager.h>
#include <string>
#include <string_view>
#include <vector>

using namespace fcitx;

namespace {

// Stands in for an application with the cursor at the end of its text:
// applies what fcitx sends, handles the keys fcitx lets through and reports
// its text as surrounding text, unless told not to like a Wayland terminal.
class FakeEditor : public InputContext {
public:
    FakeEditor(Instance *instance, const std::string &program,
               CapabilityFlags caps, bool reportSurrounding = true)
        : InputContext(instance->inputContextManager(), program),
          reportSurrounding_(reportSurrounding) {
        created();
        setCapabilityFlags(caps);
        syncSurrounding();
        focusIn();
        instance->setCurrentInputMethod(this, "bamboo", true);
    }
    ~FakeEditor() override { destroy(); }

    const char *frontend() const override { return "bambootest"; }

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
            if (auto chr = Key::keySymToUnicode(key.sym())) {
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
        instance.eventDispatcher().detach();
        instance.exit();
    });
    instance.exec();
    return 0;
}
