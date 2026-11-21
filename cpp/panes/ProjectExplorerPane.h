#pragma once

#include "core/SpellModels.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHash>
#include <QModelIndex>
#include <QSortFilterProxyModel>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#include <memory>

namespace spell {

/**
 * Filter & sort proxy over `QFileSystemModel` for Spell IDE's project tree.
 *
 * Excludes noisy build and VCS directories (`.git`, `target`, `build`,
 * `node_modules`, `.build`) within the active project root while preserving
 * ancestor paths required to reach the project root, and sorts directories
 * before files alphabetically.
 */
class ProjectTreeFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit ProjectTreeFilterProxyModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {
        setDynamicSortFilter(true);
    }

    static bool isExcludedName(const QString& name) {
        return name == QLatin1StringView(".git")
            || name == QLatin1StringView("target")
            || name == QLatin1StringView("build")
            || name == QLatin1StringView("node_modules")
            || name == QLatin1StringView(".build");
    }

    QString projectRootPath() const { return m_projectRootPath; }

    void setProjectRootPath(const QString& rootPath) {
        const QString cleaned = rootPath.isEmpty() ? QString() : QDir::cleanPath(rootPath);
        QString canonical;
        if (!cleaned.isEmpty()) {
            const QString canon = QFileInfo(cleaned).canonicalFilePath();
            if (!canon.isEmpty()) {
                canonical = QDir::cleanPath(canon);
            }
        }
        if (m_projectRootPath == cleaned && m_canonicalRootPath == canonical) {
            return;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        beginFilterChange();
        m_projectRootPath = cleaned;
        m_canonicalRootPath = canonical;
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        m_projectRootPath = cleaned;
        m_canonicalRootPath = canonical;
        invalidateFilter();
#endif
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override {
        const auto* fsModel = qobject_cast<const QFileSystemModel*>(sourceModel());
        if (!fsModel) {
            return true;
        }
        const QModelIndex idx = fsModel->index(sourceRow, 0, sourceParent);
        if (!idx.isValid()) {
            return false;
        }
        const QString name = fsModel->fileName(idx);
        if (!isExcludedName(name)) {
            return true;
        }
        const QString fullPath = QDir::cleanPath(fsModel->filePath(idx));
        return isRootOrAncestor(fullPath, m_projectRootPath)
            || isRootOrAncestor(fullPath, m_canonicalRootPath);
    }

    bool lessThan(const QModelIndex& sourceLeft, const QModelIndex& sourceRight) const override {
        const auto* fsModel = qobject_cast<const QFileSystemModel*>(sourceModel());
        if (!fsModel) {
            return QSortFilterProxyModel::lessThan(sourceLeft, sourceRight);
        }
        const bool leftIsDir = fsModel->isDir(sourceLeft);
        const bool rightIsDir = fsModel->isDir(sourceRight);
        if (leftIsDir != rightIsDir) {
            return leftIsDir;
        }
        return QString::compare(fsModel->fileName(sourceLeft),
                                fsModel->fileName(sourceRight),
                                Qt::CaseInsensitive) < 0;
    }

private:
    static bool isRootOrAncestor(const QString& candidatePath, const QString& rootPath) {
        if (rootPath.isEmpty() || candidatePath.isEmpty()) {
            return false;
        }
#ifdef Q_OS_WIN
        constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
        constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
        if (QString::compare(rootPath, candidatePath, cs) == 0) {
            return true;
        }
        const QString prefix = candidatePath.endsWith(QLatin1Char('/'))
                                   ? candidatePath
                                   : (candidatePath + QLatin1Char('/'));
        return rootPath.startsWith(prefix, cs);
    }

    QString m_projectRootPath;
    QString m_canonicalRootPath;
};

/**
 * Left workspace explorer pane for BeatrixLeslie IDE.
 *
 * Three-section vertical layout (pure monochrome Dark/White, no internal splitter,
 * no file tree panel):
 * - **Top (`SpellInfoSubpanel`)**: the sole notification & status surface in the IDE
 *   (34px left-aligned `"BEATRIXLESLIE IDE"` header, compact key-value rows for `Server`,
 *   `Model`, `Network`, `Message`, `Mode`, and conditional `Tags` / `Timers` / `Terminal`,
 *   plus the 6-row newest-first `SpellStatusTicker`).
 * - **`SpellHairline`** (1px horizontal divider).
 * - **Middle (`ProjectsSubpanel`)**: `QScrollArea` containing expandable Project
 *   sections with nested, 18px-indented Conversations, per-project & per-conversation
 *   Message Terminal pop-out buttons, and per-conversation linked-folder
 *   overrides.
 * - **`SpellHairline`** (1px horizontal divider).
 * - **Bottom (`SpellSettingsSubpanel`)**: minimal 48px bar with only the monochrome
 *   22x22 Witch Hat mark on the left and the 30x30 Settings Gear button on the right.
 */
class ProjectExplorerPane : public QWidget {
    Q_OBJECT

public:
    explicit ProjectExplorerPane(QWidget* parent = nullptr);
    ~ProjectExplorerPane() override;

    ProjectExplorerPane(const ProjectExplorerPane&) = delete;
    ProjectExplorerPane& operator=(const ProjectExplorerPane&) = delete;
    ProjectExplorerPane(ProjectExplorerPane&&) = delete;
    ProjectExplorerPane& operator=(ProjectExplorerPane&&) = delete;

    /** Stores per-thread linked folder overrides (threadId -> folderPath) and refreshes conversation rows. */
    void setConversationLinkedFolders(const QHash<QString, QString>& linkedFolders);

public slots:
    /** Synchronizes info telemetry, status ticker, projects, and nested conversations. */
    void updateSnapshot(const WorkspaceSnapshot& snap);

    /** Appends a transient app/core status notice to the `SpellStatusTicker`. */
    void pushStatusNotice(const QString& text, bool isError = false);

    /** Re-applies the active `ThemeManager` monochrome palette, icons, and hairlines. */
    void applyTheme();

signals:
    void fileOpenRequested(const QString& filePath, bool readOnly);
    void addProjectRequested();
    void removeProjectRequested(const QString& projectId);
    void selectProjectRequested(const QString& projectId);
    void renameProjectRequested(const QString& projectId, const QString& newName);
    void toggleProjectReadOnlyRequested(const QString& projectId, bool readOnly);
    void createThreadRequested(const QString& projectId);
    void selectThreadRequested(const QString& threadId);
    void renameThreadRequested(const QString& threadId, const QString& newTitle);
    void deleteThreadRequested(const QString& threadId);
    void setThreadProfileRequested(const QString& threadId, const QString& profile);
    void settingsRequested(int initialPaneIndex = 0);
    void terminalPopOutRequested(const QString& workingDir, const QString& label);
    void conversationLinkedFolderChanged(const QString& threadId, const QString& folderPath);
    void openOnlineResearchRequested();
    void airGapNowRequested();
    void toggleStageWriteRequested(bool enabled);
    void openProjectFileRequested(const QString& rootPath);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
