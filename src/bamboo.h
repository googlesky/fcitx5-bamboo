/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */
#ifndef _FCITX5_BAMBOO_BAMBOO_H_
#define _FCITX5_BAMBOO_BAMBOO_H_

#include "bamboo-core.h"
#include "bambooconfig.h"
#include <cstdint>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/signals.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fcitx {

// Owns a cgo handle. 0 is never a valid handle, the Go side returns it on
// failure.
class CGoObject {
public:
    CGoObject(uintptr_t handle = 0) : handle_(handle) {}
    ~CGoObject() { reset(); }
    CGoObject(const CGoObject &other) = delete;
    CGoObject &operator=(const CGoObject &other) = delete;

    void reset(uintptr_t handle = 0) {
        if (handle_) {
            DeleteObject(handle_);
        }
        handle_ = handle;
    }

    uintptr_t handle() const { return handle_; }

    explicit operator bool() const { return handle_ != 0; }

private:
    uintptr_t handle_;
};

class BambooState;

class BambooEngine final : public InputMethodEngineV2 {
public:
    BambooEngine(Instance *instance);

    void activate(const InputMethodEntry &entry,
                  InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry,
                    fcitx::InputContextEvent &event) override;
    void keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) override;
    void reset(const InputMethodEntry &entry,
               InputContextEvent &event) override;

    const auto &config() const { return config_; }
    const auto &customKeymap() const { return customKeymap_; }
    // The mode of the program of ic, else of its kind, else the default.
    BambooInputMode inputMode(InputContext *ic) const;
    // Terminals and code editors, where Escape switches to English.
    bool isTerminal(const InputContext *ic) const;
    // Qt terminals, which take DEL characters for BackSpace: Konsole
    // reports no text.
    bool isQtTerminal(InputContext *ic) const;
    // Remembers the typing mode of the program of ic.
    void setInputMode(InputContext *ic, BambooInputMode mode);

    Instance *instance() { return instance_; }
    FCITX_ADDON_DEPENDENCY_LOADER(clipboard, instance_->addonManager());
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());
    // Tells, once for the program of ic, of the typing modes that do not
    // wait for the reports it gives late.
    void suggestModesNotWaiting(InputContext *ic);

    void reloadConfig() override;
    const Configuration *getConfig() const override { return &config_; }

    const Configuration *getSubConfig(const std::string &path) const override;

    void setConfig(const RawConfig &config) override;

    void setSubConfig(const std::string &path,
                      const RawConfig &config) override;
    std::string subMode(const fcitx::InputMethodEntry &entry,
                        fcitx::InputContext &inputContext) override;
    std::string subModeLabelImpl(const InputMethodEntry &entry,
                                 InputContext &inputContext) override;

    uintptr_t dictionary() { return dictionary_.handle(); }
    uintptr_t macroTable() {
        return macroTableObject_[*config_.inputMethod].handle();
    }

    void refreshEngine();
    void refreshOption();
    void saveConfig() { safeSaveAsIni(config_, "conf/bamboo.conf"); }
    void updateSpellAction(InputContext *ic);
    void updateMacroAction(InputContext *ic);
    void updateInputMethodAction(InputContext *ic);
    void updateCharsetAction(InputContext *ic);

    void populateConfig();

private:
    const BambooAppMode *appMode(const std::string &program) const;

    Instance *instance_;
    BambooConfig config_;
    BambooCustomKeymap customKeymap_;
    BambooAppModeList appModes_;
    std::unordered_map<std::string, BambooMacroTable> macroTables_;
    std::unordered_map<std::string, CGoObject> macroTableObject_;
    FactoryFor<BambooState> factory_;
    std::vector<std::string> imNames_;
    std::unique_ptr<SimpleAction> inputMethodAction_;
    std::vector<std::unique_ptr<SimpleAction>> inputMethodSubAction_;
    std::unique_ptr<Menu> inputMethodMenu_;
    std::unique_ptr<SimpleAction> charsetAction_;
    std::vector<std::unique_ptr<SimpleAction>> charsetSubAction_;
    std::unique_ptr<Menu> charsetMenu_;
    std::unique_ptr<SimpleAction> spellCheckAction_;
    std::unique_ptr<SimpleAction> macroAction_;
    std::vector<ScopedConnection> connections_;
    std::vector<std::unique_ptr<HandlerTableEntry<EventHandler>>>
        eventWatchers_;
    CGoObject dictionary_;
    std::unordered_set<std::string> suggestedPrograms_;
};

class BambooFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        registerDomain("fcitx5-bamboo", FCITX_INSTALL_LOCALEDIR);
        return new BambooEngine(manager->instance());
    }
};
} // namespace fcitx

#endif // _FCITX5_BAMBOO_BAMBOO_H_
