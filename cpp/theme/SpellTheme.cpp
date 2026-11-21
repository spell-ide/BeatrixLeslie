#include "theme/SpellTheme.h"

#include <array>
#include <tuple>
#include <type_traits>

// All public Spell IDE monochrome UI palettes (`UiPalette`), syntax code themes
// (`CodePalette`: One Dark, GitHub Dark, Dracula, Monokai, Solarized Dark, GitHub Light,
// Xcode Light, Solarized Light), integrated terminal palettes (`TerminalPalette`:
// Message Terminal, Classic Dark, Classic Light, Follow Code Theme), Qt application
// stylesheet/palette builders (`buildApplicationStyleSheet`, `applyToApplication`), and
// persisted `ThemeManager` singleton methods (`QSettings("Spell", "SpellIDE")`) are
// defined inline in `theme/SpellTheme.h`.
//
// `ThemeManager`'s `Q_OBJECT` meta-object code (`themeChanged()`, `terminalThemeChanged()`)
// is generated automatically by CMake AUTOMOC from `theme/SpellTheme.h`.
//
// This translation unit verifies standalone header compilation, exact API signatures,
// enumerator coverage, and provides a non-empty archive anchor for static library builds.

namespace spell {

static_assert(std::is_enum_v<Appearance>);
static_assert(std::is_enum_v<CodeThemeId>);
static_assert(std::is_enum_v<TerminalThemeId>);

static_assert(std::array<Appearance, 2>{
                  Appearance::Dark,
                  Appearance::Light}
                  .size() == 2);

static_assert(std::array<CodeThemeId, 8>{
                  CodeThemeId::OneDark,
                  CodeThemeId::GitHubDark,
                  CodeThemeId::Dracula,
                  CodeThemeId::Monokai,
                  CodeThemeId::SolarizedDark,
                  CodeThemeId::GitHubLight,
                  CodeThemeId::XcodeLight,
                  CodeThemeId::SolarizedLight}
                  .size() == 8);

static_assert(std::array<TerminalThemeId, 4>{
                  TerminalThemeId::MessageTerminal,
                  TerminalThemeId::ClassicDark,
                  TerminalThemeId::ClassicLight,
                  TerminalThemeId::FollowCodeTheme}
                  .size() == 4);

static_assert(std::tuple_size_v<decltype(TerminalPalette::ansi)> == 16);
static_assert(std::is_base_of_v<QObject, ThemeManager>);
static_assert(!std::is_copy_constructible_v<ThemeManager>);
static_assert(!std::is_copy_assignable_v<ThemeManager>);

// Color and palette helper signatures
static_assert(std::is_same_v<decltype(&rgbHex), QColor (*)(uint32_t, double)>);
static_assert(std::is_same_v<decltype(&withAlpha), QColor (*)(const QColor&, double)>);
static_assert(std::is_same_v<decltype(&compositeOver), QColor (*)(const QColor&, const QColor&)>);
static_assert(std::is_same_v<decltype(&qssColor), QString (*)(const QColor&)>);
static_assert(std::is_same_v<decltype(&isLightCodeTheme), bool (*)(CodeThemeId)>);

static_assert(std::is_same_v<decltype(&UiPalette::dark), UiPalette (*)()>);
static_assert(std::is_same_v<decltype(&UiPalette::light), UiPalette (*)()>);
static_assert(std::is_same_v<decltype(&UiPalette::forAppearance), UiPalette (*)(Appearance)>);

static_assert(std::is_same_v<decltype(&CodePalette::codePaletteFor),
                             CodePalette (*)(CodeThemeId, Appearance)>);
static_assert(std::is_same_v<decltype(&codePaletteFor),
                             CodePalette (*)(CodeThemeId, Appearance)>);

static_assert(std::is_same_v<decltype(&TerminalPalette::ansiFromHex),
                             std::array<QColor, 16> (*)(const std::array<uint32_t, 16>&)>);
static_assert(std::is_same_v<decltype(&TerminalPalette::terminalPaletteFor),
                             TerminalPalette (*)(TerminalThemeId, CodeThemeId, Appearance)>);
static_assert(std::is_same_v<decltype(&terminalPaletteFor),
                             TerminalPalette (*)(TerminalThemeId, CodeThemeId, Appearance)>);

// ThemeManager API & signal signatures
static_assert(std::is_same_v<decltype(&ThemeManager::instance), ThemeManager& (*)()>);
static_assert(std::is_same_v<decltype(&ThemeManager::appearance),
                             Appearance (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::setAppearance),
                             void (ThemeManager::*)(Appearance)>);
static_assert(std::is_same_v<decltype(&ThemeManager::codeTheme),
                             CodeThemeId (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::setCodeTheme),
                             void (ThemeManager::*)(CodeThemeId)>);
static_assert(std::is_same_v<decltype(&ThemeManager::terminalTheme),
                             TerminalThemeId (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::setTerminalTheme),
                             void (ThemeManager::*)(TerminalThemeId)>);
static_assert(std::is_same_v<decltype(&ThemeManager::editorFontSize),
                             double (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::setEditorFontSize),
                             void (ThemeManager::*)(double)>);
static_assert(std::is_same_v<decltype(&ThemeManager::terminalFontSize),
                             double (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::setTerminalFontSize),
                             void (ThemeManager::*)(double)>);
static_assert(std::is_same_v<decltype(&ThemeManager::editorFontFamily),
                             QString (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::setEditorFontFamily),
                             void (ThemeManager::*)(const QString&)>);
static_assert(std::is_same_v<decltype(&ThemeManager::editorFont),
                             QFont (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::terminalFont),
                             QFont (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::ui),
                             UiPalette (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::code),
                             CodePalette (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::terminal),
                             TerminalPalette (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::buildApplicationStyleSheet),
                             QString (ThemeManager::*)() const>);
static_assert(std::is_same_v<decltype(&ThemeManager::applyToApplication),
                             void (ThemeManager::*)(QApplication*) const>);
static_assert(std::is_same_v<decltype(&ThemeManager::appearanceName),
                             QString (*)(Appearance)>);
static_assert(std::is_same_v<decltype(&ThemeManager::codeThemeName),
                             QString (*)(CodeThemeId)>);
static_assert(std::is_same_v<decltype(&ThemeManager::terminalThemeName),
                             QString (*)(TerminalThemeId)>);
static_assert(std::is_same_v<decltype(&ThemeManager::allCodeThemes),
                             QVector<CodeThemeId> (*)()>);
static_assert(std::is_same_v<decltype(&ThemeManager::allTerminalThemes),
                             QVector<TerminalThemeId> (*)()>);
static_assert(std::is_same_v<decltype(&ThemeManager::themeChanged),
                             void (ThemeManager::*)()>);
static_assert(std::is_same_v<decltype(&ThemeManager::terminalThemeChanged),
                             void (ThemeManager::*)()>);

namespace detail {

int spellThemeTranslationUnitAnchor() noexcept;

int spellThemeTranslationUnitAnchor() noexcept {
    return 1;
}

} // namespace detail

} // namespace spell
