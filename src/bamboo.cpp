/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

#include "bamboo.h"
#include "bambooconfig.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/charutils.h>
#include <fcitx-utils/environ.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/action.h>
#include <fcitx/addoninstance.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/menu.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fcitx {

namespace {

constexpr std::string_view MacroPrefix = "macro/";
constexpr std::string_view InputMethodActionPrefix = "bamboo-input-method-";
constexpr std::string_view CharsetActionPrefix = "bamboo-charset-";
const std::string CustomKeymapFile = "conf/bamboo-custom-keymap.conf";
const std::string AppModeFile = "conf/bamboo-app-mode.conf";

FCITX_DEFINE_LOG_CATEGORY(bamboo, "bamboo");

std::string macroFile(std::string_view imName) {
    return stringutils::concat("conf/bamboo-macro-", imName, ".conf");
}

uintptr_t newMacroTable(const BambooMacroTable &macroTable) {
    std::vector<char *> charArray;
    for (const auto &keymap : *macroTable.macros) {
        charArray.push_back(const_cast<char *>(keymap.key->data()));
        charArray.push_back(const_cast<char *>(keymap.value->data()));
    }
    charArray.push_back(nullptr);
    return NewMacroTable(charArray.data());
}

// Macro files of ibus-bamboo and fcitx5-unikey that exist.
std::vector<std::filesystem::path> macroImportFiles() {
    std::vector<std::filesystem::path> files;
    if (auto home = getEnvironment("HOME")) {
        files.push_back(std::filesystem::path(*home) /
                        ".config/ibus-bamboo/ibus-bamboo.macro.text");
    }
    if (const auto &dir =
            StandardPaths::global().userDirectory(StandardPathsType::PkgConfig);
        !dir.empty()) {
        files.push_back(dir / "unikey/macro");
    }
    std::erase_if(files, [](const std::filesystem::path &file) {
        std::error_code ec;
        return !std::filesystem::is_regular_file(file, ec);
    });
    return files;
}

// Normalization drops Shift from "~", accept keys saved either way.
bool checkHotkey(const KeyEvent &keyEvent, const KeyList &keys) {
    return keyEvent.key().checkKeyList(keys) ||
           keyEvent.rawKey().checkKeyList(keys);
}

class InputModeCandidateWord : public CandidateWord {
public:
    InputModeCandidateWord(BambooEngine *engine, BambooInputMode mode)
        : CandidateWord(Text(BambooInputModeI18NAnnotation::toString(mode))),
          engine_(engine), mode_(mode) {}

    void select(InputContext *inputContext) const override {
        engine_->setInputMode(inputContext, mode_);
    }

private:
    BambooEngine *engine_;
    BambooInputMode mode_;
};

// array is nullptr when the Go side recovered from a panic.
std::vector<std::string> convertToStringList(char **array) {
    std::vector<std::string> result;
    if (!array) {
        return result;
    }
    for (int i = 0; array[i]; i++) {
        result.push_back(array[i]);
        free(array[i]);
    }
    free(array);
    return result;
}

} // namespace

#define FCITX_BAMBOO_DEBUG() FCITX_LOGC(bamboo, Debug)
#define FCITX_BAMBOO_WARN() FCITX_LOGC(bamboo, Warn)

class BambooState final : public InputContextProperty {
public:
    BambooState(BambooEngine *engine, InputContext *ic)
        : engine_(engine), ic_(ic) {
        setEngine();
    }

    ~BambooState() {}

    void setEngine() {
        bambooEngine_.reset();

        if (*engine_->config().inputMethod == "Custom") {
            std::vector<char *> charArray;
            for (const auto &keymap : *engine_->customKeymap().customKeymap) {
                charArray.push_back(const_cast<char *>(keymap.key->data()));
                charArray.push_back(const_cast<char *>(keymap.value->data()));
            }
            charArray.push_back(nullptr);
            bambooEngine_.reset(NewCustomEngine(charArray.data(),
                                                engine_->dictionary(),
                                                engine_->macroTable()));
        } else {
            bambooEngine_.reset(NewEngine(engine_->config().inputMethod->data(),
                                          engine_->dictionary(),
                                          engine_->macroTable()));
        }
        if (!bambooEngine_) {
            FCITX_BAMBOO_WARN() << "Failed to create engine for input method "
                                << *engine_->config().inputMethod;
        }
        setOption();
    }

    void setOption() {
        if (!bambooEngine_) {
            return;
        }
        std::vector<char *> exceptions;
        for (const auto &word : *engine_->config().spellCheckExceptions) {
            exceptions.push_back(const_cast<char *>(word.data()));
        }
        exceptions.push_back(nullptr);
        FcitxBambooEngineOption option = {
            .autoNonVnRestore = *engine_->config().autoNonVnRestore,
            .ddFreeStyle = true,
            .macroEnabled = *engine_->config().macro,
            .autoCapitalizeMacro = *engine_->config().capitalizeMacro,
            .spellCheckWithDicts = *engine_->config().spellCheck,
            .outputCharset = engine_->config().outputCharset->data(),
            .modernStyle = *engine_->config().modernStyle,
            .freeMarking = *engine_->config().freeMarking,
            .spellCheckExceptions = exceptions.data(),
            .standaloneW =
                static_cast<int>(*engine_->config().quickTyping->standaloneW),
            .quickDouble = *engine_->config().quickTyping->doubleConsonants,
            .quickStart = *engine_->config().quickTyping->startConsonants,
            .quickEnd = *engine_->config().quickTyping->endConsonants,
        };
        EngineSetOption(bambooEngine_.handle(), &option);
    }

    // The mode keys are handled in for this input context right now.
    BambooInputMode effectiveMode() const {
        const auto mode = engine_->inputMode(ic_->program());
        // Addresses and numbers are never Vietnamese. URL fields are left
        // alone: browsers' address bars are searched in Vietnamese.
        if (mode == BambooInputMode::Exclude ||
            (*engine_->config().autoExcludeFields &&
             ic_->capabilityFlags().testAny(CapabilityFlags{
                 CapabilityFlag::Email, CapabilityFlag::Digit,
                 CapabilityFlag::Number, CapabilityFlag::Dialable}))) {
            return BambooInputMode::Exclude;
        }
        // Deleting blindly would corrupt text: Wayland frontends claim the
        // capability for clients that send no surrounding text.
        const auto &surroundingText = ic_->surroundingText();
        if (mode == BambooInputMode::SurroundingText &&
            (!ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) ||
             !surroundingText.isValid() ||
             surroundingText.cursor() != surroundingText.anchor())) {
            return BambooInputMode::Preedit;
        }
        return mode;
    }

    void keyEvent(KeyEvent &keyEvent) {
        // Ignore all key release.
        if (!bambooEngine_ || keyEvent.isRelease()) {
            return;
        }
        const bool restoreKey =
            keyEvent.key().checkKeyList(*engine_->config().restoreKeyStroke);
        // Like ibus-bamboo, a lone Shift or CapsLock must not end the word.
        const auto sym = keyEvent.rawKey().sym();
        if (!restoreKey &&
            (sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R ||
             sym == FcitxKey_Caps_Lock)) {
            return;
        }
        // Typing fast, the application may report its text late: the word
        // before the cursor is only trusted when reported after the last key,
        // which the application handled itself (BackSpace, arrows).
        const bool editWord = surroundingFresh_ && lastKeyToApp_;
        surroundingFresh_ = false;
        handleKey(keyEvent, restoreKey, editWord);
        lastKeyToApp_ = !keyEvent.filtered();
    }

    void surroundingTextUpdated() { surroundingFresh_ = true; }

    void handleKey(KeyEvent &keyEvent, bool restoreKey, bool editWord) {
        const auto sym = keyEvent.rawKey().sym();
        if (pickerOpen_ && pickerKeyEvent(keyEvent)) {
            return;
        }
        if (checkHotkey(keyEvent, *engine_->config().inputModeSwitchKey) &&
            !ic_->program().empty() &&
            !EngineIsTypingKey(bambooEngine_.handle(), sym,
                               keyEvent.rawKey().states())) {
            openPicker();
            keyEvent.filterAndAccept();
            return;
        }
        const auto mode = effectiveMode();
        // A word ends in the mode it started in.
        if (mode != lastMode_) {
            commitBuffer();
            lastMode_ = mode;
            ic_->updateUserInterface(UserInterfaceComponent::StatusArea);
        }
        if (mode == BambooInputMode::Exclude) {
            return;
        }
        // Like VNIKey's vim mode: normal mode commands need plain keys.
        if (keyEvent.key().check(FcitxKey_Escape) && engine_->isTerminal(ic_)) {
            commitBuffer();
            // Deactivating re-enters this state, process nothing after it.
            engine_->instance()->deactivate();
            return;
        }
        const bool surrounding = mode == BambooInputMode::SurroundingText;
        // The application changed the word (autocorrection, stale surrounding
        // text): start a new word rather than delete what is not ours.
        if (surrounding && !surroundingInSync()) {
            ResetEngine(bambooEngine_.handle());
        }

        if (restoreKey) {
            // With nothing to restore the key belongs to the application.
            if (EngineRestoreKeyStrokes(bambooEngine_.handle(), surrounding)) {
                keyEvent.filterAndAccept();
                flush();
            }
            return;
        }

        // Wayland frontends answer from a copy of the text that may lag.
        if (editWord && *engine_->config().editWordBeforeCursor &&
            !ic_->frontendName().starts_with("wayland") &&
            ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
            ic_->surroundingText().cursor() ==
                ic_->surroundingText().anchor()) {
            EngineEditWord(bambooEngine_.handle(),
                           std::string(textBeforeCursor()).c_str(), sym,
                           keyEvent.rawKey().states(), surrounding);
        }
        if (EngineProcessKeyEvent(bambooEngine_.handle(), sym,
                                  keyEvent.rawKey().states(), surrounding)) {
            keyEvent.filterAndAccept();
        }
        flush();
    }

    // Applies the engine output in order: deletion, commit, preedit.
    void flush() {
        if (const int count = EnginePullDeleteCount(bambooEngine_.handle());
            count > 0) {
            ic_->deleteSurroundingText(-count, count);
        }
        if (char *commit = EnginePullCommit(bambooEngine_.handle())) {
            if (commit[0]) {
                ic_->commitString(commit);
            }
            free(commit);
        }

        ic_->inputPanel().reset();
        UniqueCPtr<char> preedit(EnginePullPreedit(bambooEngine_.handle()));
        if (preedit && preedit.get()[0]) {
            std::string_view preeditView = preedit.get();
            Text text;
            TextFormatFlags format;
            if (ic_->capabilityFlags().test(CapabilityFlag::Preedit) &&
                *engine_->config().displayUnderline) {
                format = TextFormatFlag::Underline;
            }
            if (utf8::validate(preeditView)) {
                text.append(std::string(preeditView), format);
            }
            text.setCursor(text.textLength());

            if (ic_->capabilityFlags().test(CapabilityFlag::Preedit)) {
                ic_->inputPanel().setClientPreedit(text);
            } else {
                ic_->inputPanel().setPreedit(text);
            }
        }
        ic_->updatePreedit();
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void reset() {
        // A click moved the cursor, or focus came back.
        lastKeyToApp_ = true;
        pickerOpen_ = false;
        ic_->inputPanel().reset();
        if (bambooEngine_) {
            ResetEngine(bambooEngine_.handle());
        }
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic_->updatePreedit();
    }

    void commitBuffer() {
        pickerOpen_ = false;
        ic_->inputPanel().reset();
        if (bambooEngine_) {
            // The reason that we do not commit here is we want to force the
            // behavior. When client get unfocused, the framework will try to
            // commit the string.
            EngineCommitPreedit(bambooEngine_.handle());
            UniqueCPtr<char> commit(EnginePullCommit(bambooEngine_.handle()));
            if (commit && commit.get()[0]) {
                ic_->commitString(commit.get());
            }
        }
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic_->updatePreedit();
    }

    // ibus-bamboo's Shift+~ table choosing the typing mode of the program.
    void openPicker() {
        commitBuffer();
        auto candidates = std::make_unique<CommonCandidateList>();
        candidates->setLayoutHint(CandidateLayoutHint::Vertical);
        const auto current = engine_->inputMode(ic_->program());
        std::vector<std::string> labels;
        for (auto mode :
             {BambooInputMode::Preedit, BambooInputMode::SurroundingText,
              BambooInputMode::Exclude}) {
            labels.push_back(mode == current
                                 ? "*. "
                                 : std::to_string(labels.size() + 1) + ". ");
            candidates->append<InputModeCandidateWord>(engine_, mode);
        }
        candidates->setLabels(labels);
        candidates->setCursorIndex(static_cast<int>(current));
        ic_->inputPanel().setAuxUp(Text(
            stringutils::concat(_("Typing mode for"), " ", ic_->program())));
        ic_->inputPanel().setCandidateList(std::move(candidates));
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        pickerOpen_ = true;
    }

    void closePicker() {
        pickerOpen_ = false;
        ic_->inputPanel().reset();
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

private:
    // Empty when the application reports no text.
    std::string_view textBeforeCursor() const {
        const auto &surroundingText = ic_->surroundingText();
        const auto &text = surroundingText.text();
        const auto length = utf8::lengthValidated(text);
        if (!surroundingText.isValid() || length == utf8::INVALID_LENGTH ||
            surroundingText.cursor() > length) {
            return {};
        }
        return std::string_view(text).substr(
            0, utf8::ncharByteLength(text.begin(), surroundingText.cursor()));
    }

    bool surroundingInSync() const {
        UniqueCPtr<char> word(EngineSurroundingWord(bambooEngine_.handle()));
        return !word || !word.get()[0] ||
               textBeforeCursor().ends_with(word.get());
    }

    // Returns false when the key should go on as normal typing.
    bool pickerKeyEvent(KeyEvent &keyEvent) {
        auto candidates = ic_->inputPanel().candidateList();
        const auto &key = keyEvent.key();
        if (!candidates ||
            checkHotkey(keyEvent, *engine_->config().inputModeSwitchKey)) {
            // Like ibus-bamboo, pressed twice the key reaches the application.
            closePicker();
            return candidates != nullptr;
        }
        int index = key.digitSelection();
        if (key.check(FcitxKey_Return) || key.check(FcitxKey_KP_Enter)) {
            index = candidates->cursorIndex();
        }
        if (index >= 0 && index < candidates->size()) {
            keyEvent.filterAndAccept();
            candidates->candidate(index).select(ic_);
            return true;
        }
        const bool up = key.check(FcitxKey_Up) || key.check(FcitxKey_Left);
        if (up || key.check(FcitxKey_Down) || key.check(FcitxKey_Right)) {
            auto *movable = candidates->toCursorMovable();
            up ? movable->prevCandidate() : movable->nextCandidate();
            ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
            keyEvent.filterAndAccept();
            return true;
        }
        closePicker();
        if (key.check(FcitxKey_Escape)) {
            keyEvent.filterAndAccept();
            return true;
        }
        return false;
    }

    BambooEngine *engine_;
    InputContext *ic_;
    CGoObject bambooEngine_;
    bool pickerOpen_ = false;
    BambooInputMode lastMode_ = BambooInputMode::Preedit;
    bool surroundingFresh_ = false;
    bool lastKeyToApp_ = true;
};

BambooEngine::BambooEngine(Instance *instance)
    : instance_(instance), factory_([this](InputContext &ic) {
          return new BambooState(this, &ic);
      }) {
    Init();
    {
        auto imNames = convertToStringList(GetInputMethodNames());
        imNames.push_back("Custom");
        imNames_ = std::move(imNames);
    }
    if (std::find(imNames_.begin(), imNames_.end(), "Telex") ==
        imNames_.end()) {
        throw std::runtime_error("Failed to find required input method Telex");
    }
    FCITX_BAMBOO_DEBUG() << "Supported input methods: " << imNames_;
    config_.inputMethod.annotation().setList(imNames_);

    auto fd = StandardPaths::global().open(StandardPathsType::PkgData,
                                           "bamboo/vietnamese.cm.dict");
    if (!fd.isValid()) {
        throw std::runtime_error("Failed to load dictionary");
    }
    dictionary_.reset(NewDictionary(fd.release()));

    auto &uiManager = instance_->userInterfaceManager();
    inputMethodAction_ = std::make_unique<SimpleAction>();
    inputMethodAction_->setIcon("document-edit");
    inputMethodAction_->setShortText(_("Input Method"));
    uiManager.registerAction("bamboo-input-method", inputMethodAction_.get());

    inputMethodMenu_ = std::make_unique<Menu>();
    inputMethodAction_->setMenu(inputMethodMenu_.get());
    for (const auto &imName : imNames_) {
        inputMethodSubAction_.emplace_back(std::make_unique<SimpleAction>());
        auto *action = inputMethodSubAction_.back().get();
        action->setShortText(imName);
        action->setCheckable(true);
        uiManager.registerAction(
            stringutils::concat(InputMethodActionPrefix, imName), action);
        connections_.emplace_back(action->connect<SimpleAction::Activated>(
            [this, imName](InputContext *ic) {
                if (config_.inputMethod.value() == imName) {
                    return;
                }
                config_.inputMethod.setValue(imName);
                saveConfig();
                refreshEngine();
                updateInputMethodAction(ic);
            }));

        inputMethodMenu_->addAction(action);
    }

    charsetAction_ = std::make_unique<SimpleAction>();
    charsetAction_->setShortText(_("Output charset"));
    charsetAction_->setIcon("character-set");
    uiManager.registerAction("bamboo-charset", charsetAction_.get());
    charsetMenu_ = std::make_unique<Menu>();
    charsetAction_->setMenu(charsetMenu_.get());

    auto charsets = convertToStringList(GetCharsetNames());
    for (const auto &charset : charsets) {
        charsetSubAction_.emplace_back(std::make_unique<SimpleAction>());
        auto *action = charsetSubAction_.back().get();
        action->setShortText(charset);
        action->setCheckable(true);
        connections_.emplace_back(action->connect<SimpleAction::Activated>(
            [this, charset](InputContext *ic) {
                if (config_.outputCharset.value() == charset) {
                    return;
                }
                config_.outputCharset.setValue(charset);
                saveConfig();
                refreshEngine();
                updateCharsetAction(ic);
            }));
        uiManager.registerAction(
            stringutils::concat(CharsetActionPrefix, charset), action);
        charsetMenu_->addAction(action);
    }
    config_.outputCharset.annotation().setList(charsets);

    spellCheckAction_ = std::make_unique<SimpleAction>();
    spellCheckAction_->setLongText(_("Spell check"));
    spellCheckAction_->setIcon("tools-check-spelling");
    connections_.emplace_back(
        spellCheckAction_->connect<SimpleAction::Activated>(
            [this](InputContext *ic) {
                config_.autoNonVnRestore.setValue(!*config_.autoNonVnRestore);
                saveConfig();
                refreshOption();
                updateSpellAction(ic);
            }));
    uiManager.registerAction("bamboo-spell-check", spellCheckAction_.get());
    macroAction_ = std::make_unique<SimpleAction>();
    macroAction_->setLongText(_("Macro"));
    macroAction_->setIcon("edit-find");
    connections_.emplace_back(macroAction_->connect<SimpleAction::Activated>(
        [this](InputContext *ic) {
            config_.macro.setValue(!*config_.macro);
            saveConfig();
            refreshOption();
            updateMacroAction(ic);
        }));
    uiManager.registerAction("bamboo-macro", macroAction_.get());
    importMacroAction_ = std::make_unique<SimpleAction>();
    importMacroAction_->setShortText(_("Import ibus-bamboo/UniKey macros"));
    importMacroAction_->setIcon("document-import");
    connections_.emplace_back(
        importMacroAction_->connect<SimpleAction::Activated>(
            [this](InputContext *ic) { importMacros(ic); }));
    uiManager.registerAction("bamboo-import-macro", importMacroAction_.get());

    reloadConfig();
    instance_->inputContextManager().registerProperty("bambooState", &factory_);
    eventWatchers_.emplace_back(instance_->watchEvent(
        EventType::InputContextSurroundingTextUpdated,
        EventWatcherPhase::PostInputMethod, [this](Event &event) {
            static_cast<InputContextEvent &>(event)
                .inputContext()
                ->propertyFor(&factory_)
                ->surroundingTextUpdated();
        }));
}

void BambooEngine::reloadConfig() {
    readAsIni(config_, "conf/bamboo.conf");
    readAsIni(customKeymap_, CustomKeymapFile);
    readAsIni(appModes_, AppModeFile);
    for (const auto &imName : imNames_) {
        auto &table = macroTables_[imName];
        readAsIni(table, macroFile(imName));
        macroTableObject_[imName].reset(newMacroTable(table));
    }

    populateConfig();
}

const Configuration *BambooEngine::getSubConfig(const std::string &path) const {
    if (path == "custom_keymap") {
        return &customKeymap_;
    }
    if (path == "app_modes") {
        return &appModes_;
    }
    if (path.starts_with(MacroPrefix)) {
        const auto imName = path.substr(MacroPrefix.size());
        if (auto iter = macroTables_.find(imName); iter != macroTables_.end()) {
            return &iter->second;
        }
        return nullptr;
    }
    return nullptr;
}

void BambooEngine::setConfig(const RawConfig &config) {
    config_.load(config, true);
    saveConfig();
    populateConfig();
}

void BambooEngine::populateConfig() {
    refreshEngine();
    refreshOption();
    updateMacroAction(nullptr);
    updateSpellAction(nullptr);
    updateInputMethodAction(nullptr);
    updateCharsetAction(nullptr);
}

void BambooEngine::setSubConfig(const std::string &path,
                                const RawConfig &config) {
    if (path == "custom_keymap") {
        customKeymap_.load(config, true);
        safeSaveAsIni(customKeymap_, CustomKeymapFile);
        refreshEngine();
    } else if (path == "app_modes") {
        appModes_.load(config, true);
        safeSaveAsIni(appModes_, AppModeFile);
        refreshOption();
    } else if (path.starts_with(MacroPrefix)) {
        const auto imName = path.substr(MacroPrefix.size());
        if (auto iter = macroTables_.find(imName); iter != macroTables_.end()) {
            iter->second.load(config, true);
            safeSaveAsIni(iter->second, macroFile(imName));
            macroTableObject_[imName].reset(newMacroTable(iter->second));
            refreshEngine();
        }
    }
}

const BambooAppMode *BambooEngine::appMode(const std::string &program) const {
    if (program.empty()) {
        return nullptr;
    }
    const auto &appModes = *appModes_.appModes;
    auto iter = std::ranges::find_if(appModes, [&program](const auto &appMode) {
        return *appMode.program == program;
    });
    return iter == appModes.end() ? nullptr : &*iter;
}

void BambooEngine::importMacros(InputContext *ic) {
    const auto &imName = *config_.inputMethod;
    auto &table = macroTables_[imName];
    auto &macros = *table.macros.mutableValue();
    size_t imported = 0;
    for (const auto &file : macroImportFiles()) {
        const auto entries = convertToStringList(ReadMacroFile(file.c_str()));
        for (size_t i = 0; i + 1 < entries.size(); i += 2) {
            if (std::ranges::any_of(macros, [&](const BambooKeymap &macro) {
                    return *macro.key == entries[i];
                })) {
                continue;
            }
            auto &macro = macros.emplace_back();
            macro.key.setValue(entries[i]);
            macro.value.setValue(entries[i + 1]);
            imported++;
        }
    }
    if (imported) {
        safeSaveAsIni(table, macroFile(imName));
        macroTableObject_[imName].reset(newMacroTable(table));
        config_.macro.setValue(true);
        saveConfig();
        refreshEngine();
        updateMacroAction(ic);
    }
    instance_->showCustomInputMethodInformation(
        ic, stringutils::concat(_("Imported macros"), ": ", imported));
}

BambooInputMode BambooEngine::inputMode(const std::string &program) const {
    const auto *entry = appMode(program);
    return entry ? *entry->mode : *config_.inputMode;
}

bool BambooEngine::isTerminal(const InputContext *ic) const {
    const auto *entry = appMode(ic->program());
    return ic->capabilityFlags().test(CapabilityFlag::Terminal) ||
           (entry && *entry->terminal);
}

void BambooEngine::setInputMode(InputContext *ic, BambooInputMode mode) {
    auto &appModes = *appModes_.appModes.mutableValue();
    auto iter = std::ranges::find_if(appModes, [ic](const auto &appMode) {
        return *appMode.program == ic->program();
    });
    if (iter == appModes.end()) {
        iter = appModes.emplace(appModes.end());
        iter->program.setValue(ic->program());
    }
    iter->mode.setValue(mode);
    safeSaveAsIni(appModes_, AppModeFile);
    ic->propertyFor(&factory_)->closePicker();
    ic->updateUserInterface(UserInterfaceComponent::StatusArea);
}

std::string BambooEngine::subMode(const fcitx::InputMethodEntry & /*entry*/,
                                  fcitx::InputContext &inputContext) {
    const auto mode = inputContext.propertyFor(&factory_)->effectiveMode();
    if (mode == BambooInputMode::Preedit) {
        return *config_.inputMethod;
    }
    return stringutils::concat(*config_.inputMethod, " (",
                               BambooInputModeI18NAnnotation::toString(mode),
                               ")");
}

std::string BambooEngine::subModeLabelImpl(const InputMethodEntry & /*entry*/,
                                           InputContext &inputContext) {
    return inputContext.propertyFor(&factory_)->effectiveMode() ==
                   BambooInputMode::Exclude
               ? "EN"
               : "VI";
}

void BambooEngine::activate(const InputMethodEntry &entry,
                            InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto &statusArea = event.inputContext()->statusArea();

    updateMacroAction(event.inputContext());
    updateSpellAction(event.inputContext());
    updateInputMethodAction(event.inputContext());
    updateCharsetAction(event.inputContext());

    statusArea.addAction(StatusGroup::InputMethod, inputMethodAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, charsetAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, spellCheckAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, macroAction_.get());
    if (!macroImportFiles().empty()) {
        statusArea.addAction(StatusGroup::InputMethod,
                             importMacroAction_.get());
    }
}

void BambooEngine::deactivate(const InputMethodEntry &entry,
                              InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    if (event.type() != EventType::InputContextFocusOut) {
        state->commitBuffer();
    } else {
        state->reset();
    }
}

void BambooEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) {
    FCITX_UNUSED(entry);
    auto *state = keyEvent.inputContext()->propertyFor(&factory_);

    state->keyEvent(keyEvent);
}

void BambooEngine::reset(const InputMethodEntry &entry,
                         InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    state->reset();
}

void BambooEngine::refreshEngine() {
    FCITX_BAMBOO_DEBUG() << "Refresh engine";
    if (!factory_.registered()) {
        return;
    }

    instance_->inputContextManager().foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->setEngine();
        if (ic->hasFocus()) {
            state->reset();
        }
        return true;
    });
}

void BambooEngine::refreshOption() {
    if (!factory_.registered()) {
        return;
    }
    instance_->inputContextManager().foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->setOption();
        if (ic->hasFocus()) {
            state->reset();
        }
        return true;
    });
}

void BambooEngine::updateSpellAction(InputContext *ic) {
    spellCheckAction_->setChecked(*config_.autoNonVnRestore);
    spellCheckAction_->setShortText(*config_.autoNonVnRestore
                                        ? _("Spell Check Enabled")
                                        : _("Spell Check Disabled"));
    if (ic) {
        spellCheckAction_->update(ic);
    }
}

void BambooEngine::updateMacroAction(InputContext *ic) {
    macroAction_->setChecked(*config_.macro);
    macroAction_->setShortText(*config_.macro ? _("Macro Enabled")
                                              : _("Macro Disabled"));
    if (ic) {
        macroAction_->update(ic);
    }
}

void BambooEngine::updateInputMethodAction(InputContext *ic) {
    auto name =
        stringutils::concat(InputMethodActionPrefix, *config_.inputMethod);
    for (const auto &action : inputMethodSubAction_) {
        action->setChecked(action->name() == name);
        if (ic) {
            action->update(ic);
        }
    }
}

void BambooEngine::updateCharsetAction(InputContext *ic) {
    auto name =
        stringutils::concat(CharsetActionPrefix, *config_.outputCharset);
    for (const auto &action : charsetSubAction_) {
        action->setChecked(action->name() == name);
        if (ic) {
            action->update(ic);
        }
    }
}

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(bamboo, fcitx::BambooFactory)
