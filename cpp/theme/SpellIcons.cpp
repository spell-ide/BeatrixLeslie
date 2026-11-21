#include "theme/SpellIcons.h"

#include <type_traits>

// All public Spell IDE icon and asset utilities in `spell::icons` (`findResourceFile`,
// `appLogoPixmap`, `appWindowIcon`, `hatMarkPixmap`, and `vectorIcon`) are defined
// inline in `theme/SpellIcons.h` so callers can consume them directly without link-order
// coupling. This translation unit verifies standalone header compilation and API
// signatures, and provides a non-empty archive anchor for static library builds.

namespace spell::icons {

static_assert(std::is_same_v<decltype(&findResourceFile), QString (*)(const QString&)>);
static_assert(std::is_same_v<decltype(&appLogoPixmap), QPixmap (*)(int)>);
static_assert(std::is_same_v<decltype(&appWindowIcon), QIcon (*)()>);
static_assert(std::is_same_v<decltype(&hatMarkPixmap), QPixmap (*)(int, const QColor&)>);
static_assert(std::is_same_v<decltype(&vectorIcon), QIcon (*)(const QString&, const QColor&, int)>);

namespace detail {

int spellIconsTranslationUnitAnchor() noexcept;

int spellIconsTranslationUnitAnchor() noexcept {
    return 1;
}

} // namespace detail

} // namespace spell::icons
