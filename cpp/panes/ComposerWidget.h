#pragma once

#include "core/SpellModels.h"

#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>

class QDragEnterEvent;
class QDropEvent;
class QEvent;

namespace spell {

/**
 * Minimalist monochrome message composer widget hosted at the bottom of `AgentChatPane`.
 *
 * Layout (`12px` outer margin on pure `ui.background`):
 * - Optional hidden-by-default removable attached-file pill strip (visible only when
 *   files are attached via drag-and-drop or `addAttachedFile()`).
 * - Single rounded monochrome card (`8px` radius, `1px` `subtleBorder` brightening to
 *   `selectedBorder` when focused):
 *   - Multi-line `QPlainTextEdit` (`13pt`, placeholder `"Message BeatrixLeslie…"`): compact
 *     auto-growing height by default, or expanded (`~220px`) via the Expand button.
 *     In compact mode `Return`/`Enter` sends (`Shift+Enter` inserts a newline);
 *     `Cmd/Ctrl+Return` always sends in both compact and expanded modes.
 *   - Bottom icon strip inside the frame: Expand/Collapse (`28×26`), Pop out message
 *     terminal (`28×26`), spacer, and inverted monochrome Send/Stop (`28×26`).
 */
class ComposerWidget : public QWidget {
    Q_OBJECT

public:
    explicit ComposerWidget(QWidget* parent = nullptr);
    ~ComposerWidget() override;

    ComposerWidget(const ComposerWidget&) = delete;
    ComposerWidget& operator=(const ComposerWidget&) = delete;
    ComposerWidget(ComposerWidget&&) = delete;
    ComposerWidget& operator=(ComposerWidget&&) = delete;

    /** Switches the primary action control between Send (`false`) and Stop (`true`). */
    void setBusy(bool busy);

    /**
     * Retains endpoint readiness state for API compatibility.
     * Endpoint configuration lives strictly in Settings; the composer stays uncluttered.
     */
    void setEndpointStatus(bool configured, const QString& modelId, bool online);

    /** Synchronizes the active conversation's execution profile (`"write"` or `"ask"`). */
    void setThreadProfile(const QString& profile);
    [[nodiscard]] QString threadProfile() const;

    /** Retains Stage-Write state for API compatibility (controlled in Settings). */
    void setStageWriteMode(bool enabled);

    /** Retains terminal visibility state for API compatibility. */
    void setTerminalVisible(bool visible);

    /** Retains rubric tag state for API compatibility (managed in Settings / Info). */
    void setAvailableTags(const QVector<RubricTag>& tags, const QStringList& activeTagIds);

    /** Returns whether the prompt editor is currently in expanded multi-line mode. */
    [[nodiscard]] bool isExpanded() const;

    /** Toggles between compact auto-growing height and expanded (`~220px`) editor height. */
    void setExpanded(bool expanded);

    /** Gives keyboard focus to the multi-line prompt editor. */
    void focusInput();

    /** Re-applies the active Spell IDE monochrome palette and typography. */
    void applyTheme();

    /** Convenience accessors and mutators for prompt text and file attachments. */
    [[nodiscard]] QString inputText() const;
    void setInputText(const QString& text);
    void clearInput();

    [[nodiscard]] QStringList attachedFiles() const;
    void addAttachedFile(const QString& filePath);
    void removeAttachedFile(const QString& filePath);
    void clearAttachedFiles();

    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool isStageWriteMode() const;
    [[nodiscard]] bool isTerminalVisible() const;
    [[nodiscard]] QStringList activeTagIds() const;
    [[nodiscard]] bool canSend() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void sendRequested(const QString& text, const QStringList& attachedFiles);
    void stopRequested();
    void threadProfileToggled(const QString& nextProfile);
    void stageWriteToggled(bool enabled);
    void terminalToggled();
    void activeTagsChanged(const QStringList& tagIds);
    void configureEndpointRequested();
    void popOutTerminalRequested();
    void expandedToggled(bool expanded);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
