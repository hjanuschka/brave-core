// Copyright (c) 2025 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/ai_chat/browser_tool_provider_factory.h"

#include <memory>
#include <vector>

#include "brave/browser/ai_chat/page_content_blocks.h"
#include "brave/browser/ai_chat/tools/click_tool.h"
#include "brave/browser/ai_chat/tools/drag_and_release_tool.h"
#include "brave/browser/ai_chat/tools/history_tool.h"
#include "brave/browser/ai_chat/tools/move_mouse_tool.h"
#include "brave/browser/ai_chat/tools/navigation_tool.h"
#include "brave/browser/ai_chat/tools/scroll_tool.h"
#include "brave/browser/ai_chat/tools/select_tool.h"
#include "brave/browser/ai_chat/tools/type_tool.h"
#include "brave/browser/ai_chat/tools/wait_tool.h"
#include "brave/components/ai_chat/core/browser/tools/tool.h"
#include "brave/components/ai_chat/core/browser/tools/tool_provider.h"
#include "brave/components/ai_chat/core/browser/tools/tool_utils.h"
#include "brave/components/ai_chat/core/common/mojom/ai_chat.mojom.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/task_id.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "components/optimization_guide/content/browser/page_content_proto_provider.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"

namespace ai_chat {

namespace {

// ** Copied from chrome/browser/actor/actor_keyed_service.cc **
// TODO(crbug.com/411462297): This is a short term hack. This code will be
// deleted soon once StartTask stops creating new tabs implicitly. This adds a
// 1-second delay to wait for about:blank to load. This can be replaced by ~100
// lines of complex code that tries to precisely wait for navigation commit, but
// that would be overkill.
constexpr base::TimeDelta kDelayForNewTab = base::Seconds(1);

// Implementation of ToolProvider that provides browser-specific
// tools for conversations.
// It is responsible for grouping browser action tasks (a set of tabs)
// that the tools for a conversation perform actions on.
class BrowserToolProvider : public ToolProvider,
                            public BrowserToolTaskProvider {
 public:
  explicit BrowserToolProvider(Profile* profile,
                               raw_ptr<actor::ActorKeyedService> actor_service)
      : profile_(profile), actor_service_(actor_service) {
    // Only create page actor tools if this profile is allowed to.
    if (actor_service_) {
      // Each conversation can have a different actor service task,
      // and operate on a different set of tabs.
      // If we want to delay creation of the task, we'll need to perhaps
      // intercept all tool use calls and create or choose which task to use
      // at that time. Tool::UseTool will have to change to
      // ToolProvider::UseTool, or similar.
      // TODO(cr140): actor_service_->CreateTask() and we don't need to specify
      // tab at this point.
      optimization_guide::proto::BrowserStartTask task;
      task_id_ = actor_service_->CreateTask();
    }

    ~BrowserToolProvider() override = default;

    BrowserToolProvider(const BrowserToolProvider&) = delete;
    BrowserToolProvider& operator=(const BrowserToolProvider&) = delete;

    // ToolProvider implementation
    std::vector<Tool*> GetTools() override {
      std::vector<Tool*> tool_ptrs;
      tool_ptrs.reserve(tools_.size());
      for (const auto& tool : tools_) {
        tool_ptrs.push_back(tool.get());
      }
      return tool_ptrs;
    }

    mojom::ConversationCapability GetConversationCapability() override {
      return actor_service_ ? mojom::ConversationCapability::CONTENT_AGENT
                            : mojom::ConversationCapability::CHAT;
    }

    void StopAllTasks() override {
      actor_service_->StopTask(task_id_);
    }

    // BrowserToolTaskProvider implementation
    actor::TaskId GetTaskId() override {
      return task_id_;
    }

    void GetOrCreateTabHandleForTask(
        base::OnceCallback<void(tabs::TabHandle)> callback) override {
      if (!task_tab_handle_.Get()) {
        // Get the most recently active browser for this profile.
        Browser* browser = chrome::FindTabbedBrowser(
            profile_, /*match_original_profiles=*/false);
        // If no browser exists create one.
        if (!browser) {
          browser = Browser::Create(
              Browser::CreateParams(profile_, /*user_gesture=*/false));
        }
        // Create a new tab because we are only allowed to act on
        // certain URLs. Safer to start on a blank page.
        content::WebContents* new_contents = chrome::AddAndReturnTabAt(
            browser, GURL(url::kAboutBlankURL), -1, false);
        // We are opening in the background (and previewing in the conversation)
        // so ensure it's loaded.
        new_contents->GetController().LoadIfNecessary();

        task_tab_handle_ =
            tabs::TabInterface::GetFromContents(new_contents)->GetHandle();
        for (auto& observer : observers_) {
          observer.OnContentTaskStarted(task_tab_handle_);
        }
      }
      // TODO(cr140): This can be removed once task tab creation is more
      // graceful in actor_keyed_service.cc.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&BrowserToolProvider::FinishGetTabHandleForTask,
                         weak_ptr_factory_.GetWeakPtr(), std::move(callback)),
          kDelayForNewTab);
    }

    void ExecuteActions(optimization_guide::proto::Actions actions,
                        Tool::UseToolCallback callback) override {
      task_tab_handle_.Get()->GetContents()->UpdateWebContentsVisibility(
          content::Visibility::VISIBLE);
      actor_service_->PerformActions(
          std::move(actions),
          base::BindOnce(&BrowserToolProvider::OnActionsFinished,
                         weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
    }

   private:
    void CreateTools() {
      tools_.clear();
      // TODO(petemill): Construct some tools and add them to `tools_`
      // Tab management tool is independent of actor_service

      if (actor_service_) {
        // TaskId is consistent for this conversation. But we might
        // want to make this more dynamic and have the the tool use request
        // specify which task (set of tabs) to act on.
        tools_.push_back(std::make_unique<ClickTool>(this, actor_service_));
        tools_.push_back(
            std::make_unique<DragAndReleaseTool>(this, actor_service_));
        tools_.push_back(std::make_unique<HistoryTool>(this, actor_service_));
        tools_.push_back(std::make_unique<MoveMouseTool>(this, actor_service_));
        tools_.push_back(
            std::make_unique<NavigationTool>(this, actor_service_));
        tools_.push_back(std::make_unique<ScrollTool>(this, actor_service_));
        tools_.push_back(std::make_unique<SelectTool>(this, actor_service_));
        tools_.push_back(std::make_unique<TypeTool>(this, actor_service_));
        tools_.push_back(std::make_unique<WaitTool>(this, actor_service_));
      }
    }

    void FinishGetTabHandleForTask(
        base::OnceCallback<void(tabs::TabHandle)> callback) {
      std::move(callback).Run(task_tab_handle_);
    }

    void OnActionsFinished(Tool::UseToolCallback callback,
                           optimization_guide::proto::ActionsResult result) {
      if (result.action_result() ==
          static_cast<int32_t>(actor::mojom::ActionResultCode::kOk)) {
        // TODO(cr140): Use multi_source_page_context_fetcher.h (or use it
        // via ActorKeyedService). For now we have to call into glic.
        // Or we can avoid glic via:
        // - Annotated page content from `optimization_guide::GetAIPageContent`
        // - Screenshot from our own implementation
        // glic::mojom::GetTabContextOptions options;
        // options.include_annotated_page_content = true;
        // options.include_viewport_screenshot = true;

        // Get page content
        auto options = blink::mojom::AIPageContentOptions::New();
        options->mode = blink::mojom::AIPageContentMode::kActionableElements;

        optimization_guide::GetAIPageContent(
            task_tab_handle_.Get()->GetContents(), std::move(options),
            base::BindOnce(&BrowserToolProvider::ReceivedAnnotatedPageContent,
                           weak_ptr_factory_.GetWeakPtr(),
                           std::move(callback)));
      } else {
        DLOG(ERROR)
            << "Action failed, see actor.mojom for result code meaning: "
            << result.action_result();
        std::move(callback).Run(CreateContentBlocksForText("Action failed"));
      }
    }

    void ReceivedAnnotatedPageContent(
        Tool::UseToolCallback callback,
        std::optional<optimization_guide::AIPageContentResult> content) {
      if (!content.has_value()) {
        LOG(ERROR) << "Error getting page content";
        std::move(callback).Run(
            CreateContentBlocksForText("Error getting page content"));
        return;
      }

      auto apc = content->proto;

      if (!apc.has_root_node()) {
        LOG(ERROR) << "No root node";
        std::move(callback).Run(CreateContentBlocksForText("No root node"));
        return;
      }

      auto content_blocks = ConvertAnnotatedPageContentToBlocks(apc);
      content_blocks.insert(
          content_blocks.begin(),
          std::move(CreateContentBlocksForText("Action successful")[0]));
      std::move(callback).Run(std::move(content_blocks));
    }

    // Browser-specific tools owned by this provider
    std::vector<std::unique_ptr<Tool>> tools_;
    actor::TaskId task_id_;
    tabs::TabHandle task_tab_handle_;
    raw_ptr<actor::ActorKeyedService> actor_service_ = nullptr;

    base::WeakPtrFactory<BrowserToolProvider> weak_ptr_factory_{this};
  };

}  // namespace

// BrowserToolProviderFactory implementation

BrowserToolProviderFactory::BrowserToolProviderFactory(
    Profile* profile,
    actor::ActorKeyedService* actor_service)
    : profile_(profile), actor_service_(actor_service) {
}

BrowserToolProviderFactory::~BrowserToolProviderFactory() = default;

std::unique_ptr<ToolProvider> BrowserToolProviderFactory::CreateToolProvider() {
  return std::make_unique<BrowserToolProvider>(profile_, actor_service_.get());
}

}  // namespace ai_chat
