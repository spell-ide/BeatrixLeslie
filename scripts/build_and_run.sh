#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="build"
BUILD_TYPE="Release"
RUN_TESTS=0
VERIFY_LOADER=0
DEPLOY_QT=0
INSTALL_PREFIX=""
RUN_APP=0
RUN_ARGS=()

usage() {
    cat <<'EOF'
Usage: scripts/build_and_run.sh [options] [-- <app-args...>]

Build, test, bundle Qt frameworks, verify dynamic loader paths, install, and launch BeatrixLeslie IDE.

Options:
  --build-dir <dir>     CMake build directory (default: build)
  --build-type <type>   CMake build configuration: Release, Debug, RelWithDebInfo (default: Release)
  --debug               Shorthand for --build-type Debug
  --release             Shorthand for --build-type Release
  --test                Run Rust unit tests (ephemeral keyring) and CTest Qt tests
  --deploy-qt           Bundle Qt frameworks, plugins, and QScintilla2 into a self-contained macOS .app
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
        --deploy-qt)
            DEPLOY_QT=1
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

echo "==> Configuring BeatrixLeslie IDE (${BUILD_TYPE}) in ${RESOLVED_BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${RESOLVED_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DSPELL_BUILD_RUST=ON

echo "==> Building BeatrixLeslie IDE (parallel jobs: ${PARALLEL_JOBS})"
cmake --build "${RESOLVED_BUILD_DIR}" --config "${BUILD_TYPE}" --parallel "${PARALLEL_JOBS}"

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

find_macdeployqt() {
    local candidate
    for candidate in macdeployqt macdeployqt6; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            command -v "${candidate}"
            return 0
        fi
    done

    local search_dir
    for search_dir in \
        "/opt/homebrew/opt/qtbase/bin" \
        "/opt/homebrew/opt/qt/bin" \
        "/opt/homebrew/bin" \
        "/usr/local/opt/qtbase/bin" \
        "/usr/local/opt/qt/bin" \
        "/usr/local/bin"; do
        for candidate in macdeployqt macdeployqt6; do
            if [[ -x "${search_dir}/${candidate}" ]]; then
                printf '%s\n' "${search_dir}/${candidate}"
                return 0
            fi
        done
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

    local macdeployqt_bin=""
    if ! macdeployqt_bin="$(find_macdeployqt)"; then
        echo "Error: macdeployqt / macdeployqt6 not found in PATH or standard Homebrew prefixes." >&2
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
            local cand_dir
            for cand_dir in \
                "/opt/homebrew/opt/qscintilla2/lib" \
                "/usr/local/opt/qscintilla2/lib" \
                "/opt/homebrew/lib" \
                "/usr/local/lib"; do
                if [[ -e "${cand_dir}/${qsci_base}" ]]; then
                    qsci_src="${cand_dir}/${qsci_base}"
                    break
                fi
            done
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
    local qt_lib_dir
    for qt_lib_dir in \
        "/opt/homebrew/opt/qtbase/lib" \
        "/opt/homebrew/opt/qtsvg/lib" \
        "/opt/homebrew/lib" \
        "/usr/local/opt/qtbase/lib" \
        "/usr/local/opt/qtsvg/lib" \
        "/usr/local/lib"; do
        if [[ -d "${qt_lib_dir}" ]]; then
            libpath_args+=("-libpath=${qt_lib_dir}")
        fi
    done

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

    # 4. Inside-out ad-hoc codesigning and strict signature verification.
    echo "==> Applying inside-out ad-hoc code signatures to ${bundle_dir}"
    local dylib
    for dylib in "${frameworks_dir}/"*.dylib; do
        [[ -f "${dylib}" && ! -L "${dylib}" ]] || continue
        codesign --force --sign - "${dylib}"
    done
    local fw
    for fw in "${frameworks_dir}/"*.framework; do
        [[ -d "${fw}" && ! -L "${fw}" ]] || continue
        codesign --force --sign - "${fw}"
    done
    local plugin
    for plugin in "${bundle_dir}/Contents/PlugIns/"*/*.dylib; do
        [[ -f "${plugin}" && ! -L "${plugin}" ]] || continue
        codesign --force --sign - "${plugin}"
    done
    codesign --force --sign - "${bundle_dir}"
    codesign --verify --deep --strict "${bundle_dir}"
    echo "==> Self-contained macOS deployment completed for ${bundle_dir}"
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
        *)
            echo "Warning: --deploy-qt in build_and_run.sh applies only to macOS bundles (detected: ${OS_NAME})." >&2
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
                # In strict installed mode without macdeployqt, Qt/QScintilla frameworks may remain
                # in Homebrew while libspell_core.dylib MUST resolve inside Contents/Frameworks.
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
            if [[ -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                verify_macos_bundle "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app" 0 0
                verify_macos_bundle "${RESOLVED_INSTALL_PREFIX}/BeatrixLeslieIDE.app" 1 "${DEPLOY_QT}"
            else
                verify_macos_bundle "${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app" "${DEPLOY_QT}" "${DEPLOY_QT}"
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
            if [[ "${DEPLOY_QT}" -eq 1 && -n "${RESOLVED_INSTALL_PREFIX}" ]]; then
                APP_EXEC="${RESOLVED_INSTALL_PREFIX}/BeatrixLeslieIDE.app/Contents/MacOS/BeatrixLeslieIDE"
            else
                APP_EXEC="${RESOLVED_BUILD_DIR}/BeatrixLeslieIDE.app/Contents/MacOS/BeatrixLeslieIDE"
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
