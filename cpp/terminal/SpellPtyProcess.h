#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QtGlobal>

#include <memory>

namespace spell {

/**
 * Cross-platform pseudo-terminal (PTY) process runner for Spell IDE.
 *
 * Uses POSIX `forkpty` / `openpty` on Unix-like platforms (macOS, Linux) and
 * Windows ConPTY (`CreatePseudoConsole`) on Windows — all OS-specific handles
 * and headers are isolated inside `SpellPtyProcess.cpp` via PIMPL.
 *
 * Before launching the shell, secret environment variables (`SPELL_API_*`,
 * `OPENAI_API_*`, `ANTHROPIC_API_*`, etc.) are scrubbed so credentials are never
 * exposed to the interactive shell, and terminal capabilities are advertised via
 * `TERM=xterm-256color`, `COLORTERM=truecolor`, and `TERM_PROGRAM=SpellIDE`.
 */
class SpellPtyProcess : public QObject {
    Q_OBJECT

public:
    explicit SpellPtyProcess(QObject* parent = nullptr);
    ~SpellPtyProcess() override;

    SpellPtyProcess(const SpellPtyProcess&) = delete;
    SpellPtyProcess& operator=(const SpellPtyProcess&) = delete;
    SpellPtyProcess(SpellPtyProcess&&) = delete;
    SpellPtyProcess& operator=(SpellPtyProcess&&) = delete;

    /**
     * Spawns an interactive shell attached to a PTY of `cols x rows` cells.
     *
     * - `workingDirectory`: initial working directory (falls back to user home
     *   if empty or non-existent).
     * - `shellPath`: explicit shell executable path (falls back to
     *   `defaultShellPath()` when empty).
     * - `cols`, `rows`: initial PTY window size (clamped to >= 2).
     *
     * If a shell is already running, it is stopped first. Returns `true` if the
     * child process started successfully; otherwise populates `lastError()` and
     * emits `errorOccurred()`.
     */
    bool start(const QString& workingDirectory = QString(),
               const QString& shellPath = QString(),
               int cols = 80,
               int rows = 24);

    /** Terminates the running shell cleanly (graceful hangup, then forced kill). */
    void stop();

    /** Writes raw UTF-8 / VT input bytes to the PTY master stream. */
    void writeData(const QByteArray& bytes);

    /** Notifies the PTY kernel driver of a new terminal grid size (`cols x rows`). */
    void resize(int cols, int rows);

    /** Sends an interrupt (`Ctrl-C` / `0x03`, or process-group signal when emulated). */
    void sendInterrupt();

    /** Returns `true` while the PTY child process is alive and attached. */
    bool isRunning() const;

    /** Returns the working directory associated with the current session. */
    QString workingDirectory() const;

    /** Returns the resolved shell executable path for the current or last session. */
    QString shellPath() const;

    /** Returns the last startup or I/O error message, or an empty string if none. */
    QString lastError() const;

    /** Returns the OS process identifier of the running child, or `0` if stopped. */
    qint64 processId() const;

    // -------------------------------------------------------------------------
    // Portable environment & shell resolution helpers
    // -------------------------------------------------------------------------

    /**
     * Resolves the platform default interactive shell executable
     * (e.g. `$SHELL` / `/bin/zsh` / `/bin/bash` / `/bin/sh` on POSIX, or
     * `pwsh.exe` / `powershell.exe` / `%COMSPEC%` / `cmd.exe` on Windows).
     */
    static QString defaultShellPath();

    /**
     * Returns `true` if `key` matches a credential environment variable that must
     * be scrubbed before spawning the interactive shell (`SPELL_API_*`,
     * `OPENAI_API_*`, `ANTHROPIC_API_*`, etc.).
     */
    static bool isSecretEnvironmentKey(const QString& key);

    /**
     * Produces a sanitized copy of `base` with all secret variables removed and
     * `TERM=xterm-256color`, `COLORTERM=truecolor`, `TERM_PROGRAM=SpellIDE` set.
     */
    static QProcessEnvironment buildSanitizedEnvironment(
        const QProcessEnvironment& base = QProcessEnvironment::systemEnvironment());

signals:
    /** Emitted on the Qt event loop whenever output bytes arrive from the PTY. */
    void dataReceived(const QByteArray& chunk);

    /** Emitted once when the child shell terminates with exit status `exitCode`. */
    void processExited(int exitCode);

    /** Emitted when spawning or communicating with the PTY fails. */
    void errorOccurred(const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace spell
