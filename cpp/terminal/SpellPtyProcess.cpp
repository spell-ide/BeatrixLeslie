#include "SpellPtyProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1StringView>
#include <QMetaObject>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#elif _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#else
#if defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace spell {

namespace {

constexpr int kMinGridDim = 2;
constexpr int kMaxGridDim = 32767;

int clampGridDim(int value) {
    return std::clamp(value, kMinGridDim, kMaxGridDim);
}

QString expandUserTilde(const QString& rawPath) {
    const QString trimmed = rawPath.trimmed();
    if (trimmed == QLatin1StringView("~")) {
        return QDir::homePath();
    }
    if (trimmed.startsWith(QLatin1StringView("~/"))
        || trimmed.startsWith(QLatin1StringView("~\\"))) {
        return QDir::homePath() + trimmed.mid(1);
    }
    return trimmed;
}

QString resolveWorkingDirectory(const QString& requested) {
    const QString expanded = expandUserTilde(requested);
    if (!expanded.isEmpty()) {
        const QFileInfo info(expanded);
        if (info.exists()) {
            if (info.isDir()) {
                return QDir::cleanPath(info.absoluteFilePath());
            }
            const QString parentDir = info.absolutePath();
            const QFileInfo parentInfo(parentDir);
            if (parentInfo.exists() && parentInfo.isDir()) {
                return QDir::cleanPath(parentInfo.absoluteFilePath());
            }
        }
    }

    const QString home = QDir::homePath();
    const QFileInfo homeInfo(home);
    if (!home.isEmpty() && homeInfo.exists() && homeInfo.isDir()) {
        return QDir::cleanPath(homeInfo.absoluteFilePath());
    }
    return QDir::currentPath();
}

#ifndef _WIN32

bool isExecutableFile(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo info(path);
    return info.exists() && info.isFile() && info.isExecutable();
}

bool isStandardLoginShellName(const QString& baseName) {
    static constexpr const char* kLoginShells[] = {
        "zsh", "bash", "sh", "dash", "ksh", "mksh", "csh", "tcsh", "fish", "ash",
    };
    for (const char* name : kLoginShells) {
        if (baseName == QLatin1StringView(name)) {
            return true;
        }
    }
    return false;
}

bool supportsNoMonitorFlag(const QString& baseName) {
    return baseName == QLatin1StringView("zsh")
        || baseName == QLatin1StringView("bash")
        || baseName == QLatin1StringView("sh")
        || baseName == QLatin1StringView("ksh");
}

int decodeWaitStatus(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

void setCloExec(int fd) {
    if (fd < 0) {
        return;
    }
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void setNonBlocking(int fd) {
    if (fd < 0) {
        return;
    }
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

#else

QString formatWin32Error(DWORD errorCode) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    QString message;
    if (length > 0 && buffer != nullptr) {
        message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
        LocalFree(buffer);
    } else {
        message = QStringLiteral("Win32 error 0x%1").arg(static_cast<qulonglong>(errorCode), 8, 16, QLatin1Char('0'));
    }
    return message;
}

#endif

} // namespace

struct SpellPtyProcess::Impl {
    SpellPtyProcess* owner = nullptr;

    mutable std::mutex metaMutex;
    QString workingDirectory;
    QString shellPath;
    QString lastError;

    mutable std::mutex ioMutex;
    std::atomic<bool> running{false};
    std::atomic<qint64> pidAtomic{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> suppressOutput{false};
    std::atomic<bool> destroying{false};
    std::atomic<bool> reaped{true};
    std::atomic<bool> exitEmitted{true};

    std::thread readerThread;

#ifdef _WIN32
    HPCON hpc = nullptr;
    HANDLE pipeInWrite = INVALID_HANDLE_VALUE;
    HANDLE pipeOutRead = INVALID_HANDLE_VALUE;
    HANDLE processHandle = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    DWORD processIdWin = 0;

    void closePipesLocked() {
        if (pipeInWrite != INVALID_HANDLE_VALUE) {
            CloseHandle(pipeInWrite);
            pipeInWrite = INVALID_HANDLE_VALUE;
        }
        if (pipeOutRead != INVALID_HANDLE_VALUE) {
            CloseHandle(pipeOutRead);
            pipeOutRead = INVALID_HANDLE_VALUE;
        }
    }

    void closePseudoConsoleLocked() {
        if (hpc != nullptr) {
            ClosePseudoConsole(hpc);
            hpc = nullptr;
        }
    }

    void writeRawWin32(const QByteArray& bytes) {
        if (bytes.isEmpty()) {
            return;
        }
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            if (stopRequested.load(std::memory_order_acquire)) {
                return;
            }
            HANDLE handle = INVALID_HANDLE_VALUE;
            {
                std::lock_guard<std::mutex> lock(ioMutex);
                handle = pipeInWrite;
                if (handle == INVALID_HANDLE_VALUE) {
                    return;
                }
                const DWORD chunkLen = static_cast<DWORD>(
                    std::min<qsizetype>(bytes.size() - offset, 65536));
                DWORD written = 0;
                const BOOL ok = WriteFile(
                    handle,
                    bytes.constData() + offset,
                    chunkLen,
                    &written,
                    nullptr);
                if (!ok || written == 0) {
                    return;
                }
                offset += static_cast<qsizetype>(written);
            }
        }
    }
#else
    int masterFd = -1;
    int wakeupPipe[2] = {-1, -1};
    pid_t childPid = -1;
    bool emulatesTerminalSignals = false;

    void closeMasterFd() {
        int fdToClose = -1;
        {
            std::lock_guard<std::mutex> lock(ioMutex);
            fdToClose = masterFd;
            masterFd = -1;
        }
        if (fdToClose >= 0) {
            ::close(fdToClose);
        }
    }

    void closeWakeupPipe() {
        int writeEnd = -1;
        int readEnd = -1;
        {
            std::lock_guard<std::mutex> lock(ioMutex);
            writeEnd = wakeupPipe[1];
            readEnd = wakeupPipe[0];
            wakeupPipe[1] = -1;
            wakeupPipe[0] = -1;
        }
        if (writeEnd >= 0) {
            ::close(writeEnd);
        }
        if (readEnd >= 0) {
            ::close(readEnd);
        }
    }

    void signalWakeup() {
        std::lock_guard<std::mutex> lock(ioMutex);
        if (wakeupPipe[1] >= 0) {
            const char byte = 1;
            const ssize_t ignored = ::write(wakeupPipe[1], &byte, 1);
            (void)ignored;
        }
    }

    void writeRawPosix(const QByteArray& bytes) {
        if (bytes.isEmpty()) {
            return;
        }
        qsizetype offset = 0;
        int spins = 0;
        while (offset < bytes.size()) {
            if (stopRequested.load(std::memory_order_acquire)) {
                return;
            }
            ssize_t written = -1;
            int savedErrno = 0;
            {
                std::lock_guard<std::mutex> lock(ioMutex);
                if (masterFd < 0) {
                    return;
                }
                written = ::write(
                    masterFd,
                    bytes.constData() + offset,
                    static_cast<std::size_t>(bytes.size() - offset));
                savedErrno = errno;
            }
            if (written > 0) {
                offset += static_cast<qsizetype>(written);
                spins = 0;
            } else if (written < 0 && savedErrno == EINTR) {
                continue;
            } else if (written < 0 && (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) && spins < 200) {
                ++spins;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else {
                return;
            }
        }
    }
#endif

    void setErrorAndEmit(const QString& message) {
        {
            std::lock_guard<std::mutex> lock(metaMutex);
            lastError = message;
        }
        if (owner != nullptr && !destroying.load(std::memory_order_acquire)) {
            emit owner->errorOccurred(message);
        }
    }

    void postData(uint64_t sessionGen, QByteArray chunk) {
        if (chunk.isEmpty() || owner == nullptr) {
            return;
        }
        if (suppressOutput.load(std::memory_order_acquire)
            || destroying.load(std::memory_order_acquire)) {
            return;
        }
        SpellPtyProcess* target = owner;
        QMetaObject::invokeMethod(
            target,
            [target, sessionGen, data = std::move(chunk)]() {
                if (!target->m_impl) {
                    return;
                }
                if (target->m_impl->generation.load(std::memory_order_acquire) != sessionGen) {
                    return;
                }
                if (target->m_impl->suppressOutput.load(std::memory_order_acquire)) {
                    return;
                }
                emit target->dataReceived(data);
            },
            Qt::QueuedConnection);
    }

    void postExit(uint64_t sessionGen, int exitCode) {
        if (owner == nullptr || destroying.load(std::memory_order_acquire)) {
            return;
        }
        if (exitEmitted.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        SpellPtyProcess* target = owner;
        QMetaObject::invokeMethod(
            target,
            [target, sessionGen, exitCode]() {
                if (!target->m_impl) {
                    return;
                }
                if (target->m_impl->generation.load(std::memory_order_acquire) != sessionGen) {
                    return;
                }
                emit target->processExited(exitCode);
            },
            Qt::QueuedConnection);
    }

    void joinReaderThread() {
        if (readerThread.joinable()) {
            if (readerThread.get_id() == std::this_thread::get_id()) {
                readerThread.detach();
            } else {
                readerThread.join();
            }
        }
    }

    void stopInternal(bool fromDestructor) {
        if (fromDestructor) {
            destroying.store(true, std::memory_order_release);
        }
        stopRequested.store(true, std::memory_order_release);
        suppressOutput.store(true, std::memory_order_release);

#ifdef _WIN32
        if (stopEvent != nullptr) {
            SetEvent(stopEvent);
        }

        joinReaderThread();

        HANDLE procToClose = INVALID_HANDLE_VALUE;
        {
            std::lock_guard<std::mutex> lock(ioMutex);
            closePipesLocked();
            closePseudoConsoleLocked();
            procToClose = processHandle;
            processHandle = INVALID_HANDLE_VALUE;
            processIdWin = 0;
        }

        int exitCode = 0;
        if (procToClose != INVALID_HANDLE_VALUE) {
            if (!reaped.load(std::memory_order_acquire)) {
                DWORD waitResult = WaitForSingleObject(procToClose, 250);
                if (waitResult == WAIT_TIMEOUT) {
                    TerminateProcess(procToClose, 1);
                    WaitForSingleObject(procToClose, 1000);
                }
                DWORD winExitCode = 0;
                if (GetExitCodeProcess(procToClose, &winExitCode)) {
                    exitCode = static_cast<int>(winExitCode);
                } else {
                    exitCode = -1;
                }
                reaped.store(true, std::memory_order_release);
            }
            CloseHandle(procToClose);
        }

        if (stopEvent != nullptr) {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }

        running.store(false, std::memory_order_release);
        pidAtomic.store(0, std::memory_order_release);

        if (!fromDestructor) {
            postExit(generation.load(std::memory_order_acquire), exitCode);
        }
#else
        pid_t pidToStop = -1;
        {
            std::lock_guard<std::mutex> lock(ioMutex);
            pidToStop = childPid;
        }

        // Wake the reader thread immediately so join() never blocks on poll().
        signalWakeup();

        // Send graceful hangup to the session process group before joining.
        if (pidToStop > 0 && !reaped.load(std::memory_order_acquire)) {
            (void)::kill(-pidToStop, SIGHUP);
            (void)::kill(pidToStop, SIGHUP);
        }

        joinReaderThread();

        // Now no background thread can touch masterFd, wakeupPipe, or waitpid().
        closeMasterFd();
        closeWakeupPipe();

        int exitCode = 0;
        if (pidToStop > 0 && !reaped.load(std::memory_order_acquire)) {
            int status = 0;
            bool gotStatus = false;

            // Allow up to ~300 ms for graceful exit (SIGHUP, then SIGTERM at 100 ms).
            for (int attempt = 0; attempt < 30; ++attempt) {
                const pid_t r = ::waitpid(pidToStop, &status, WNOHANG);
                if (r == pidToStop) {
                    gotStatus = true;
                    break;
                }
                if (r < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    // ECHILD: already reaped.
                    gotStatus = true;
                    status = 0;
                    break;
                }
                if (attempt == 10) {
                    (void)::kill(-pidToStop, SIGTERM);
                    (void)::kill(pidToStop, SIGTERM);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (!gotStatus) {
                (void)::kill(-pidToStop, SIGKILL);
                (void)::kill(pidToStop, SIGKILL);
                for (int attempt = 0; attempt < 50; ++attempt) {
                    const pid_t r = ::waitpid(pidToStop, &status, WNOHANG);
                    if (r == pidToStop) {
                        gotStatus = true;
                        break;
                    }
                    if (r < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            exitCode = gotStatus ? decodeWaitStatus(status) : -1;
            reaped.store(true, std::memory_order_release);
        }

        {
            std::lock_guard<std::mutex> lock(ioMutex);
            childPid = -1;
            emulatesTerminalSignals = false;
        }
        running.store(false, std::memory_order_release);
        pidAtomic.store(0, std::memory_order_release);

        if (!fromDestructor) {
            postExit(generation.load(std::memory_order_acquire), exitCode);
        }
#endif
    }
};

SpellPtyProcess::SpellPtyProcess(QObject* parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
}

SpellPtyProcess::~SpellPtyProcess() {
    if (m_impl) {
        m_impl->stopInternal(/*fromDestructor=*/true);
    }
}

bool SpellPtyProcess::start(const QString& workingDirectory,
                            const QString& shellPath,
                            int cols,
                            int rows) {
    // Stop any existing session cleanly before starting a new one.
    stop();

    const uint64_t sessionGen = m_impl->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_impl->stopRequested.store(false, std::memory_order_release);
    m_impl->suppressOutput.store(false, std::memory_order_release);
    m_impl->reaped.store(false, std::memory_order_release);
    m_impl->exitEmitted.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(m_impl->metaMutex);
        m_impl->lastError.clear();
    }

    const int clampedCols = clampGridDim(cols);
    const int clampedRows = clampGridDim(rows);
    const QString resolvedDir = resolveWorkingDirectory(workingDirectory);

    QString resolvedShell;
    const QString requestedShell = expandUserTilde(shellPath);
    if (requestedShell.isEmpty()) {
        resolvedShell = defaultShellPath();
    } else if (requestedShell.contains(QLatin1Char('/'))
               || requestedShell.contains(QLatin1Char('\\'))) {
        resolvedShell = QDir::cleanPath(QFileInfo(requestedShell).absoluteFilePath());
    } else {
        resolvedShell = QStandardPaths::findExecutable(requestedShell);
        if (resolvedShell.isEmpty()) {
            resolvedShell = requestedShell;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->metaMutex);
        m_impl->workingDirectory = resolvedDir;
        m_impl->shellPath = resolvedShell;
    }

    const QProcessEnvironment sanitizedEnv = buildSanitizedEnvironment();

#ifdef _WIN32
    const QFileInfo shellInfo(resolvedShell);
    if (!shellInfo.exists() || !shellInfo.isFile()) {
        const QString found = QStandardPaths::findExecutable(resolvedShell);
        if (!found.isEmpty()) {
            resolvedShell = found;
            std::lock_guard<std::mutex> lock(m_impl->metaMutex);
            m_impl->shellPath = resolvedShell;
        } else {
            m_impl->reaped.store(true, std::memory_order_release);
            m_impl->exitEmitted.store(true, std::memory_order_release);
            m_impl->setErrorAndEmit(
                QStringLiteral("Shell executable is not runnable: %1").arg(resolvedShell));
            return false;
        }
    }

    HANDLE inRead = INVALID_HANDLE_VALUE;
    HANDLE inWrite = INVALID_HANDLE_VALUE;
    HANDLE outRead = INVALID_HANDLE_VALUE;
    HANDLE outWrite = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&inRead, &inWrite, nullptr, 0)) {
        const DWORD err = GetLastError();
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("Failed to create PTY input pipe: %1").arg(formatWin32Error(err)));
        return false;
    }
    if (!CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        const DWORD err = GetLastError();
        CloseHandle(inRead);
        CloseHandle(inWrite);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("Failed to create PTY output pipe: %1").arg(formatWin32Error(err)));
        return false;
    }

    COORD consoleSize{};
    consoleSize.X = static_cast<SHORT>(clampedCols);
    consoleSize.Y = static_cast<SHORT>(clampedRows);

    HPCON hpc = nullptr;
    const HRESULT hr = CreatePseudoConsole(consoleSize, inRead, outWrite, 0, &hpc);
    // ConPTY duplicates the pipe ends it needs; close our copies so EOF propagates cleanly.
    CloseHandle(inRead);
    CloseHandle(outWrite);

    if (FAILED(hr) || hpc == nullptr) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("CreatePseudoConsole failed (HRESULT 0x%1)")
                .arg(static_cast<qulonglong>(static_cast<unsigned long>(hr)), 8, 16, QLatin1Char('0')));
        return false;
    }

    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
    if (attrListSize == 0) {
        const DWORD err = GetLastError();
        CloseHandle(outRead);
        CloseHandle(inWrite);
        ClosePseudoConsole(hpc);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("InitializeProcThreadAttributeList sizing failed: %1")
                .arg(formatWin32Error(err)));
        return false;
    }
    std::vector<BYTE> attrListStorage(attrListSize);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrListStorage.data());

    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrListSize)) {
        const DWORD err = GetLastError();
        CloseHandle(outRead);
        CloseHandle(inWrite);
        ClosePseudoConsole(hpc);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("InitializeProcThreadAttributeList failed: %1").arg(formatWin32Error(err)));
        return false;
    }

    if (!UpdateProcThreadAttribute(
            attrList,
            0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            hpc,
            sizeof(hpc),
            nullptr,
            nullptr)) {
        const DWORD err = GetLastError();
        DeleteProcThreadAttributeList(attrList);
        CloseHandle(outRead);
        CloseHandle(inWrite);
        ClosePseudoConsole(hpc);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("UpdateProcThreadAttribute failed: %1").arg(formatWin32Error(err)));
        return false;
    }

    STARTUPINFOEXW siEx{};
    siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    siEx.lpAttributeList = attrList;

    // Build case-insensitively sorted double-null-terminated UTF-16 environment block.
    QStringList envEntries = sanitizedEnv.toStringList();
    std::sort(envEntries.begin(), envEntries.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    std::vector<wchar_t> envBlock;
    for (const QString& entry : envEntries) {
        const std::wstring w = entry.toStdWString();
        envBlock.insert(envBlock.end(), w.begin(), w.end());
        envBlock.push_back(L'\0');
    }
    if (envBlock.empty()) {
        envBlock.push_back(L'\0');
    }
    envBlock.push_back(L'\0');

    QString cmdString = QDir::toNativeSeparators(resolvedShell);
    if (!cmdString.startsWith(QLatin1Char('"'))) {
        cmdString = QLatin1Char('"') + cmdString + QLatin1Char('"');
    }
    const std::wstring cmdWide = cmdString.toStdWString();
    std::vector<wchar_t> cmdLine(cmdWide.begin(), cmdWide.end());
    cmdLine.push_back(L'\0');

    const std::wstring cwdWide = QDir::toNativeSeparators(resolvedDir).toStdWString();

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(),
        cwdWide.empty() ? nullptr : cwdWide.c_str(),
        &siEx.StartupInfo,
        &pi);

    DeleteProcThreadAttributeList(attrList);

    if (!created) {
        const DWORD err = GetLastError();
        CloseHandle(outRead);
        CloseHandle(inWrite);
        ClosePseudoConsole(hpc);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("CreateProcessW failed for '%1': %2")
                .arg(resolvedShell, formatWin32Error(err)));
        return false;
    }

    CloseHandle(pi.hThread);

    HANDLE stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEv == nullptr) {
        const DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(outRead);
        CloseHandle(inWrite);
        ClosePseudoConsole(hpc);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("CreateEventW failed: %1").arg(formatWin32Error(err)));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->ioMutex);
        m_impl->hpc = hpc;
        m_impl->pipeInWrite = inWrite;
        m_impl->pipeOutRead = outRead;
        m_impl->processHandle = pi.hProcess;
        m_impl->stopEvent = stopEv;
        m_impl->processIdWin = pi.dwProcessId;
    }
    m_impl->pidAtomic.store(static_cast<qint64>(pi.dwProcessId), std::memory_order_release);
    m_impl->running.store(true, std::memory_order_release);

    Impl* impl = m_impl.get();
    HANDLE readPipe = outRead;
    HANDLE procHandle = pi.hProcess;
    HANDLE stopWaitHandle = stopEv;

    m_impl->readerThread = std::thread([impl, sessionGen, readPipe, procHandle, stopWaitHandle]() {
        std::vector<char> buffer(65536);

        auto drainAvailable = [&]() -> bool {
            while (!impl->stopRequested.load(std::memory_order_acquire)) {
                DWORD avail = 0;
                if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr)) {
                    return false;
                }
                if (avail == 0) {
                    return true;
                }
                const DWORD toRead = static_cast<DWORD>(
                    std::min<std::size_t>(avail, buffer.size()));
                DWORD bytesRead = 0;
                if (!ReadFile(readPipe, buffer.data(), toRead, &bytesRead, nullptr)) {
                    return false;
                }
                if (bytesRead == 0) {
                    return false;
                }
                impl->postData(
                    sessionGen,
                    QByteArray(buffer.data(), static_cast<qsizetype>(bytesRead)));
            }
            return false;
        };

        HANDLE waitHandles[2] = {procHandle, stopWaitHandle};
        bool processFinished = false;

        while (!impl->stopRequested.load(std::memory_order_acquire)) {
            if (!drainAvailable()) {
                break;
            }
            const DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, 10);
            if (waitRes == WAIT_OBJECT_0) {
                processFinished = true;
                // Allow ConPTY a brief window to flush any final buffered console output.
                for (int flushAttempt = 0; flushAttempt < 3; ++flushAttempt) {
                    if (!drainAvailable()) {
                        break;
                    }
                    if (WaitForSingleObject(stopWaitHandle, 10) == WAIT_OBJECT_0) {
                        break;
                    }
                }
                break;
            }
            if (waitRes == WAIT_OBJECT_0 + 1) {
                break;
            }
        }

        if (impl->stopRequested.load(std::memory_order_acquire)) {
            return;
        }

        if (!processFinished) {
            // Pipe closed before process signaled; wait briefly for final exit code.
            WaitForMultipleObjects(2, waitHandles, FALSE, 250);
        }

        if (impl->stopRequested.load(std::memory_order_acquire)) {
            return;
        }

        DWORD winExitCode = 0;
        int exitCode = 0;
        if (GetExitCodeProcess(procHandle, &winExitCode) && winExitCode != STILL_ACTIVE) {
            exitCode = static_cast<int>(winExitCode);
        } else {
            exitCode = -1;
        }

        impl->reaped.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(impl->ioMutex);
            impl->closePipesLocked();
            impl->closePseudoConsoleLocked();
            impl->processIdWin = 0;
        }
        impl->pidAtomic.store(0, std::memory_order_release);
        impl->running.store(false, std::memory_order_release);
        impl->postExit(sessionGen, exitCode);
    });

    return true;
#else
    if (!isExecutableFile(resolvedShell)) {
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("Shell executable is not runnable: %1").arg(resolvedShell));
        return false;
    }

    const bool inAppSandbox = qEnvironmentVariableIsSet("APP_SANDBOX_CONTAINER_ID");
    const QString baseName = QFileInfo(resolvedShell).fileName();

    // Prepare all child arguments, environment strings, and paths before fork()
    // so the child executes strictly async-signal-safe operations.
    const std::string execPathUtf8 = QFile::encodeName(resolvedShell).toStdString();
    const std::string cwdUtf8 = QFile::encodeName(resolvedDir).toStdString();
    const std::string homeUtf8 =
        QFile::encodeName(sanitizedEnv.value(QStringLiteral("HOME"), QDir::homePath())).toStdString();

    std::vector<std::string> argvStorage;
    if (isStandardLoginShellName(baseName)) {
        argvStorage.push_back(("-" + baseName).toUtf8().toStdString());
    } else {
        argvStorage.push_back(baseName.toUtf8().toStdString());
    }
    if (inAppSandbox && supportsNoMonitorFlag(baseName)) {
        // Under an App Sandbox where TIOCSCTTY is denied, disable job control
        // so the shell does not emit "can't set tty pgrp" warnings on startup.
        argvStorage.push_back("+m");
    }

    std::vector<char*> argvPtrs;
    argvPtrs.reserve(argvStorage.size() + 1);
    for (std::string& arg : argvStorage) {
        argvPtrs.push_back(arg.data());
    }
    argvPtrs.push_back(nullptr);

    QStringList envList = sanitizedEnv.toStringList();
    envList.sort();
    std::vector<std::string> envStorage;
    envStorage.reserve(static_cast<std::size_t>(envList.size()));
    for (const QString& entry : envList) {
        envStorage.push_back(entry.toUtf8().toStdString());
    }
    std::vector<char*> envpPtrs;
    envpPtrs.reserve(envStorage.size() + 1);
    for (std::string& entry : envStorage) {
        envpPtrs.push_back(entry.data());
    }
    envpPtrs.push_back(nullptr);

    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(clampedCols);
    ws.ws_row = static_cast<unsigned short>(clampedRows);
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    int masterFd = -1;
    int slaveFd = -1;
    if (::openpty(&masterFd, &slaveFd, nullptr, nullptr, &ws) != 0) {
        const int err = errno;
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("openpty failed: %1").arg(QString::fromLocal8Bit(std::strerror(err))));
        return false;
    }

    // Enable UTF-8 input erase handling on the PTY line discipline when supported.
    struct termios tio{};
    if (::tcgetattr(slaveFd, &tio) == 0) {
#ifdef IUTF8
        tio.c_iflag |= IUTF8;
#endif
        (void)::tcsetattr(slaveFd, TCSANOW, &tio);
    }

    int wakePipe[2] = {-1, -1};
    if (::pipe(wakePipe) != 0) {
        const int err = errno;
        ::close(masterFd);
        ::close(slaveFd);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("pipe failed: %1").arg(QString::fromLocal8Bit(std::strerror(err))));
        return false;
    }

    setCloExec(masterFd);
    setCloExec(wakePipe[0]);
    setCloExec(wakePipe[1]);
    setNonBlocking(wakePipe[0]);
    setNonBlocking(wakePipe[1]);

    const long openMaxRaw = ::sysconf(_SC_OPEN_MAX);
    const int closeLimit = static_cast<int>(
        std::clamp<long>(openMaxRaw > 0 ? openMaxRaw : 1024L, 64L, 4096L));

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int err = errno;
        ::close(masterFd);
        ::close(slaveFd);
        ::close(wakePipe[0]);
        ::close(wakePipe[1]);
        m_impl->reaped.store(true, std::memory_order_release);
        m_impl->exitEmitted.store(true, std::memory_order_release);
        m_impl->setErrorAndEmit(
            QStringLiteral("fork failed: %1").arg(QString::fromLocal8Bit(std::strerror(err))));
        return false;
    }

    if (pid == 0) {
        // Child process: async-signal-safe syscalls only.
        if (masterFd > STDERR_FILENO) {
            ::close(masterFd);
        }
        if (wakePipe[0] > STDERR_FILENO) {
            ::close(wakePipe[0]);
        }
        if (wakePipe[1] > STDERR_FILENO) {
            ::close(wakePipe[1]);
        }

        // Restore default signal dispositions in the interactive shell.
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTSTP, SIG_DFL);
        ::signal(SIGTTIN, SIG_DFL);
        ::signal(SIGTTOU, SIG_DFL);
        ::signal(SIGCHLD, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGHUP, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);

        (void)::setsid();
#ifdef TIOCSCTTY
        (void)::ioctl(slaveFd, TIOCSCTTY, 0);
#endif

        if (slaveFd != STDIN_FILENO) {
            (void)::dup2(slaveFd, STDIN_FILENO);
        }
        if (slaveFd != STDOUT_FILENO) {
            (void)::dup2(slaveFd, STDOUT_FILENO);
        }
        if (slaveFd != STDERR_FILENO) {
            (void)::dup2(slaveFd, STDERR_FILENO);
        }

        for (int fd = STDERR_FILENO + 1; fd < closeLimit; ++fd) {
            ::close(fd);
        }
        if (slaveFd >= closeLimit) {
            ::close(slaveFd);
        }

        if (!cwdUtf8.empty() && ::chdir(cwdUtf8.c_str()) != 0) {
            if (!homeUtf8.empty()) {
                (void)::chdir(homeUtf8.c_str());
            }
        }

        // Execute shell with sanitized environment (argv[0] = "-<shell>" for login shell).
        ::execve(execPathUtf8.c_str(), argvPtrs.data(), envpPtrs.data());
        ::_exit(127);
    }

    // Parent process.
    ::close(slaveFd);
    setNonBlocking(masterFd);

    {
        std::lock_guard<std::mutex> lock(m_impl->ioMutex);
        m_impl->masterFd = masterFd;
        m_impl->wakeupPipe[0] = wakePipe[0];
        m_impl->wakeupPipe[1] = wakePipe[1];
        m_impl->childPid = pid;
        m_impl->emulatesTerminalSignals = inAppSandbox;
    }
    m_impl->pidAtomic.store(static_cast<qint64>(pid), std::memory_order_release);
    m_impl->running.store(true, std::memory_order_release);

    Impl* impl = m_impl.get();
    const int ptyFd = masterFd;
    const int wakeReadFd = wakePipe[0];

    m_impl->readerThread = std::thread([impl, sessionGen, ptyFd, wakeReadFd, pid]() {
        std::vector<char> buffer(65536);

        auto drainPtyNonBlocking = [&]() -> bool {
            while (!impl->stopRequested.load(std::memory_order_acquire)) {
                const ssize_t n = ::read(ptyFd, buffer.data(), buffer.size());
                if (n > 0) {
                    impl->postData(sessionGen, QByteArray(buffer.data(), static_cast<qsizetype>(n)));
                    if (static_cast<std::size_t>(n) < buffer.size()) {
                        return true;
                    }
                } else if (n == 0) {
                    return false;
                } else {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return true;
                    }
                    // EIO (slave closed) or fatal error.
                    return false;
                }
            }
            return false;
        };

        int status = 0;
        bool reapedLocally = false;

        while (!impl->stopRequested.load(std::memory_order_acquire)) {
            struct pollfd fds[2]{};
            fds[0].fd = ptyFd;
            fds[0].events = POLLIN;
            fds[1].fd = wakeReadFd;
            fds[1].events = POLLIN;

            const int ready = ::poll(fds, 2, 200);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if (ready == 0) {
                // Periodic check catches a shell exit even if a background child keeps the slave open.
                const pid_t r = ::waitpid(pid, &status, WNOHANG);
                if (r == pid) {
                    reapedLocally = true;
                    (void)drainPtyNonBlocking();
                    break;
                }
                if (r < 0 && errno != EINTR) {
                    reapedLocally = true;
                    status = 0;
                    break;
                }
                continue;
            }

            if ((fds[1].revents & POLLIN) != 0) {
                char discard[64];
                while (::read(wakeReadFd, discard, sizeof(discard)) > 0) {
                }
                if (impl->stopRequested.load(std::memory_order_acquire)) {
                    break;
                }
            }

            bool eof = false;
            if ((fds[0].revents & POLLIN) != 0) {
                if (!drainPtyNonBlocking()) {
                    eof = true;
                }
            }

            if (impl->stopRequested.load(std::memory_order_acquire)) {
                break;
            }

            if (eof || (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                // Drain any last buffered bytes before reaping.
                (void)drainPtyNonBlocking();
                break;
            }
        }

        if (impl->stopRequested.load(std::memory_order_acquire)) {
            return;
        }

        // Wait for the child to become reapable while staying interruptible by stop().
        while (!reapedLocally && !impl->stopRequested.load(std::memory_order_acquire)) {
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                reapedLocally = true;
                break;
            }
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
                reapedLocally = true;
                status = 0;
                break;
            }
            struct pollfd wakePoll{};
            wakePoll.fd = wakeReadFd;
            wakePoll.events = POLLIN;
            (void)::poll(&wakePoll, 1, 15);
        }

        if (!reapedLocally) {
            return;
        }

        // Final drain in case trailing output arrived right as the child exited.
        if (!impl->stopRequested.load(std::memory_order_acquire)) {
            (void)drainPtyNonBlocking();
        }

        const int exitCode = decodeWaitStatus(status);
        impl->reaped.store(true, std::memory_order_release);
        impl->closeMasterFd();
        {
            std::lock_guard<std::mutex> lock(impl->ioMutex);
            impl->childPid = -1;
            impl->emulatesTerminalSignals = false;
        }
        impl->pidAtomic.store(0, std::memory_order_release);
        impl->running.store(false, std::memory_order_release);
        impl->postExit(sessionGen, exitCode);
    });

    return true;
#endif
}

void SpellPtyProcess::stop() {
    if (m_impl) {
        m_impl->stopInternal(/*fromDestructor=*/false);
    }
}

void SpellPtyProcess::writeData(const QByteArray& bytes) {
    if (bytes.isEmpty() || !m_impl || !isRunning()) {
        return;
    }

#ifdef _WIN32
    m_impl->writeRawWin32(bytes);
#else
    bool emulateSignals = false;
    pid_t pgid = -1;
    struct termios tio{};
    bool gotTermios = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->ioMutex);
        emulateSignals = m_impl->emulatesTerminalSignals;
        pgid = m_impl->childPid;
        if (emulateSignals && m_impl->masterFd >= 0 && pgid > 0) {
            gotTermios = (::tcgetattr(m_impl->masterFd, &tio) == 0);
        }
    }

    if (emulateSignals && gotTermios && (tio.c_lflag & ISIG) != 0 && pgid > 0) {
        const unsigned char vintr = static_cast<unsigned char>(tio.c_cc[VINTR]);
        const unsigned char vquit = static_cast<unsigned char>(tio.c_cc[VQUIT]);
        const unsigned char vsusp = static_cast<unsigned char>(tio.c_cc[VSUSP]);
        const unsigned char disabled = static_cast<unsigned char>(_POSIX_VDISABLE);

        QByteArray run;
        run.reserve(bytes.size());
        auto flushRun = [&]() {
            if (!run.isEmpty()) {
                m_impl->writeRawPosix(run);
                run.clear();
            }
        };

        for (char ch : bytes) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte == vintr && vintr != disabled) {
                flushRun();
                (void)::killpg(pgid, SIGINT);
            } else if (byte == vquit && vquit != disabled) {
                flushRun();
                (void)::killpg(pgid, SIGQUIT);
            } else if (byte == vsusp && vsusp != disabled) {
                flushRun();
                // Drop VSUSP when controlling-terminal job control is unavailable.
            } else {
                run.append(ch);
            }
        }
        flushRun();
        return;
    }

    m_impl->writeRawPosix(bytes);
#endif
}

void SpellPtyProcess::resize(int cols, int rows) {
    if (!m_impl) {
        return;
    }
    const int clampedCols = clampGridDim(cols);
    const int clampedRows = clampGridDim(rows);

#ifdef _WIN32
    std::lock_guard<std::mutex> lock(m_impl->ioMutex);
    if (m_impl->hpc == nullptr) {
        return;
    }
    COORD size{};
    size.X = static_cast<SHORT>(clampedCols);
    size.Y = static_cast<SHORT>(clampedRows);
    (void)ResizePseudoConsole(m_impl->hpc, size);
#else
    pid_t emulatedPgid = -1;
    {
        std::lock_guard<std::mutex> lock(m_impl->ioMutex);
        if (m_impl->masterFd < 0) {
            return;
        }
        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(clampedCols);
        ws.ws_row = static_cast<unsigned short>(clampedRows);
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        (void)::ioctl(m_impl->masterFd, TIOCSWINSZ, &ws);
        if (m_impl->emulatesTerminalSignals && m_impl->childPid > 0) {
            emulatedPgid = m_impl->childPid;
        }
    }
    if (emulatedPgid > 0) {
        (void)::killpg(emulatedPgid, SIGWINCH);
    }
#endif
}

void SpellPtyProcess::sendInterrupt() {
    if (!m_impl || !isRunning()) {
        return;
    }

#ifndef _WIN32
    {
        std::lock_guard<std::mutex> lock(m_impl->ioMutex);
        if (m_impl->emulatesTerminalSignals && m_impl->masterFd >= 0 && m_impl->childPid > 0) {
            struct termios tio{};
            if (::tcgetattr(m_impl->masterFd, &tio) == 0 && (tio.c_lflag & ISIG) != 0) {
                const pid_t pgid = m_impl->childPid;
                (void)::killpg(pgid, SIGINT);
                return;
            }
        }
    }
#endif

    writeData(QByteArray(1, '\x03'));
}

bool SpellPtyProcess::isRunning() const {
    return m_impl && m_impl->running.load(std::memory_order_acquire);
}

QString SpellPtyProcess::workingDirectory() const {
    if (!m_impl) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_impl->metaMutex);
    return m_impl->workingDirectory;
}

QString SpellPtyProcess::shellPath() const {
    if (!m_impl) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_impl->metaMutex);
    return m_impl->shellPath;
}

QString SpellPtyProcess::lastError() const {
    if (!m_impl) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_impl->metaMutex);
    return m_impl->lastError;
}

qint64 SpellPtyProcess::processId() const {
    if (!m_impl) {
        return 0;
    }
    return m_impl->pidAtomic.load(std::memory_order_acquire);
}

QString SpellPtyProcess::defaultShellPath() {
#ifdef _WIN32
    const QString pwsh = QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
    if (!pwsh.isEmpty()) {
        return pwsh;
    }
    const QString powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (!powershell.isEmpty()) {
        return powershell;
    }
    const QString comspec = qEnvironmentVariable("COMSPEC").trimmed();
    if (!comspec.isEmpty()) {
        const QFileInfo info(comspec);
        if (info.exists() && info.isFile()) {
            return info.absoluteFilePath();
        }
    }
    const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
    if (!cmd.isEmpty()) {
        return cmd;
    }
    return QStringLiteral("cmd.exe");
#else
    const QString envShell = qEnvironmentVariable("SHELL").trimmed();
    if (isExecutableFile(envShell)) {
        return envShell;
    }

    static constexpr const char* kCandidates[] = {
        "/bin/zsh",
        "/usr/bin/zsh",
        "/bin/bash",
        "/usr/bin/bash",
        "/bin/sh",
    };
    for (const char* candidate : kCandidates) {
        const QString path = QString::fromLatin1(candidate);
        if (isExecutableFile(path)) {
            return path;
        }
    }
    return QStringLiteral("/bin/sh");
#endif
}

bool SpellPtyProcess::isSecretEnvironmentKey(const QString& key) {
    const QString upper = key.trimmed().toUpper();
    if (upper.isEmpty()) {
        return false;
    }

    static constexpr const char* kSecretPrefixes[] = {
        "SPELL_API_",
        "OPENAI_API_",
        "ANTHROPIC_API_",
        "BEATRIX_API_",
        "GEMINI_API_",
        "OPENROUTER_API_",
    };
    for (const char* prefix : kSecretPrefixes) {
        if (upper.startsWith(QLatin1StringView(prefix))) {
            return true;
        }
    }

    static constexpr const char* kExactSecretKeys[] = {
        "SPELL_API_KEY",
        "OPENAI_API_KEY",
        "ANTHROPIC_API_KEY",
        "SPELL_AUTH_TOKEN",
        "SPELL_SECRET_KEY",
    };
    for (const char* exact : kExactSecretKeys) {
        if (upper == QLatin1StringView(exact)) {
            return true;
        }
    }

    return false;
}

QProcessEnvironment SpellPtyProcess::buildSanitizedEnvironment(const QProcessEnvironment& base) {
    QProcessEnvironment sanitized = base;

    const QStringList keys = sanitized.keys();
    for (const QString& key : keys) {
        if (isSecretEnvironmentKey(key)) {
            sanitized.remove(key);
        }
    }

    // Clear host terminal session identifiers so interactive shells start cleanly.
    sanitized.remove(QStringLiteral("TERM_SESSION_ID"));

    // Advertise Spell IDE 256-color / truecolor terminal capabilities.
    sanitized.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    sanitized.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    sanitized.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("SpellIDE"));

    const QString appVersion = QCoreApplication::applicationVersion().trimmed();
    sanitized.insert(
        QStringLiteral("TERM_PROGRAM_VERSION"),
        appVersion.isEmpty() ? QStringLiteral("2.0") : appVersion);

#ifndef _WIN32
    if (!sanitized.contains(QStringLiteral("LANG"))
        || sanitized.value(QStringLiteral("LANG")).trimmed().isEmpty()) {
        sanitized.insert(QStringLiteral("LANG"), QStringLiteral("en_US.UTF-8"));
    }
    if (!sanitized.contains(QStringLiteral("HOME"))
        || sanitized.value(QStringLiteral("HOME")).trimmed().isEmpty()) {
        const QString home = QDir::homePath();
        if (!home.isEmpty()) {
            sanitized.insert(QStringLiteral("HOME"), home);
        }
    }
    if (!sanitized.contains(QStringLiteral("PATH"))
        || sanitized.value(QStringLiteral("PATH")).trimmed().isEmpty()) {
        sanitized.insert(
            QStringLiteral("PATH"),
            QStringLiteral("/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"));
    }
#endif

    return sanitized;
}

} // namespace spell
