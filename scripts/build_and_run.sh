#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="build"
BUILD_TYPE="Release"
TARGET_ARCH=""
STRICT_WARNINGS=0
NO_WERROR=0
RUN_TESTS=0
VERIFY_LOADER=0
DEPLOY_QT=0
INSTALL_PREFIX=""
APPIMAGE_OUTPUT=""
CODESIGN_IDENTITY="${SPELL_CODESIGN_IDENTITY:--}"
NOTARIZE_PROFILE="${SPELL_NOTARIZE_PROFILE:-}"
RUN_APP=0
RUN_ARGS=()

usage() {
    cat <<'EOF'
Usage: scripts/build_and_run.sh [options] [-- <app-args...>]

Build, test, bundle Qt runtimes, sign, verify dynamic loader paths, package, and launch BeatrixLeslie IDE.

Options:
  --build-dir <dir>            CMake build directory (default: build)
  --build-type <type>          CMake build configuration: Release, Debug, RelWithDebInfo (default: Release)
  --debug                      Shorthand for --build-type Debug
  --release                    Shorthand for --build-type Release
  --arch <arch>                macOS target architecture: arm64, x86_64, or universal2
  --strict-warnings            Enforce C++ -Werror and Rust -D warnings
  --no-werror                  Disable C++ warnings-as-errors (-DSPELL_WARNINGS_AS_ERRORS=OFF)
  --test                       Run Rust unit tests (ephemeral keyring) and CTest Qt tests
  --deploy-qt                  Bundle self-contained Qt & QScintilla2 runtime (macOS .app or Linux --install prefix)
  --package-appimage <path>    Package deployed Linux prefix into a self-contained x86_64 AppImage
  --codesign-identity <id>     macOS codesign identity (default: $SPELL_CODESIGN_IDENTITY or '-' ad-hoc)
  --notarize-profile <name>    macOS notarytool keychain profile (default: $SPELL_NOTARIZE_PROFILE)
  --verify-loader              Verify dynamic library loader paths, RPATHs, and headless startup
  --install <prefix>           Install portable application layout into <prefix>
  --run                        Launch the built BeatrixLeslieIDE executable
  -h, --help                   Show this help message and exit
EOF
}

resolve_path() {
    local raw="$1"
    if [[ "${raw}" == /* ]]; then
        printf '%s\n' "${raw}"
    elif [[ "${raw}" == ./* || "${raw}" == ../* ]]; then
        printf '%s/%s\n' "$(pwd)" "${raw}"
    else
        printf '%s/%s\n' "${ROOT_DIR}" "${raw}"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            if [[ $# -lt 2 ]]; then
                echo "Error: --build-dir requires a directory argument." >&2
                exit 1
            fi
            BUILD_DIR="$2"
            shift 2
            ;;
        --build-type)
            if [[ $# -lt 2 ]]; then
                echo "Error: --build-type requires an argument." >&2
                exit 1
            fi
            BUILD_TYPE="$2"
            shift 2
            ;;
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --release)
            BUILD_TYPE="Release"
            shift
            ;;
        --arch)
            if [[ $# -lt 2 ]]; then
                echo "Error: --arch requires arm64, x86_64, or universal2." >&2
                exit 1
            fi
            TARGET_ARCH="$2"
            shift 2
            ;;
        --strict-warnings)
            STRICT_WARNINGS=1
            NO_WERROR=0
            shift
            ;;
        --no-werror)
            NO_WERROR=1
            STRICT_WARNINGS=0
            shift
            ;;
        --test)
            RUN_TESTS=1
            shift
            ;;
        --deploy-qt)
            DEPLOY_QT=1
            shift
            ;;
        --package-appimage)
            if [[ $# -lt 2 ]]; then
                echo "Error: --package-appimage requires an output .AppImage path." >&2
                exit 1
            fi
            APPIMAGE_OUTPUT="$2"
            DEPLOY_QT=1
            shift 2
            ;;
        --codesign-identity)
            if [[ $# -lt 2 ]]; then
                echo "Error: --codesign-identity requires an identity argument." >&2
                exit 1
            fi
            CODESIGN_IDENTITY="$2"
            shift 2
            ;;
        --notarize-profile)
            if [[ $# -lt 2 ]]; then
                echo "Error: --notarize-profile requires a keychain profile argument." >&2
                exit 1
            fi
            NOTARIZE_PROFILE="$2"
            shift 2
            ;;
        --verify-loader)
            VERIFY_LOADER=1
            shift
            ;;
        --install)
            if [[ $# -lt 2 ]]; then
                echo "Error: --install requires a prefix directory argument." >&2
                exit 1
            fi
            INSTALL_PREFIX="$2"
            shift 2
            ;;
        --run)
            RUN_APP=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            while [[ $# -gt 0 ]]; do
                RUN_ARGS+=("$1")
                shift
            done
            ;;
        *)
            echo "Error: Unknown option '$1'. Run with --help for usage." >&2
            exit 1
            ;;
    esac
done

RESOLVED_BUILD_DIR="$(resolve_path "${BUILD_DIR}")"
OS_NAME="$(uname -s)"

if [[ -n "${TARGET_ARCH}" ]]; then
    if [[ "${OS_NAME}" != "Darwin" ]]; then
        echo "Error: --arch is only supported on macOS (detected: ${OS_NAME})." >&2
        exit 1
    fi
    case "${TARGET_ARCH}" in
        arm64|x86_64|universal2)
            ;;
        *)
            echo "Error: Unsupported --arch '${TARGET_ARCH}' (expected arm64, x86_64, or universal2)." >&2
            exit 1
            ;;
    esac
fi

if [[ -n "${APPIMAGE_OUTPUT}" && "${OS_NAME}" != "Linux" ]]; then
    echo "Error: --package-appimage is only supported on Linux (detected: ${OS_NAME})." >&2
    exit 1
fi

if [[ "${OS_NAME}" == "Linux" && "${DEPLOY_QT}" -eq 1 && -z "${INSTALL_PREFIX}" ]]; then
    echo "Error: --deploy-qt (and --package-appimage) on Linux requires --install <prefix>." >&2
    exit 1
fi

# Return prioritized Homebrew root prefixes on macOS (honoring `brew --prefix` and `--arch`).
macos_brew_roots() {
    local brew_pref=""
    if command -v brew >/dev/null 2>&1; then
        brew_pref="$(brew --prefix 2>/dev/null || true)"
    fi

    local candidates=()
    if [[ "${TARGET_ARCH}" == "x86_64" ]]; then
        [[ "${brew_pref}" == /usr/local* ]] && candidates+=("${brew_pref}")
        candidates+=("/usr/local" "/opt/homebrew")
        [[ -n "${brew_pref}" ]] && candidates+=("${brew_pref}")
    elif [[ "${TARGET_ARCH}" == "arm64" ]]; then
        [[ "${brew_pref}" == /opt/homebrew* ]] && candidates+=("${brew_pref}")
        candidates+=("/opt/homebrew" "/usr/local")
        [[ -n "${brew_pref}" ]] && candidates+=("${brew_pref}")
    else
        [[ -n "${brew_pref}" ]] && candidates+=("${brew_pref}")
        if [[ "$(uname -m)" == "x86_64" ]]; then
            candidates+=("/usr/local" "/opt/homebrew")
        else
            candidates+=("/opt/homebrew" "/usr/local")
        fi
    fi

    local seen="|"
    local root
    for root in "${candidates[@]}"; do
        [[ -n "${root}" && -d "${root}" ]] || continue
        if [[ "${seen}" != *"|${root}|"* ]]; then
            seen="${seen}${root}|"
            printf '%s\n' "${root}"
        fi
    done
}

detect_parallel_jobs() {
    local cpus=4
    if command -v nproc >/dev/null 2>&1; then
        cpus="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        cpus="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
    fi
    if [[ ! "${cpus}" =~ ^[0-9]+$ || "${cpus}" -lt 1 ]]; then
        cpus=4
    fi
    # On Linux, cap C++20/Qt6 compilation jobs to ~2 GB RAM per compiler process
    # so machines and CI runners never OOM or thrash swap (bare `cmake --build --parallel`
    # passes unbounded `-j` to GNU Make).
    if [[ -r /proc/meminfo ]]; then
        local mem_kb
        mem_kb="$(awk '/^MemTotal:/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 0)"
        if [[ "${mem_kb}" =~ ^[0-9]+$ && "${mem_kb}" -gt 0 ]]; then
            local max_by_ram=$(( mem_kb / 2097152 ))
            if [[ "${max_by_ram}" -lt 1 ]]; then
                max_by_ram=1
            fi
            if [[ "${cpus}" -gt "${max_by_ram}" ]]; then
                cpus="${max_by_ram}"
            fi
        fi
    fi
    printf '%s\n' "${cpus}"
}

PARALLEL_JOBS="$(detect_parallel_jobs)"

CMAKE_ARGS=(
    -S "${ROOT_DIR}"
    -B "${RESOLVED_BUILD_DIR}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DSPELL_BUILD_RUST=ON
)

if [[ "${NO_WERROR}" -eq 1 ]]; then
    CMAKE_ARGS+=(-DSPELL_WARNINGS_AS_ERRORS=OFF)
else
    CMAKE_ARGS+=(-DSPELL_WARNINGS_AS_ERRORS=ON)
fi

if [[ "${OS_NAME}" == "Darwin" ]]; then
    case "${TARGET_ARCH}" in
        arm64)
            CMAKE_ARGS+=(-DCMAKE_OSX_ARCHITECTURES=arm64)
            ;;
        x86_64)
            CMAKE_ARGS+=(-DCMAKE_OSX_ARCHITECTURES=x86_64)
            ;;
        universal2)
            CMAKE_ARGS+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
            ;;
    esac

    MACOS_PREFIX_ENTRIES=()
    if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
        MACOS_PREFIX_ENTRIES+=("${CMAKE_PREFIX_PATH}")
    fi
    while IFS= read -r brew_root; do
        [[ -z "${brew_root}" ]] && continue
        for formula in qtbase qtsvg qscintilla2 qt; do
            if [[ -d "${brew_root}/opt/${formula}" ]]; then
                MACOS_PREFIX_ENTRIES+=("${brew_root}/opt/${formula}")
            fi
        done
    done < <(macos_brew_roots)

    if [[ ${#MACOS_PREFIX_ENTRIES[@]} -gt 0 ]]; then
        JOINED_PREFIX_PATH="$(IFS=';'; printf '%s' "${MACOS_PREFIX_ENTRIES[*]}")"
        CMAKE_ARGS+=("-DCMAKE_PREFIX_PATH=${JOINED_PREFIX_PATH}")
    fi
fi

if [[ "${STRICT_WARNINGS}" -eq 1 ]]; then
    if [[ -n "${RUSTFLAGS:-}" ]]; then
        export RUSTFLAGS="${RUSTFLAGS} -D warnings"
    else
        export RUSTFLAGS="-D warnings"
    fi
    echo "==> Strict warnings enabled (SPELL_WARNINGS_AS_ERRORS=ON, RUSTFLAGS='${RUSTFLAGS}')"
fi

echo "==> Configuring BeatrixLeslie IDE (${BUILD_TYPE}) in ${RESOLVED_BUILD_DIR}"
cmake "${CMAKE_ARGS[@]}"

echo "==> Building BeatrixLeslie IDE (parallel jobs: ${PARALLEL_JOBS})"
cmake --build "${RESOLVED_BUILD_DIR}" --config "${BUILD_TYPE}" --parallel "${PARALLEL_JOBS}"

if [[ "${RUN_TESTS}" -eq 1 ]]; then
    echo "==> Running spell_core Rust tests (SPELL_EPHEMERAL_KEYRING=1)"
    CARGO_TEST_ARGS=(
        --manifest-path "${ROOT_DIR}/rust/spell_core/Cargo.toml"
        --target-dir "${ROOT_DIR}/rust/spell_core/target"
    )
    if [[ "${OS_NAME}" == "Darwin" ]]; then
        case "${TARGET_ARCH}" in
            arm64)
                CARGO_TEST_ARGS+=(--target aarch64-apple-darwin)
                ;;
            x86_64)
                CARGO_TEST_ARGS+=(--target x86_64-apple-darwin)
                ;;
        esac
    fi
    SPELL_EPHEMERAL_KEYRING=1 cargo test "${CARGO_TEST_ARGS[@]}"

    echo "==> Running SpellQtTests via CTest"
    ctest --test-dir "${RESOLVED_BUILD_DIR}" --build-config "${BUILD_TYPE}" --output-on-failure
fi

RESOLVED_INSTALL_PREFIX=""
if [[ -n "${INSTALL_PREFIX}" ]]; then
    RESOLVED_INSTALL_PREFIX="$(resolve_path "${INSTALL_PREFIX}")"
    echo "==> Installing BeatrixLeslie IDE to ${RESOLVED_INSTALL_PREFIX}"
    cmake --install "${RESOLVED_BUILD_DIR}" --config "${BUILD_TYPE}" --prefix "${RESOLVED_INSTALL_PREFIX}"
fi

find_macdeployqt() {
    local brew_root candidate
    while IFS= read -r brew_root; do
        [[ -z "${brew_root}" ]] && continue
        for search_dir in "${brew_root}/opt/qtbase/bin" "${brew_root}/opt/qt/bin" "${brew_root}/bin"; do
            for candidate in macdeployqt macdeployqt6; do
                if [[ -x "${search_dir}/${candidate}" ]]; then
                    printf '%s\n' "${search_dir}/${candidate}"
                    return 0
                fi
            done
        done
    done < <(macos_brew_roots)

    for candidate in macdeployqt macdeployqt6; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            command -v "${candidate}"
            return 0
        fi
    done

    return 1
}

deploy_macos_bundle() {
    local bundle_dir="$1"
    local app_bin="${bundle_dir}/Contents/MacOS/BeatrixLeslieIDE"
    local frameworks_dir="${bundle_dir}/Contents/Frameworks"

    if [[ ! -x "${app_bin}" ]]; then
        echo "Error: Cannot deploy missing bundle executable ${app_bin}" >&2
        exit 1
    fi

    if [[ -n "${NOTARIZE_PROFILE}" && "${CODESIGN_IDENTITY}" == "-" ]]; then
        echo "Error: --notarize-profile requires a Developer ID --codesign-identity (ad-hoc '-' cannot be notarized)." >&2
        exit 1
    fi

    local macdeployqt_bin=""
    if ! macdeployqt_bin="$(find_macdeployqt)"; then
        echo "Error: macdeployqt / macdeployqt6 not found in PATH or Homebrew prefixes." >&2
        exit 1
    fi

    echo "==> Deploying self-contained Qt & QScintilla2 runtime into ${bundle_dir}"
    mkdir -p "${frameworks_dir}"
    chmod -R u+w "${bundle_dir}/Contents"

    # 1. Bundle QScintilla2 into Contents/Frameworks BEFORE running macdeployqt so
    #    macdeployqt sees its Qt dependencies (e.g. QtPrintSupport) and rewrites them.
    while IFS= read -r qsci_dep; do
        [[ -z "${qsci_dep}" ]] && continue
        local qsci_src=""
        local qsci_base=""
        case "${qsci_dep}" in
            @rpath/*)
                qsci_base="${qsci_dep#@rpath/}"
                if [[ -e "${frameworks_dir}/${qsci_base}" ]]; then
                    continue
                fi
                ;;
            @executable_path/*|@loader_path/*)
                continue
                ;;
            /*)
                qsci_src="${qsci_dep}"
                qsci_base="$(basename "${qsci_dep}")"
                ;;
            *)
                qsci_base="$(basename "${qsci_dep}")"
                ;;
        esac
        if [[ -z "${qsci_src}" ]]; then
            local brew_root cand_dir
            while IFS= read -r brew_root; do
                [[ -z "${brew_root}" ]] && continue
                for cand_dir in "${brew_root}/opt/qscintilla2/lib" "${brew_root}/lib"; do
                    if [[ -e "${cand_dir}/${qsci_base}" ]]; then
                        qsci_src="${cand_dir}/${qsci_base}"
                        break 2
                    fi
                done
            done < <(macos_brew_roots)
        fi
        if [[ -z "${qsci_src}" || ! -e "${qsci_src}" ]]; then
            echo "Error: QScintilla2 dylib '${qsci_dep}' referenced by ${app_bin} could not be located" >&2
            exit 1
        fi
        echo "==> Bundling QScintilla2 (${qsci_src} -> Contents/Frameworks/${qsci_base})"
        cp -L "${qsci_src}" "${frameworks_dir}/${qsci_base}"
        chmod u+w "${frameworks_dir}/${qsci_base}"
        install_name_tool -id "@rpath/${qsci_base}" "${frameworks_dir}/${qsci_base}"
        if [[ "${qsci_dep}" != "@rpath/${qsci_base}" ]]; then
            install_name_tool -change "${qsci_dep}" "@rpath/${qsci_base}" "${app_bin}"
        fi
    done < <(otool -L "${app_bin}" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \(compatibility version.*$//' | grep -Ei 'qscintilla2.*dylib' || true)

    # 2. Run macdeployqt to bundle QtCore, QtGui, QtWidgets, QtSvg, and required PlugIns.
    local libpath_args=()
    local brew_root qt_lib_dir
    while IFS= read -r brew_root; do
        [[ -z "${brew_root}" ]] && continue
        for qt_lib_dir in \
            "${brew_root}/opt/qtbase/lib" \
            "${brew_root}/opt/qtsvg/lib" \
            "${brew_root}/lib"; do
            if [[ -d "${qt_lib_dir}" ]]; then
                libpath_args+=("-libpath=${qt_lib_dir}")
            fi
        done
    done < <(macos_brew_roots)

    echo "==> Running ${macdeployqt_bin} on ${bundle_dir}"
    if [[ ${#libpath_args[@]} -gt 0 ]]; then
        "${macdeployqt_bin}" "${bundle_dir}" -always-overwrite "${libpath_args[@]}"
    else
        "${macdeployqt_bin}" "${bundle_dir}" -always-overwrite
    fi

    # 3. Post-pass fixup: ensure write permissions, sanitize LC_ID_DYLIB, rewrite any
    #    residual Homebrew / /usr/local / /Users load commands, and strip build-host RPATHs.
    chmod -R u+w "${bundle_dir}/Contents"

    local macho_files=()
    local cand
    for cand in \
        "${bundle_dir}/Contents/MacOS/"* \
        "${bundle_dir}/Contents/Frameworks/"*.dylib \
        "${bundle_dir}/Contents/Frameworks/"*.framework/Versions/*/* \
        "${bundle_dir}/Contents/PlugIns/"*/*.dylib; do
        [[ -f "${cand}" && ! -L "${cand}" ]] || continue
        [[ ! -L "$(dirname "${cand}")" ]] || continue
        if file -b "${cand}" | grep -Fq "Mach-O"; then
            macho_files+=("${cand}")
        fi
    done

    local macho
    if [[ ${#macho_files[@]} -gt 0 ]]; then
        for macho in "${macho_files[@]}"; do
            local curr_id
            curr_id="$(otool -D "${macho}" | sed -n '2p' | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
            if [[ -n "${curr_id}" && "${curr_id}" == /* ]]; then
                if [[ "${macho}" == "${frameworks_dir}/"*.framework/Versions/*/* ]]; then
                    local fw_rel="${macho#${frameworks_dir}/}"
                    install_name_tool -id "@rpath/${fw_rel}" "${macho}"
                elif [[ "${macho}" == "${frameworks_dir}/"*.dylib ]]; then
                    install_name_tool -id "@rpath/$(basename "${macho}")" "${macho}"
                fi
            fi

            local dep
            while IFS= read -r dep; do
                [[ -z "${dep}" ]] && continue
                if [[ "${dep}" == /opt/homebrew/* || "${dep}" == /usr/local/* || "${dep}" == /Users/* ]]; then
                    if [[ "${dep}" =~ /(Qt[^/]+\.framework)/Versions/([^/]+)/([^/]+)$ ]]; then
                        local fw_name="${BASH_REMATCH[1]}"
                        local fw_ver="${BASH_REMATCH[2]}"
                        local fw_bin="${BASH_REMATCH[3]}"
                        if [[ -e "${frameworks_dir}/${fw_name}/Versions/${fw_ver}/${fw_bin}" ]]; then
                            install_name_tool -change "${dep}" "@rpath/${fw_name}/Versions/${fw_ver}/${fw_bin}" "${macho}"
                        elif [[ -e "${frameworks_dir}/${fw_name}/Versions/A/${fw_bin}" ]]; then
                            install_name_tool -change "${dep}" "@rpath/${fw_name}/Versions/A/${fw_bin}" "${macho}"
                        elif [[ -e "${frameworks_dir}/${fw_name}/Versions/Current/${fw_bin}" ]]; then
                            install_name_tool -change "${dep}" "@rpath/${fw_name}/Versions/Current/${fw_bin}" "${macho}"
                        fi
                    elif [[ "${dep}" == *.dylib ]]; then
                        local dep_base
                        dep_base="$(basename "${dep}")"
                        if [[ ! -f "${frameworks_dir}/${dep_base}" && -e "${dep}" ]]; then
                            cp -L "${dep}" "${frameworks_dir}/${dep_base}"
                            chmod u+w "${frameworks_dir}/${dep_base}"
                            install_name_tool -id "@rpath/${dep_base}" "${frameworks_dir}/${dep_base}"
                        fi
                        if [[ -f "${frameworks_dir}/${dep_base}" ]]; then
                            install_name_tool -change "${dep}" "@rpath/${dep_base}" "${macho}"
                        fi
                    fi
                fi
            done < <(otool -L "${macho}" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \(compatibility version.*$//')

            local rp
            while IFS= read -r rp; do
                [[ -z "${rp}" ]] && continue
                if [[ "${rp}" == /opt/homebrew/* || "${rp}" == /usr/local/* || "${rp}" == /Users/* ]]; then
                    install_name_tool -delete_rpath "${rp}" "${macho}"
                fi
            done < <(otool -l "${macho}" | awk '/cmd LC_RPATH/{flag=1; next} flag && $1=="path"{print $2; flag=0}')
        done
    fi

    # 4. Inside-out codesigning (ad-hoc '-' or Developer ID hardened runtime + timestamp).
    if [[ "${CODESIGN_IDENTITY}" == "-" ]]; then
        echo "==> Applying inside-out ad-hoc code signatures to ${bundle_dir}"
        local dylib
        for dylib in "${frameworks_dir}/"*.dylib; do
            [[ -f "${dylib}" && ! -L "${dylib}" ]] || continue
            codesign --force --sign - "${dylib}"
        done
        local plugin
        for plugin in "${bundle_dir}/Contents/PlugIns/"*/*.dylib; do
            [[ -f "${plugin}" && ! -L "${plugin}" ]] || continue
            codesign --force --sign - "${plugin}"
        done
        local fw
        for fw in "${frameworks_dir}/"*.framework; do
            [[ -d "${fw}" && ! -L "${fw}" ]] || continue
            codesign --force --sign - "${fw}"
        done
        codesign --force --sign - "${bundle_dir}"
    else
        echo "==> Signing ${bundle_dir} with Developer ID identity '${CODESIGN_IDENTITY}' (hardened runtime + timestamp)"
        local nested_macho
        for nested_macho in \
            "${frameworks_dir}/"*.dylib \
            "${bundle_dir}/Contents/PlugIns/"*/*.dylib; do
            [[ -f "${nested_macho}" && ! -L "${nested_macho}" ]] || continue
            if file -b "${nested_macho}" | grep -Fq "Mach-O"; then
                codesign --force --options runtime --timestamp --sign "${CODESIGN_IDENTITY}" "${nested_macho}"
            fi
        done
        local fw
        for fw in "${frameworks_dir}/"*.framework; do
            [[ -d "${fw}" && ! -L "${fw}" ]] || continue
            codesign --force --options runtime --timestamp --sign "${CODESIGN_IDENTITY}" "${fw}"
        done
        codesign --force --options runtime --timestamp --sign "${CODESIGN_IDENTITY}" "${bundle_dir}"
    fi

    codesign --verify --deep --strict "${bundle_dir}"

    # 5. Optional Apple notarization & stapling when NOTARIZE_PROFILE is configured.
    if [[ -n "${NOTARIZE_PROFILE}" ]]; then
        echo "==> Submitting ${bundle_dir} for Apple notarization (profile: ${NOTARIZE_PROFILE})"
        local notarize_tmp
        notarize_tmp="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/spell-notarize.XXXXXX")"
        local notarize_zip="${notarize_tmp}/BeatrixLeslieIDE-notarize.zip"
        ditto -c -k --sequesterRsrc --keepParent "${bundle_dir}" "${notarize_zip}"
        xcrun notarytool submit "${notarize_zip}" --keychain-profile "${NOTARIZE_PROFILE}" --wait
        rm -rf "${notarize_tmp}"

        echo "==> Stapling and validating notarization ticket on ${bundle_dir}"
        xcrun stapler staple "${bundle_dir}"
        xcrun stapler validate "${bundle_dir}"
    fi

    echo "==> Self-contained macOS deployment completed for ${bundle_dir}"
}

find_linux_qt6_plugins_dir() {
    local cand=""
    if command -v qmake6 >/dev/null 2>&1; then
        cand="$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
        if [[ -n "${cand}" && -d "${cand}" ]]; then
            printf '%s\n' "${cand}"
            return 0
        fi
    fi
    if command -v qtpaths6 >/dev/null 2>&1; then
        cand="$(qtpaths6 --plugin-dir 2>/dev/null || true)"
        if [[ -n "${cand}" && -d "${cand}" ]]; then
            printf '%s\n' "${cand}"
            return 0
        fi
    fi
    if command -v qmake >/dev/null 2>&1; then
        local qver
        qver="$(qmake -query QT_VERSION 2>/dev/null || true)"
        if [[ "${qver}" == 6.* ]]; then
            cand="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
            if [[ -n "${cand}" && -d "${cand}" ]]; then
                printf '%s\n' "${cand}"
                return 0
            fi
        fi
    fi

    for cand in \
        "/usr/lib/x86_64-linux-gnu/qt6/plugins" \
        "/usr/lib/aarch64-linux-gnu/qt6/plugins" \
        "/usr/lib64/qt6/plugins" \
        "/usr/lib/qt6/plugins"; do
        if [[ -d "${cand}" ]]; then
            printf '%s\n' "${cand}"
            return 0
        fi
    done

    return 1
}

find_linux_qt6_lib_dir() {
    local cand=""
    if command -v qmake6 >/dev/null 2>&1; then
        cand="$(qmake6 -query QT_INSTALL_LIBS 2>/dev/null || true)"
        if [[ -n "${cand}" && -d "${cand}" ]]; then
            printf '%s\n' "${cand}"
            return 0
        fi
    fi
    for cand in \
        "/usr/lib/x86_64-linux-gnu" \
        "/usr/lib/aarch64-linux-gnu" \
        "/usr/lib64" \
        "/usr/lib"; do
        if [[ -d "${cand}" ]]; then
            printf '%s\n' "${cand}"
            return 0
        fi
    done
    return 1
}

is_excluded_linux_syslib() {
    local libname="$1"
    case "${libname}" in
        linux-vdso.so*|ld-linux*|libc.so*|libm.so*|libdl.so*|librt.so*|libpthread.so*|libutil.so*|libresolv.so*|libnss_*.so*|\
        libstdc++.so*|libgcc_s.so*|\
        libGL*|libEGL*|libOpenGL*|libgbm.so*|libdrm.so*|libvulkan.so*|\
        libwayland-client.so*|libwayland-cursor.so*|libwayland-egl.so*|libwayland-server.so*|\
        libX11.so*|libX11-xcb.so*|libxcb.so*|\
        libfontconfig.so*|libfreetype.so*|\
        libdbus-1.so*|libsystemd.so*|libudev.so*|\
        libz.so*|libexpat.so*|libglib-2.0.so*|libgio-2.0.so*|libgobject-2.0.so*|libgmodule-2.0.so*|libharfbuzz.so*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

extract_linux_hicolor_icons() {
    local prefix="$1"
    local dir256="${prefix}/share/icons/hicolor/256x256/apps"
    local dir512="${prefix}/share/icons/hicolor/512x512/apps"
    local out256="${dir256}/com.spell.BeatrixLeslieIDE.png"
    local out512="${dir512}/com.spell.BeatrixLeslieIDE.png"
    local icns_src="${ROOT_DIR}/Resources/AppIcon.icns"
    local fallback_png="${ROOT_DIR}/Resources/AppIconRounded.png"

    mkdir -p "${dir256}" "${dir512}"

    if command -v python3 >/dev/null 2>&1 && [[ -f "${icns_src}" ]]; then
        python3 - "${icns_src}" "${fallback_png}" "${out256}" "${out512}" <<'PY'
import shutil
import struct
import sys
from pathlib import Path

icns_path = Path(sys.argv[1])
fallback_path = Path(sys.argv[2])
out_256 = Path(sys.argv[3])
out_512 = Path(sys.argv[4])

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
chunks = {}

data = icns_path.read_bytes()
if len(data) >= 8 and data[:4] == b"icns":
    pos = 8
    while pos + 8 <= len(data):
        ostype = data[pos:pos + 4].decode("latin1", errors="ignore")
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        if size < 8 or pos + size > len(data):
            break
        payload = data[pos + 8:pos + size]
        if payload.startswith(PNG_MAGIC):
            chunks[ostype] = payload
        pos += size

for target_path, candidates in ((out_256, ("ic08", "ic13")), (out_512, ("ic09", "ic14"))):
    written = False
    for key in candidates:
        if key in chunks:
            target_path.write_bytes(chunks[key])
            written = True
            break
    if not written:
        shutil.copyfile(fallback_path, target_path)
PY
    else
        cp -L "${fallback_png}" "${out256}"
        cp -L "${fallback_png}" "${out512}"
    fi
}

deploy_linux_bundle() {
    local prefix="$1"
    local app_bin="${prefix}/bin/BeatrixLeslieIDE"
    local lib_dir="${prefix}/lib"
    local plugins_dir="${prefix}/plugins"

    if [[ ! -x "${app_bin}" ]]; then
        echo "Error: Cannot deploy missing Linux executable ${app_bin}" >&2
        exit 1
    fi
    if ! command -v patchelf >/dev/null 2>&1; then
        echo "Error: 'patchelf' is required for --deploy-qt on Linux (e.g. sudo apt-get install patchelf)." >&2
        exit 1
    fi
    if ! command -v file >/dev/null 2>&1; then
        echo "Error: 'file' is required for --deploy-qt on Linux (e.g. sudo apt-get install file)." >&2
        exit 1
    fi

    local qt_plugins_src=""
    if ! qt_plugins_src="$(find_linux_qt6_plugins_dir)"; then
        echo "Error: Could not locate Qt 6 plugins directory (install qt6-base-dev-tools or check qmake6)." >&2
        exit 1
    fi
    local qt_lib_src=""
    qt_lib_src="$(find_linux_qt6_lib_dir || true)"

    echo "==> Deploying self-contained Linux Qt 6 & QScintilla2 runtime into ${prefix}"
    echo "    Qt 6 plugins source: ${qt_plugins_src}"
    mkdir -p "${prefix}/bin" "${lib_dir}" "${plugins_dir}"

    # 1. Write qt.conf alongside bin/BeatrixLeslieIDE (zero LD_LIBRARY_PATH wrapper needed).
    cat > "${prefix}/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
Libraries = lib
EOF

    # 2. Copy required Qt 6 plugin subdirectories (skipping libqgtk3.so).
    mkdir -p "${plugins_dir}/platforms"
    local plat_glob plat_file
    for plat_glob in \
        "${qt_plugins_src}/platforms/libqxcb.so" \
        "${qt_plugins_src}/platforms/libqoffscreen.so" \
        "${qt_plugins_src}/platforms/libqminimal.so" \
        "${qt_plugins_src}/platforms/libqminimalegl.so" \
        "${qt_plugins_src}/platforms/"libqwayland*.so; do
        [[ -f "${plat_glob}" ]] || continue
        cp -L "${plat_glob}" "${plugins_dir}/platforms/$(basename "${plat_glob}")"
    done

    local subdir so_src
    for subdir in \
        xcbglintegrations \
        wayland-shell-integration \
        wayland-graphics-integration-client \
        wayland-decoration-client \
        platforminputcontexts \
        tls; do
        if [[ -d "${qt_plugins_src}/${subdir}" ]]; then
            mkdir -p "${plugins_dir}/${subdir}"
            for so_src in "${qt_plugins_src}/${subdir}/"*.so; do
                [[ -f "${so_src}" ]] || continue
                case "$(basename "${so_src}")" in
                    *gtk*) continue ;;
                esac
                cp -L "${so_src}" "${plugins_dir}/${subdir}/$(basename "${so_src}")"
            done
        fi
    done

    if [[ -d "${qt_plugins_src}/imageformats" ]]; then
        mkdir -p "${plugins_dir}/imageformats"
        local img_plugin
        for img_plugin in libqsvg.so libqjpeg.so libqico.so libqgif.so; do
            if [[ -f "${qt_plugins_src}/imageformats/${img_plugin}" ]]; then
                cp -L "${qt_plugins_src}/imageformats/${img_plugin}" "${plugins_dir}/imageformats/${img_plugin}"
            fi
        done
    fi

    if [[ -f "${qt_plugins_src}/iconengines/libqsvgicon.so" ]]; then
        mkdir -p "${plugins_dir}/iconengines"
        cp -L "${qt_plugins_src}/iconengines/libqsvgicon.so" "${plugins_dir}/iconengines/libqsvgicon.so"
    fi

    if [[ -f "${qt_plugins_src}/platformthemes/libxdgdesktopportal.so" ]]; then
        mkdir -p "${plugins_dir}/platformthemes"
        cp -L "${qt_plugins_src}/platformthemes/libxdgdesktopportal.so" "${plugins_dir}/platformthemes/libxdgdesktopportal.so"
    fi

    if [[ ! -f "${plugins_dir}/platforms/libqxcb.so" || ! -f "${plugins_dir}/platforms/libqoffscreen.so" ]]; then
        echo "Error: Missing required Qt platform plugins (libqxcb.so / libqoffscreen.so) in ${plugins_dir}/platforms" >&2
        exit 1
    fi

    # 3. Recursively resolve shared library dependencies across BeatrixLeslieIDE, libspell_core.so,
    #    and all bundled Qt plugins, copying non-host-ABI libraries into ${prefix}/lib/.
    local bfs_queue=("${app_bin}")
    local visited="|${app_bin}|"
    if [[ -f "${lib_dir}/libspell_core.so" ]]; then
        bfs_queue+=("${lib_dir}/libspell_core.so")
        visited="${visited}${lib_dir}/libspell_core.so|"
    fi
    local plugin_so
    for plugin_so in "${plugins_dir}/"*/*.so; do
        [[ -f "${plugin_so}" ]] || continue
        bfs_queue+=("${plugin_so}")
        visited="${visited}${plugin_so}|"
    done

    local discovery_ld_path="${lib_dir}"
    if [[ -n "${qt_lib_src}" ]]; then
        discovery_ld_path="${discovery_ld_path}:${qt_lib_src}"
    fi
    if [[ -n "${QSCINTILLA_LIB_DIR:-}" && -d "${QSCINTILLA_LIB_DIR}" ]]; then
        discovery_ld_path="${discovery_ld_path}:${QSCINTILLA_LIB_DIR}"
    fi

    local idx=0
    while [[ "${idx}" -lt "${#bfs_queue[@]}" ]]; do
        local current_elf="${bfs_queue[${idx}]}"
        idx=$(( idx + 1 ))

        local ldd_lines
        ldd_lines="$(LD_LIBRARY_PATH="${discovery_ld_path}" ldd "${current_elf}" 2>/dev/null || true)"

        local needed_list=""
        if command -v readelf >/dev/null 2>&1; then
            needed_list="$(readelf -d "${current_elf}" 2>/dev/null | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\].*/\1/p' || true)"
        else
            needed_list="$(printf '%s\n' "${ldd_lines}" | awk '/=>/ {print $1}')"
        fi

        local soname
        while IFS= read -r soname; do
            [[ -z "${soname}" ]] && continue
            if is_excluded_linux_syslib "${soname}"; then
                continue
            fi

            local dest_lib="${lib_dir}/${soname}"
            if [[ ! -f "${dest_lib}" ]]; then
                local resolved_src=""
                resolved_src="$(printf '%s\n' "${ldd_lines}" | awk -v target="${soname}" '$1 == target && $2 == "=>" && $3 ~ /^\// {print $3; exit}')"
                if [[ -z "${resolved_src}" && -n "${qt_lib_src}" && -e "${qt_lib_src}/${soname}" ]]; then
                    resolved_src="${qt_lib_src}/${soname}"
                fi
                if [[ -z "${resolved_src}" || ! -e "${resolved_src}" ]]; then
                    echo "Error: Could not resolve required shared library '${soname}' needed by ${current_elf}" >&2
                    exit 1
                fi
                cp -L "${resolved_src}" "${dest_lib}"
                chmod 0755 "${dest_lib}"
            fi

            if [[ "${visited}" != *"|${dest_lib}|"* ]]; then
                visited="${visited}${dest_lib}|"
                bfs_queue+=("${dest_lib}")
            fi
        done <<<"${needed_list}"
    done

    # 4. Patch ELF RPATHs via patchelf so zero LD_LIBRARY_PATH wrapper is ever needed.
    chmod u+w "${app_bin}"
    patchelf --set-rpath '$ORIGIN:$ORIGIN/../lib' "${app_bin}"

    local bundled_so
    for bundled_so in "${lib_dir}/"*.so*; do
        [[ -f "${bundled_so}" && ! -L "${bundled_so}" ]] || continue
        if ! file -b "${bundled_so}" | grep -Fq "ELF"; then
            continue
        fi
        if command -v readelf >/dev/null 2>&1; then
            if ! readelf -d "${bundled_so}" 2>/dev/null | grep -Fq "(NEEDED)"; then
                continue
            fi
        fi
        chmod u+w "${bundled_so}"
        patchelf --set-rpath '$ORIGIN' "${bundled_so}"
    done

    for plugin_so in "${plugins_dir}/"*/*.so; do
        [[ -f "${plugin_so}" && ! -L "${plugin_so}" ]] || continue
        if ! file -b "${plugin_so}" | grep -Fq "ELF"; then
            continue
        fi
        chmod u+w "${plugin_so}"
        patchelf --set-rpath '$ORIGIN/../../lib:$ORIGIN' "${plugin_so}"
    done

    # 5. Install desktop entry and extract true 256x256 (ic08) & 512x512 (ic09) hicolor PNG icons.
    mkdir -p "${prefix}/share/applications"
    cp -L "${ROOT_DIR}/Resources/com.spell.BeatrixLeslieIDE.desktop" "${prefix}/share/applications/com.spell.BeatrixLeslieIDE.desktop"
    extract_linux_hicolor_icons "${prefix}"

    echo "==> Self-contained Linux deployment completed for ${prefix}"
}

package_linux_appimage() {
    local prefix="$1"
    local out_raw="$2"
    local out_path
    out_path="$(resolve_path "${out_raw}")"

    echo "==> Packaging self-contained Linux AppImage: ${out_path}"
    mkdir -p "$(dirname "${out_path}")"

    local stage_dir
    stage_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/spell-appimage.XXXXXX")"
    local appdir="${stage_dir}/AppDir"
    mkdir -p "${appdir}/usr"
    cp -a "${prefix}/." "${appdir}/usr/"

    cp -L "${ROOT_DIR}/Resources/com.spell.BeatrixLeslieIDE.desktop" "${appdir}/com.spell.BeatrixLeslieIDE.desktop"

    local icon_256="${appdir}/usr/share/icons/hicolor/256x256/apps/com.spell.BeatrixLeslieIDE.png"
    if [[ -f "${icon_256}" ]]; then
        cp -L "${icon_256}" "${appdir}/com.spell.BeatrixLeslieIDE.png"
    else
        cp -L "${ROOT_DIR}/Resources/AppIconRounded.png" "${appdir}/com.spell.BeatrixLeslieIDE.png"
    fi
    ln -sf com.spell.BeatrixLeslieIDE.png "${appdir}/.DirIcon"

    cat > "${appdir}/AppRun" <<'EOF'
#!/bin/sh
set -eu
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
exec "${HERE}/usr/bin/BeatrixLeslieIDE" "$@"
EOF
    chmod 0755 "${appdir}/AppRun"

    local appimagetool_bin=""
    if command -v appimagetool >/dev/null 2>&1; then
        appimagetool_bin="$(command -v appimagetool)"
    elif command -v appimagetool-x86_64.AppImage >/dev/null 2>&1; then
        appimagetool_bin="$(command -v appimagetool-x86_64.AppImage)"
    else
        appimagetool_bin="${RUNNER_TEMP:-/tmp}/appimagetool-x86_64.AppImage"
        if [[ ! -x "${appimagetool_bin}" ]]; then
            echo "==> Downloading appimagetool-x86_64.AppImage to ${appimagetool_bin}"
            curl --fail --location --retry 3 --retry-delay 2 \
                -o "${appimagetool_bin}" \
                "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
            chmod +x "${appimagetool_bin}"
        fi
    fi

    ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "${appimagetool_bin}" "${appdir}" "${out_path}"
    chmod +x "${out_path}"
    rm -rf "${stage_dir}"

    echo "==> Smoke-testing AppImage (--appimage-extract-and-run --version)"
    local appimage_ver
    appimage_ver="$(env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen SPELL_EPHEMERAL_KEYRING=1 "${out_path}" --appimage-extract-and-run --version)"
    echo "${appimage_ver}"
    if ! grep -Fq "BeatrixLeslie IDE" <<<"${appimage_ver}"; then
        echo "Error: AppImage --version smoke test returned unexpected output: ${appimage_ver}" >&2
        exit 1
    fi
    echo "==> AppImage verified: ${out_path}"
}

if [[ "${DEPLOY_QT}" -eq 1 ]]; then
    case "${OS_NAME}" in
        Darwin)
            if [[ -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                deploy_macos_bundle "${RESOLVED_INSTALL_PREFIX}/BeatrixLeslieIDE.app"
            else
                deploy_macos_bundle "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app"
            fi
            ;;
        Linux)
            deploy_linux_bundle "${RESOLVED_INSTALL_PREFIX}"
            ;;
        *)
            echo "Warning: --deploy-qt in build_and_run.sh is supported on macOS and Linux (detected: ${OS_NAME})." >&2
            ;;
    esac
fi

verify_macos_bundle() {
    local bundle_dir="$1"
    local strict_install_rpath="$2"
    local strict_self_contained="${3:-0}"
    local app_bin="${bundle_dir}/Contents/MacOS/BeatrixLeslieIDE"
    local bundled_dylib="${bundle_dir}/Contents/Frameworks/libspell_core.dylib"
    local exe_dir="${bundle_dir}/Contents/MacOS"

    echo "==> Verifying macOS bundle loader metadata (strict_rpath=${strict_install_rpath}, self_contained=${strict_self_contained}): ${bundle_dir}"
    if [[ ! -x "${app_bin}" ]]; then
        echo "Error: Missing executable ${app_bin}" >&2
        exit 1
    fi
    if [[ ! -f "${bundled_dylib}" ]]; then
        echo "Error: Missing bundled dylib ${bundled_dylib}" >&2
        exit 1
    fi

    if [[ -n "${TARGET_ARCH}" ]] && command -v lipo >/dev/null 2>&1; then
        local app_archs dylib_archs
        app_archs="$(lipo -archs "${app_bin}")"
        dylib_archs="$(lipo -archs "${bundled_dylib}")"
        echo "    Executable archs: ${app_archs} | libspell_core.dylib archs: ${dylib_archs}"
        case "${TARGET_ARCH}" in
            arm64)
                if [[ "${app_archs}" != *"arm64"* || "${dylib_archs}" != *"arm64"* ]]; then
                    echo "Error: Expected arm64 slice in ${app_bin} (${app_archs}) and ${bundled_dylib} (${dylib_archs})" >&2
                    exit 1
                fi
                ;;
            x86_64)
                if [[ "${app_archs}" != *"x86_64"* || "${dylib_archs}" != *"x86_64"* ]]; then
                    echo "Error: Expected x86_64 slice in ${app_bin} (${app_archs}) and ${bundled_dylib} (${dylib_archs})" >&2
                    exit 1
                fi
                ;;
            universal2)
                for req_arch in arm64 x86_64; do
                    if [[ "${app_archs}" != *"${req_arch}"* || "${dylib_archs}" != *"${req_arch}"* ]]; then
                        echo "Error: Universal2 bundle missing '${req_arch}' slice (app: ${app_archs}, dylib: ${dylib_archs})" >&2
                        exit 1
                    fi
                done
                ;;
        esac
    fi

    local app_otool
    app_otool="$(otool -L "${app_bin}")"
    echo "${app_otool}"

    local dylib_otool
    dylib_otool="$(otool -L "${bundled_dylib}")"
    echo "${dylib_otool}"

    if ! grep -Fq "libspell_core.dylib" <<<"${app_otool}"; then
        echo "Error: ${app_bin} does not reference libspell_core.dylib" >&2
        exit 1
    fi

    if printf '%s\n' "${app_otool}" | tail -n +2 | grep -Eq '^[[:space:]]*/[^[:space:]]*libspell_core\.dylib'; then
        echo "Error: ${app_bin} embeds a non-portable absolute path to libspell_core.dylib" >&2
        exit 1
    fi

    local dylib_id
    dylib_id="$(otool -D "${bundled_dylib}" | sed -n '2p' | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
    if [[ "${dylib_id}" != "@rpath/libspell_core.dylib" && "${dylib_id}" != "@executable_path/../Frameworks/libspell_core.dylib" ]]; then
        echo "Error: ${bundled_dylib} install_name id is '${dylib_id}', expected '@rpath/libspell_core.dylib'" >&2
        exit 1
    fi

    local rpaths=()
    while IFS= read -r rp; do
        if [[ -n "${rp}" ]]; then
            rpaths+=("${rp}")
        fi
    done < <(otool -l "${app_bin}" | awk '/cmd LC_RPATH/{flag=1; next} flag && $1=="path"{print $2; flag=0}')

    if [[ "${#rpaths[@]}" -eq 0 ]]; then
        echo "Error: ${app_bin} has no LC_RPATH entries" >&2
        exit 1
    fi

    if [[ "${strict_install_rpath}" -eq 1 || "${strict_self_contained}" -eq 1 ]]; then
        for rp in "${rpaths[@]}"; do
            if [[ "${rp}" != "@executable_path/../Frameworks" ]]; then
                echo "Error: Binary ${app_bin} contains non-portable LC_RPATH '${rp}' (expected only '@executable_path/../Frameworks')" >&2
                exit 1
            fi
        done
    fi

    while IFS= read -r dep; do
        [[ -z "${dep}" ]] && continue
        case "${dep}" in
            /System/*|/usr/lib/*)
                # Provided by macOS dyld shared cache
                ;;
            @rpath/*)
                local rel="${dep#@rpath/}"
                local found=0
                for rp in "${rpaths[@]}"; do
                    local expanded="${rp/#@executable_path/${exe_dir}}"
                    expanded="${expanded/#@loader_path/${exe_dir}}"
                    if [[ -e "${expanded}/${rel}" ]]; then
                        found=1
                        break
                    fi
                done
                if [[ "${found}" -eq 0 ]]; then
                    if [[ "${strict_self_contained}" -eq 0 && "${strict_install_rpath}" -eq 1 && "${rel}" != "libspell_core.dylib" ]]; then
                        continue
                    fi
                    echo "Error: Unresolved @rpath dependency '${dep}' in ${app_bin}" >&2
                    exit 1
                fi
                ;;
            @executable_path/*)
                local target="${exe_dir}/${dep#@executable_path/}"
                if [[ ! -e "${target}" ]]; then
                    echo "Error: Missing @executable_path dependency '${target}'" >&2
                    exit 1
                fi
                ;;
            @loader_path/*)
                local target="${exe_dir}/${dep#@loader_path/}"
                if [[ ! -e "${target}" ]]; then
                    echo "Error: Missing @loader_path dependency '${target}'" >&2
                    exit 1
                fi
                ;;
            /*)
                if [[ ! -e "${dep}" ]]; then
                    echo "Error: Missing shared library '${dep}' referenced by ${app_bin}" >&2
                    exit 1
                fi
                ;;
        esac
    done < <(printf '%s\n' "${app_otool}" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \(compatibility version.*$//')

    if [[ "${strict_self_contained}" -eq 1 ]]; then
        local cocoa_plugin="${bundle_dir}/Contents/PlugIns/platforms/libqcocoa.dylib"
        if [[ ! -f "${cocoa_plugin}" ]]; then
            echo "Error: Self-contained bundle is missing Qt Cocoa platform plugin (${cocoa_plugin})" >&2
            exit 1
        fi

        local scan_files=()
        local cand
        for cand in \
            "${bundle_dir}/Contents/MacOS/"* \
            "${bundle_dir}/Contents/Frameworks/"*.dylib \
            "${bundle_dir}/Contents/Frameworks/"*.framework/Versions/*/* \
            "${bundle_dir}/Contents/PlugIns/"*/*.dylib; do
            [[ -f "${cand}" && ! -L "${cand}" ]] || continue
            [[ ! -L "$(dirname "${cand}")" ]] || continue
            if file -b "${cand}" | grep -Fq "Mach-O"; then
                scan_files+=("${cand}")
            fi
        done

        local macho
        for macho in "${scan_files[@]}"; do
            local dep
            while IFS= read -r dep; do
                [[ -z "${dep}" ]] && continue
                case "${dep}" in
                    /opt/homebrew*|/usr/local*|/Users/*)
                        echo "Error: Non-portable external dependency '${dep}' found in ${macho}" >&2
                        exit 1
                        ;;
                esac
            done < <(otool -L "${macho}" | tail -n +2 | sed -E 's/^[[:space:]]+//; s/ \(compatibility version.*$//')

            local mrp
            while IFS= read -r mrp; do
                [[ -z "${mrp}" ]] && continue
                case "${mrp}" in
                    /opt/homebrew*|/usr/local*|/Users/*)
                        echo "Error: Non-portable LC_RPATH '${mrp}' found in ${macho}" >&2
                        exit 1
                        ;;
                esac
            done < <(otool -l "${macho}" | awk '/cmd LC_RPATH/{flag=1; next} flag && $1=="path"{print $2; flag=0}')
        done
        echo "==> Audited ${#scan_files[@]} Mach-O binaries: zero external Homebrew / /usr/local / /Users paths"
    fi

    if command -v codesign >/dev/null 2>&1; then
        echo "==> Verifying code signature on ${bundle_dir}"
        codesign --verify --deep --strict "${bundle_dir}"
    fi
    echo "==> macOS loader verification passed for ${bundle_dir}"
}

verify_linux_binary() {
    local app_bin="$1"
    local strict_self_contained="${2:-0}"
    local deployed_prefix="${3:-}"

    echo "==> Verifying Linux ELF dynamic loader resolution (self_contained=${strict_self_contained}): ${app_bin}"
    if [[ ! -x "${app_bin}" ]]; then
        echo "Error: Missing executable ${app_bin}" >&2
        exit 1
    fi

    local ldd_out
    ldd_out="$(env -u LD_LIBRARY_PATH ldd "${app_bin}")"
    echo "${ldd_out}"

    if grep -Fq "not found" <<<"${ldd_out}"; then
        echo "Error: Unresolved shared library ('not found') in ${app_bin}" >&2
        exit 1
    fi
    if ! grep -Fq "libspell_core.so" <<<"${ldd_out}"; then
        echo "Error: ${app_bin} does not link libspell_core.so" >&2
        exit 1
    fi

    if command -v readelf >/dev/null 2>&1; then
        local needed_out
        needed_out="$(readelf -d "${app_bin}" | grep '(NEEDED)' || true)"
        if grep -Eq '\[/.*libspell_core\.so\]' <<<"${needed_out}"; then
            echo "Error: ${app_bin} embeds an absolute DT_NEEDED path for libspell_core.so" >&2
            exit 1
        fi
    fi

    if [[ "${strict_self_contained}" -eq 1 && -n "${deployed_prefix}" ]]; then
        echo "==> Verifying self-contained Linux bundle layout in ${deployed_prefix}"
        local req_path
        for req_path in \
            "${deployed_prefix}/bin/qt.conf" \
            "${deployed_prefix}/plugins/platforms/libqxcb.so" \
            "${deployed_prefix}/plugins/platforms/libqoffscreen.so" \
            "${deployed_prefix}/share/applications/com.spell.BeatrixLeslieIDE.desktop" \
            "${deployed_prefix}/share/icons/hicolor/256x256/apps/com.spell.BeatrixLeslieIDE.png" \
            "${deployed_prefix}/share/icons/hicolor/512x512/apps/com.spell.BeatrixLeslieIDE.png"; do
            if [[ ! -f "${req_path}" ]]; then
                echo "Error: Self-contained Linux prefix is missing required file ${req_path}" >&2
                exit 1
            fi
        done

        local found_qtwidgets=0
        local cand_lib
        for cand_lib in "${deployed_prefix}/lib/"libQt6Widgets.so*; do
            if [[ -f "${cand_lib}" ]]; then
                found_qtwidgets=1
                break
            fi
        done
        if [[ "${found_qtwidgets}" -eq 0 ]]; then
            echo "Error: Missing bundled libQt6Widgets.so* in ${deployed_prefix}/lib" >&2
            exit 1
        fi

        local found_qsci=0
        for cand_lib in "${deployed_prefix}/lib/"libqscintilla2_qt6.so*; do
            if [[ -f "${cand_lib}" ]]; then
                found_qsci=1
                break
            fi
        done
        if [[ "${found_qsci}" -eq 0 ]]; then
            echo "Error: Missing bundled libqscintilla2_qt6.so* in ${deployed_prefix}/lib" >&2
            exit 1
        fi

        local plugin_so plugin_ldd
        local plugin_count=0
        for plugin_so in "${deployed_prefix}/plugins/"*/*.so; do
            [[ -f "${plugin_so}" ]] || continue
            plugin_count=$(( plugin_count + 1 ))
            plugin_ldd="$(env -u LD_LIBRARY_PATH ldd "${plugin_so}")"
            if grep -Fq "not found" <<<"${plugin_ldd}"; then
                echo "Error: Unresolved shared library ('not found') in plugin ${plugin_so}:" >&2
                echo "${plugin_ldd}" >&2
                exit 1
            fi
        done
        echo "==> Audited ${plugin_count} bundled Qt 6 plugins: zero unresolved shared libraries"

        echo "==> Running headless offscreen startup check (${app_bin} --version)"
        local ver_out
        ver_out="$(env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen SPELL_EPHEMERAL_KEYRING=1 "${app_bin}" --version)"
        echo "${ver_out}"
        if ! grep -Fq "BeatrixLeslie IDE" <<<"${ver_out}"; then
            echo "Error: Headless --version check returned unexpected output: ${ver_out}" >&2
            exit 1
        fi
    fi

    echo "==> Linux loader verification passed for ${app_bin}"
}

if [[ "${VERIFY_LOADER}" -eq 1 ]]; then
    case "${OS_NAME}" in
        Darwin)
            if [[ -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                verify_macos_bundle "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app" 0 0
                verify_macos_bundle "${RESOLVED_INSTALL_PREFIX}/BeatrixLeslieIDE.app" 1 "${DEPLOY_QT}"
            else
                verify_macos_bundle "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app" "${DEPLOY_QT}" "${DEPLOY_QT}"
            fi
            ;;
        Linux)
            verify_linux_binary "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE" 0 ""
            if [[ -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                verify_linux_binary "${RESOLVED_INSTALL_PREFIX}/bin/BeatrixLeslieIDE" "${DEPLOY_QT}" "${RESOLVED_INSTALL_PREFIX}"
            fi
            ;;
        *)
            echo "Warning: --verify-loader is only supported on macOS (Darwin) and Linux (detected: ${OS_NAME})." >&2
            ;;
    esac
fi

if [[ -n "${APPIMAGE_OUTPUT}" ]]; then
    package_linux_appimage "${RESOLVED_INSTALL_PREFIX}" "${APPIMAGE_OUTPUT}"
fi

if [[ "${RUN_APP}" -eq 1 ]]; then
    case "${OS_NAME}" in
        Darwin)
            if [[ "${DEPLOY_QT}" -eq 1 && -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                APP_EXEC="${RESOLVED_INSTALL_PREFIX}/BeatrixLeslieIDE.app/Contents/MacOS/BeatrixLeslieIDE"
            else
                APP_EXEC="${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app/Contents/MacOS/BeatrixLeslieIDE"
            fi
            ;;
        Linux)
            if [[ "${DEPLOY_QT}" -eq 1 && -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                APP_EXEC="${RESOLVED_INSTALL_PREFIX}/bin/BeatrixLeslieIDE"
            else
                APP_EXEC="${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE"
            fi
            ;;
        *)
            APP_EXEC="${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE"
            ;;
    esac

    if [[ ! -x "${APP_EXEC}" ]]; then
        echo "Error: Built executable not found at ${APP_EXEC}" >&2
        exit 1
    fi

    echo "==> Launching ${APP_EXEC}"
    if [[ ${#RUN_ARGS[@]} -gt 0 ]]; then
        exec "${APP_EXEC}" "${RUN_ARGS[@]}"
    else
        exec "${APP_EXEC}"
    fi
fi
