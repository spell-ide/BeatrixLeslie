#include "settings/AboutSettingsPane.h"

#include "theme/SpellIcons.h"
#include "theme/SpellTheme.h"
#include "widgets/SpellBadges.h"

#include <Qsci/qsciglobal.h>

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QSysInfo>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>

namespace spell {

namespace {

constexpr auto kBundleIdentifier = "com.spell.SpellIDE";
constexpr auto kFallbackVersion = "1.0.0";

QString resolvedAppVersion() {
    const QString appVersion = QCoreApplication::applicationVersion().trimmed();
    return appVersion.isEmpty() ? QString::fromLatin1(kFallbackVersion) : appVersion;
}

QString qscintillaVersionSummary() {
#ifdef QSCINTILLA_VERSION_STR
    return QStringLiteral("QScintilla %1").arg(QString::fromLatin1(QSCINTILLA_VERSION_STR));
#else
    return QStringLiteral("QScintilla (Qt 6)");
#endif
}

QString platformSummary() {
    const QString osName = QSysInfo::prettyProductName().trimmed();
    const QString arch = QSysInfo::currentCpuArchitecture().trimmed();
    if (!osName.isEmpty() && !arch.isEmpty()) {
        return QStringLiteral("%1 (%2)").arg(osName, arch);
    }
    if (!osName.isEmpty()) {
        return osName;
    }
    if (!arch.isEmpty()) {
        return arch;
    }
    return QStringLiteral("Cross-platform desktop");
}

QString formatByteSize(qint64 bytes) {
    if (bytes < 0) {
        return {};
    }
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    const double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        return QStringLiteral("%1 KB").arg(QString::number(kb, 'f', kb >= 100.0 ? 0 : 1));
    }
    const double mb = kb / 1024.0;
    return QStringLiteral("%1 MB").arg(QString::number(mb, 'f', 1));
}

} // namespace

struct AboutSettingsPane::Impl {
    QFrame* brandCard = nullptr;
    QLabel* logoLabel = nullptr;
    QLabel* titleLabel = nullptr;
    SpellBadge* editionBadge = nullptr;
    QLabel* taglineLabel = nullptr;
    QLabel* hatMarkLabel = nullptr;
    QLabel* versionLabel = nullptr;
    QLabel* copyrightLabel = nullptr;

    SpellSectionHeader* stackHeader = nullptr;
    QFrame* stackCard = nullptr;
    QVector<SpellValueRow*> stackRows;
    QVector<SpellHairline*> stackHairlines;

    SpellSectionHeader* licensesHeader = nullptr;
    QFrame* licensesCard = nullptr;
    QFrame* licensesToolbar = nullptr;
    QLabel* licenseSourceLabel = nullptr;
    QPushButton* copyButton = nullptr;
    QPushButton* openFileButton = nullptr;
    QPlainTextEdit* licensesViewer = nullptr;

    QString licenseFilePath;
    QString licenseText;
    bool licensesLoaded = false;
    uint64_t copyFeedbackGen = 0;

    void loadLicenses();
};

void AboutSettingsPane::Impl::loadLicenses() {
    licenseFilePath.clear();
    licenseText.clear();
    licensesLoaded = false;

    const QStringList candidates = {
        QStringLiteral("THIRD_PARTY_LICENSES.txt"),
        QStringLiteral("THIRD_PARTY_NOTICES.md"),
        QStringLiteral("LICENSE")
    };

    for (const QString& candidate : candidates) {
        const QString foundPath = icons::findResourceFile(candidate);
        if (foundPath.isEmpty()) {
            continue;
        }
        QFile file(foundPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QByteArray raw = file.readAll();
        const QString text = QString::fromUtf8(raw);
        if (text.trimmed().isEmpty()) {
            continue;
        }

        const QFileInfo fi(foundPath);
        licenseFilePath = fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath();
        licenseText = text;
        licensesLoaded = true;

        licensesViewer->setPlainText(licenseText);
        licensesViewer->moveCursor(QTextCursor::Start);

        const QString sizeStr = formatByteSize(fi.size());
        if (!sizeStr.isEmpty()) {
            licenseSourceLabel->setText(
                QStringLiteral("%1  \u00B7  %2").arg(fi.fileName(), sizeStr));
        } else {
            licenseSourceLabel->setText(fi.fileName());
        }
        licenseSourceLabel->setToolTip(QDir::toNativeSeparators(licenseFilePath));

        copyButton->setEnabled(true);
        openFileButton->setEnabled(true);
        openFileButton->setToolTip(
            QStringLiteral("Open %1").arg(QDir::toNativeSeparators(licenseFilePath)));
        return;
    }

    licensesViewer->setPlainText(QStringLiteral(
        "Open-source licenses and notices are packaged with BeatrixLeslie IDE in "
        "THIRD_PARTY_LICENSES.txt (or THIRD_PARTY_NOTICES.md).\n\n"
        "No license file was found in the active application resource paths."));
    licensesViewer->moveCursor(QTextCursor::Start);

    licenseSourceLabel->setText(QStringLiteral("THIRD_PARTY_LICENSES.txt not found"));
    licenseSourceLabel->setToolTip(
        QStringLiteral("Expected in Resources/THIRD_PARTY_LICENSES.txt"));

    copyButton->setEnabled(false);
    openFileButton->setEnabled(false);
    openFileButton->setToolTip(QStringLiteral("License file not available on disk"));
}

AboutSettingsPane::AboutSettingsPane(QWidget* parent)
    : QWidget(parent)
    , m_impl(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("aboutSettingsPane"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(14);

    // -------------------------------------------------------------------------
    // 1. Centered BeatrixLeslie IDE Brand Header Card
    // -------------------------------------------------------------------------
    m_impl->brandCard = new QFrame(this);
    m_impl->brandCard->setObjectName(QStringLiteral("aboutBrandCard"));
    m_impl->brandCard->setFrameShape(QFrame::NoFrame);
    m_impl->brandCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* brandLayout = new QVBoxLayout(m_impl->brandCard);
    brandLayout->setContentsMargins(16, 14, 16, 14);
    brandLayout->setSpacing(5);
    brandLayout->setAlignment(Qt::AlignHCenter);

    m_impl->logoLabel = new QLabel(m_impl->brandCard);
    m_impl->logoLabel->setObjectName(QStringLiteral("aboutLogoLabel"));
    m_impl->logoLabel->setFixedSize(72, 72);
    m_impl->logoLabel->setAlignment(Qt::AlignCenter);
    m_impl->logoLabel->setAccessibleName(QStringLiteral("BeatrixLeslie IDE Logo"));
    brandLayout->addWidget(m_impl->logoLabel, 0, Qt::AlignHCenter);
    brandLayout->addSpacing(3);

    auto* titleRow = new QHBoxLayout();
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleRow->setAlignment(Qt::AlignCenter);

    m_impl->titleLabel = new QLabel(QStringLiteral("BeatrixLeslie IDE"), m_impl->brandCard);
    m_impl->titleLabel->setObjectName(QStringLiteral("aboutTitleLabel"));
    QFont titleFont = m_impl->titleLabel->font();
    titleFont.setPointSizeF(17.0);
    titleFont.setWeight(QFont::Bold);
    m_impl->titleLabel->setFont(titleFont);
    titleRow->addWidget(m_impl->titleLabel, 0, Qt::AlignVCenter);

    m_impl->editionBadge = new SpellBadge(
        QStringLiteral("STANDALONE"), BadgeTone::Accent, m_impl->brandCard);
    titleRow->addWidget(m_impl->editionBadge, 0, Qt::AlignVCenter);
    brandLayout->addLayout(titleRow);

    m_impl->taglineLabel = new QLabel(
        QStringLiteral("Standalone endpoint-agnostic AI coding IDE for any OpenAI-compatible model"),
        m_impl->brandCard);
    m_impl->taglineLabel->setObjectName(QStringLiteral("aboutTaglineLabel"));
    m_impl->taglineLabel->setAlignment(Qt::AlignCenter);
    m_impl->taglineLabel->setWordWrap(true);
    QFont taglineFont = m_impl->taglineLabel->font();
    taglineFont.setPointSizeF(11.5);
    m_impl->taglineLabel->setFont(taglineFont);
    brandLayout->addWidget(m_impl->taglineLabel);

    auto* metaRow = new QHBoxLayout();
    metaRow->setContentsMargins(0, 1, 0, 0);
    metaRow->setSpacing(6);
    metaRow->setAlignment(Qt::AlignCenter);

    m_impl->hatMarkLabel = new QLabel(m_impl->brandCard);
    m_impl->hatMarkLabel->setObjectName(QStringLiteral("aboutHatMarkLabel"));
    m_impl->hatMarkLabel->setFixedSize(15, 15);
    m_impl->hatMarkLabel->setAlignment(Qt::AlignCenter);
    metaRow->addWidget(m_impl->hatMarkLabel, 0, Qt::AlignVCenter);

    m_impl->versionLabel = new QLabel(
        QStringLiteral("Version %1  \u00B7  %2")
            .arg(resolvedAppVersion(), QString::fromLatin1(kBundleIdentifier)),
        m_impl->brandCard);
    m_impl->versionLabel->setObjectName(QStringLiteral("aboutVersionLabel"));
    m_impl->versionLabel->setAlignment(Qt::AlignCenter);
    m_impl->versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metaRow->addWidget(m_impl->versionLabel, 0, Qt::AlignVCenter);
    brandLayout->addLayout(metaRow);

    m_impl->copyrightLabel = new QLabel(
        QStringLiteral("Copyright \u00A9 2026 BeatrixLeslie IDE."),
        m_impl->brandCard);
    m_impl->copyrightLabel->setObjectName(QStringLiteral("aboutCopyrightLabel"));
    m_impl->copyrightLabel->setAlignment(Qt::AlignCenter);
    QFont copyrightFont = m_impl->copyrightLabel->font();
    copyrightFont.setPointSizeF(10.0);
    m_impl->copyrightLabel->setFont(copyrightFont);
    brandLayout->addWidget(m_impl->copyrightLabel);

    rootLayout->addWidget(m_impl->brandCard);

    // -------------------------------------------------------------------------
    // 2. Concise Portable Runtime Stack Summary
    // -------------------------------------------------------------------------
    auto* stackSection = new QVBoxLayout();
    stackSection->setContentsMargins(0, 0, 0, 0);
    stackSection->setSpacing(6);

    m_impl->stackHeader = new SpellSectionHeader(
        QStringLiteral("Portable Stack"), QStringLiteral("cpu"), BadgeTone::Accent, this);
    stackSection->addWidget(m_impl->stackHeader);

    m_impl->stackCard = new QFrame(this);
    m_impl->stackCard->setObjectName(QStringLiteral("aboutStackCard"));
    m_impl->stackCard->setFrameShape(QFrame::NoFrame);
    m_impl->stackCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* stackCardLayout = new QVBoxLayout(m_impl->stackCard);
    stackCardLayout->setContentsMargins(12, 8, 12, 8);
    stackCardLayout->setSpacing(5);

    auto addStackRow = [&](const QString& label, const QString& value) {
        if (!m_impl->stackRows.isEmpty()) {
            auto* line = new SpellHairline(Qt::Horizontal, m_impl->stackCard);
            m_impl->stackHairlines.append(line);
            stackCardLayout->addWidget(line);
        }
        auto* row = new SpellValueRow(label, value, m_impl->stackCard);
        row->setValueIsPrimary(true);
        m_impl->stackRows.append(row);
        stackCardLayout->addWidget(row);
    };

    addStackRow(QStringLiteral("Core Engine"),
                QStringLiteral("Rust native core (spell_core \u00B7 SQLite3)"));
    addStackRow(QStringLiteral("UI Framework"),
                QStringLiteral("C++20 \u00B7 Qt %1").arg(QString::fromLatin1(qVersion())));
    addStackRow(QStringLiteral("Code Editor"), qscintillaVersionSummary());
    addStackRow(QStringLiteral("Platform"), platformSummary());

    stackSection->addWidget(m_impl->stackCard);
    rootLayout->addLayout(stackSection);

    // -------------------------------------------------------------------------
    // 3. Scrollable Third-Party Licenses Viewer
    // -------------------------------------------------------------------------
    auto* licensesSection = new QVBoxLayout();
    licensesSection->setContentsMargins(0, 0, 0, 0);
    licensesSection->setSpacing(6);

    m_impl->licensesHeader = new SpellSectionHeader(
        QStringLiteral("Third-Party Licenses"), QStringLiteral("file"), BadgeTone::Accent, this);
    m_impl->licensesHeader->setToolTip(
        QStringLiteral("Open-source components included in BeatrixLeslie IDE with their license texts."));
    licensesSection->addWidget(m_impl->licensesHeader);

    m_impl->licensesCard = new QFrame(this);
    m_impl->licensesCard->setObjectName(QStringLiteral("aboutLicensesCard"));
    m_impl->licensesCard->setFrameShape(QFrame::NoFrame);
    m_impl->licensesCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* licensesCardLayout = new QVBoxLayout(m_impl->licensesCard);
    licensesCardLayout->setContentsMargins(0, 0, 0, 0);
    licensesCardLayout->setSpacing(0);

    m_impl->licensesToolbar = new QFrame(m_impl->licensesCard);
    m_impl->licensesToolbar->setObjectName(QStringLiteral("aboutLicensesToolbar"));
    m_impl->licensesToolbar->setFrameShape(QFrame::NoFrame);

    auto* toolbarLayout = new QHBoxLayout(m_impl->licensesToolbar);
    toolbarLayout->setContentsMargins(10, 6, 10, 6);
    toolbarLayout->setSpacing(6);

    m_impl->licenseSourceLabel = new QLabel(m_impl->licensesToolbar);
    m_impl->licenseSourceLabel->setObjectName(QStringLiteral("aboutLicenseSourceLabel"));
    m_impl->licenseSourceLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarLayout->addWidget(m_impl->licenseSourceLabel, 1);

    m_impl->copyButton = new QPushButton(QStringLiteral("Copy"), m_impl->licensesToolbar);
    m_impl->copyButton->setObjectName(QStringLiteral("aboutCopyLicensesButton"));
    m_impl->copyButton->setCursor(Qt::PointingHandCursor);
    m_impl->copyButton->setToolTip(QStringLiteral("Copy full third-party license text to clipboard"));
    toolbarLayout->addWidget(m_impl->copyButton);

    m_impl->openFileButton = new QPushButton(QStringLiteral("Open File"), m_impl->licensesToolbar);
    m_impl->openFileButton->setObjectName(QStringLiteral("aboutOpenLicensesButton"));
    m_impl->openFileButton->setCursor(Qt::PointingHandCursor);
    toolbarLayout->addWidget(m_impl->openFileButton);

    licensesCardLayout->addWidget(m_impl->licensesToolbar);

    m_impl->licensesViewer = new QPlainTextEdit(m_impl->licensesCard);
    m_impl->licensesViewer->setObjectName(QStringLiteral("aboutLicensesViewer"));
    m_impl->licensesViewer->setReadOnly(true);
    m_impl->licensesViewer->setUndoRedoEnabled(false);
    m_impl->licensesViewer->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_impl->licensesViewer->setFrameShape(QFrame::NoFrame);
    m_impl->licensesViewer->setTabStopDistance(24.0);
    m_impl->licensesViewer->setMinimumHeight(180);
    m_impl->licensesViewer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    licensesCardLayout->addWidget(m_impl->licensesViewer, 1);

    licensesSection->addWidget(m_impl->licensesCard, 1);
    rootLayout->addLayout(licensesSection, 1);

    // -------------------------------------------------------------------------
    // Signals & Initial State
    // -------------------------------------------------------------------------
    connect(m_impl->copyButton, &QPushButton::clicked, this, [this]() {
        if (!m_impl->licensesLoaded || m_impl->licenseText.isEmpty()) {
            return;
        }
        if (auto* clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(m_impl->licenseText);
        }
        const uint64_t gen = ++m_impl->copyFeedbackGen;
        m_impl->copyButton->setText(QStringLiteral("Copied"));
        QPointer<QPushButton> btn(m_impl->copyButton);
        QTimer::singleShot(1600, this, [this, btn, gen]() {
            if (btn && m_impl && m_impl->copyFeedbackGen == gen) {
                btn->setText(QStringLiteral("Copy"));
            }
        });
    });

    connect(m_impl->openFileButton, &QPushButton::clicked, this, [this]() {
        if (m_impl->licenseFilePath.isEmpty()) {
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_impl->licenseFilePath));
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });

    m_impl->loadLicenses();
    applyTheme();
}

AboutSettingsPane::~AboutSettingsPane() = default;

void AboutSettingsPane::applyTheme() {
    const auto& tm = ThemeManager::instance();
    const UiPalette u = tm.ui();

    const QColor solidElevated = compositeOver(u.elevatedSurface, u.background);
    const QColor solidSelected = compositeOver(u.selectedFill, u.background);
    const QColor solidBorder = compositeOver(u.subtleBorder, u.background);
    const QColor solidDivider = compositeOver(u.divider, u.background);
    const QColor solidSecondary = compositeOver(u.textSecondary, u.background);
    const QColor solidMuted = compositeOver(u.textMuted, u.background);

    // Re-render logo and re-tint witch hat branding mark for the active palette.
    m_impl->logoLabel->setPixmap(icons::appLogoPixmap(72));
    m_impl->hatMarkLabel->setPixmap(icons::hatMarkPixmap(14, solidSecondary));

    // Re-tint compact action icons.
    m_impl->copyButton->setIcon(icons::vectorIcon(QStringLiteral("copy"), u.textPrimary, 12));
    m_impl->copyButton->setIconSize(QSize(12, 12));
    m_impl->openFileButton->setIcon(
        icons::vectorIcon(QStringLiteral("external-link"), u.textPrimary, 12));
    m_impl->openFileButton->setIconSize(QSize(12, 12));

    // Synchronize monospace fonts with ThemeManager.
    QFont metaFont = tm.editorFont();
    metaFont.setPointSizeF(10.5);
    m_impl->versionLabel->setFont(metaFont);

    QFont sourceFont = tm.editorFont();
    sourceFont.setPointSizeF(10.0);
    m_impl->licenseSourceLabel->setFont(sourceFont);

    QFont licenseFont = tm.editorFont();
    licenseFont.setPointSizeF(std::clamp(tm.editorFontSize() - 2.5, 9.5, 11.5));
    m_impl->licensesViewer->setFont(licenseFont);

    // Apply monochrome surface & border styles scoped strictly by object name.
    const QString style = QStringLiteral(
        "QWidget#aboutSettingsPane {\n"
        "  background-color: %10;\n"
        "}\n"
        "QFrame#aboutBrandCard {\n"
        "  background-color: %1;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 10px;\n"
        "}\n"
        "QLabel#aboutLogoLabel, QLabel#aboutHatMarkLabel {\n"
        "  background-color: transparent;\n"
        "}\n"
        "QLabel#aboutTitleLabel {\n"
        "  color: %3;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QLabel#aboutTaglineLabel {\n"
        "  color: %4;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QLabel#aboutVersionLabel {\n"
        "  color: %4;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QLabel#aboutCopyrightLabel {\n"
        "  color: %5;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QFrame#aboutStackCard {\n"
        "  background-color: %6;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 8px;\n"
        "}\n"
        "QFrame#aboutLicensesCard {\n"
        "  background-color: %6;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 8px;\n"
        "}\n"
        "QFrame#aboutLicensesToolbar {\n"
        "  background-color: %1;\n"
        "  border: none;\n"
        "  border-bottom: 1px solid %7;\n"
        "  border-top-left-radius: 7px;\n"
        "  border-top-right-radius: 7px;\n"
        "}\n"
        "QLabel#aboutLicenseSourceLabel {\n"
        "  color: %4;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QPushButton#aboutCopyLicensesButton, QPushButton#aboutOpenLicensesButton {\n"
        "  background-color: %6;\n"
        "  color: %3;\n"
        "  border: 1px solid %2;\n"
        "  border-radius: 5px;\n"
        "  padding: 2px 8px;\n"
        "  font-size: 11px;\n"
        "  font-weight: 500;\n"
        "}\n"
        "QPushButton#aboutCopyLicensesButton:hover, QPushButton#aboutOpenLicensesButton:hover {\n"
        "  background-color: %8;\n"
        "  border-color: %9;\n"
        "}\n"
        "QPushButton#aboutCopyLicensesButton:disabled, QPushButton#aboutOpenLicensesButton:disabled {\n"
        "  color: %5;\n"
        "  border-color: %7;\n"
        "  background-color: transparent;\n"
        "}\n"
        "QPlainTextEdit#aboutLicensesViewer {\n"
        "  background-color: %6;\n"
        "  color: %3;\n"
        "  border: none;\n"
        "  border-bottom-left-radius: 7px;\n"
        "  border-bottom-right-radius: 7px;\n"
        "  padding: 8px 10px;\n"
        "  selection-background-color: %8;\n"
        "  selection-color: %3;\n"
        "}\n"
    ).arg(
        qssColor(solidElevated),     // %1
        qssColor(solidBorder),       // %2
        qssColor(u.textPrimary),     // %3
        qssColor(solidSecondary),    // %4
        qssColor(solidMuted),        // %5
        qssColor(u.surface),         // %6
        qssColor(solidDivider),      // %7
        qssColor(solidSelected),     // %8
        qssColor(u.selectedBorder),  // %9
        qssColor(u.background)       // %10
    );
    setStyleSheet(style);

    // Propagate theme updates to Spell reusable widgets.
    m_impl->editionBadge->applyTheme();
    m_impl->stackHeader->applyTheme();
    m_impl->licensesHeader->applyTheme();
    for (SpellValueRow* row : m_impl->stackRows) {
        row->applyTheme();
    }
    for (SpellHairline* line : m_impl->stackHairlines) {
        line->applyTheme();
    }
}

} // namespace spell
