#pragma once

#include "SpellTerminalSurface.h"
#include "../core/SpellModels.h"

#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

#include <memory>

class QEvent;

namespace spell {

class SpellCoreClient;

/**
 * Integrated terminal panel for Spell IDE (`SpellTerminalPanel`).
 *
 * Composes:
 * - A compact header bar displaying the live terminal title, `cols x rows` grid
 *   size indicator, working directory, and Clear / Restart / Pop Out (or Dock
 *   Back) / Close controls.
 * - An inline approval banner shown whenever one or more `TerminalApprovalRequest`
 *   items are pending from the agent, with **Approve & Run** and **Deny** buttons
 *   wired to `SpellCoreClient::respondTerminalApproval()`.
 * - An embedded `SpellTerminalSurface*` that can be reparented seamlessly between
 *   this docked panel and a standalone top-level popped-out window without
 *   interrupting the live PTY shell session.
 */
class SpellTerminalPanel : public QWidget {
    Q_OBJECT

public:
    explicit SpellTerminalPanel(SpellCoreClient* client = nullptr, QWidget* parent = nullptr);
    ~SpellTerminalPanel() override;

    SpellTerminalPanel(const SpellTerminalPanel&) = delete;
    SpellTerminalPanel& operator=(const SpellTerminalPanel&) = delete;
    SpellTerminalPanel(SpellTerminalPanel&&) = delete;
    SpellTerminalPanel& operator=(SpellTerminalPanel&&) = delete;

    /** Attaches or replaces the `SpellCoreClient` used for terminal approval resolution. */
    void attachClient(SpellCoreClient* client);

    /** Returns the live `SpellTerminalSurface` (stays valid across pop-out / dock-back). */
    SpellTerminalSurface* surface() const;

    /**
     * Returns the standalone top-level window hosting the terminal when popped out,
     * or `nullptr` if the pop-out window has not been created yet.
     */
    QWidget* popOutWindow() const;

    /** Returns `true` when the terminal surface is currently hosted in the pop-out window. */
    bool isPoppedOut() const;

    // -------------------------------------------------------------------------
    // Shell lifecycle delegation
    // -------------------------------------------------------------------------

    void startShell(const QString& cwd = QString());
    void ensureShellRunning(const QString& cwd = QString());
    void restartShell(const QString& cwd = QString());
    void stopShell();
    void sendCommandText(const QString& text, bool pressEnter = true);

    bool isShellRunning() const;
    QString workingDirectory() const;
    QString recentOutputText(int maxLines = 120) const;
    int gridCols() const;
    int gridRows() const;

    /** Optional badge explaining why the agent requested or opened the terminal. */
    void setAgentReason(const QString& reason);
    QString agentReason() const;

    // -------------------------------------------------------------------------
    // Pending agent terminal-command approval banners
    // -------------------------------------------------------------------------

    /** Enqueues or updates a pending `TerminalApprovalRequest` banner above the surface. */
    void showApprovalRequest(const TerminalApprovalRequest& request);

    /** Dismisses a specific approval banner by `approvalId`, or all banners if empty. */
    void clearApprovalRequest(const QString& approvalId = QString());

    /** Replaces the list of displayed pending approval banners (e.g. from `WorkspaceSnapshot`). */
    void setPendingApprovals(const QVector<TerminalApprovalRequest>& requests);

    QVector<TerminalApprovalRequest> pendingApprovals() const;
    bool hasPendingApprovals() const;

    /**
     * Executes the requested command in the live terminal, responds via
     * `SpellCoreClient::respondTerminalApproval()`, and emits `approvalResponded()`.
     */
    void approveRequest(const QString& approvalId);

    /**
     * Rejects the requested command without typing into the terminal, responds via
     * `SpellCoreClient::respondTerminalApproval()`, and emits `approvalResponded()`.
     */
    void denyRequest(const QString& approvalId);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    /** Clears the terminal buffer and scrollback. */
    void clearTerminal();

    /** Reparents the live `SpellTerminalSurface*` into a standalone top-level window. */
    void popOut();

    /** Reparents the live `SpellTerminalSurface*` back into this docked panel. */
    void dockBack();

    /** Switches seamlessly between popped-out (`true`) and docked (`false`) modes. */
    void setPoppedOut(bool poppedOut);

    /** Toggles between docked and popped-out presentation. */
    void togglePopOut();

    /** Refreshes header chrome, approval banners, pop-out window, and surface theme styling. */
    void applyTheme();

signals:
    /** Emitted whenever the terminal transitions between docked (`false`) and popped-out (`true`). */
    void popOutToggled(bool poppedOut);

    /** Emitted when the user clicks the panel's Close button. */
    void closeRequested();

    /** Emitted after the user approves or denies a pending `TerminalApprovalRequest`. */
    void approvalResponded(const QString& id, bool approved, const QString& output);

    /** Forwarded from `SpellTerminalSurface::spellHandOffRequested()`. */
    void spellHandOffRequested(const QString& instruction,
                               const QString& recentOutput,
                               const QString& cwd);

    /** Emitted when the embedded shell starts (`true`) or exits (`false`). */
    void shellStateChanged(bool running);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
