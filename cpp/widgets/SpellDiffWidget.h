#pragma once

#include "core/SpellModels.h"
#include "theme/SpellTheme.h"

#include <QFrame>
#include <QString>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace spell {

class SpellBadge;

/**
 * Card widget rendering a `StagedDiff` with:
 * - Clickable file header (emits `openFileRequested`), status badge, and `+added` / `-removed` counters.
 * - Syntax-colored unified diff view with `#3FB950` (added) and `#F85149` (removed) tinted
 *   full-width line backgrounds.
 * - Optional **Approve** & **Reject** buttons shown when `showActions()` is enabled and the
 *   diff status is pending (`"pending"`, `"staged"`, or `"verified"`).
 */
class SpellDiffWidget : public QFrame {
    Q_OBJECT

public:
    explicit SpellDiffWidget(QWidget* parent = nullptr);
    explicit SpellDiffWidget(const StagedDiff& diff, QWidget* parent = nullptr);
    SpellDiffWidget(const StagedDiff& diff, bool showActions, QWidget* parent = nullptr);
    ~SpellDiffWidget() override = default;

    const StagedDiff& diff() const { return m_diff; }
    QString diffId() const { return m_diff.id; }
    QString filePath() const { return m_diff.filePath; }
    bool showActions() const { return m_showActions; }
    bool isCollapsed() const { return m_collapsed; }
    int maxDiffHeight() const { return m_maxDiffHeight; }

    void setDiff(const StagedDiff& diff);
    void setShowActions(bool show);
    void setCollapsed(bool collapsed);
    void setMaxDiffHeight(int maxHeightPx);

    QPushButton* approveButton() const { return m_approveButton; }
    QPushButton* rejectButton() const { return m_rejectButton; }

    /**
     * Returns true when `status` represents an actionable pending diff
     * (`"pending"`, `"staged"`, or `"verified"`, case-insensitive).
     */
    static bool isPendingStatus(const QString& status);

    /**
     * Counts `+` (excluding `+++`) and `-` (excluding `---`) lines in a unified diff string.
     */
    static void computeLineCounts(const QString& unifiedDiff,
                                  int* outAdded,
                                  int* outRemoved);

public slots:
    void applyTheme();

signals:
    void approveClicked(const QString& diffId);
    void rejectClicked(const QString& diffId);
    void openFileRequested(const QString& filePath);

private:
    void setupUi();
    void updateHeaderAndActions();
    void renderUnifiedDiff();
    void updateDiffViewHeight();

    StagedDiff m_diff;
    bool m_showActions = true;
    bool m_collapsed = false;
    int m_maxDiffHeight = 320;

    SpellBadge* m_statusBadge = nullptr;
    QPushButton* m_fileButton = nullptr;
    QLabel* m_addedLabel = nullptr;
    QLabel* m_removedLabel = nullptr;
    QPushButton* m_approveButton = nullptr;
    QPushButton* m_rejectButton = nullptr;
    QLabel* m_summaryLabel = nullptr;
    QPlainTextEdit* m_diffView = nullptr;
};

} // namespace spell
