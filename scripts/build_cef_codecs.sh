#!/usr/bin/env bash
# Build a minimal CEF binary distribution with H.264/AAC enabled.
#
# This is intentionally separate from fetch_cef.sh: Spotify's stock CEF
# archives cannot acquire codecs at Aurora link or run time. A Chromium/CEF
# source build is large and may take hours. The output is a complete CEF binary
# distribution suitable for CEF_ROOT, not a replacement FFmpeg library.
#
# Usage:
#   CEF_BRANCH=7390 ./scripts/build_cef_codecs.sh
#
# Optional:
#   CEF_BUILD_ROOT=/large/disk/cef-codec-build
#   DEPOT_TOOLS_DIR=/path/to/depot_tools
#   CEF_OUTPUT_DIR=$PWD/third_party/cef-codec
#
# The branch must be a CEF release branch compatible with the Chromium version
# Aurora is meant to ship. Review codec patent/licensing obligations before
# distributing the result.
set -euo pipefail

if [[ -z "${CEF_BRANCH:-}" ]]; then
    echo "CEF_BRANCH is required (a CEF release branch number, for example 7390)." >&2
    exit 2
fi
if [[ ! "${CEF_BRANCH}" =~ ^[0-9]+$ ]]; then
    echo "CEF_BRANCH must be a numeric CEF release branch." >&2
    exit 2
fi

case "$(uname -s)" in
    Linux*) platform="linux64"; arch_flag="--x64-build" ;;
    Darwin*)
        if [[ "$(uname -m)" == "arm64" ]]; then
            platform="macosarm64"; arch_flag="--arm64-build"
        else
            platform="macosx64"; arch_flag="--x64-build"
        fi
        ;;
    MINGW*|MSYS*|CYGWIN*) platform="windows64"; arch_flag="--x64-build" ;;
    *)
        echo "Unsupported host. Build on Linux, macOS, or Windows with Git Bash." >&2
        exit 2
        ;;
esac

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_root="${CEF_BUILD_ROOT:-${repo_root}/.cef-codec-build}"
depot_tools="${DEPOT_TOOLS_DIR:-${build_root}/depot_tools}"
output_dir="${CEF_OUTPUT_DIR:-${repo_root}/third_party/cef-codec}"
python_bin="${PYTHON:-python3}"
automate="${build_root}/automate/automate-git.py"

if [[ -d "${output_dir}" ]] &&
   [[ -n "$(find "${output_dir}" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "CEF_OUTPUT_DIR must be empty: ${output_dir}" >&2
    exit 1
fi
mkdir -p "${build_root}" "${output_dir}"

if [[ ! -d "${depot_tools}/.git" ]]; then
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git \
        "${depot_tools}"
fi
export PATH="${depot_tools}:${PATH}"

if [[ ! -f "${automate}" ]]; then
    mkdir -p "$(dirname "${automate}")"
    curl -fL \
        https://bitbucket.org/chromiumembedded/cef/raw/master/tools/automate/automate-git.py \
        -o "${automate}"
fi

# Preserve caller-supplied GN settings while forcing the two codec settings.
# CEF's automated build script produces the actual args.gn in its out dir.
if [[ "${platform}" == "linux64" ]]; then
    platform_gn="use_sysroot=true symbol_level=1 is_cfi=false"
else
platform_gn=""
fi
export GN_DEFINES="${GN_DEFINES:-} is_official_build=true ${platform_gn} ffmpeg_branding=Chrome proprietary_codecs=true"
export CEF_ARCHIVE_FORMAT=tar.bz2
args=(
    "${automate}"
    "--download-dir=${build_root}/chromium_git"
    "--depot-tools-dir=${depot_tools}"
    "--branch=${CEF_BRANCH}"
    --minimal-distrib
    --force-clean
    --no-chromium-history
    --no-debug-build
    "${arch_flag}"
)
if [[ "${arch_flag}" == "--x64-build" || "${platform}" == "macosarm64" ]]; then
    args+=(--with-pgo-profiles)
fi

echo "Building codec-enabled CEF branch ${CEF_BRANCH} for ${platform}."
echo "This downloads and builds Chromium; expect substantial time and disk use."
"${python_bin}" "${args[@]}"

distrib_dir="${build_root}/chromium_git/chromium/src/cef/binary_distrib"
archives=()
while IFS= read -r archive; do archives+=("${archive}"); done < <(
    find "${distrib_dir}" -maxdepth 1 -type f \
        -name "cef_binary_*_${platform}_minimal.tar.bz2" -print | sort)

if [[ "${arch_flag}" == "--arm64-build" ]]; then
    args_name="Release_GN_arm64"
else
    args_name="Release_GN_x64"
fi
gn_args="${build_root}/chromium_git/chromium/src/out/${args_name}/args.gn"
if [[ ! -f "${gn_args}" ]]; then
    echo "Cannot find CEF's actual GN arguments: ${gn_args}" >&2
    exit 1
fi
source_description=""
if [[ "${#archives[@]}" -gt 0 ]]; then
    archive="${archives[$((${#archives[@]} - 1))]}"
    tar -xf "${archive}" -C "${output_dir}" --strip-components=1
    source_description="${archive}"
else
    directories=()
    while IFS= read -r directory; do directories+=("${directory}"); done < <(
        find "${distrib_dir}" -maxdepth 1 -type d \
            -name "cef_binary_*_${platform}_minimal" -print | sort)
    if [[ "${#directories[@]}" -eq 0 ]]; then
        echo "No minimal CEF distribution was produced." >&2
        exit 1
    fi
    distribution="${directories[$((${#directories[@]} - 1))]}"
    cp -R "${distribution}/." "${output_dir}/"
    source_description="${distribution}"
fi
cp "${gn_args}" "${output_dir}/codec-args.gn"
cat > "${output_dir}/CODEC_BUILD.txt" <<EOF
Codec-enabled CEF release branch: ${CEF_BRANCH}
Platform: ${platform}
GN_DEFINES: ${GN_DEFINES}
Source distribution: ${source_description}
EOF

echo "Codec-enabled CEF is ready at ${output_dir}"
echo "Configure Aurora with:"
echo "  cmake -S ${repo_root} -B ${repo_root}/build -DAURORA_ENABLE_CEF=ON \\"
echo "    -DAURORA_BUILD_APP=ON -DAURORA_REQUIRE_H264_AAC=ON -DCEF_ROOT=${output_dir}"
