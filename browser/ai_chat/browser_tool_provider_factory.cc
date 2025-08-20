// Copyright (c) 2025 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/ai_chat/browser_tool_provider_factory.h"

#include <memory>
#include <vector>

#include "brave/components/ai_chat/core/browser/tools/tool.h"
#include "brave/components/ai_chat/core/browser/tools/tool_provider.h"
#include "brave/components/ai_chat/core/common/buildflags/buildflags.h"
#include "chrome/browser/profiles/profile.h"

#if BUILDFLAG(ENABLE_TAB_MANAGEMENT_TOOL)
#include "brave/browser/ai_chat/tools/tab_management_tool.h"
#endif

namespace ai_chat {

namespace {

// Implementation of ToolProvider that provides browser-specific
// tools for conversations.
// It is responsible for grouping browser action tasks (a set of tabs)
// that the tools for a conversation perform actions on.
class BrowserToolProvider : public ToolProvider {
 public:
  explicit BrowserToolProvider(Profile* profile) : profile_(profile) {
    CreateTools();
  }

  ~BrowserToolProvider() override = default;

  BrowserToolProvider(const BrowserToolProvider&) = delete;
  BrowserToolProvider& operator=(const BrowserToolProvider&) = delete;

  // ToolProvider implementation
  std::vector<Tool*> GetTools() override {
    std::vector<Tool*> tool_ptrs;
#if BUILDFLAG(ENABLE_TAB_MANAGEMENT_TOOL)
    tool_ptrs.push_back(tab_management_tool_.get());
#endif
    return tool_ptrs;
  }

 private:
  void CreateTools() {
#if BUILDFLAG(ENABLE_TAB_MANAGEMENT_TOOL)
    tab_management_tool_ = std::make_unique<TabManagementTool>(profile_);
#endif
  }

  void OnNewGenerationLoop() override {
    // We don't clear tools because all our tools have state that is ok
    // to persist across generations:
    // - Tab Management Tool wants its permission to be persistent for a single
    //   whole conversation and not be reset for each message generation.
  }

  // Browser-specific tools owned by this provider
#if BUILDFLAG(ENABLE_TAB_MANAGEMENT_TOOL)
  std::unique_ptr<TabManagementTool> tab_management_tool_ = nullptr;
#endif
  raw_ptr<Profile> profile_ = nullptr;
};

}  // namespace

// BrowserToolProviderFactory implementation

BrowserToolProviderFactory::BrowserToolProviderFactory(Profile* profile)
    : profile_(profile) {}

BrowserToolProviderFactory::~BrowserToolProviderFactory() = default;

std::unique_ptr<ToolProvider> BrowserToolProviderFactory::CreateToolProvider() {
  return std::make_unique<BrowserToolProvider>(profile_);
}

}  // namespace ai_chat
