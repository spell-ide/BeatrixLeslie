#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="build"
BUILD_TYPE="Release"
RUN_TESTS=0
VERIFY_LOADER=0
INSTALL_PREFIX=""
RUN_APP=0
RUN_ARGS=()

usage() {
    cat <<'EOF'
Usage: scripts/build_and_run.sh [options] [-- <app-args...>]

Build, test, verify dynamic loader paths, install, and launch BeatrixLeslie IDE.

Options:
  --build-dir <dir>     CMake build directory (default: build)
  --build-type <type>   CMake build configuration: Release, Debug, RelWithDebInfo (default: Release)
  --debug               Shorthand for --build-type Debug
  --release             Shorthand for --build-type Release
  --test                Run Rust unit tests (ephemeral keyring) and CTest Qt tests
  --verify-loader       Verify dynamic library loader paths (otool -L / codesign on macOS, ldd on Linux)
  --install <prefix>    Install portable application layout into <prefix>
  --run                 Launch the built BeatrixLeslieIDE executable
  -h, --help            Show this help message and exit
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
        --test)
            RUN_TESTS=1
            shift
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

echo "==> Configuring BeatrixLeslie IDE (${BUILD_TYPE}) in ${RESOLVED_BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${RESOLVED_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DSPELL_BUILD_RUST=ON

echo "==> Building BeatrixLeslie IDE"
cmake --build "${RESOLVED_BUILD_DIR}" --config "${BUILD_TYPE}" --parallel

if [[ "${RUN_TESTS}" -eq 1 ]]; then
    echo "==> Running spell_core Rust tests (SPELL_EPHEMERAL_KEYRING=1)"
    SPELL_EPHEMERAL_KEYRING=1 cargo test \
        --manifest-path "${ROOT_DIR}/rust/spell_core/Cargo.toml" \
        --target-dir "${ROOT_DIR}/rust/spell_core/target"

    echo "==> Running SpellQtTests via CTest"
    ctest --test-dir "${RESOLVED_BUILD_DIR}" --build-config "${BUILD_TYPE}" --output-on-failure
fi

RESOLVED_INSTALL_PREFIX=""
if [[ -n "${INSTALL_PREFIX}" ]]; then
    RESOLVED_INSTALL_PREFIX="$(resolve_path "${INSTALL_PREFIX}")"
    echo "==> Installing BeatrixLeslie IDE to ${RESOLVED_INSTALL_PREFIX}"
    cmake --install "${RESOLVED_BUILD_DIR}" --config "${BUILD_TYPE}" --prefix "${RESOLVED_INSTALL_PREFIX}"
fi

verify_macos_bundle() {
    local bundle_dir="$1"
    local strict_install_rpath="$2"
    local app_bin="${bundle_dir}/Contents/MacOS/BeatrixLeslieIDE"
    local bundled_dylib="${bundle_dir}/Contents/Frameworks/libspell_core.dylib"
    local exe_dir="${bundle_dir}/Contents/MacOS"

    echo "==> Verifying macOS bundle loader metadata: ${bundle_dir}"
    if [[ ! -x "${app_bin}" ]]; then
        echo "Error: Missing executable ${app_bin}" >&2
        exit 1
    fi
    if [[ ! -f "${bundled_dylib}" ]]; then
        echo "Error: Missing bundled dylib ${bundled_dylib}" >&2
        exit 1
    fi

    local app_otool
    app_otool="$(otool -L "${app_bin}")"
    echo "${app_otool}"

    local dylib_otool
    dylib_otool="$(otool -L "${bundled_dylib}")"
    echo "${dylib_otool}"

    if ! grep -Fq "@rpath/libspell_core.dylib" <<<"${app_otool}"; then
        echo "Error: ${app_bin} does not reference @rpath/libspell_core.dylib" >&2
        exit 1
    fi

    if printf '%s\n' "${app_otool}" | tail -n +2 | grep -Eq '^[[:space:]]*/[^[:space:]]*libspell_core\.dylib'; then
        echo "Error: ${app_bin} embeds a non-portable absolute path to libspell_core.dylib" >&2
        exit 1
    fi

    local dylib_id
    dylib_id="$(otool -D "${bundled_dylib}" | sed -n '2p' | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
    if [[ "${dylib_id}" != "@rpath/libspell_core.dylib" ]]; then
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

    if [[ "${strict_install_rpath}" -eq 1 ]]; then
        for rp in "${rpaths[@]}"; do
            if [[ "${rp}" != "@executable_path/../Frameworks" ]]; then
                echo "Error: Installed binary ${app_bin} contains non-portable LC_RPATH '${rp}' (expected only '@executable_path/../Frameworks')" >&2
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
                # In strict installed mode without macdeployqt, Qt/QScintilla frameworks may remain
                # in Homebrew while libspell_core.dylib MUST resolve inside Contents/Frameworks.
                if [[ "${found}" -eq 0 ]]; then
                    if [[ "${strict_install_rpath}" -eq 1 && "${rel}" != "libspell_core.dylib" ]]; then
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

    if command -v codesign >/dev/null 2>&1; then
        echo "==> Verifying code signature on ${bundle_dir}"
        codesign --verify --deep --strict "${bundle_dir}"
    fi
    echo "==> macOS loader verification passed for ${bundle_dir}"
}

verify_linux_binary() {
    local app_bin="$1"
    echo "==> Verifying Linux ELF dynamic loader resolution: ${app_bin}"
    if [[ ! -x "${app_bin}" ]]; then
        echo "Error: Missing executable ${app_bin}" >&2
        exit 1
    fi

    local ldd_out
    ldd_out="$(ldd "${app_bin}")"
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
    echo "==> Linux loader verification passed for ${app_bin}"
}

if [[ "${VERIFY_LOADER}" -eq 1 ]]; then
    case "${OS_NAME}" in
        Darwin)
            verify_macos_bundle "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app" 0
            if [[ -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                verify_macos_bundle "${RESOLVED_INSTALL_PREFIX}/BeatrixLeslieIDE.app" 1
            fi
            ;;
        Linux)
            verify_linux_binary "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE"
            if [[ -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                verify_linux_binary "${RESOLVED_INSTALL_PREFIX}/bin/BeatrixLeslieIDE"
            fi
            ;;
        *)
            echo "Warning: --verify-loader is only supported on macOS (Darwin) and Linux (detected: ${OS_NAME})." >&2
            ;;
    esac
fi

if [[ "${RUN_APP}" -eq 1 ]]; then
    case "${OS_NAME}" in
        Darwin)
            APP_EXEC="${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app/Contents/MacOS/BeatrixLeslieIDE"
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
