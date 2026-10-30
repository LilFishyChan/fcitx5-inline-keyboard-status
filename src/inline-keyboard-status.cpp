// Inline Keyboard Status —— fcitx5 UI addon
//
// 职责：把输入面板状态推给 DMS 的触摸键盘，并接受它的指令
// （按键注入、文本提交、取全量候选、选词）。
//
// 架构完全按 fcitx5 官方接口来：
//
//   1. 状态来源是 UserInterface::update()
//      UserInterfaceManager::flush() 会对「真正变了」的那个 InputContext
//      逐个 component 调 update()（见 src/lib/fcitx/userinterfacemanager.cpp
//      的 updateSingleComponent<> 与 flush()）。传进来的 ic 就是变化的那个，
//      不需要自己猜，也不需要轮询。
//
//   2. 键盘开合由 VirtualKeyboardUserInterface 的三个纯虚函数驱动
//      isVirtualKeyboardVisible / showVirtualKeyboard / hideVirtualKeyboard。
//
//   3. 全量候选用 BulkCandidateList（头文件原文 "useful for virtual keyboard"）：
//      totalSize() 优先；它返回 -1 表示实现没有明确总数，此时按头文件说明
//      「candidateFromAll 越界会抛异常、catch 它当结束」来遍历。
//
//   4. 输入法变化由事件驱动（InputContextSwitchInputMethod /
//      InputMethodGroupChanged），不轮询。
//
// 通道：$XDG_RUNTIME_DIR/inline-keyboard.sock，一行一条
//   命令（插件 -> addon）：key:<keysym>[:<mods>] / text:<utf8> / expand /
//                          select:<globalIndex> / mode:<osk|physical> / ui:<0|1>
//   推送（addon -> 插件）：state:<json> / allcand:<json> / imlist:<json> /
//                          error:<msg>
//
// 文件只作自检兜底（probe.sh 用），内容与推送一致：
//   inline-keyboard-status.json / inline-keyboard-allcand.json / inline-keyboard-imlist.json

#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>

#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "client-channel.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "addon-support.h"
#include "candidate-payload.h"
#include "keysym-map.h"

namespace {

// 一次全量取数的上限。只是防御实现异常时死循环，不是业务限制：
// 正常情况由 totalSize() 或越界异常结束。
constexpr int kMaxCandidateScan = 4000;

// 顶部候选给插件多少个。插件按宽度自己裁，这里多给一些留余量。
constexpr int kTopCandidateSlots = 18;

} // namespace

class InlineKeyboardStatus : public fcitx::VirtualKeyboardUserInterface {
public:
    explicit InlineKeyboardStatus(fcitx::Instance *instance) : instance_(instance) {
        const std::string base = inline_keyboard::runtimeDir() + "/inline-keyboard";
        statusPath_ = base + "-status.json";
        allCandPath_ = base + "-allcand.json";
        imListPath_ = base + "-imlist.json";
        logPath_ = base + "-addon.log";

        // 构造期只做不依赖 addon 完全就绪的事：开 socket、订事件。
        // 绝不在这里调 updateAvailability()（那时 addon 还没构造完）。
        setupSocket();
        watchInputMethodEvents();
    }

    // ---------------- fcitx5 UserInterface 接口 ----------------

    // 是否对 fcitx5 声明可用。false 时 classicui 接管候选窗。
    bool available() override { return uiAvailable_; }
    void suspend() override {}
    void resume() override {}

    // 状态推送的唯一入口。fcitx5 会对「发生变化的那个 InputContext」
    // 调这个方法（UserInterfaceManager::flush() -> updateDispatch）。
    void update(fcitx::UserInterfaceComponent component,
                fcitx::InputContext *ic) override {
        if (!ic) {
            return;
        }
        // InputPanel 是候选与预编辑，StatusArea 是输入法标签/图标。
        // 两者都走同一条状态推送，内容本来就一起给。
        if (component == fcitx::UserInterfaceComponent::InputPanel ||
            component == fcitx::UserInterfaceComponent::StatusArea) {
            pushState(ic);
        }
    }

    bool isVirtualKeyboardVisible() const override { return shown_; }

    // 由 fcitx5 在切 InputMethodMode 时自己调用，**不能在这里切模式** ——
    // 原因见 markShown()。
    void showVirtualKeyboard() override { markShown(true); }

    // fcitx5 在 InputContextFocusOut 时会调这里（instance.cpp 那段监听）。
    // 完全按官方语义处理：置 shown_ = false 并把 UI 交还 classicui。
    // 「失焦后把 UI 抢回来」这件事不在这里做 —— 那是 DMS 插件该管的
    // （它知道键盘是否真的还开着），addon 只管如实反映 fcitx5 的状态。
    void hideVirtualKeyboard() override { markShown(false); }

private:
    // ---------------- 键盘开合 ----------------

    // 收起后延迟交还 UI：窗口刚销毁时 InputPanel 的候选挂载点还在切换，
    // 立刻交还再打开会取不到候选。留一拍让窗口先消失。
    // 期间如果又开了（shown_ 变回 true），这次交还作废。
    void scheduleRestore() {
        restoreTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kRestoreDelayUs, 0,
            [this](fcitx::EventSourceTime *, uint64_t) {
                if (shown_) {
                    logEvent("restore cancelled shown=1");
                    return true;
                }
                instance_->setInputMethodMode(
                    fcitx::InputMethodMode::PhysicalKeyboard);
                instance_->userInterfaceManager().updateAvailability();
                logEvent("restore done");
                return true;
            });
    }

    // 键盘开合的单一入口，两个来源共用：
    //   · fcitx5 的 show/hideVirtualKeyboard 回调（官方路径）
    //   · socket 的连接/断开（插件从不发 mode:osk，只能这么感知）
    // 只记录开合事实，**绝不碰 InputMethodMode**：fcitx5 切模式时本来就会回调
    // show/hide，在这里再切就是「切模式 → 回调 → 再切模式」的回环 —— 早期版本
    // 每秒来回十几次，UI 在 addon 和 classicui 之间反复重选（候选挂载点跟着换，
    // 表现为按键打不进去、候选永远空），socket 也被顶断（实测 BrokenPipe）。
    // 模式统一由 ensureOnScreenMode() 在注入按键前保证，职责单一。
    void markShown(bool shown) {
        if (shown == shown_)
            return;
        shown_ = shown;
        logEvent(std::string("setShown shown=") + (shown ? "1" : "0"));

        if (!shown) {
            // 只记录，不交还。
            //
            // 键盘是 WlrLayer.Overlay 浮层，从不注册成 InputContext，所以
            // fcitx5 会因为**任何焦点变化**调 hideVirtualKeyboard —— 用户切到
            // 别的窗口、甚至只是在应用里点一下，都会被当成「键盘该收了」。
            // 以前这里排 restore，结果是键盘还开着、UI 已经被交还给 classicui，
            // 候选窗突然变回带序号的老样式，空格上的输入法标签也不再更新。
            //
            // UI 归属只由插件显式控制：打开时 reclaim，关闭时 release。
            return;
        }

        // 弹出时声明自己是当前输入法前端。
        // UIType 已经是 OnScreenKeyboard，配合 setInputMethodMode 让 fcitx5
        // 把 UI 判给 addon（classicui 随之 suspend，否则两个候选窗同时显示）。
        instance_->setInputMethodMode(fcitx::InputMethodMode::OnScreenKeyboard);
        writeImList();
        // 同步补推一次状态：update() 只在预编辑/候选变化时才来，
        // 键盘刚开、还没打字时一次都不会调，空格上的输入法标签就是空的。
        // 必须在这里同步取 ic —— 放进延迟回调里会在 IC 已销毁后再取快照，
        // 直接空指针跳转把 fcitx5 带崩。
        if (auto *ctx = activeContext())
            pushState(ctx);

        // 必须**延迟**再让 fcitx5 重选 UI：立刻调时 addon 状态还没稳定，
        // 实测不生效（原始实现也是这么写的，注释记着「立刻调实测不生效」）。
        // 少了这一步，模式虽然切到 OnScreenKeyboard，UI 却仍留在 classicui，
        // update() 一次都不会被调用 —— 表现为按键打得进去、候选永远空。
        // 返回值必须接住：addTimeEvent 给的是 unique_ptr，立刻析构的话
        // 回调永远不会跑（原始实现那条也没存，所以它其实从没生效过）。
        uiReselectTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kUiReselectDelayUs, 0,
            [this](fcitx::EventSourceTime *, uint64_t) {
                instance_->userInterfaceManager().updateAvailability();
                logEvent("ui reselect requested");
                return true;
            });
    }

    static constexpr uint64_t kRestoreDelayUs = 300000;
    // 切模式后 UI 重选的延迟。原始实现用 50ms，沿用。
    static constexpr uint64_t kUiReselectDelayUs = 50000;

    // ---------------- 输入法变化：事件驱动 ----------------

    void watchInputMethodEvents() {
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextSwitchInputMethod,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto &icEvent = static_cast<fcitx::InputContextEvent &>(event);
                if (auto *ic = icEvent.inputContext()) {
                    pushState(ic);
                }
            }));
        // 聚焦变化必须监听：窗口切换时 fcitx5 不会为新的 context 调 update()
        // （那边只在 InputPanel 真变了时才发），空格上的输入法状态就会停在
        // 上一个窗口。官方的 virtualkeyboard 也是靠 Activated/Deactivated
        // 这类事件同步的。
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextFocusIn,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto &icEvent = static_cast<fcitx::InputContextEvent &>(event);
                if (auto *ic = icEvent.inputContext()) {
                    logEvent("focus in fe=" + std::string(ic->frontendName()));
                    focused_ = true;
                    pushLine(inline_keyboard::focusMessage(true));
                    pushState(ic);
                }
            }));
        // 失焦：只发一条不依赖 context 的消息。这里**不能**读 InputContext
        // （失焦那一刻它正在销毁），所以只置标志 + pushLine。
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextFocusOut,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &) {
                if (!focused_)
                    return;
                focused_ = false;
                pushLine(inline_keyboard::focusMessage(false));
            }));
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextInputMethodActivated,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto &icEvent = static_cast<fcitx::InputContextEvent &>(event);
                if (auto *ic = icEvent.inputContext()) {
                    pushState(ic);
                }
            }));
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputContextInputMethodDeactivated,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &event) {
                auto &icEvent = static_cast<fcitx::InputContextEvent &>(event);
                if (auto *ic = icEvent.inputContext()) {
                    pushState(ic);
                }
            }));
        handlers_.emplace_back(instance_->watchEvent(
            fcitx::EventType::InputMethodGroupChanged,
            fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &) {
                writeImList();
                if (auto *ic = activeContext()) {
                    pushState(ic);
                }
            }));
    }

    // ---------------- context ----------------

    // 官方虚拟键盘 UI 的做法（src/ui/virtualkeyboard/virtualkeyboard.cpp）：
    // instance()->mostRecentInputContext()。
    // 它能覆盖「当前有焦点」和「刚失焦」两种情况（见 instance.h 注释），
    // 键盘是 layer shell 浮层、不注册 text-input，输入上下文始终属于
    // 真正接收输入的应用窗口，所以取到的就是它。
    // 内部是 TrackableObjectReference，context 析构后自动置空。
    fcitx::InputContext *activeContext() {
        return instance_->mostRecentInputContext();
    }

    // ---------------- 状态推送 ----------------

    // 一次状态快照的全部内容。分开存是为了让「取数据」和「拼 JSON」
    // 各自可读、可测，也避免单个函数超过一屏。
    struct StateSnapshot {
        std::string preedit;
        int cursor = 0;
        bool embedded = false;
        std::string imName;
        std::string imLabel;
        std::string imIcon;
        bool isKeyboard = false;
        std::vector<std::string> items;
        bool hasMore = false;
        bool hasPrev = false;
        bool hasNext = false;
    };

    // 从 context 里取一份快照。只读，不改任何状态。
    StateSnapshot collectSnapshot(fcitx::InputContext *ic) {
        StateSnapshot snapshot;
        auto &panel = ic->inputPanel();

        // wayland 下拼音串在 clientPreedit 里，panel.preedit() 常常是空的
        const fcitx::Text &clientPreedit = panel.clientPreedit();
        const fcitx::Text &preedit =
            clientPreedit.empty() ? panel.preedit() : clientPreedit;
        snapshot.preedit = preedit.toString();
        snapshot.cursor = preedit.cursor();
        // 应用自己把预编辑渲染进输入框时，键盘不用再画一遍
        snapshot.embedded = !clientPreedit.empty();

        if (auto list = panel.candidateList()) {
            collectTopCandidates(preedit, list.get(), snapshot.items,
                                 snapshot.hasMore);
            if (auto *pageable = list->toPageable()) {
                snapshot.hasPrev = pageable->hasPrev();
                snapshot.hasNext = pageable->hasNext();
            }
        }

        const std::string imName = instance_->currentInputMethod();
        auto *entry = imName.empty()
                          ? instance_->inputMethodEntry(ic)
                          : instance_->inputMethodManager().entry(imName);
        snapshot.imName = entry ? entry->uniqueName() : imName;
        snapshot.isKeyboard = entry ? entry->isKeyboard() : false;

        // label / icon 是 **per-context** 的：rime 的中英文子模式、keyboard-us
        // 的 "en" 都挂在具体 context 上，不是全局值。所以必须取传入的 ic 自己的，
        // 不能用 activeContext() 绕过 —— 那会在切换窗口时读到别的窗口的子模式，
        // 空格上显示的输入法状态就卡在上一个窗口不动。
        // 只有 ic 自己没有挂引擎时（比如纯布局还没绑定），才退回全局。
        const std::string contextIm = instance_->inputMethod(ic);
        if (contextIm.empty()) {
            // 没有引擎的纯布局 context：退回全局输入法的条目标签
            auto *globalEntry =
                instance_->inputMethodManager().entry(instance_->currentInputMethod());
            snapshot.imLabel = globalEntry ? globalEntry->label() : std::string();
            snapshot.imIcon =
                inline_keyboard::resolveIcon(globalEntry ? globalEntry->icon() : std::string());
        } else {
            snapshot.imLabel = instance_->inputMethodLabel(ic);
            snapshot.imIcon = inline_keyboard::resolveIcon(instance_->inputMethodIcon(ic));
        }
        return snapshot;
    }

    // 快照 -> 一行 JSON（字段名是插件侧的契约，不要随意改）
    // addon 自己的 UI 名（inline-keyboard-status）。插件拿它和 state.ui 比，
    // 不用在 QML 里硬编码字符串 —— 名字是 addon 注册时定的，硬编码过一次
    // 就因为下划线和连字符的差别误判成「被抢走了」，白白多 reclaim。
    // addonInfo() 在构造期还是空的（AddonInstance 没有 afterLoad 之类
    // 的"加载完成"回调），所以推迟到第一次真正用的时候再取，取到就缓存。
    std::string selfUiName() {
        if (selfUiName_.empty()) {
            selfUiName_ = addonInfo() ? addonInfo()->uniqueName() : std::string();
        }
        return selfUiName_;
    }

    // 当前正在用的 UI 实现名（classicui / inline-keyboard-status / ...）。
    // 插件靠它判断自己的 UI 是否被顶掉，然后决定要不要申领回来。
    std::string currentUiName() const {
        return instance_->userInterfaceManager().currentUI();
    }

    std::string buildStateJson(const StateSnapshot &snapshot) {
        std::string json = std::string("{\"preedit\":\"") +
                           inline_keyboard::escapeJsonString(snapshot.preedit) +
                           "\",\"cursor\":" + std::to_string(snapshot.cursor) +
                           ",\"visible\":true,\"keyboard\":" +
                           (snapshot.isKeyboard ? "true" : "false") +
                           ",\"im\":\"" +
                           inline_keyboard::escapeJsonString(snapshot.imName) +
                           "\",\"imlabel\":\"" +
                           inline_keyboard::escapeJsonString(snapshot.imLabel) +
                           "\",\"imicon\":\"" +
                           inline_keyboard::escapeJsonString(snapshot.imIcon) +
                           "\",\"hasPrev\":" +
                           (snapshot.hasPrev ? "true" : "false") +
                           ",\"hasNext\":" +
                           (snapshot.hasNext ? "true" : "false") +
                           ",\"hasMore\":" +
                           (snapshot.hasMore ? "true" : "false") +
                           ",\"embedded\":" +
                           (snapshot.embedded ? "true" : "false") +
                           ",\"ui\":\"" +
                           inline_keyboard::escapeJsonString(currentUiName()) +
                           "\",\"self\":\"" +
                           inline_keyboard::escapeJsonString(selfUiName()) +
                           "\",\"items\":[";
        for (size_t i = 0; i < snapshot.items.size(); i++) {
            if (i) {
                json += ",";
            }
            json += "\"" + inline_keyboard::escapeJsonString(snapshot.items[i]) + "\"";
        }
        json += "]}";
        return json;
    }

    void pushState(fcitx::InputContext *ic) {
        const StateSnapshot snapshot = collectSnapshot(ic);
        const std::string json = buildStateJson(snapshot);

        // 只在 socket 没连上时才落盘。
        // 这个文件是给 probe.sh 自检用的兜底，socket 通着的时候写它纯属浪费：
        // addon 每 40ms 推一次状态，等于每秒 25 次磁盘 IO。
        if (clientFd_ < 0 || clientClosing_) {
            inline_keyboard::atomicWrite(statusPath_, json + "\n");
        }
        // 只在内容真的变了时打一行：状态推送是排查「候选为什么不出现」
        // 的第一手材料，每次都打会被心跳刷屏。
        if (json != lastStateJson_) {
            logEvent("state preedit=" + snapshot.preedit +
                     " items=" + std::to_string(snapshot.items.size()) +
                     " im=" + snapshot.imName +
                     " fe=" + std::string(ic->frontendName()));
        }
        pushDeduped(json);
    }

    // 顶部候选：取前 kTopCandidateSlots 个，并判断还有没有更多。
    //
    // 缓存 key 用「预编辑 + 首页首项 + 当前页大小」：rime 每页固定 6 个，
    // 只比 size 的话翻页检测不到（页大小不变、内容变了）。
    void collectTopCandidates(const fcitx::Text &preedit,
                              fcitx::CandidateList *list,
                              std::vector<std::string> &items, bool &hasMore) {
        const std::string key = preedit.toString() + "\x1f" +
                                firstCandidateText(list) + "\x1f" +
                                std::to_string(list->size());
        if (key == topCacheKey_) {
            items = topCacheItems_;
            hasMore = topCacheHasMore_;
            return;
        }

        std::vector<std::string> collected;
        bool more = false;
        readCandidates(list, kTopCandidateSlots, collected, more);

        topCacheKey_ = key;
        topCacheItems_ = collected;
        topCacheHasMore_ = more;
        items = collected;
        hasMore = more;
    }

    // 当前页第一个候选的文字。取不到就返回空串。
    std::string firstCandidateText(fcitx::CandidateList *list) {
        if (!list || list->size() <= 0) {
            return {};
        }
        const auto &candidate = list->candidate(0);
        if (candidate.isPlaceHolder()) {
            return {};
        }
        return candidate.text().toString();
    }

    // 按官方契约读候选。
    //
    //   BulkCandidateList（头文件原文 "useful for virtual keyboard"）：
    //     totalSize() 可能返回 -1（实现没有明确总数）；
    //     candidateFromAll(idx) 越界时「may raise exception」，
    //     官方建议 catch 它当遍历结束信号。
    //   所以先用 totalSize()，拿不到再靠异常兜底。
    void readCandidates(fcitx::CandidateList *list, int limit,
                        std::vector<std::string> &out, bool &hasMore) {
        hasMore = false;
        if (!list) {
            return;
        }

        if (auto *bulk = list->toBulk()) {
            readBulkCandidates(bulk, limit, out, hasMore);
            if (!out.empty() || hasMore) {
                return;
            }
        }

        // 不支持 Bulk 的输入法：只能读当前页
        const int pageSize = list->size();
        for (int i = 0; i < pageSize && static_cast<int>(out.size()) < limit;
             i++) {
            const auto &candidate = list->candidate(i);
            if (!candidate.isPlaceHolder()) {
                out.push_back(candidate.text().toString());
            }
        }
        if (auto *pageable = list->toPageable()) {
            hasMore = pageable->hasNext();
        }
    }

    void readBulkCandidates(fcitx::BulkCandidateList *bulk, int limit,
                            std::vector<std::string> &out, bool &hasMore) {
        const int total = bulk->totalSize();
        logEvent("bulk totalSize=" + std::to_string(total) +
                 " limit=" + std::to_string(limit));

        if (total > 0) {
            // 实现给了明确总数，直接按它走
            const int upper = std::min(total, limit);
            for (int i = 0; i < upper; i++) {
                const auto &candidate = bulk->candidateFromAll(i);
                if (!candidate.isPlaceHolder()) {
                    out.push_back(candidate.text().toString());
                }
            }
            hasMore = total > limit;
            return;
        }

        // totalSize() == -1：实现没有明确总数，
        // 按头文件说明用越界异常当结束信号，多取一个来判断 hasMore。
        for (int i = 0; i < kMaxCandidateScan; i++) {
            try {
                const auto &candidate = bulk->candidateFromAll(i);
                if (candidate.isPlaceHolder()) {
                    continue;
                }
                if (static_cast<int>(out.size()) >= limit) {
                    hasMore = true;
                    break;
                }
                out.push_back(candidate.text().toString());
            } catch (const std::exception &error) {
                logEvent("bulk end idx=" + std::to_string(i) +
                         " why=" + std::string(error.what()));
                break;
            } catch (...) {
                logEvent("bulk end idx=" + std::to_string(i) + " why=unknown");
                break;
            }
        }
    }

    // ---------------- 全量候选 ----------------

    void writeAllCandidates() {
        fcitx::InputContext *ic = activeContext();
        if (!ic) {
            pushLine("error:no-context");
            return;
        }

        std::vector<std::string> words;
        bool hasMore = false;
        if (auto list = ic->inputPanel().candidateList()) {
            readCandidates(list.get(), kMaxCandidateScan, words, hasMore);
        }

        const std::string json = inline_keyboard::buildCandidatePayload(words);
        inline_keyboard::atomicWrite(allCandPath_, json + "\n");
        pushLine("allcand:" + json);
        logEvent("allcand count=" + std::to_string(words.size()) +
                 " more=" + (hasMore ? "1" : "0") +
                 " fe=" + std::string(ic->frontendName()));
    }

    // ---------------- 输入法清单 ----------------

    void writeImList() {
        auto &manager = instance_->inputMethodManager();
        std::string json = "[";
        bool first = true;
        for (const auto &item : manager.currentGroup().inputMethodList()) {
            auto *entry = manager.entry(item.name());
            if (!entry) {
                continue;
            }
            if (!first) {
                json += ",";
            }
            first = false;
            json += "{\"name\":\"" +
                    inline_keyboard::escapeJsonString(entry->uniqueName()) +
                    "\",\"title\":\"" +
                    inline_keyboard::escapeJsonString(entry->name()) +
                    "\",\"label\":\"" +
                    inline_keyboard::escapeJsonString(entry->label()) +
                    "\",\"icon\":\"" +
                    inline_keyboard::escapeJsonString(inline_keyboard::resolveIcon(entry->icon())) +
                    "\",\"keyboard\":" +
                    (entry->isKeyboard() ? "true" : "false") + "}";
        }
        json += "]";
        inline_keyboard::atomicWrite(imListPath_, json + "\n");
        pushLine("imlist:" + json);
    }

    // ---------------- 选词 ----------------

    // 插件给的 index 是**原始全局下标**（candidateFromAll 的参数）。
    // 之前这里用 list->size() 当上界，导致第 7 个之后永远选不中 ——
    // size() 只是当前页的条数。
    void selectCandidate(int index) {
        fcitx::InputContext *ic = activeContext();
        if (!ic) {
            pushLine("error:no-context");
            return;
        }
        auto list = ic->inputPanel().candidateList();
        if (!list) {
            pushLine("error:no-candidates");
            return;
        }

        if (auto *bulk = list->toBulk()) {
            try {
                const auto &candidate = bulk->candidateFromAll(index);
                if (candidate.isPlaceHolder()) {
                    return;
                }
                const std::string text = candidate.text().toString();
                candidate.select(ic);
                logEvent("select idx=" + std::to_string(index) +
                         " text=" + text);
                return;
            } catch (const std::exception &error) {
                logEvent("select idx=" + std::to_string(index) +
                         " throw=" + std::string(error.what()));
                return;
            } catch (...) {
                logEvent("select idx=" + std::to_string(index) +
                         " throw=unknown");
                return;
            }
        }

        // 不支持 Bulk 的输入法：只能选当前页
        if (index < 0 || index >= list->size()) {
            return;
        }
        const auto &candidate = list->candidate(index);
        if (!candidate.isPlaceHolder()) {
            candidate.select(ic);
        }
    }

    // ---------------- 按键注入 ----------------

    void injectKey(const std::string &keysymName, int modifiers) {
        fcitx::InputContext *ic = activeContext();
        if (!ic) {
            logEvent("key skipped reason=no-context");
            return;
        }
        const fcitx::KeySym keysym = fcitx::Key::keySymFromString(keysymName);
        if (keysym == FcitxKey_None) {
            logEvent("key skipped reason=unknown-keysym name=" + keysymName);
            return;
        }

        fcitx::KeyStates states;
        if (modifiers & 1) {
            states |= fcitx::KeyState::Shift;
        }
        if (modifiers & 2) {
            states |= fcitx::KeyState::Ctrl;
        }
        if (modifiers & 4) {
            states |= fcitx::KeyState::Alt;
        }
        if (modifiers & 8) {
            states |= fcitx::KeyState::Super;
        }
        // Virtual 标记必须保留：fcitx5 的 InputContextVirtualKeyboardEvent
        // 处理器靠它识别来源，去掉后事件被直接丢弃（实测一个字符都不上屏）。
        states |= fcitx::KeyState::Virtual;

        const fcitx::Key key(keysym, states);
        for (const bool release : {false, true}) {
            fcitx::VirtualKeyboardEvent event(ic, release);
            event.setKey(key);
            ic->virtualKeyboardEvent(event);
        }
        logEvent("key sent name=" + keysymName +
                 " fe=" + std::string(ic->frontendName()));

    }

    // ---------------- 文本提交 ----------------

    void commitText(const std::string &utf8) {
        if (utf8.empty()) {
            return;
        }
        fcitx::InputContext *ic = activeContext();
        if (!ic) {
            logEvent("text skipped reason=no-context");
            return;
        }
        // 有预编辑就先清掉，否则文本会插进拼音串中间
        auto &panel = ic->inputPanel();
        if (!panel.clientPreedit().empty() || !panel.preedit().empty()) {
            ic->reset();
        }
        logEvent("text commit bytes=" + std::to_string(utf8.size()) +
                 " fe=" + std::string(ic->frontendName()));
        ic->commitString(utf8);
    }

    // ---------------- socket ----------------

    void setupSocket() {
        sockPath_ = inline_keyboard::runtimeDir() + "/inline-keyboard.sock";
        ::unlink(sockPath_.c_str());
        listenFd_ =
            ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listenFd_ < 0) {
            logEvent("socket failed errno=" + std::to_string(errno));
            return;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, sockPath_.c_str(),
                     sizeof(address.sun_path) - 1);
        if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) < 0 ||
            ::listen(listenFd_, 4) < 0) {
            logEvent("bind failed errno=" + std::to_string(errno) +
                     " path=" + sockPath_);
            ::close(listenFd_);
            listenFd_ = -1;
            return;
        }
        logEvent("listen path=" + sockPath_);
        acceptWatcher_ = instance_->eventLoop().addIOEvent(
            listenFd_, fcitx::IOEventFlags{fcitx::IOEventFlag::In},
            [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
                acceptClient();
                return true;
            });
    }

    void acceptClient() {
        const int fd = ::accept4(listenFd_, nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return;
        }
        // 单客户端设计：新连接顶掉旧的。这里在 accept 回调里，
        // 旧的 clientWatcher_ 不在 dispatch 状态，可以立即关
        closeClientNow("replaced-by-new-client");
        clientFd_ = fd;
        clientBuffer_.clear();
        lastStateJson_.clear();

        // 立刻给一份完整快照。不能等下一次 update()：
        // 键盘没输入时 update() 不会触发，新客户端会一直空等。
        writeImList();
        // 焦点状态也是状态的一部分，连上就先告诉客户端一次
        pushLine(inline_keyboard::focusMessage(focused_));
        if (auto *ic = activeContext()) {
            pushState(ic);
        }

        clientWatcher_ = instance_->eventLoop().addIOEvent(
            fd, fcitx::IOEventFlags{fcitx::IOEventFlag::In},
            [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
                readClient();
                return true;
            });

        // socket 连上就代表键盘打开了（插件只在 keyboardOpen 时才连，
        // 收起立即断开）。这是唯一可靠的开关信号：插件不发 mode:osk，
        // 光等 showVirtualKeyboard() 的话永远不会被调用，模式一直停在
        // PhysicalKeyboard，fcitx5 就把 VirtualKeyboardEvent 全当噪声丢了。
        markShown(true);
    }

    // 立即关：只能在「不在这个 client 的 IO 回调里」时调（accept 回调、析构）。
    // 会一并取消已排队的延迟关闭。
    void closeClientNow(const char *why = "unknown") {
        if (deferClose_) {
            deferClose_.reset();
        }
        const bool had = clientFd_ >= 0;
        if (had) {
            logEvent(std::string("client closed why=") + why);
        }
        clientWatcher_.reset();
        if (clientFd_ >= 0) {
            ::close(clientFd_);
            clientFd_ = -1;
        }
        clientBuffer_.clear();
        clientClosing_ = false;
        if (had) {
            markShown(false);
        }
    }

    // 延迟关：IO 回调里用。destroy 正在 dispatch 的 EventSource 会让 sd-event
    // 报 EBADF，fcitx5 把 EventLoopException 当致命错误直接 abort
    // （event_sdevent.cpp:277）。
    void closeClient(const char *why = "unknown") {
        if (clientFd_ < 0 || clientClosing_)
            return;
        clientClosing_ = true;
        logEvent(std::string("client closed why=") + why);
        deferClose_ = instance_->eventLoop().addDeferEvent(
            [this](fcitx::EventSource *) {
                deferClose_.reset();
                clientWatcher_.reset();
                if (clientFd_ >= 0) {
                    ::close(clientFd_);
                    clientFd_ = -1;
                }
                clientBuffer_.clear();
                clientClosing_ = false;
                markShown(false);
                return true;
            });
    }

    void readClient() {
        char buffer[4096];
        const ssize_t count = ::read(clientFd_, buffer, sizeof(buffer));
        if (count <= 0) {
            closeClient(count == 0 ? "peer-eof" : "read-error");
            return;
        }
        clientBuffer_.append(buffer, static_cast<size_t>(count));
        size_t newline = 0;
        while ((newline = clientBuffer_.find('\n')) != std::string::npos) {
            const std::string line = clientBuffer_.substr(0, newline);
            clientBuffer_.erase(0, newline + 1);
            if (!line.empty()) {
                handleLine(line);
            }
        }
    }

    void handleLine(const std::string &line) {
        if (line.rfind("key:", 0) == 0) {
            std::string rest = line.substr(4);
            int modifiers = 0;
            const auto colon = rest.find(':');
            if (colon != std::string::npos) {
                modifiers = std::atoi(rest.c_str() + colon + 1);
                rest.resize(colon);
            }
            injectKey(inline_keyboard::toKeysymName(rest), modifiers);
            return;
        }
        if (line.rfind("text:", 0) == 0) {
            commitText(line.substr(5));
            return;
        }
        if (line == "expand") {
            writeAllCandidates();
            return;
        }
        if (line.rfind("select:", 0) == 0) {
            selectCandidate(std::atoi(line.c_str() + 7));
            return;
        }
        if (line == "release") {
            // 插件关闭键盘时显式要求交还 UI。
            // 只靠 socket 断开（peer-eof -> markShown(false)）不够：
            // shown_ 可能已经是 false，那条路会被状态去重挡掉。
            logEvent("release requested");
            // 插件已经明确说"键盘关了"，就以它为准把状态置成收起。
            // 不置的话 scheduleRestore 的回调会看到 shown_ 仍是 true
            // 而取消这次交还（日志 "restore cancelled shown=1"）。
            shown_ = false;
            scheduleRestore();
            return;
        }
        if (line == "reclaim") {
            // 插件发现 UI 被顶掉了，要求重新申领。
            // 只做「切模式 + 重选 UI」，不改 shown_（键盘是否开着由插件判断）。
            auto &manager = instance_->userInterfaceManager();
            const std::string previousIm = instance_->currentInputMethod();
            instance_->setInputMethodMode(
                fcitx::InputMethodMode::OnScreenKeyboard);
            if (!previousIm.empty() &&
                instance_->currentInputMethod() != previousIm) {
                instance_->setCurrentInputMethod(previousIm);
            }
            manager.updateAvailability();
            logEvent("reclaim ui, now=" + manager.currentUI());
            return;
        }
        if (line.rfind("mode:", 0) == 0) {
            const bool onScreen = line.substr(5) == "osk";
            // 只切模式，**不**调 updateAvailability()。
            // 官方 Instance::setInputMethodMode 是完整操作：同值直接 return，
            // 否则写值并 postEvent(InputMethodModeChangedEvent)（instance.cpp:1592）。
            // 再调 updateAvailability 会让 fcitx5 重新挑选 UI 实现，
            // 插件的 socket 被关掉重连 —— 实测就是这里抛的 BrokenPipe。
            const std::string previousIm = instance_->currentInputMethod();
            instance_->setInputMethodMode(
                onScreen ? fcitx::InputMethodMode::OnScreenKeyboard
                         : fcitx::InputMethodMode::PhysicalKeyboard);
            // 切模式会把输入法重置成默认，恢复用户原来的选择
            if (!previousIm.empty() && instance_->currentInputMethod() != previousIm) {
                instance_->setCurrentInputMethod(previousIm);
            }
            logEvent(std::string("mode=") + (onScreen ? "osk" : "physical") +
                     " im=" + instance_->currentInputMethod());
            return;
        }
        if (line.rfind("ui:", 0) == 0) {
            uiAvailable_ = line.substr(3) != "0";
            instance_->userInterfaceManager().updateAvailability();
            logEvent(std::string("ui=") +
                     (uiAvailable_ ? "available" : "unavailable"));
            return;
        }

        // 不认识的指令不能静默丢：插件会等一个永远不来的效果
        logEvent("error unknown-command=" + line.substr(0, 40));
        pushLine("error:unknown-command:" + line.substr(0, 40));
    }

    void pushLine(const std::string &line) {
        if (clientFd_ < 0 || clientClosing_) {
            return;
        }
        if (!inline_keyboard::writeLineToClient(clientFd_, line)) {
            closeClient("write-error");
        }
    }

    // 状态没变就不推，避免每次 InputPanel 更新都刷一遍 socket
    void pushDeduped(const std::string &json) {
        if (json == lastStateJson_) {
            return;
        }
        lastStateJson_ = json;
        pushLine("state:" + json);
    }

    // ---------------- 日志 ----------------

    // 追加一行到 inline-keyboard-addon.log（毫秒单调时钟）。
    // fcitx5 崩的时候没有别的上下文，定位全靠「最后一行是什么」。
    // 超过 256KB 砍掉前半，避免无限长。
    void logEvent(const std::string &what) {
        if (logPath_.empty()) {
            return;
        }
        struct timespec stamp {};
        clock_gettime(CLOCK_MONOTONIC, &stamp);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%ld.%03ld ",
                      static_cast<long>(stamp.tv_sec),
                      stamp.tv_nsec / 1000000);
        const std::string line = std::string(buffer) + what + "\n";

        struct stat info {};
        if (::stat(logPath_.c_str(), &info) == 0 && info.st_size > 256 * 1024) {
            std::string tail;
            if (FILE *file = std::fopen(logPath_.c_str(), "r")) {
                std::fseek(file, info.st_size / 2, SEEK_SET);
                char chunk[4096];
                size_t count;
                while ((count = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
                    tail.append(chunk, count);
                }
                std::fclose(file);
            }
            const auto newline = tail.find('\n');
            if (newline != std::string::npos) {
                tail.erase(0, newline + 1);
            }
            inline_keyboard::atomicWrite(logPath_, tail);
        }
        if (FILE *file = std::fopen(logPath_.c_str(), "a")) {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fclose(file);
        }
    }

    ~InlineKeyboardStatus() override {
        handlers_.clear();
        closeClientNow();
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
        if (!sockPath_.empty()) {
            ::unlink(sockPath_.c_str());
        }
    }

    fcitx::Instance *instance_;

    // socket
    std::string sockPath_;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string clientBuffer_;
    std::unique_ptr<fcitx::EventSourceIO> acceptWatcher_;
    std::unique_ptr<fcitx::EventSourceIO> clientWatcher_;
    // 已排队关闭：这段窗口里 fd 还在，但不能再往里写
    bool clientClosing_ = false;
    std::unique_ptr<fcitx::EventSource> deferClose_;

    // 文件兜底（probe.sh 用）
    std::string statusPath_;
    std::string allCandPath_;
    std::string imListPath_;
    std::string logPath_;

    std::string lastStateJson_;
    uint64_t lastModeAttempt_ = 0;
    std::string selfUiName_;

    // 键盘开合
    bool shown_ = false;
    // 当前有没有输入框拿着焦点，变化时随 focus:0/1 推给客户端
    bool focused_ = false;
    bool uiAvailable_ = true;
    // addTimeEvent 返回 unique_ptr：必须持有，析构掉的话回调不会跑
    std::unique_ptr<fcitx::EventSourceTime> restoreTimer_;
    std::unique_ptr<fcitx::EventSourceTime> uiReselectTimer_;

    // 事件订阅
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>>
        handlers_;

    // 顶部候选缓存
    std::string topCacheKey_;
    std::vector<std::string> topCacheItems_;
    bool topCacheHasMore_ = false;
};

class InlineKeyboardStatusFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new InlineKeyboardStatus(manager->instance());
    }
};

// BACKWARDS 版本同时导出无后缀和带后缀两个符号：
// fcitx5 的加载器找的是无后缀的 fcitx_addon_factory_instance。
FCITX_ADDON_FACTORY_V2_BACKWARDS(inline_keyboard_status, InlineKeyboardStatusFactory);
