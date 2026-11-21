#include "core/SpellModels.h"

#include <QCoreApplication>
#include <QMetaType>

#include <type_traits>

// All 15 Spell IDE data model structs (`ModelDescriptor`, `ProjectInfo`,
// `ThreadSummary`, `UiChatEvent`, `StagedDiff`, `AgentTimer`, `RubricTag`,
// `SubagentSlotStatus`, `ActiveSubagentInfo`, `OnlineResearchSettings`,
// `ToolPermissionEntry`, `TerminalApprovalRequest`, `SystemInfo`,
// `TelemetrySnapshot`, and `WorkspaceSnapshot`), along with their `fromJson` /
// `toJson` methods and snake_case / camelCase parsing helpers, are defined
// inline in `core/SpellModels.h`.
//
// This translation unit:
//  1. Verifies standalone compilation of `core/SpellModels.h`.
//  2. Statically validates every model's value semantics, `fromJson` / `toJson`
//     signatures, equality comparability, and `Q_DECLARE_METATYPE` registration.
//  3. Pre-registers every model type and `QVector<T>` container with Qt's meta-
//     type system at `QCoreApplication` startup for cross-thread queued signals.
//  4. Provides a non-empty archive anchor symbol for static library builds.

namespace spell {

namespace {

template <typename T>
constexpr bool verifyModelContract() noexcept {
    static_assert(std::is_default_constructible_v<T>,
                  "Spell model type must be default-constructible");
    static_assert(std::is_copy_constructible_v<T>,
                  "Spell model type must be copy-constructible");
    static_assert(std::is_copy_assignable_v<T>,
                  "Spell model type must be copy-assignable");
    static_assert(std::is_move_constructible_v<T>,
                  "Spell model type must be move-constructible");
    static_assert(std::is_move_assignable_v<T>,
                  "Spell model type must be move-assignable");
    static_assert(std::is_same_v<decltype(&T::fromJson), T (*)(const QJsonObject&)>,
                  "Spell model type must provide static T fromJson(const QJsonObject&)");
    static_assert(std::is_same_v<decltype(&T::toJson), QJsonObject (T::*)() const>,
                  "Spell model type must provide QJsonObject toJson() const");
    static_assert(std::is_same_v<decltype(std::declval<const T&>() == std::declval<const T&>()), bool>,
                  "Spell model type must be equality-comparable");
    static_assert(QMetaTypeId2<T>::Defined,
                  "Spell model type must be declared with Q_DECLARE_METATYPE");
    return true;
}

static_assert(verifyModelContract<ModelDescriptor>());
static_assert(verifyModelContract<ProjectInfo>());
static_assert(verifyModelContract<ThreadSummary>());
static_assert(verifyModelContract<UiChatEvent>());
static_assert(verifyModelContract<StagedDiff>());
static_assert(verifyModelContract<AgentTimer>());
static_assert(verifyModelContract<RubricTag>());
static_assert(verifyModelContract<SubagentSlotStatus>());
static_assert(verifyModelContract<ActiveSubagentInfo>());
static_assert(verifyModelContract<OnlineResearchSettings>());
static_assert(verifyModelContract<ToolPermissionEntry>());
static_assert(verifyModelContract<TerminalApprovalRequest>());
static_assert(verifyModelContract<SystemInfo>());
static_assert(verifyModelContract<TelemetrySnapshot>());
static_assert(verifyModelContract<WorkspaceSnapshot>());

} // namespace

static void registerSpellModelMetaTypes() {
    qRegisterMetaType<spell::ModelDescriptor>("spell::ModelDescriptor");
    qRegisterMetaType<spell::ProjectInfo>("spell::ProjectInfo");
    qRegisterMetaType<spell::ThreadSummary>("spell::ThreadSummary");
    qRegisterMetaType<spell::UiChatEvent>("spell::UiChatEvent");
    qRegisterMetaType<spell::StagedDiff>("spell::StagedDiff");
    qRegisterMetaType<spell::AgentTimer>("spell::AgentTimer");
    qRegisterMetaType<spell::RubricTag>("spell::RubricTag");
    qRegisterMetaType<spell::SubagentSlotStatus>("spell::SubagentSlotStatus");
    qRegisterMetaType<spell::ActiveSubagentInfo>("spell::ActiveSubagentInfo");
    qRegisterMetaType<spell::OnlineResearchSettings>("spell::OnlineResearchSettings");
    qRegisterMetaType<spell::ToolPermissionEntry>("spell::ToolPermissionEntry");
    qRegisterMetaType<spell::TerminalApprovalRequest>("spell::TerminalApprovalRequest");
    qRegisterMetaType<spell::SystemInfo>("spell::SystemInfo");
    qRegisterMetaType<spell::TelemetrySnapshot>("spell::TelemetrySnapshot");
    qRegisterMetaType<spell::WorkspaceSnapshot>("spell::WorkspaceSnapshot");

    qRegisterMetaType<QVector<spell::ModelDescriptor>>("QVector<spell::ModelDescriptor>");
    qRegisterMetaType<QVector<spell::ProjectInfo>>("QVector<spell::ProjectInfo>");
    qRegisterMetaType<QVector<spell::ThreadSummary>>("QVector<spell::ThreadSummary>");
    qRegisterMetaType<QVector<spell::UiChatEvent>>("QVector<spell::UiChatEvent>");
    qRegisterMetaType<QVector<spell::StagedDiff>>("QVector<spell::StagedDiff>");
    qRegisterMetaType<QVector<spell::AgentTimer>>("QVector<spell::AgentTimer>");
    qRegisterMetaType<QVector<spell::RubricTag>>("QVector<spell::RubricTag>");
    qRegisterMetaType<QVector<spell::SubagentSlotStatus>>("QVector<spell::SubagentSlotStatus>");
    qRegisterMetaType<QVector<spell::ActiveSubagentInfo>>("QVector<spell::ActiveSubagentInfo>");
    qRegisterMetaType<QVector<spell::ToolPermissionEntry>>("QVector<spell::ToolPermissionEntry>");
    qRegisterMetaType<QVector<spell::TerminalApprovalRequest>>("QVector<spell::TerminalApprovalRequest>");
}

Q_COREAPP_STARTUP_FUNCTION(registerSpellModelMetaTypes)

namespace detail {

int spellModelsTranslationUnitAnchor() noexcept;

int spellModelsTranslationUnitAnchor() noexcept {
    return 15;
}

} // namespace detail

} // namespace spell
