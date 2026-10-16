%global build_timestamp %(date +"%Y%m%d")

# use sed to replace these values
%global build_version 0
%global branch 0
%global commit 0

# Keep the application-facing version strict SemVer while translating only
# the RPM package version into RPM ordering syntax. Prereleases sort below the
# final release; stable.N respins sort above it.
%global build_semver %{lua:local v=rpm.expand("%{build_version}"); if v:sub(1,1)=="v" then v=v:sub(2) end; print(v)}
%global rpm_version %{lua:local v=rpm.expand("%{build_semver}"); local p=v:find("-",1,true); if p then if v:sub(p+1,p+6)=="stable" then v=v:sub(1,p-1).."+stable"..v:sub(p+7) else v=v:sub(1,p-1).."~"..v:sub(p+1) end end; print(v)}

%undefine _hardened_build

# Define _metainfodir for OpenSUSE if not already defined
%if 0%{?suse_version}
%if !0%{?_metainfodir:1}
%global _metainfodir %{_datadir}/metainfo
%endif
%endif

Name: vibepollo
Version: %{rpm_version}
Release: 1%{?dist}
Summary: Self-hosted game stream host for Moonlight.
License: GPLv3-only
URL: https://github.com/Nonary/Vibepollo
Source0: tarball.tar.gz

# Common BuildRequires
BuildRequires: cmake >= 3.25.0
BuildRequires: desktop-file-utils
BuildRequires: git
BuildRequires: libcap-devel
BuildRequires: libcurl-devel
BuildRequires: libdrm-devel
BuildRequires: libevdev-devel
BuildRequires: libnotify-devel
BuildRequires: libva-devel
BuildRequires: libX11-devel
BuildRequires: libxcb-devel
BuildRequires: libXcursor-devel
BuildRequires: libXfixes-devel
BuildRequires: libXi-devel
BuildRequires: libXinerama-devel
BuildRequires: libXrandr-devel
BuildRequires: libXtst-devel
BuildRequires: openssl-devel
BuildRequires: pipewire-devel
BuildRequires: rpm-build
BuildRequires: systemd-rpm-macros
BuildRequires: wget
BuildRequires: which

%if 0%{?fedora}
# Fedora-specific BuildRequires
BuildRequires: appstream
# BuildRequires: boost-devel >= 1.86.0
BuildRequires: glslc
BuildRequires: libappstream-glib
BuildRequires: vulkan-loader-devel
BuildRequires: libayatana-appindicator3-devel
BuildRequires: libgudev
BuildRequires: mesa-libGL-devel
BuildRequires: mesa-libgbm-devel
BuildRequires: miniupnpc-devel
BuildRequires: numactl-devel
BuildRequires: opus-devel
BuildRequires: pulseaudio-libs-devel
BuildRequires: python3-jinja2
BuildRequires: python3-setuptools
BuildRequires: systemd-udev
%{?sysusers_requires_compat}
%endif

%if 0%{?suse_version}
# OpenSUSE-specific BuildRequires
BuildRequires: AppStream
BuildRequires: appstream-glib
BuildRequires: libappindicator3-devel
BuildRequires: libgudev-1_0-devel
BuildRequires: Mesa-libGL-devel
BuildRequires: libgbm-devel
BuildRequires: libminiupnpc-devel
BuildRequires: libnuma-devel
BuildRequires: libopus-devel
BuildRequires: libpulse-devel
BuildRequires: python311
BuildRequires: python311-Jinja2
BuildRequires: python311-setuptools
%if !0%{?sle_version}
BuildRequires: shaderc
%endif
BuildRequires: udev
%if !0%{?sle_version}
BuildRequires: vulkan-devel
%endif
%endif

# Conditional BuildRequires for cuda-gcc based on distribution version
%if 0%{?fedora}
%if 0%{?fedora} <= 41
BuildRequires: gcc13
BuildRequires: gcc13-c++
%global gcc_version 13
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 42 && 0%{?fedora} <= 43
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%elif 0%{?fedora} >= 44
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%endif
%endif

%if 0%{?suse_version}
%if 0%{?suse_version} <= 1699
# OpenSUSE Leap 15.x
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%else
# OpenSUSE Tumbleweed
BuildRequires: gcc14
BuildRequires: gcc14-c++
%global gcc_version 14
%global cuda_version 12.9.1
%global cuda_build 575.57.08
%endif
%endif

%global cuda_dir %{_builddir}/cuda

# Common runtime requirements
Requires: miniupnpc >= 2.2.4
Requires: kmod
Requires: iproute
Requires: jq
Requires: /usr/bin/python3
Requires: /usr/bin/pactl
Requires: /usr/bin/parec
Requires: /usr/bin/python3
Requires: /usr/bin/wayland-info
Requires: /usr/bin/xdpyinfo
Requires: socat
Requires: util-linux
Recommends: dkms
Recommends: gcc
Recommends: kernel-devel
Recommends: make

%if 0%{?fedora}
# Fedora runtime requirements
Requires: python3 >= 3.9
Requires: libayatana-appindicator3 >= 0.5.3
Requires: libcap >= 2.22
Requires: libcurl >= 7.0
Requires: libdrm > 2.4.97
Requires: libevdev >= 1.5.6
Requires: libkscreen
Requires: libopusenc >= 0.2.1
Requires: libva >= 2.14.0
Requires: libwayland-client >= 1.20.0
Requires: libX11 >= 1.7.3.1
Requires: numactl-libs >= 2.0.14
Requires: openssl >= 3.0.2
Requires: pulseaudio-libs >= 10.0
Requires: vulkan-loader
%endif

%if 0%{?suse_version}
# OpenSUSE runtime requirements
Requires: python311
Requires: libappindicator3-1
Requires: libcap2
Requires: libcurl4
Requires: libdrm2
Requires: libevdev2
# The binary moved between openSUSE KScreen package generations; use the RPM
# file capability so zypper selects the provider for the active release.
Requires: /usr/bin/kscreen-doctor
Requires: libopusenc0
Requires: libva2
Requires: libwayland-client0
Requires: libX11-6
Requires: libnuma1
Requires: libopenssl3
Requires: libpulse0
%if !0%{?sle_version}
Requires: libvulkan1
%endif
%endif

%description
Self-hosted game stream host for Moonlight.

%prep
# extract tarball to current directory
mkdir -p %{_builddir}/Sunshine
tar -xzf %{SOURCE0} -C %{_builddir}/Sunshine

# list directory
ls -a %{_builddir}/Sunshine

%build
# exit on error
set -e

# Detect the architecture and Fedora version
architecture=$(uname -m)

cuda_supported_architectures=("x86_64" "aarch64")

# prepare CMAKE args
cmake_args=(
  "-B=%{_builddir}/Sunshine/build"
  "-G=Unix Makefiles"
  "-S=."
  "-DBUILD_DOCS=OFF"
  "-DBUILD_TESTS=ON"
  "-DBUILD_WERROR=ON"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_INSTALL_PREFIX=%{_prefix}"
  "-DSUNSHINE_ASSETS_DIR=%{_datadir}/vibepollo"
  "-DSUNSHINE_EXECUTABLE_PATH=%{_bindir}/vibepollo"
  "-DSUNSHINE_ENABLE_DRM=ON"
  "-DSUNSHINE_ENABLE_KWIN=ON"
  "-DSUNSHINE_ENABLE_PORTAL=ON"
  "-DSUNSHINE_ENABLE_WAYLAND=ON"
  "-DSUNSHINE_ENABLE_X11=ON"
  "-DSUNSHINE_PUBLISHER_NAME=Nonary"
  "-DSUNSHINE_PUBLISHER_WEBSITE=https://github.com/Nonary/Vibepollo"
  "-DSUNSHINE_PUBLISHER_ISSUE_URL=https://github.com/Nonary/Vibepollo/issues"
)

export CC=gcc-%{gcc_version}
export CXX=g++-%{gcc_version}

function install_cuda() {
  # check if we need to install cuda
  if [ -f "%{cuda_dir}/bin/nvcc" ]; then
    echo "cuda already installed"
    return
  fi

  local cuda_prefix="https://developer.download.nvidia.com/compute/cuda/"
  local cuda_suffix=""
  if [ "$architecture" == "aarch64" ]; then
    local cuda_suffix="_sbsa"
  fi

  local url="${cuda_prefix}%{cuda_version}/local_installers/cuda_%{cuda_version}_%{cuda_build}_linux${cuda_suffix}.run"
  echo "cuda url: ${url}"
  wget \
    "$url" \
    --progress=bar:force:noscroll \
    --retry-connrefused \
    --tries=3 \
    -q -O "%{_builddir}/cuda.run"
  chmod a+x "%{_builddir}/cuda.run"
  "%{_builddir}/cuda.run" \
    --no-drm \
    --no-man-page \
    --no-opengl-libs \
    --override \
    --silent \
    --toolkit \
    --toolkitpath="%{cuda_dir}"
  rm "%{_builddir}/cuda.run"

  # we need to patch math_functions.h depending on the CUDA major version
  # see https://forums.developer.nvidia.com/t/error-exception-specification-is-incompatible-for-cospi-sinpi-cospif-sinpif-with-glibc-2-41/323591/3
  local cuda_major
  cuda_major=$(echo "%{cuda_version}" | cut -d. -f1)
  local patch_file=""
  if [ "${cuda_major}" -eq 12 ]; then
    # CUDA 12.x: the extern declarations lack noexcept(true); add it to match glibc 2.41.
    patch_file="cuda-12-math_functions.patch"
  elif [ "${cuda_major}" -eq 13 ]; then
    # CUDA 13.x: the extern declarations already have noexcept(true), but the __func__()
    # macro invocations at the bottom still lack it, causing a redeclaration conflict.
    patch_file="cuda-13-math_functions.patch"
  else
    echo "Warning: no math_functions.h patch available for CUDA ${cuda_major}.x, skipping."
  fi

  if [ -n "${patch_file}" ]; then
    echo "Applying CUDA patch: ${patch_file}"
    patch -p2 \
      --backup \
      --directory="%{cuda_dir}" \
      --verbose \
      < "%{_builddir}/Sunshine/packaging/linux/patches/${architecture}/${patch_file}"
  fi
}

if [ -n "%{cuda_version}" ] && [[ " ${cuda_supported_architectures[@]} " =~ " ${architecture} " ]]; then
  install_cuda
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=ON" "-DSUNSHINE_REQUIRE_CUDA_PASCAL=ON")
  cmake_args+=("-DCMAKE_CUDA_COMPILER:PATH=%{cuda_dir}/bin/nvcc")
  cmake_args+=("-DCMAKE_CUDA_HOST_COMPILER=gcc-%{gcc_version}")
else
  cmake_args+=("-DSUNSHINE_ENABLE_CUDA=OFF")
fi

# setup the version
export BRANCH=%{branch}
export BUILD_VERSION=%{build_semver}
export COMMIT=%{commit}

# Disable Vulkan on openSUSE Leap (shaderc/glslang not in official repos)
%if 0%{?sle_version}
cmake_args+=("-DSUNSHINE_ENABLE_VULKAN=OFF")
cmake_args+=("-DSUNSHINE_ENABLE_PYROWAVE=OFF")
%endif

# cmake
cd %{_builddir}/Sunshine
echo "cmake args:"
echo "${cmake_args[@]}"
cmake "${cmake_args[@]}"
make -j$(nproc) -C "%{_builddir}/Sunshine/build"

%check
# validate the metainfo file
appstreamcli validate %{buildroot}%{_metainfodir}/*.metainfo.xml
appstream-util validate %{buildroot}%{_metainfodir}/*.metainfo.xml
desktop-file-validate %{buildroot}%{_datadir}/applications/*.desktop

%install
cd %{_builddir}/Sunshine/build
%make_install
# The shared CMake stage includes ALPM-only pretransaction files. RPM has its
# own package scriptlets and must not ship files outside its manifest.
rm -f %{buildroot}%{_prefix}/libexec/vibeshine/vibepollo-package-preflight
rm -f %{buildroot}%{_datadir}/libalpm/hooks/00-vibepollo-quiesce.hook
rm -f %{buildroot}%{_datadir}/vibepollo/arch-package-hooks
rmdir %{buildroot}%{_datadir}/libalpm/hooks %{buildroot}%{_datadir}/libalpm 2>/dev/null || true

%pre
export LC_ALL=C
vibepollo_controller=%{_prefix}/libexec/vibeshine/vibepollo-session-controller
vibepollo_legacy_host=%{_prefix}/libexec/vibeshine/vibepollo-machine-host
vibepollo_legacy_handoff=%{_prefix}/libexec/vibeshine/vibepollo-session-handoff
vibepollo_runtime_root=/run/vibepollo
vibepollo_broker_socket=/run/vibepollo/session-broker.sock
vibepollo_control_socket=/run/vibeshine/vkms-control.sock
vibepollo_host_upgrade_dropin_dir=/run/systemd/system/vibepollo.service.d
vibepollo_host_upgrade_dropin=$vibepollo_host_upgrade_dropin_dir/90-vibepollo-safe-upgrade.conf
vibepollo_controller_was_frozen=0
vibepollo_upgrade_kill_mode=
vibepollo_legacy_handoff_directory=/run/vibepollo/session-handoffs
vibepollo_legacy_restore_directory=/run/vibepollo/session-restores
vibepollo_legacy_transition_lock=/run/vibepollo/session-handoff.lock
vibepollo_legacy_prelogin_marker=/run/vibepollo/prelogin-handoff-complete
vibepollo_cgroup_is_quiescent() {
  vibepollo_control_group=$1
  [ -n "$vibepollo_control_group" ] || return 0
  case "$vibepollo_control_group" in /*) ;; *) return 1 ;; esac
  case "$vibepollo_control_group" in */../* | */..) return 1 ;; esac
  vibepollo_cgroup_path=/sys/fs/cgroup$vibepollo_control_group
  if [ ! -e "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ]; then return 0; fi
  [ -d "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ] && \
    [ -f "$vibepollo_cgroup_path/cgroup.events" ] && \
    [ ! -L "$vibepollo_cgroup_path/cgroup.events" ] && \
    grep -qx 'populated 0' "$vibepollo_cgroup_path/cgroup.events"
}
vibepollo_unit_is_quiescent() {
  vibepollo_properties=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup 2>/dev/null) || return 1
  vibepollo_property_lines=$(printf '%%s\n' "$vibepollo_properties" | wc -l | tr -d ' ')
  case "$1:$vibepollo_property_lines" in
    *.socket:4 | *.socket:5 | *:5) ;;
    *) return 1 ;;
  esac
  for vibepollo_property in LoadState ActiveState SubState ControlGroup; do
    vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | \
      grep -c "^$vibepollo_property=" || true)
    [ "$vibepollo_property_count" = 1 ] || return 1
  done
  vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | grep -c '^MainPID=' || true)
  case "$1:$vibepollo_property_count" in
    *.socket:0 | *.socket:1 | *:1) ;;
    *) return 1 ;;
  esac
  vibepollo_load=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^LoadState=//p')
  vibepollo_state=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ActiveState=//p')
  vibepollo_substate=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^SubState=//p')
  vibepollo_pid=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^MainPID=//p')
  case "$1" in *.socket) [ -n "$vibepollo_pid" ] || vibepollo_pid=0 ;; esac
  vibepollo_control_group=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ControlGroup=//p')
  case "$vibepollo_load:$vibepollo_state:$vibepollo_substate" in
    not-found:inactive:dead | \
    loaded:inactive:dead | loaded:failed:failed | \
    masked:inactive:dead | masked:failed:failed | \
    masked-runtime:inactive:dead | masked-runtime:failed:failed) ;;
    *) return 1 ;;
  esac
  [ "$vibepollo_pid" = 0 ] && vibepollo_cgroup_is_quiescent "$vibepollo_control_group"
}
vibepollo_unit_is_disabled() {
  vibepollo_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  case "$vibepollo_enabled" in
    disabled | masked | masked-runtime | static | indirect | generated | transient | linked | linked-runtime | not-found) return 0 ;;
    *) return 1 ;;
  esac
}
vibepollo_unit_is_masked() {
  vibepollo_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  vibepollo_load=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_enabled" in
    masked | masked-runtime) [ "$vibepollo_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibepollo_restore_template_is_masked() {
  vibepollo_restore_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    'vibepollo-session-restore@.service' 2>/dev/null || true)
  vibepollo_restore_load=$(timeout --signal=KILL 5 systemctl show \
    'vibepollo-session-restore@.service' --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_restore_mask" in
    masked | masked-runtime) [ "$vibepollo_restore_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibepollo_host_unit_is_masked() {
  vibepollo_host_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibepollo.service 2>/dev/null || true)
  vibepollo_host_load=$(timeout --signal=KILL 5 systemctl show \
    vibepollo.service --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_host_mask" in
    masked | masked-runtime) [ "$vibepollo_host_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibepollo_broker_socket_is_masked() {
  vibepollo_socket_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibepollo-session-exec.socket 2>/dev/null || true)
  vibepollo_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-exec.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_socket_mask" in
    masked | masked-runtime) [ "$vibepollo_socket_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibepollo_control_socket_is_masked() {
  vibepollo_socket_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibeshine-vkms-control.socket 2>/dev/null || true)
  vibepollo_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-vkms-control.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_socket_mask" in
    masked | masked-runtime) [ "$vibepollo_socket_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibepollo_restore_unit_is_safe() {
  vibepollo_restore_unit=$1
  case "$vibepollo_restore_unit" in vibepollo-session-restore@*.service) ;; *) return 1 ;; esac
  vibepollo_restore_instance=${vibepollo_restore_unit#vibepollo-session-restore@}
  vibepollo_restore_instance=${vibepollo_restore_instance%.service}
  case "$vibepollo_restore_instance" in [1-9]*) ;; *) return 1 ;; esac
  case "$vibepollo_restore_instance" in *[!0-9]*) return 1 ;; esac
}
vibepollo_broker_unit_is_safe() {
  vibepollo_broker_unit=$1
  case "$vibepollo_broker_unit" in vibepollo-session-exec@*.service) ;; *) return 1 ;; esac
  vibepollo_broker_instance=${vibepollo_broker_unit#vibepollo-session-exec@}
  vibepollo_broker_instance=${vibepollo_broker_instance%.service}
  [ -n "$vibepollo_broker_instance" ] || return 1
  case "$vibepollo_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
vibepollo_control_unit_is_safe() {
  vibepollo_control_unit=$1
  case "$vibepollo_control_unit" in vibeshine-vkms-control@*.service) ;; *) return 1 ;; esac
  vibepollo_control_instance=${vibepollo_control_unit#vibeshine-vkms-control@}
  vibepollo_control_instance=${vibepollo_control_instance%.service}
  [ -n "$vibepollo_control_instance" ] || return 1
  case "$vibepollo_control_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
# Read the exact fenced host identity. A changed invocation must never inherit
# an earlier upgrade's escalation request, even if Linux has reused its PID.
vibepollo_package_capture_host_executables() {
  if [ ! -e /usr/bin/vibepollo ] && [ ! -L /usr/bin/vibepollo ] &&
     [ ! -e /usr/libexec/vibeshine/vibepollo-host ] && [ ! -L /usr/libexec/vibeshine/vibepollo-host ]; then
    printf '[]\n'
    return 0
  fi
  [ -x /usr/bin/python3 ] || {
    echo 'error: trusted Python pid/process inspection is required before replacing an installed host.' >&2
    return 1
  }
  timeout --signal=KILL 10 /usr/bin/python3 -I - <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
records = []
try:
    for name in paths:
        path = pathlib.Path(name)
        try:
            entry = path.lstat()
        except FileNotFoundError:
            continue
        if entry.st_uid != 0 or (stat.S_ISLNK(entry.st_mode) and name != paths[0]):
            raise ValueError("unsafe installed host executable")
        resolved = path.resolve(strict=True)
        identity = resolved.stat()
        if identity.st_uid != 0 or not stat.S_ISREG(identity.st_mode) or identity.st_mode & 0o022:
            raise ValueError("unsafe installed host executable")
        # A public package entrypoint may be a root-owned symlink; both its
        # original and resolved directory ancestry must remain root-owned.
        for parent in set((path.parent, *path.parent.parents, resolved.parent, *resolved.parent.parents)):
            owner = parent.stat()
            if owner.st_uid != 0 or not stat.S_ISDIR(owner.st_mode) or owner.st_mode & 0o022:
                raise ValueError("unsafe installed host executable ancestry")
        records.append({"path": name, "device": identity.st_dev, "inode": identity.st_ino})
    print(json.dumps(records, separators=(",", ":")))
except (OSError, ValueError) as error:
    print(f"error: installed host identity could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_package_unmanaged_host_absent() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before" || return 1
  vibepollo_package_executable_records=$(cat -- "$vibepollo_package_transaction/host-executables.before") || return 1
  if [ ! -x /usr/bin/python3 ]; then
    [ "$vibepollo_package_executable_records" = '[]' ] || {
      echo 'error: trusted Python process inspection is required for the retained old host inode snapshot.' >&2
      return 1
    }
    # First-install pre-hooks can run before Python dependencies are installed.
    # An empty capture still requires a bounded exact-path scan, including old
    # root-owned deleted executables; it is never treated as an absence proof.
    timeout --signal=KILL 10 /bin/sh -s <<'HOST_PROOF'
command -v readlink >/dev/null 2>&1 && command -v stat >/dev/null 2>&1 || exit 1
for executable in /proc/[0-9]*/exe; do
  target=$(readlink -v -- "$executable" 2>&1)
  status=$?
  if [ "$status" -ne 0 ]; then
    case "$target" in
      *': No such file or directory') continue ;;
      *) echo 'error: executable scope inspection is uncertain; replacement is blocked.' >&2; exit 1 ;;
    esac
  fi
  case "$target" in
    /usr/bin/vibepollo | '/usr/bin/vibepollo (deleted)' | \
    /usr/libexec/vibeshine/vibepollo-host | '/usr/libexec/vibeshine/vibepollo-host (deleted)')
      identity=$(stat -Lc '%%u:%%F' -- "$executable" 2>/dev/null) || exit 1
      if [ "$identity" = '0:regular file' ]; then
        process_id=${executable%/exe}
        process_id=${process_id##*/}
        echo "error: verified old host executable remains in PID $process_id; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership." >&2
        exit 1
      fi
      ;;
  esac
done
HOST_PROOF
    return $?
  fi
  timeout --signal=KILL 10 /usr/bin/python3 -I - "$vibepollo_package_executable_records" <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
try:
    records = json.loads(sys.argv[1])
    if not isinstance(records, list) or len(records) > 2:
        raise ValueError("invalid executable identity snapshot")
    identities = set()
    names = set()
    for record in records:
        if not isinstance(record, dict) or set(record) != {"path", "device", "inode"}:
            raise ValueError("invalid executable identity snapshot")
        if record["path"] not in paths or record["path"] in names:
            raise ValueError("invalid executable identity snapshot")
        if any(type(record[key]) is not int or record[key] <= 0 for key in ("device", "inode")):
            raise ValueError("invalid executable identity snapshot")
        names.add(record["path"])
        identities.add((record["device"], record["inode"]))
    # Linux retains executable inode identity after unlink/replacement. Scan
    # every scope, using no argv/name matching and issuing no process signals.
    for process in pathlib.Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            executable = process / "exe"
            identity = executable.stat()
            target = os.readlink(executable)
        except (FileNotFoundError, ProcessLookupError):
            continue
        def report_old_owner(deleted=False):
            uid, scope = "unknown", "<unavailable>"
            try:
                # Diagnostics must describe the same observed task and inode.
                # PID reuse or disappearing metadata still blocks replacement;
                # never attribute a different process's UID/scope to this owner.
                before = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                status = (process / "status").read_text()
                uid_lines = [line.split()[1:] for line in status.splitlines() if line.startswith("Uid:")]
                if len(uid_lines) != 1 or len(uid_lines[0]) != 4 or not all(value.isdecimal() for value in uid_lines[0]):
                    raise ValueError("uncertain process UID")
                cgroup = (process / "cgroup").read_text()
                if len(cgroup) > 8192:
                    raise ValueError("oversized process scope")
                after = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                confirmed = executable.stat()
                if before != after or (confirmed.st_dev, confirmed.st_ino) != (identity.st_dev, identity.st_ino):
                    raise ValueError("changed process identity")
                uid, scope = uid_lines[0][1], cgroup.rstrip("\n")
            except (OSError, ValueError, IndexError):
                pass
            kind = "deleted old" if deleted else "old"
            print(f"error: verified {kind} host executable remains in PID {process.name}, UID {uid}, scope {json.dumps(scope)}; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership.", file=sys.stderr)
        if (identity.st_dev, identity.st_ino) in identities:
            report_old_owner()
            sys.exit(1)
        # A failed older transaction may lack a retained inode record. A
        # kernel-reported deleted executable at either exact root-owned package
        # path still supplies positive evidence of an old owner, never absence.
        if target in tuple(name + " (deleted)" for name in paths) and identity.st_uid == 0 and stat.S_ISREG(identity.st_mode) and not identity.st_mode & 0o022:
            report_old_owner(deleted=True)
            sys.exit(1)
except (OSError, ValueError, TypeError) as error:
    print(f"error: cross-scope old host absence could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_package_backup_for_force() {
  # Forced termination can interrupt a configuration write. Keep a private
  # snapshot before signalling; never follow service-owned links or mounts.
  # Logs are excluded because an active stream can keep changing them.
  umask 077
  vibepollo_backup_base=/var/lib
  vibepollo_backup_root=$vibepollo_backup_base/vibepollo-package-backups
  [ -d "$vibepollo_backup_base" ] && [ ! -L "$vibepollo_backup_base" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_base")" = '0:0:755:directory' ] || return 1
  if [ ! -e "$vibepollo_backup_root" ] && [ ! -L "$vibepollo_backup_root" ]; then
    mkdir -m 0700 -- "$vibepollo_backup_root" || return 1
  fi
  [ -d "$vibepollo_backup_root" ] && [ ! -L "$vibepollo_backup_root" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_root")" = '0:0:700:directory' ] || return 1
  vibepollo_backup_directory=$(mktemp -d "$vibepollo_backup_root/forced-stop.XXXXXX") || return 1
  [ -d "$vibepollo_backup_directory" ] && [ ! -L "$vibepollo_backup_directory" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_directory")" = '0:0:700:directory' ] || return 1
  set --
  for vibepollo_backup_source in etc/vibepollo var/lib/vibepollo; do
    if [ -e "/$vibepollo_backup_source" ] || [ -L "/$vibepollo_backup_source" ]; then
      [ -d "/$vibepollo_backup_source" ] && [ ! -L "/$vibepollo_backup_source" ] || return 1
      set -- "$@" "$vibepollo_backup_source"
    fi
  done
  command -v tar >/dev/null 2>&1 || return 1
  if [ "$#" -gt 0 ]; then
    timeout --signal=TERM --kill-after=2 30 tar --one-file-system \
      --exclude=var/lib/vibepollo/logs -C / -cf "$vibepollo_backup_directory/machine-state.tar" -- "$@" \
      >"$vibepollo_backup_directory/backup.log" 2>&1 || {
        echo "error: machine-state backup failed; forced shutdown was not attempted. Diagnostics: $vibepollo_backup_directory/backup.log" >&2
        return 1
      }
  else
    timeout --signal=TERM --kill-after=2 30 tar -cf "$vibepollo_backup_directory/machine-state.tar" \
      --files-from=/dev/null >"$vibepollo_backup_directory/backup.log" 2>&1 || return 1
  fi
  chmod 0600 -- "$vibepollo_backup_directory/machine-state.tar" "$vibepollo_backup_directory/backup.log" || return 1
  {
    printf 'product=vibepollo\noperation=forced-package-stop\nsnapshot=machine-state.tar\n'
    printf 'invocation=%%s\ncgroup=%%s\n' "$vibepollo_original_invocation" "$vibepollo_original_cgroup"
    printf 'Warning: snapshot taken while the old host may still be running; logs excluded.\n'
    if command -v pacman >/dev/null 2>&1; then timeout --signal=KILL 5 pacman -Q vibepollo 2>/dev/null || :; fi
    if command -v dpkg-query >/dev/null 2>&1; then timeout --signal=KILL 5 dpkg-query -W vibepollo 2>/dev/null || :; fi
    if command -v rpm >/dev/null 2>&1; then timeout --signal=KILL 5 rpm -q vibepollo 2>/dev/null || :; fi
  } >"$vibepollo_backup_directory/recovery.log" || return 1
  chmod 0600 -- "$vibepollo_backup_directory/recovery.log" || return 1
  echo "warning: forced shutdown will terminate active streams and may leave normal cleanup incomplete. Private machine-state snapshot and recovery log: $vibepollo_backup_directory" >&2
}

vibepollo_package_log_force_outcome() {
  case "$1" in pending | complete | kill-failed | still-populated | identity-changed | graceful-after-backup) ;; *) return 1 ;; esac
  case "${vibepollo_backup_directory:-}" in /var/lib/vibepollo-package-backups/forced-stop.*) ;; *) return 1 ;; esac
  [ -d "$vibepollo_backup_directory" ] && [ ! -L "$vibepollo_backup_directory" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_directory")" = '0:0:700:directory' ] || return 1
  [ -f "$vibepollo_backup_directory/recovery.log" ] && [ ! -L "$vibepollo_backup_directory/recovery.log" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_backup_directory/recovery.log")" = '0:0:600:1:regular file' ] || return 1
  printf 'outcome=%%s\n' "$1" >>"$vibepollo_backup_directory/recovery.log"
}



vibepollo_read_host_stop_identity() {
  if [ "$#" -eq 1 ]; then
    vibepollo_stop_properties=$1
  else
    vibepollo_stop_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
      --property=Id --property=InvocationID --property=MainPID --property=ControlGroup \
      --property=Restart --property=RefuseManualStart 2>/dev/null) || return 1
  fi
  vibepollo_stop_id='' vibepollo_stop_invocation='' vibepollo_stop_pid=''
  vibepollo_stop_cgroup='' vibepollo_stop_restart='' vibepollo_stop_fenced=''
  vibepollo_stop_fields=0
  while IFS= read -r vibepollo_stop_line; do
    vibepollo_stop_fields=$((vibepollo_stop_fields + 1))
    case "$vibepollo_stop_line" in
      Id=*) vibepollo_stop_id=${vibepollo_stop_line#*=} ;;
      InvocationID=*) vibepollo_stop_invocation=${vibepollo_stop_line#*=} ;;
      MainPID=*) vibepollo_stop_pid=${vibepollo_stop_line#*=} ;;
      ControlGroup=*) vibepollo_stop_cgroup=${vibepollo_stop_line#*=} ;;
      Restart=*) vibepollo_stop_restart=${vibepollo_stop_line#*=} ;;
      RefuseManualStart=*) vibepollo_stop_fenced=${vibepollo_stop_line#*=} ;;
      *) return 1 ;;
    esac
  done <<EOF
$vibepollo_stop_properties
EOF
  [ "$vibepollo_stop_fields" -eq 6 ] && [ "$vibepollo_stop_id" = vibepollo.service ] && \
    [ "$vibepollo_stop_restart" = no ] && [ "$vibepollo_stop_fenced" = yes ] || return 1
  [ "${#vibepollo_stop_invocation}" -eq 32 ] || return 1
  case "$vibepollo_stop_invocation" in *[!0-9a-f]*) return 1 ;; esac
  case "$vibepollo_stop_pid" in ''|*[!0-9]*) return 1 ;; esac
  case "$vibepollo_stop_cgroup" in
    */../*|*/./*|*//*|*/..|*/.) return 1 ;;
    /*/vibepollo.service) ;;
    *) return 1 ;;
  esac
}

# Linux exposes a monotonic clock here; wall-clock corrections must not shorten
# the graceful shutdown interval or leave a retry waiting without a deadline.
vibepollo_host_stop_now() {
  IFS=' .' read -r vibepollo_stop_now vibepollo_stop_unused < /proc/uptime || return 1
  case "$vibepollo_stop_now" in ''|*[!0-9]*) return 1 ;; esac
}

vibepollo_stop_host_for_package() (
  vibepollo_unit_is_quiescent vibepollo.service && return 0
  vibepollo_read_host_stop_identity || return 1
  vibepollo_current_stop_properties=$vibepollo_stop_properties
  # Retain the original invocation across a timed-out transaction retry. A
  # replacement owner must not inherit permission to force the former owner.
  if [ -e "$vibepollo_package_transaction/host-stop.before" ] || [ -L "$vibepollo_package_transaction/host-stop.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-stop.before" || return 1
    vibepollo_saved_stop_properties=$(cat -- "$vibepollo_package_transaction/host-stop.before") || return 1
    vibepollo_read_host_stop_identity "$vibepollo_saved_stop_properties" || return 1
  else
    vibepollo_package_write_record host-stop.before "$vibepollo_current_stop_properties" || return 1
  fi
  vibepollo_original_invocation=$vibepollo_stop_invocation
  vibepollo_original_pid=$vibepollo_stop_pid
  vibepollo_original_cgroup=$vibepollo_stop_cgroup
  vibepollo_read_host_stop_identity "$vibepollo_current_stop_properties" || return 1
  [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] && \
    [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] && \
    { [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || [ "$vibepollo_stop_pid" = 0 ]; } || return 1
  vibepollo_host_stop_now || return 1
  vibepollo_host_stop_deadline=$((vibepollo_stop_now + 60))
  # TERM reaches the supervisor first so capture/encoding can drain while its
  # broker clients remain connected. Older hosts may lack an internal deadline.
  timeout --signal=TERM --kill-after=2 60 systemctl stop vibepollo.service \
    2>/dev/null || :
  while :; do
    vibepollo_unit_is_quiescent vibepollo.service && return 0
    vibepollo_read_host_stop_identity || return 1
    [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] && \
      [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] || return 1
    # MainPID can become zero while old subordinate clients remain in the
    # cgroup. A different nonzero owner is never an escalation target.
    [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || \
      [ "$vibepollo_stop_pid" = 0 ] || return 1
    vibepollo_host_stop_now || return 1
    [ "$vibepollo_stop_now" -lt "$vibepollo_host_stop_deadline" ] || break
    sleep 0.1
  done
  if [ "${VIBEPOLLO_PACKAGE_FORCE:-}" != 1 ]; then
    if command -v vibepollo_package_run_is_owned >/dev/null 2>&1 && vibepollo_package_run_is_owned; then
      vibepollo_package_write_record failure-class host-stop-deadline || return 1
    fi
    echo 'error: Vibepollo did not stop within 60 seconds; replacement is blocked. Retry with VIBEPOLLO_PACKAGE_FORCE=1 (or installer --force-stop) to authorize forced shutdown.' >&2
    return 1
  fi
  # Preserve private machine state and the recovery record before the first
  # irreversible forced signal. Missing/failed backup support blocks force.
  command -v vibepollo_package_backup_for_force >/dev/null 2>&1 && \
    vibepollo_package_backup_for_force || return 1
  # Recheck after the potentially slow backup; never inherit an old target.
  if vibepollo_unit_is_quiescent vibepollo.service; then
    vibepollo_package_log_force_outcome graceful-after-backup || return 1
    return 0
  fi
  if ! vibepollo_read_host_stop_identity; then
    vibepollo_package_log_force_outcome identity-changed || :
    return 1
  fi
  if ! { [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] &&
         [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] &&
         { [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || [ "$vibepollo_stop_pid" = 0 ]; }; }; then
    vibepollo_package_log_force_outcome identity-changed || :
    return 1
  fi
  command -v vibepollo_package_log_force_outcome >/dev/null 2>&1 && \
    vibepollo_package_log_force_outcome pending || return 1
  echo 'warning: forced shutdown authorized; terminating the fenced Vibepollo service cgroup after its graceful deadline.' >&2
  timeout --signal=KILL 5 systemctl kill --signal=KILL \
    vibepollo.service 2>/dev/null || {
      vibepollo_package_log_force_outcome kill-failed || :
      return 1
    }
  # Let the manager complete its existing stop job, then require the complete
  # stopped-state and empty-cgroup proof even when force was authorized.
  vibepollo_host_stop_now || return 1
  vibepollo_host_reap_deadline=$((vibepollo_stop_now + 15))
  timeout --signal=TERM --kill-after=2 15 systemctl stop vibepollo.service \
    2>/dev/null || :
  # The kill request can return before the kernel finishes releasing imports.
  # Poll only the existing target; never restart shutdown or adopt a new owner.
  while :; do
    if vibepollo_unit_is_quiescent vibepollo.service; then
      vibepollo_package_log_force_outcome complete || return 1
      echo 'Vibepollo forced shutdown completed; the old service cgroup is empty.' >&2
      return 0
    fi
    vibepollo_read_host_stop_identity || return 1
    if [ "$vibepollo_stop_invocation" != "$vibepollo_original_invocation" ] ||
       [ "$vibepollo_stop_cgroup" != "$vibepollo_original_cgroup" ]; then
      vibepollo_package_log_force_outcome identity-changed || :
      return 1
    fi
    vibepollo_host_stop_now || return 1
    [ "$vibepollo_stop_now" -lt "$vibepollo_host_reap_deadline" ] || break
    sleep 0.1
  done
  vibepollo_package_log_force_outcome still-populated || :
  echo 'error: forced shutdown did not empty the old Vibepollo service cgroup; replacement remains blocked.' >&2
  return 1
)

vibepollo_stop_exact_unit() {
  if [ "$1" = vibepollo.service ]; then
    vibepollo_stop_host_for_package
    return $?
  fi
  timeout --signal=TERM --kill-after=2 60 systemctl stop "$1" 2>/dev/null
}
vibepollo_bounded_unit_list() (
  vibepollo_unit_pattern=$1
  vibepollo_unit_list=$(mktemp /run/vibepollo-unit-list.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_unit_list"' 0
  trap 'exit 1' HUP INT TERM
  chmod 0600 "$vibepollo_unit_list" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units --all --plain \
      --no-legend --no-pager --full "$vibepollo_unit_pattern" \
      >"$vibepollo_unit_list" 2>/dev/null
  ) || return 1
  vibepollo_unit_list_size=$(stat -c '%%s' -- "$vibepollo_unit_list") || return 1
  case "$vibepollo_unit_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_unit_list_size" -le 65536 ] || return 1
  cat -- "$vibepollo_unit_list"
)
vibepollo_control_instances_are_quiescent() (
  vibepollo_control_attempt=0
  vibepollo_control_clean_passes=0
  while [ "$vibepollo_control_attempt" -lt 100 ]; do
    vibepollo_control_dirty=0
    vibepollo_controls=$(vibepollo_bounded_unit_list \
      'vibeshine-vkms-control@*.service') || return 1
    vibepollo_seen_units='
'
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *"\r"*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1; vibepollo_load=$2
      vibepollo_state=$3; vibepollo_substate=$4
      vibepollo_control_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      vibepollo_unit_is_quiescent "$vibepollo_unit" || vibepollo_control_dirty=1
    done <<EOF
$vibepollo_controls
EOF
    if [ "$vibepollo_control_dirty" -eq 0 ]; then
      vibepollo_control_clean_passes=$((vibepollo_control_clean_passes + 1))
      [ "$vibepollo_control_clean_passes" -lt 5 ] || {
        [ ! -e "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ] || return 1
        return 0
      }
    else
      vibepollo_control_clean_passes=0
    fi
    vibepollo_control_attempt=$((vibepollo_control_attempt + 1))
    sleep 0.1
  done
  return 1
)
vibepollo_stop_brokers() (
  vibepollo_broker_attempt=0
  vibepollo_broker_clean_passes=0
  while [ "$vibepollo_broker_attempt" -lt 20 ]; do
    vibepollo_broker_dirty=0
    vibepollo_brokers=$(vibepollo_bounded_unit_list \
      'vibepollo-session-exec@*.service') || return 1
    vibepollo_seen_units='
'
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *"
"*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1; vibepollo_load=$2
      vibepollo_state=$3; vibepollo_substate=$4
      vibepollo_broker_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      if ! vibepollo_unit_is_quiescent "$vibepollo_unit"; then
        vibepollo_broker_dirty=1
        vibepollo_stop_exact_unit "$vibepollo_unit"
        vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
      fi
    done <<EOF
$vibepollo_brokers
EOF
    if [ "$vibepollo_broker_dirty" -eq 0 ]; then
      vibepollo_broker_clean_passes=$((vibepollo_broker_clean_passes + 1))
      [ "$vibepollo_broker_clean_passes" -lt 5 ] || return 0
    else
      vibepollo_broker_clean_passes=0
    fi
    vibepollo_broker_attempt=$((vibepollo_broker_attempt + 1))
    sleep 0.1
  done
  return 1
)
vibepollo_stop_restore_instances() (
  vibepollo_restores=$(vibepollo_bounded_unit_list \
    'vibepollo-session-restore@*.service') || return 1
  vibepollo_seen_units='
'
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1; vibepollo_load=$2
    vibepollo_state=$3; vibepollo_substate=$4
    vibepollo_restore_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_stop_exact_unit "$vibepollo_unit"
    vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
  done <<EOF
$vibepollo_restores
EOF
)
vibepollo_brokers_are_quiescent() {
  vibepollo_stop_brokers
}
vibepollo_restore_instances_are_quiescent() (
  vibepollo_restores=$(vibepollo_bounded_unit_list \
    'vibepollo-session-restore@*.service') || return 1
  vibepollo_seen_units='
'
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1; vibepollo_load=$2
    vibepollo_state=$3; vibepollo_substate=$4
    vibepollo_restore_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
  done <<EOF
$vibepollo_restores
EOF
)
vibepollo_privileged_helper_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] && [ -x "$1" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = \
    '0:0:755:1:regular file' ]
}
vibepollo_select_upgrade_kill_mode() {
  # A stopped, empty host has no signal contract to preserve. In particular,
  # disabled installations must not depend on obsolete helper source text.
  if vibepollo_unit_is_quiescent vibepollo.service; then
    vibepollo_upgrade_kill_mode=control-group
    return 0
  fi
  vibepollo_privileged_helper_is_safe "$vibepollo_legacy_host" || return 1
  if grep -Fqx '  trap request_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=mixed
  elif grep -Fqx '  trap mark_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=control-group
  elif grep -Fqx "  trap 'forward_host_signal TERM' TERM" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal INT' INT" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal HUP' HUP" "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=process
  else
    # Unknown trusted supervisors get TERM first, without cancelling their
    # clients early. Bounded exact-unit shutdown still must prove an empty
    # cgroup before the package manager can replace any files.
    vibepollo_upgrade_kill_mode=mixed
  fi
}
vibepollo_prepare_host_upgrade_fence() (
  vibepollo_package_preserve_unowned_host_mask || exit 1
  [ "$vibepollo_package_preserve_host_mask" -eq 0 ] || exit 0
  [ ! -L "$vibepollo_host_upgrade_dropin_dir" ] && \
    [ ! -L "$vibepollo_host_upgrade_dropin" ] || exit 1
  install -d -o root -g root -m 0755 -- "$vibepollo_host_upgrade_dropin_dir" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_host_upgrade_dropin_dir")" = \
    '0:0:755:directory' ] || exit 1
  vibepollo_upgrade_temporary=$(mktemp /run/vibepollo-host-upgrade.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_upgrade_temporary"' 0
  case "$vibepollo_upgrade_kill_mode" in mixed | process | control-group) ;; *) exit 1 ;; esac
  printf '[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=%%s\nSendSIGKILL=no\nRestart=no\n' \
    "$vibepollo_upgrade_kill_mode" >"$vibepollo_upgrade_temporary" || exit 1
  chmod 0600 -- "$vibepollo_upgrade_temporary" || exit 1
  if [ -e "$vibepollo_host_upgrade_dropin" ]; then
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || exit 1
    if cmp -s -- "$vibepollo_upgrade_temporary" "$vibepollo_host_upgrade_dropin"; then
      vibepollo_package_record_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
      exit 0
    fi
    # A retry may select a new stop mode after the old host finishes. Only an
    # unchanged owned fence can be replaced, and only with an empty old host.
    vibepollo_unit_is_quiescent vibepollo.service && vibepollo_package_no_jobs || exit 1
    vibepollo_package_remove_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
    [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || exit 1
    rm -f -- "$vibepollo_package_transaction/fence.owned" || exit 1
  fi
  install -o root -g root -m 0644 -- "$vibepollo_upgrade_temporary" \
    "$vibepollo_host_upgrade_dropin" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
    '0:0:644:1:regular file' ] || exit 1
  vibepollo_package_record_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
)
vibepollo_activate_host_upgrade_fence() {
  vibepollo_package_preserve_unowned_host_mask || return 1
  [ "$vibepollo_package_preserve_host_mask" -eq 0 ] || return 0
  vibepollo_package_open_owned_host_for_stop || exit 1
  systemctl daemon-reload || exit 1
  vibepollo_host_stop_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=LoadState --property=RefuseManualStart --property=KillMode --property=SendSIGKILL --property=Restart 2>/dev/null) || exit 1
  if printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'LoadState=not-found'; then
    vibepollo_unit_is_quiescent vibepollo.service || exit 1
    return 0
  fi
  vibepollo_package_properties_are_unique "$vibepollo_host_stop_properties" LoadState RefuseManualStart KillMode SendSIGKILL Restart || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'RefuseManualStart=yes' || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | \
    grep -qx "KillMode=$vibepollo_upgrade_kill_mode" || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'SendSIGKILL=no' || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'Restart=no' || exit 1
}
vibepollo_host_is_stable_or_quiescent() {
  vibepollo_host_state=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup --property=Job 2>/dev/null) || return 1
  vibepollo_host_active=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^ActiveState=//p')
  vibepollo_host_substate=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^SubState=//p')
  vibepollo_host_pid=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^MainPID=//p')
  vibepollo_host_cgroup=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^ControlGroup=//p')
  vibepollo_host_job=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^Job=//p')
  [ -z "$vibepollo_host_job" ] || return 1
  if [ "$vibepollo_host_active" = active ] && [ "$vibepollo_host_substate" = running ]; then
    case "$vibepollo_host_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
    case "$vibepollo_host_cgroup" in /*) return 0 ;; *) return 1 ;; esac
  fi
  case "$vibepollo_host_active:$vibepollo_host_substate:$vibepollo_host_pid" in
    inactive:dead:0 | failed:failed:0) vibepollo_cgroup_is_quiescent "$vibepollo_host_cgroup" ;;
    *) return 1 ;;
  esac
}
vibepollo_freeze_controller() {
  if vibepollo_unit_is_quiescent vibepollo-session-controller.service; then
    vibepollo_controller_was_frozen=0
    return 0
  fi
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  vibepollo_controller_active=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ActiveState=//p')
  vibepollo_controller_substate=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^SubState=//p')
  vibepollo_controller_pid=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^MainPID=//p')
  vibepollo_controller_cgroup=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ControlGroup=//p')
  vibepollo_controller_invocation=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^InvocationID=//p')
  [ "${#vibepollo_controller_invocation}" -eq 32 ] || return 1
  case "$vibepollo_controller_invocation" in *[!0-9a-f]*) return 1 ;; esac
  [ "$vibepollo_controller_active" = active ] && [ "$vibepollo_controller_substate" = running ] || return 1
  case "$vibepollo_controller_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in /*) ;; *) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in */../* | */..) return 1 ;; esac
  # A timed-out freeze may have succeeded; the abort path must attempt thaw.
  vibepollo_controller_was_frozen=1
  timeout --signal=TERM --kill-after=2 15 systemctl freeze \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=frozen' || return 1
  vibepollo_controller_freeze_path=/sys/fs/cgroup$vibepollo_controller_cgroup/cgroup.freeze
  vibepollo_controller_events_path=/sys/fs/cgroup$vibepollo_controller_cgroup/cgroup.events
  [ -f "$vibepollo_controller_freeze_path" ] && [ ! -L "$vibepollo_controller_freeze_path" ] && \
    grep -qx '1' "$vibepollo_controller_freeze_path" && \
    [ -f "$vibepollo_controller_events_path" ] && [ ! -L "$vibepollo_controller_events_path" ] && \
    grep -qx 'populated 1' "$vibepollo_controller_events_path" && \
    grep -qx 'frozen 1' "$vibepollo_controller_events_path" || return 1
}
vibepollo_controller_remains_frozen() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=frozen' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation"
}
vibepollo_thaw_controller() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  vibepollo_controller_remains_frozen || return 1
  timeout --signal=KILL 15 systemctl thaw \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_was_frozen=0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation"
}
vibepollo_run_optional_legacy_command() {
  if [ ! -e "$vibepollo_legacy_host" ] && [ ! -L "$vibepollo_legacy_host" ]; then return 2; fi
  vibepollo_privileged_helper_is_safe "$vibepollo_legacy_host" || return 1
  timeout --signal=KILL 40 "$vibepollo_legacy_host" "$1" >/dev/null 2>&1
  vibepollo_command_status=$?
  [ "$vibepollo_command_status" -eq 0 ] && return 0
  [ "$vibepollo_command_status" -eq 2 ] && return 2
  return 1
}
vibepollo_runtime_root_is_safe_or_absent() {
  if [ ! -e "$vibepollo_runtime_root" ] && [ ! -L "$vibepollo_runtime_root" ]; then return 0; fi
  [ -d "$vibepollo_runtime_root" ] && [ ! -L "$vibepollo_runtime_root" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_runtime_root")" = '0:0:755:directory' ]
}
vibepollo_legacy_handoff_process_is_running() {
  for vibepollo_proc in /proc/[0-9]*; do
    [ -d "$vibepollo_proc" ] || continue
    if grep -Fzxq -- "$vibepollo_legacy_handoff" "$vibepollo_proc/cmdline" 2>/dev/null; then return 0; fi
  done
  return 1
}
vibepollo_wait_for_legacy_handoff() {
  vibepollo_handoff_attempt=0
  vibepollo_handoff_clean_passes=0
  while [ "$vibepollo_handoff_attempt" -lt 400 ]; do
    if vibepollo_legacy_handoff_process_is_running; then
      vibepollo_handoff_clean_passes=0
    else
      vibepollo_handoff_clean_passes=$((vibepollo_handoff_clean_passes + 1))
      [ "$vibepollo_handoff_clean_passes" -lt 10 ] || return 0
    fi
    vibepollo_handoff_attempt=$((vibepollo_handoff_attempt + 1))
    sleep 0.1
  done
  return 1
}
vibepollo_disable_legacy_handoff() {
  if [ ! -e "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ]; then return 0; fi
  [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] || return 1
  vibepollo_handoff_attributes=$(stat -c '%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff") || return 1
  case "$vibepollo_handoff_attributes" in
    '0:0:755:1' | '0:0:0:1') ;;
    *) return 1 ;;
  esac
  vibepollo_legacy_handoff_identity=$(stat -Lc '%%d:%%i' -- "$vibepollo_legacy_handoff") || return 1
  chmod 000 -- "$vibepollo_legacy_handoff" || return 1
  vibepollo_handoff_attributes=$(stat -Lc '%%d:%%i:%%u:%%g:%%a:%%h' -- \
    "$vibepollo_legacy_handoff") || return 1
  [ "$vibepollo_handoff_attributes" = \
    "$vibepollo_legacy_handoff_identity:0:0:0:1" ] || return 1
  vibepollo_wait_for_legacy_handoff
}
vibepollo_legacy_state_file_is_safe() {
  vibepollo_legacy_state_path=$1
  vibepollo_legacy_state_name=${vibepollo_legacy_state_path##*/}
  case "$vibepollo_legacy_state_name" in [1-9]*) ;; *) return 1 ;; esac
  case "$vibepollo_legacy_state_name" in *[!0-9]*) return 1 ;; esac
  [ -f "$vibepollo_legacy_state_path" ] && [ ! -L "$vibepollo_legacy_state_path" ] || return 1
  vibepollo_legacy_state_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%s' -- \
    "$vibepollo_legacy_state_path") || return 1
  case "$vibepollo_legacy_state_attributes" in
    0:0:600:1:* | 0:0:644:1:*) ;;
    *) return 1 ;;
  esac
  vibepollo_legacy_state_size=${vibepollo_legacy_state_attributes##*:}
  case "$vibepollo_legacy_state_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_legacy_state_size" -le 4096 ] || return 1
  {
    IFS= read -r vibepollo_legacy_state_user &&
      IFS= read -r vibepollo_legacy_state_uid &&
      IFS= read -r vibepollo_legacy_state_token &&
      ! IFS= read -r vibepollo_legacy_state_extra
  } <"$vibepollo_legacy_state_path" || return 1
  case "$vibepollo_legacy_state_user" in [a-z_]*) ;; *) return 1 ;; esac
  case "$vibepollo_legacy_state_user" in *[!a-z0-9_-]*) return 1 ;; esac
  [ "$vibepollo_legacy_state_uid" = "$vibepollo_legacy_state_name" ] || return 1
  [ "${#vibepollo_legacy_state_token}" -eq 32 ] || return 1
  case "$vibepollo_legacy_state_token" in *[!0-9a-f]*) return 1 ;; esac
}
vibepollo_remove_legacy_state_directory() {
  vibepollo_legacy_directory=$1
  if [ ! -e "$vibepollo_legacy_directory" ] && [ ! -L "$vibepollo_legacy_directory" ]; then return 0; fi
  [ -d "$vibepollo_legacy_directory" ] && [ ! -L "$vibepollo_legacy_directory" ] || return 1
  vibepollo_legacy_directory_attributes=$(stat -c '%%u:%%g:%%a' -- \
    "$vibepollo_legacy_directory") || return 1
  case "$vibepollo_legacy_directory_attributes" in
    '0:0:700' | '0:0:755') ;;
    *) return 1 ;;
  esac
  for vibepollo_legacy_state_path in "$vibepollo_legacy_directory"/* \
    "$vibepollo_legacy_directory"/.[!.]* "$vibepollo_legacy_directory"/..?*; do
    if [ ! -e "$vibepollo_legacy_state_path" ] && [ ! -L "$vibepollo_legacy_state_path" ]; then continue; fi
    vibepollo_legacy_state_file_is_safe "$vibepollo_legacy_state_path" || return 1
    rm -f -- "$vibepollo_legacy_state_path" || return 1
  done
  rmdir -- "$vibepollo_legacy_directory" || return 1
  [ ! -e "$vibepollo_legacy_directory" ] && [ ! -L "$vibepollo_legacy_directory" ]
}
vibepollo_remove_legacy_prelogin_marker() {
  if [ ! -e "$vibepollo_legacy_prelogin_marker" ] && [ ! -L "$vibepollo_legacy_prelogin_marker" ]; then return 0; fi
  [ -f "$vibepollo_legacy_prelogin_marker" ] && [ ! -L "$vibepollo_legacy_prelogin_marker" ] || return 1
  vibepollo_prelogin_marker_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%s' -- \
    "$vibepollo_legacy_prelogin_marker") || return 1
  case "$vibepollo_prelogin_marker_attributes" in 0:0:644:1:*) ;; *) return 1 ;; esac
  vibepollo_prelogin_marker_size=${vibepollo_prelogin_marker_attributes##*:}
  case "$vibepollo_prelogin_marker_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_prelogin_marker_size" -le 256 ] || return 1
  {
    IFS= read -r vibepollo_prelogin_marker_user &&
      ! IFS= read -r vibepollo_prelogin_marker_extra
  } <"$vibepollo_legacy_prelogin_marker" || return 1
  case "$vibepollo_prelogin_marker_user" in [a-z_]*) ;; *) return 1 ;; esac
  case "$vibepollo_prelogin_marker_user" in *[!a-z0-9_-]*) return 1 ;; esac
  rm -f -- "$vibepollo_legacy_prelogin_marker" || return 1
  [ ! -e "$vibepollo_legacy_prelogin_marker" ] && [ ! -L "$vibepollo_legacy_prelogin_marker" ]
}
vibepollo_cleanup_legacy_transition_state() {
  (
    vibepollo_runtime_root_is_safe_or_absent || exit 1
    if [ -e "$vibepollo_legacy_transition_lock" ] || [ -L "$vibepollo_legacy_transition_lock" ]; then
      [ -f "$vibepollo_legacy_transition_lock" ] && [ ! -L "$vibepollo_legacy_transition_lock" ] || exit 1
      [ "$(stat -c '%%u:%%g:%%a:%%h:%%s' -- "$vibepollo_legacy_transition_lock")" = \
        '0:0:600:1:0' ] || exit 1
      exec 9<>"$vibepollo_legacy_transition_lock" || exit 1
      timeout --signal=KILL 10 flock --exclusive 9 || exit 1
    fi
    vibepollo_wait_for_legacy_handoff || exit 1
    vibepollo_remove_legacy_state_directory "$vibepollo_legacy_handoff_directory" || exit 1
    vibepollo_remove_legacy_state_directory "$vibepollo_legacy_restore_directory" || exit 1
    vibepollo_remove_legacy_prelogin_marker || exit 1
    if [ -e "$vibepollo_legacy_transition_lock" ] || [ -L "$vibepollo_legacy_transition_lock" ]; then
      rm -f -- "$vibepollo_legacy_transition_lock" || exit 1
    fi
    [ ! -e "$vibepollo_legacy_transition_lock" ] && [ ! -L "$vibepollo_legacy_transition_lock" ]
  )
}
# Package barriers are a transaction-owned resource, not administrator policy.
# Never source this record: fixed files contain only validated identities.
vibepollo_package_transaction=/run/vibepollo-package-transaction
vibepollo_package_lock_held=0
vibepollo_package_transaction_is_safe() {
  [ -d "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_package_transaction")" = '0:0:700:directory' ]
}

vibepollo_package_record_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:600:1:regular file' ] &&
    [ "$(stat -c '%%s' -- "$1")" -le 4096 ]
}

vibepollo_package_write_record() (
  vibepollo_package_transaction_is_safe || exit 1
  vibepollo_package_temporary=$(mktemp "$vibepollo_package_transaction/.record.XXXXXX") || exit 1
  trap 'rm -f -- "$vibepollo_package_temporary"' 0
  chmod 0600 -- "$vibepollo_package_temporary" || exit 1
  printf '%%s\n' "$2" >"$vibepollo_package_temporary" || exit 1
  mv -fT -- "$vibepollo_package_temporary" "$vibepollo_package_transaction/$1"
)

vibepollo_package_path_identity() {
  if [ ! -e "$1" ] && [ ! -L "$1" ]; then
    printf 'absent\n'
  elif [ -L "$1" ] && [ "$(readlink -- "$1")" = /dev/null ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  elif [ "$1" = "$vibepollo_host_upgrade_dropin" ] &&
       [ -f "$1" ] && [ ! -L "$1" ] &&
       [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:644:1:regular file' ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  else
    return 1
  fi
}

vibepollo_package_snapshot_path() {
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  case "$vibepollo_package_identity" in
    absent | *:0:0:'symbolic link' | *:0:0:'regular file') ;;
    *) return 1 ;;
  esac
  vibepollo_package_write_record "$1.before" "$vibepollo_package_identity"
}

vibepollo_package_cleanup_record_temporaries() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_has_temporary=0
  # First validate the complete set, so one malformed entry prevents every
  # deletion. Writers inherit fd 8; a fresh caller cannot hold the lock while
  # an interrupted operation's surviving writer is still publishing a record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    vibepollo_package_has_temporary=1
    vibepollo_package_suffix=${vibepollo_package_path##*/}
    case "$vibepollo_package_suffix" in
      .record.*) vibepollo_package_suffix=${vibepollo_package_suffix#.record.} ;;
      .mask.*) vibepollo_package_suffix=${vibepollo_package_suffix#.mask.} ;;
      *) return 1 ;;
    esac
    [ "${#vibepollo_package_suffix}" -eq 6 ] || return 1
    case "$vibepollo_package_suffix" in *[!A-Za-z0-9]*) return 1 ;; esac
    if [ -L "$vibepollo_package_path" ]; then
      case "${vibepollo_package_path##*/}" in .mask.*) ;; *) return 1 ;; esac
      [ "$(readlink -- "$vibepollo_package_path")" = /dev/null ] || return 1
      vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%h:%%F' -- "$vibepollo_package_path") || return 1
      case "$vibepollo_package_temporary_attributes" in
        '0:0:1:symbolic link' | '0:0:2:symbolic link') continue ;;
        *) return 1 ;;
      esac
    fi
    [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
    vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path") || return 1
    case "$vibepollo_package_temporary_attributes" in
      '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;;
      *) return 1 ;;
    esac
  done
  [ "$vibepollo_package_has_temporary" -eq 1 ] || return 0
  [ "${vibepollo_package_lock_held:-0}" -eq 1 ] || return 1
  flock --nonblock 8 || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
}

vibepollo_package_snapshot_host_executables() {
  if [ -e "$vibepollo_package_transaction/host-executables.before" ] || [ -L "$vibepollo_package_transaction/host-executables.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before"
    return $?
  fi
  vibepollo_package_executables=$(vibepollo_package_capture_host_executables) || return 1
  vibepollo_package_write_record host-executables.before "$vibepollo_package_executables"
}

vibepollo_package_lock() {
  [ "${vibepollo_package_lock_held:-0}" -eq 0 ] || return 0
  vibepollo_package_transaction_is_safe || return 1
  if [ -e "$vibepollo_package_transaction/lock" ] || [ -L "$vibepollo_package_transaction/lock" ]; then
    [ -f "$vibepollo_package_transaction/lock" ] && [ ! -L "$vibepollo_package_transaction/lock" ] || return 1
    vibepollo_package_lock_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_transaction/lock") || return 1
    case "$vibepollo_package_lock_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  else
    (umask 077; set -C; printf 'package-lock\n' >"$vibepollo_package_transaction/lock") || return 1
  fi
  exec 8<>"$vibepollo_package_transaction/lock" || return 1
  flock --nonblock 8 || return 1
  vibepollo_package_lock_held=1
  vibepollo_package_cleanup_record_temporaries
}

vibepollo_package_properties_are_unique() {
  vibepollo_package_property_output=$1
  shift
  [ "$(printf '%%s\n' "$vibepollo_package_property_output" | wc -l | tr -d ' ')" = "$#" ] || return 1
  for vibepollo_package_property do
    [ "$(printf '%%s\n' "$vibepollo_package_property_output" | grep -c "^$vibepollo_package_property=" || true)" = 1 ] || return 1
  done
}

vibepollo_package_controller_identity_is_safe() {
  vibepollo_package_properties_are_unique "$1" MainPID ControlGroup FreezerState InvocationID || return 1
  vibepollo_package_identity_pid=$(printf '%%s\n' "$1" | sed -n 's/^MainPID=//p')
  vibepollo_package_identity_group=$(printf '%%s\n' "$1" | sed -n 's/^ControlGroup=//p')
  vibepollo_package_identity_invocation=$(printf '%%s\n' "$1" | sed -n 's/^InvocationID=//p')
  vibepollo_package_identity_freezer=$(printf '%%s\n' "$1" | sed -n 's/^FreezerState=//p')
  case "$vibepollo_package_identity_pid" in '' | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_package_identity_group" in '' | /*) ;; *) return 1 ;; esac
  case "$vibepollo_package_identity_group" in */../* | */.. | *'
'*) return 1 ;; esac
  case "$vibepollo_package_identity_freezer" in '' | running | frozen) ;; *) return 1 ;; esac
  if [ "$vibepollo_package_identity_pid" != 0 ]; then
    [ -n "$vibepollo_package_identity_group" ] && [ -n "$vibepollo_package_identity_freezer" ] || return 1
  fi
  if [ "$vibepollo_package_identity_pid" = 0 ] && [ -z "$vibepollo_package_identity_invocation" ]; then return 0; fi
  [ "${#vibepollo_package_identity_invocation}" -eq 32 ] || return 1
  case "$vibepollo_package_identity_invocation" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_record_directory_is_safe() {
  vibepollo_package_transaction_is_safe || return 1
  # Refuse unknown files rather than recursively deleting a damaged record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/* "$vibepollo_package_transaction"/.[!.]* "$vibepollo_package_transaction"/..?*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    case "${vibepollo_package_path##*/}" in
      lock | phase | controller | handoff.before | host-executables.before | host-stop.before | run-id | failure-class | legacy-adopted | fence.before | fence.owned | vibepollo.service.before | vibepollo.service.owned | vibepollo-session-exec.socket.before | vibepollo-session-exec.socket.owned | vibeshine-vkms-control.socket.before | vibeshine-vkms-control.socket.owned | vibepollo-session-restore@.service.before | vibepollo-session-restore@.service.owned) ;;
      *) return 1 ;;
    esac
    if [ "${vibepollo_package_path##*/}" = lock ] && [ ! -s "$vibepollo_package_path" ]; then
      [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] &&
        [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path")" = '0:0:600:1:regular empty file' ] || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_path" || return 1
    fi
  done
}

vibepollo_package_run_id_is_safe() {
  [ "${#1}" -eq 32 ] || return 1
  case "$1" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_preserve_unowned_host_mask() {
  vibepollo_package_preserve_host_mask=0
  for vibepollo_package_mask_path in /etc/systemd/system/vibepollo.service /run/systemd/system/vibepollo.service; do
    [ -L "$vibepollo_package_mask_path" ] || continue
    [ "$(readlink -- "$vibepollo_package_mask_path")" = /dev/null ] || continue
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    case "$vibepollo_package_mask_identity" in
      *:0:0:'symbolic link') ;;
      *) echo "error: unsafe host mask at $vibepollo_package_mask_path; package operation is blocked." >&2; return 1 ;;
    esac
    if [ "$vibepollo_package_mask_path" = /run/systemd/system/vibepollo.service ] &&
       { [ -e "$vibepollo_package_transaction/vibepollo.service.owned" ] || [ -L "$vibepollo_package_transaction/vibepollo.service.owned" ]; }; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/vibepollo.service.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/vibepollo.service.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    vibepollo_package_preserve_host_mask=1
  done
  [ "$vibepollo_package_preserve_host_mask" -eq 1 ] || return 0
  vibepollo_package_mask_state=$(timeout --signal=KILL 5 systemctl is-enabled vibepollo.service 2>/dev/null || true)
  vibepollo_package_mask_load=$(timeout --signal=KILL 5 systemctl show vibepollo.service --property=LoadState 2>/dev/null) || vibepollo_package_mask_load=uncertain
  vibepollo_package_mask_verified=0
  case "$vibepollo_package_mask_state:$vibepollo_package_mask_load" in
    masked:LoadState=masked | masked-runtime:LoadState=masked) vibepollo_package_mask_verified=1 ;;
  esac
  if [ "$vibepollo_package_mask_verified" -ne 1 ] ||
     ! vibepollo_unit_is_quiescent vibepollo.service ||
     ! vibepollo_package_no_jobs; then
    echo 'error: an unowned Vibepollo host mask exists, but the host is active, populated, has a pending job, or its state is uncertain; package replacement is blocked. Force cannot remove an administrator or legacy mask without proving its activation ownership.' >&2
    return 1
  fi
  # Preserve the actual mask and any unowned fence byte-for-byte. The mask
  # supplies no-start admission while the full generation proof still runs.
  return 0
}

vibepollo_package_prepare_postinstall_start() {
  vibepollo_package_start_deferred=0
  vibepollo_package_preserve_unowned_host_mask || return 1
  if [ "$vibepollo_package_preserve_host_mask" -eq 1 ]; then
    vibepollo_package_start_deferred=1
  fi
  for vibepollo_package_start_unit in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    for vibepollo_package_start_directory in /etc/systemd/system /run/systemd/system; do
      vibepollo_package_start_path=$vibepollo_package_start_directory/$vibepollo_package_start_unit
      [ -L "$vibepollo_package_start_path" ] || continue
      [ "$(readlink -- "$vibepollo_package_start_path")" = /dev/null ] || continue
      vibepollo_package_start_identity=$(vibepollo_package_path_identity "$vibepollo_package_start_path") || return 1
      case "$vibepollo_package_start_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      if [ "$vibepollo_package_start_directory" = /run/systemd/system ] &&
         { [ -e "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ]; }; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" || return 1
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned")" = "$vibepollo_package_start_identity" ] || return 1
        continue
      fi
      vibepollo_package_start_deferred=1
    done
  done
  if [ -e "$vibepollo_package_transaction/fence.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
    vibepollo_package_prior_fence=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
    if [ "$vibepollo_package_prior_fence" != absent ]; then
      [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_prior_fence" ] || return 1
      vibepollo_package_start_deferred=1
    fi
  fi
  [ "$vibepollo_package_start_deferred" -eq 1 ] || return 0
  # This must be the full actual generation proof (controller, broker/apps,
  # restore/control instances, session record and retained executable inodes).
  # The local host-empty + no-jobs classifier is insufficient for release.
  if ! vibepollo_package_complete_quiescence_is_safe || ! vibepollo_package_release; then
    echo 'error: safe package shutdown recovery could not finish; automatic service activation remains deferred. Resolve the preceding check and retry package configuration; administrator masks were preserved.' >&2
    return 1
  fi
  echo 'package: preserved administrator host mask or start fence; configuration completed and automatic service activation is deferred.' >&2
  return 0
}

vibepollo_package_bind_run() {
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    vibepollo_package_write_record run-id "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
  elif [ ! -e "$vibepollo_package_transaction/run-id" ] && [ ! -L "$vibepollo_package_transaction/run-id" ]; then
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record failure-class none
}

vibepollo_package_run_is_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
  vibepollo_package_record_run=$(cat -- "$vibepollo_package_transaction/run-id") || return 1
  vibepollo_package_run_id_is_safe "$vibepollo_package_record_run" || return 1
  [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ] || return 0
  vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" &&
    [ "$vibepollo_package_record_run" = "$VIBEPOLLO_PACKAGE_RUN_ID" ]
}

vibepollo_package_force_is_applicable() (
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" &&
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/failure-class" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = late ] &&
    [ "$(cat -- "$vibepollo_package_transaction/failure-class")" = host-stop-deadline ]
)

vibepollo_package_refresh_missing_owned() {
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || continue
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    if [ "$vibepollo_package_current" = absent ]; then
      rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    else
      [ "$vibepollo_package_current" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" ] || return 1
    fi
  done
}

vibepollo_package_cleanup_retired() (
  vibepollo_package_original_root=$vibepollo_package_transaction
  vibepollo_package_retired_root=$vibepollo_package_original_root.retired
  [ -e "$vibepollo_package_retired_root" ] || [ -L "$vibepollo_package_retired_root" ] || return 0
  # This independent root-owned lock survives removal of the retired record's
  # own lock inode, including SIGKILL between the final unlink and rmdir.
  vibepollo_package_gc_lock=$vibepollo_package_original_root.gc-lock
  if [ ! -e "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ]; then
    (umask 077; set -C; printf 'package-gc\n' >"$vibepollo_package_gc_lock") || return 1
  fi
  [ -f "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ] || return 1
  vibepollo_package_gc_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_gc_lock") || return 1
  case "$vibepollo_package_gc_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  exec 7<>"$vibepollo_package_gc_lock" || return 1
  timeout --signal=KILL 5 flock --exclusive 7 || return 1
  vibepollo_package_transaction=$vibepollo_package_retired_root
  [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ] || return 0
  vibepollo_package_transaction_is_safe && vibepollo_package_record_directory_is_safe || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/*; do
    [ -e "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
  rmdir -- "$vibepollo_package_transaction" || return 1
)

vibepollo_package_adopt_legacy_barriers() {
  # Only the exact old package signature, with every admission path closed
  # and the complete old generation drained, permits ownership migration.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
  vibepollo_package_adoption_pending=0
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    vibepollo_package_adoption_pending=1
    # The marker is published only after the exact old fence, all original
    # mask identities and full generation proof were verified. An interrupted
    # conversion may leave some .before snapshots unconverted when old package
    # unwind removes the fence. Resume only those original masks, never a
    # replacement path or an unrecorded new mask.
    if [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ]; then
      vibepollo_package_complete_quiescence_is_safe || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      if [ -e "$vibepollo_package_transaction/fence.owned" ] || [ -L "$vibepollo_package_transaction/fence.owned" ]; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.owned" || return 1
      fi
      # Validate every path before changing any record. Repeat the identity
      # checks during publication so interruption leaves resumable metadata.
      for vibepollo_package_adoption_pass in validate commit; do
        for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
          vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
          if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
            vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
            vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
          fi
          vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
          if [ "$vibepollo_package_current" != absent ]; then
            case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
            [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
            [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
          fi
          [ "$vibepollo_package_adoption_pass" = commit ] || continue
          if [ "$vibepollo_package_current" = absent ]; then
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
            rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          else
            vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
          fi
        done
      done
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      vibepollo_package_write_record fence.before absent || return 1
      rm -f -- "$vibepollo_package_transaction/fence.owned" || return 1
      vibepollo_package_write_record phase preflight || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      rm -f -- "$vibepollo_package_transaction/legacy-adopted" || return 1
      return 0
    fi
  fi
  [ "$vibepollo_package_before" != absent ] || [ "$vibepollo_package_adoption_pending" -eq 1 ] || return 0
  [ -f "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = '0:0:644:1:regular file' ] || return 1
  vibepollo_package_legacy_contents=$(cat -- "$vibepollo_host_upgrade_dropin") || return 1
  case "$vibepollo_package_legacy_contents" in
    '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=mixed
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no') ;;
    *) [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1; return 0 ;;
  esac
  [ "$(stat -c '%%s' -- "$vibepollo_host_upgrade_dropin")" = "$(printf '%%s\n' "$vibepollo_package_legacy_contents" | wc -c | tr -d ' ')" ] || {
    [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1
    return 0
  }
  vibepollo_package_complete_quiescence_is_safe || {
    echo 'error: an earlier package fence remains, but the old host or session has not fully drained; it cannot be adopted safely.' >&2
    return 1
  }
  # Generation inspection can take time. An administrator may edit the same
  # inode while it runs; do not convert authority for changed fence bytes.
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
  done
  if [ -e "$vibepollo_package_transaction/fence.owned" ]; then
    vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.owned") || return 1
  fi
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_before" ] || return 1
  vibepollo_package_adoption_fence_identity=$vibepollo_package_before
  vibepollo_package_write_record legacy-adopted yes || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    # Recheck each original identity at publication, rather than claiming a
    # replacement that appeared after the inventory pass.
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
    if [ "$vibepollo_package_name" != fence ]; then
      [ "$(readlink -- "$vibepollo_package_owned_path")" = /dev/null ] || return 1
    fi
    vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
    vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
  done
  vibepollo_package_write_record phase preflight || return 1
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_adoption_fence_identity" ] || return 1
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  # Every owned identity and before snapshot is now durable; the pending
  # adoption marker is no longer needed by later release/retry operations.
  rm -f -- "$vibepollo_package_transaction/legacy-adopted"
}

vibepollo_package_rebaseline_unwound_run() {
  # Pending legacy authority is not a wholly unwound transaction even when
  # no .owned file has been published yet. Reconcile it before a new target.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    return 0
  fi
  # A native package retry, or a fresh explicit installer operation, may
  # select the current healthy invocation after every owned barrier unwound.
  # This grants a fresh target, never proof that old process resources vanished.
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    [ -e "$vibepollo_package_transaction/run-id" ] || return 0
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
    # A same-run force retry must retain its original invocation identity.
    [ "$(cat -- "$vibepollo_package_transaction/run-id")" != "$VIBEPOLLO_PACKAGE_RUN_ID" ] || return 0
  fi
  for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
    [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 0
  done
  vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    [ "$(vibepollo_package_path_identity "$vibepollo_package_owned_path")" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" ] || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  if [ -z "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    # A native retry is a new operation. Retire any earlier wrapper's nonce
    # atomically before refreshing phase or discarding its saved stop identity.
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  rm -f -- "$vibepollo_package_transaction/host-stop.before" || return 1
  vibepollo_package_prior_phase=early
}

vibepollo_package_begin() {
  vibepollo_package_cleanup_retired || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_record_directory_is_safe || return 1
    vibepollo_package_refresh_missing_owned || return 1
    if [ ! -e "$vibepollo_package_transaction/phase" ] && [ ! -L "$vibepollo_package_transaction/phase" ]; then
      for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
        [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
      done
      vibepollo_package_forget || return 1
      vibepollo_package_begin
      return $?
    fi
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
    vibepollo_package_prior_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
    case "$vibepollo_package_prior_phase" in early | late | preflight) ;; *) return 1 ;; esac
    for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    done
    vibepollo_package_snapshot_host_executables || return 1
    vibepollo_package_rebaseline_unwound_run || return 1
    vibepollo_package_bind_run || return 1
    vibepollo_package_adopt_legacy_barriers || return 1
    return 0
  fi
  # mkdir is exclusive; a competing package operation may not share ownership.
  (umask 077; mkdir -- "$vibepollo_package_transaction") || return 1
  vibepollo_package_transaction_created=1
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_lock || return 1
  vibepollo_package_snapshot_host_executables || return 1
  vibepollo_package_prior_phase=early
  vibepollo_package_snapshot_path fence "$vibepollo_host_upgrade_dropin" || return 1
  vibepollo_package_handoff=absent
  if [ -e "${vibepollo_legacy_handoff:-/usr/libexec/vibeshine/vibepollo-session-handoff}" ] || [ -L "$vibepollo_legacy_handoff" ]; then
    [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] || return 1
    vibepollo_package_handoff=$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff") || return 1
    case "$vibepollo_package_handoff" in *:0:0:755:1 | *:0:0:0:1) ;; *) return 1 ;; esac
  fi
  vibepollo_package_write_record handoff.before "$vibepollo_package_handoff" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_snapshot_path "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  vibepollo_package_bind_run || return 1
  vibepollo_package_adopt_legacy_barriers
}

vibepollo_package_record_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 0
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_identity" != absent ] || return 1
  if [ -e "$vibepollo_package_transaction/$1.owned" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_identity" ]
  else
    vibepollo_package_write_record "$1.owned" "$vibepollo_package_identity"
  fi
}

vibepollo_package_mask() (
  vibepollo_package_mask_stage=
  trap '[ -z "$vibepollo_package_mask_stage" ] || rm -f -- "$vibepollo_package_mask_stage"' 0
  for vibepollo_package_name do
    case "$vibepollo_package_name" in
      vibepollo.service | vibepollo-session-exec.socket | vibeshine-vkms-control.socket | vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = absent ] &&
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    if [ "$vibepollo_package_mask_identity" = absent ]; then
      # Keep a private inode as creation evidence. Publish its ownership intent
      # before linking it, so interruption cannot strand our published mask.
      # ln -P -T never replaces an administrator's competing path.
      vibepollo_package_mask_stage=$(mktemp "$vibepollo_package_transaction/.mask.XXXXXX") || return 1
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      ln -s /dev/null "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_created_mask=$(vibepollo_package_path_identity "$vibepollo_package_mask_stage") || return 1
      case "$vibepollo_package_created_mask" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
      vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_created_mask" || return 1
      if ! ln -P -T -- "$vibepollo_package_mask_stage" "$vibepollo_package_mask_path" 2>/dev/null; then
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        if [ "$vibepollo_package_mask_identity" != "$vibepollo_package_created_mask" ]; then
          # An exclusive-link failure published none of our inode. Drop only
          # our exact intent, then preserve a valid competing external mask.
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_created_mask" ] || return 1
          rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$vibepollo_package_mask_identity" != absent ] || return 1
        fi
      else
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        [ "$vibepollo_package_mask_identity" = "$vibepollo_package_created_mask" ] || return 1
      fi
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_mask_stage=
      if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then continue; fi
    fi
    # Existing unowned masks, including ones added since a failed attempt's
    # snapshot, remain policy. Refresh their preserved baseline, never claim.
    case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_write_record "$vibepollo_package_name.before" "$vibepollo_package_mask_identity" || return 1
    [ "$(vibepollo_package_path_identity "$vibepollo_package_mask_path")" = "$vibepollo_package_mask_identity" ] || return 1
  done
  timeout --signal=KILL 15 systemctl mask --runtime "$@" 2>/dev/null
  vibepollo_package_mask_status=$?
  # The manager can reload our prepublished masks, but cannot establish their
  # ownership. Even a timed-out client must leave every recorded inode intact.
  for vibepollo_package_name do
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
    else
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = "$vibepollo_package_mask_identity" ] || return 1
    fi
  done
  return "$vibepollo_package_mask_status"
)

vibepollo_package_mark_late() {
  vibepollo_package_write_record phase late
}

vibepollo_package_remove_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/$1.before") || return 1
  [ "$vibepollo_package_before" = absent ] || return 0
  vibepollo_package_current=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_current" != absent ] || return 0
  if [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ]; then
    # An idle Arch retry can skip mask creation. Reconcile an external mask
    # added since the snapshot without claiming it or reopening its admission.
    # New package masks publish intent first, so a missing record is not ours.
    case "$1:$2" in
      vibepollo.service:/run/systemd/system/vibepollo.service | vibepollo-session-exec.socket:/run/systemd/system/vibepollo-session-exec.socket | vibeshine-vkms-control.socket:/run/systemd/system/vibeshine-vkms-control.socket | vibepollo-session-restore@.service:/run/systemd/system/vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_lock || return 1
    vibepollo_package_complete_quiescence_is_safe || return 1
    [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ] || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    vibepollo_package_write_record "$1.before" "$vibepollo_package_current" || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    return 0
  fi
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_current" ] || return 1
  rm -f -- "$2"
}

vibepollo_package_no_jobs() {
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_package_job=$(timeout --signal=KILL 5 systemctl show "$vibepollo_package_name" --property=Job 2>/dev/null) || return 1
    [ "$vibepollo_package_job" = 'Job=' ] || return 1
  done
}

vibepollo_package_restore_barriers() {
  vibepollo_package_keep_legacy=${1:-0}
  vibepollo_package_transaction_is_safe || return 1
  # An interrupted ownership conversion must finish (or refuse a changed
  # legacy fence) before release can remove any path by its saved inode.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_adopt_legacy_barriers || return 1
  fi
  vibepollo_package_remove_owned fence "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_keep_legacy" -eq 1 ] && [ "$vibepollo_package_name" = 'vibepollo-session-restore@.service' ]; then continue; fi
    vibepollo_package_remove_owned "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  timeout --signal=KILL 15 systemctl daemon-reload || return 1
}

vibepollo_package_forget() {
  # Failed initial adoption can already own barriers. Retain its authority
  # until a proved resume completes; a created-directory flag cannot erase it.
  [ ! -e "$vibepollo_package_transaction/legacy-adopted" ] &&
    [ ! -L "$vibepollo_package_transaction/legacy-adopted" ] || return 1
  vibepollo_package_transaction_is_safe && vibepollo_package_cleanup_record_temporaries &&
    vibepollo_package_record_directory_is_safe || return 1
  vibepollo_package_forget_root=$vibepollo_package_transaction
  [ ! -e "$vibepollo_package_forget_root.retired" ] && [ ! -L "$vibepollo_package_forget_root.retired" ] || return 1
  mv -T -- "$vibepollo_package_forget_root" "$vibepollo_package_forget_root.retired" || return 1
  exec 8>&-
  vibepollo_package_lock_held=0
  vibepollo_package_cleanup_retired
}

vibepollo_package_restore_legacy_handoff() {
  # Only failed/cancelled transactions restore the same installed old inode.
  # Successful replacement deliberately retires this obsolete admission path.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/handoff.before" || return 1
  vibepollo_package_handoff=$(cat -- "$vibepollo_package_transaction/handoff.before") || return 1
  case "$vibepollo_package_handoff" in
    *:0:0:755:1)
      vibepollo_package_handoff_expected=${vibepollo_package_handoff%%:755:1}:0:1
      if [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] &&
         [ "$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff")" = "$vibepollo_package_handoff_expected" ]; then
        chmod 0755 -- "$vibepollo_legacy_handoff" || return 1
      fi
      ;;
    absent | *:0:0:0:1) ;;
    *) return 1 ;;
  esac
}

vibepollo_package_restore_early() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = early ] || return 1
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/controller" || return 1
    vibepollo_package_controller=$(cat -- "$vibepollo_package_transaction/controller") || return 1
    vibepollo_package_current_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" &&
      vibepollo_package_controller_identity_is_safe "$vibepollo_package_current_controller" || return 1
    # Thaw only our original exact unit identity, never an administrator's freeze.
    vibepollo_package_original_identity=$(printf '%%s\n' "$vibepollo_package_controller" | sed '/^FreezerState=/d')
    vibepollo_package_current_identity=$(printf '%%s\n' "$vibepollo_package_current_controller" | sed '/^FreezerState=/d')
    [ "$vibepollo_package_original_identity" = "$vibepollo_package_current_identity" ] || return 1
    vibepollo_package_restore_barriers || return 1
    if printf '%%s\n' "$vibepollo_package_controller" | grep -qx 'FreezerState=running' &&
       printf '%%s\n' "$vibepollo_package_current_controller" | grep -qx 'FreezerState=frozen'; then
      timeout --signal=KILL 15 systemctl thaw vibepollo-session-controller.service 2>/dev/null || return 1
    fi
    vibepollo_controller_was_frozen=0
  fi
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget
}

vibepollo_package_release() {
  # Called only after the old generation has completely drained.
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  [ ! -e /run/vibepollo/session.env ] && [ ! -L /run/vibepollo/session.env ] || return 1
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_restore_barriers 1 || return 1
    vibepollo_package_forget || return 1
  fi
}

vibepollo_package_recover_failed_transaction() (
  # The caller must wait for its package-manager child before invoking this.
  # Releasing a preflight while replacement is still executing is forbidden.
  if [ ! -e "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ]; then return 0; fi
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  vibepollo_package_recovery_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
  case "$vibepollo_package_recovery_phase" in
    early) vibepollo_package_restore_early; return $? ;;
    late | preflight) ;;
    *) return 1 ;;
  esac
  vibepollo_package_complete_quiescence_is_safe || return 1
  vibepollo_package_restore_barriers || return 1
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget || return 1
  echo 'package recovery: old host and session cleanup verified; transaction-owned admission fences released without starting services.' >&2
)








vibepollo_package_complete_quiescence_is_safe() {
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket vibepollo-prelogin.service vibepollo-machine-prepare.service; do
    vibepollo_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  vibepollo_control_instances_are_quiescent &&
    vibepollo_brokers_are_quiescent &&
    vibepollo_restore_instances_are_quiescent || return 1
  for vibepollo_package_runtime_path in /run/vibepollo/session.env "$vibepollo_broker_socket" "$vibepollo_control_socket" "$vibepollo_legacy_handoff_directory" "$vibepollo_legacy_restore_directory" "$vibepollo_legacy_transition_lock" "$vibepollo_legacy_prelogin_marker"; do
    [ -n "$vibepollo_package_runtime_path" ] && [ ! -e "$vibepollo_package_runtime_path" ] && [ ! -L "$vibepollo_package_runtime_path" ] || return 1
  done
}

vibepollo_package_preserved_preflight_is_safe() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = preflight ] || return 1
  vibepollo_restore_template_is_masked && vibepollo_control_socket_is_masked &&
    vibepollo_broker_socket_is_masked && vibepollo_host_unit_is_masked || return 1
  vibepollo_package_complete_quiescence_is_safe
}

vibepollo_package_open_owned_host_for_stop() {
  if [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ]; then return 0; fi
  # A prior late stop may finish after its client timed out. Re-expose the
  # installed unit only when the old cgroup is empty, retaining its start fence.
  vibepollo_unit_is_quiescent vibepollo.service && vibepollo_package_no_jobs || return 1
  vibepollo_package_remove_owned vibepollo.service /run/systemd/system/vibepollo.service || return 1
  [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ] || return 1
  rm -f -- "$vibepollo_package_transaction/vibepollo.service.owned"
}









vibepollo_quiesce_step=''
vq() { vibepollo_quiesce_step=$*; "$@"; }
vibepollo_abort_quiesce() {
  vibepollo_package_abort_done=1
  trap - HUP INT TERM
  echo "error: could not safely quiesce the existing Vibepollo host (failed step: ${vibepollo_quiesce_step:-unknown}); package replacement is blocked." >&2

  # Before broker admission closes, preserve the existing GPU-owning host.
  # Keep any runtime fence/masks for a deliberate retry or reboot instead of
  # racing a restart against a partially completed shutdown.
  if [ "${vibepollo_shutdown_started:-0}" -eq 0 ]; then
    if vibepollo_package_restore_early; then
      echo 'error: package replacement was aborted; transaction-owned admission fences were restored.' >&2
    else
      echo 'error: shutdown recovery is uncertain; owned fences are retained. Retry this package operation to finish exact host and session cleanup.' >&2
    fi
    return 0
  fi
  # The failed attempt already spent its shutdown budget. Do not signal a
  # changed/new invocation or spend a second grace interval from the abort trap.
  if ! vibepollo_unit_is_quiescent vibepollo.service; then
    echo 'error: the old host still owns resources; its controller and admission fences remain protected. Retry this package operation to finish bounded shutdown.' >&2
    return 1
  fi
  if [ "${vibepollo_controller_was_frozen:-0}" -eq 1 ]; then
    vibepollo_thaw_controller || {
      echo 'error: original controller identity could not be verified for thaw; transaction fences were retained.' >&2
      return 1
    }
  fi
  vibepollo_stop_exact_unit vibepollo-session-controller.service
  # Socket paths that outlived a masked-then-stopped unit would keep the next
  # controller's cleanup failing; drop them only once their unit is stopped.
  if vibepollo_unit_is_quiescent vibepollo-session-exec.socket &&
     [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" 2>/dev/null || true
  fi
  if vibepollo_unit_is_quiescent vibeshine-vkms-control.socket &&
     [ -S "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ]; then
    rm -f -- "$vibepollo_control_socket" 2>/dev/null || true
  fi

  if vibepollo_package_recover_failed_transaction; then
    echo 'error: shutdown failed after the old generation drained; transaction-owned fences were released without restarting services.' >&2
  else
    echo 'error: retained transaction record; retry the package operation to finish exact host and generation cleanup.' >&2
  fi
}
vibepollo_quiesce_or_abort() (
  # Isolate lifecycle signal/EXIT traps from the package-manager shell.
  vibepollo_quiesce_step=''
  vibepollo_package_abort_done=0
  vibepollo_shutdown_started=0
  vibepollo_package_transaction_created=0
  trap 'vibepollo_package_status=$?; if [ "$vibepollo_package_status" -ne 0 ] && [ "$vibepollo_package_abort_done" -eq 0 ]; then vibepollo_abort_quiesce; fi' 0
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
  if ! vq vibepollo_package_begin; then
    if [ "$vibepollo_package_transaction_created" -eq 1 ]; then
      vibepollo_package_forget || true
    fi
    vibepollo_package_abort_done=1
    echo 'error: package transaction ownership record is unsafe or incomplete; no new shutdown was started.' >&2
    return 1
  fi
  if vibepollo_quiesce_machine_host; then
    vq vibepollo_package_unmanaged_host_absent || return 1
    vibepollo_package_write_record phase preflight || return 1
    return 0
  fi
  vibepollo_abort_quiesce
  return 1
)
vibepollo_quiesce_machine_host() {
  vibepollo_have_systemd=0
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_have_systemd=1
    vibepollo_package_preserved_preflight_is_safe && return 0
    vibepollo_shutdown_started=0
    vibepollo_pre_handoff_identity=''
    if [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ]; then
      vibepollo_pre_handoff_identity=$(stat -Lc '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff" 2>/dev/null || true)
    fi
    vq vibepollo_select_upgrade_kill_mode || return 1
    vq vibepollo_prepare_host_upgrade_fence || return 1
    vq vibepollo_freeze_controller || return 1
    if [ "${vibepollo_package_prior_phase:-early}" = early ]; then
      vq vibepollo_host_is_stable_or_quiescent || return 1
    fi
    vq vibepollo_activate_host_upgrade_fence || return 1
    vq vibepollo_controller_remains_frozen || return 1
    if [ "${vibepollo_package_prior_phase:-early}" = early ]; then
      vq vibepollo_host_is_stable_or_quiescent || return 1
    fi
    vq vibepollo_package_mask 'vibepollo-session-restore@.service' 2>/dev/null || return 1
    vq vibepollo_restore_template_is_masked || return 1
    vq vibepollo_disable_legacy_handoff || return 1
    vq vibepollo_package_mask vibeshine-vkms-control.socket 2>/dev/null || return 1
    vq vibepollo_control_socket_is_masked || return 1
    vq vibepollo_stop_exact_unit vibeshine-vkms-control.socket
    vq vibepollo_unit_is_quiescent vibeshine-vkms-control.socket || return 1
    vq vibepollo_control_instances_are_quiescent || return 1
    vq vibepollo_package_mark_late || return 1
    vibepollo_shutdown_started=1
    vq vibepollo_package_mask vibepollo-session-exec.socket 2>/dev/null || return 1
    vq vibepollo_broker_socket_is_masked || return 1
    vq vibepollo_stop_exact_unit vibepollo-session-exec.socket
    vq vibepollo_unit_is_quiescent vibepollo-session-exec.socket || return 1
    vq vibepollo_stop_exact_unit vibepollo.service
    vq vibepollo_unit_is_quiescent vibepollo.service || return 1
    vq vibepollo_package_mask vibepollo.service 2>/dev/null || return 1
    vq vibepollo_host_unit_is_masked || return 1
    vq vibepollo_stop_restore_instances || return 1
    vq vibepollo_stop_brokers || return 1
    vq vibepollo_controller_remains_frozen || return 1
    # systemd refuses StopUnit for a frozen service. Admission and the host are
    # already masked here, so thaw the controller only for its ordered stop.
    vq vibepollo_thaw_controller || return 1
    vq vibepollo_stop_exact_unit vibepollo-session-controller.service
    vq vibepollo_unit_is_quiescent vibepollo-session-controller.service || return 1
    vq vibepollo_stop_exact_unit vibepollo-prelogin.service
    vq vibepollo_stop_exact_unit vibepollo-machine-prepare.service
  fi

  if [ "$vibepollo_have_systemd" -eq 0 ]; then vibepollo_disable_legacy_handoff || return 1; fi
  vibepollo_runtime_root_is_safe_or_absent || return 1

  if [ "$vibepollo_have_systemd" -eq 0 ] && \
     { [ -e "$vibepollo_controller" ] || [ -L "$vibepollo_controller" ]; }; then
    vq vibepollo_privileged_helper_is_safe "$vibepollo_controller" || return 1
    timeout --signal=KILL 40 "$vibepollo_controller" cleanup || return 1
  elif [ "$vibepollo_have_systemd" -eq 0 ]; then
    vq vibepollo_run_optional_legacy_command cleanup
    vibepollo_legacy_status=$?
    case "$vibepollo_legacy_status" in 0 | 2) ;; *) return 1 ;; esac
  fi

  vibepollo_run_optional_legacy_command remove-pam
  vibepollo_legacy_status=$?
  case "$vibepollo_legacy_status" in 0 | 2) ;; *) return 1 ;; esac

  if [ "$vibepollo_have_systemd" -eq 1 ]; then
    vq vibepollo_restore_template_is_masked || return 1
    vq vibepollo_host_unit_is_masked || return 1
    vq vibepollo_broker_socket_is_masked || return 1
    vq vibepollo_stop_restore_instances || return 1
    vq vibepollo_stop_exact_unit vibepollo.service
    vq vibepollo_unit_is_quiescent vibepollo.service || return 1
    vq vibepollo_stop_exact_unit vibepollo-session-exec.socket
    vq vibepollo_stop_brokers || return 1
    for vibepollo_unit in vibepollo-session-exec.socket vibepollo-session-controller.service \
      vibepollo.service vibepollo-prelogin.service vibepollo-machine-prepare.service; do
      vibepollo_stop_exact_unit "$vibepollo_unit"
      vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
    done
    vq vibepollo_brokers_are_quiescent || return 1
    vq vibepollo_restore_instances_are_quiescent || return 1
  fi
  vibepollo_cleanup_legacy_transition_state || return 1
  if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" || return 1
  fi
  [ ! -e "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ] && \
    [ ! -e "$vibepollo_legacy_handoff_directory" ] && [ ! -L "$vibepollo_legacy_handoff_directory" ] && \
    [ ! -e "$vibepollo_legacy_restore_directory" ] && [ ! -L "$vibepollo_legacy_restore_directory" ] && \
    [ ! -e "$vibepollo_legacy_transition_lock" ] && [ ! -L "$vibepollo_legacy_transition_lock" ] && \
    [ ! -e "$vibepollo_legacy_prelogin_marker" ] && [ ! -L "$vibepollo_legacy_prelogin_marker" ] || return 1
  [ ! -e /run/vibepollo/session.env ] && [ ! -L /run/vibepollo/session.env ] || return 1

}
if ! vibepollo_quiesce_or_abort; then
  echo "error: installed Vibepollo services did not quiesce; replacement is blocked and admission remains disabled." >&2
  exit 1
fi

%post
export LC_ALL=C
# Note: this is copied from the postinst script

vibepollo_controller=%{_prefix}/libexec/vibeshine/vibepollo-session-controller
vibepollo_session_record=/run/vibepollo/session.env
vibepollo_legacy_acl=/run/vibepollo/runtime-acl
vibepollo_broker_socket=/run/vibepollo/session-broker.sock
vibepollo_host_upgrade_dropin_dir=/run/systemd/system/vibepollo.service.d
vibepollo_host_upgrade_dropin=$vibepollo_host_upgrade_dropin_dir/90-vibepollo-safe-upgrade.conf
vibepollo_privileged_helper_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] && [ -x "$1" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = \
    '0:0:755:1:regular file' ]
}

vibepollo_controller="%{_prefix}/libexec/vibeshine/vibepollo-session-controller"
vibepollo_legacy_host="%{_prefix}/libexec/vibeshine/vibepollo-machine-host"
vibepollo_legacy_handoff="%{_prefix}/libexec/vibeshine/vibepollo-session-handoff"
vibepollo_runtime_root=/run/vibepollo
vibepollo_broker_socket=/run/vibepollo/session-broker.sock
vibepollo_control_socket=/run/vibeshine/vkms-control.sock
vibepollo_host_upgrade_dropin_dir=/run/systemd/system/vibepollo.service.d
vibepollo_host_upgrade_dropin=$vibepollo_host_upgrade_dropin_dir/90-vibepollo-safe-upgrade.conf
vibepollo_controller_was_frozen=0
vibepollo_upgrade_kill_mode=
vibepollo_legacy_handoff_directory=/run/vibepollo/session-handoffs
vibepollo_legacy_restore_directory=/run/vibepollo/session-restores
vibepollo_legacy_transition_lock=/run/vibepollo/session-handoff.lock
vibepollo_legacy_prelogin_marker=/run/vibepollo/prelogin-handoff-complete


# Package barriers are a transaction-owned resource, not administrator policy.
# Never source this record: fixed files contain only validated identities.
vibepollo_package_transaction=/run/vibepollo-package-transaction
vibepollo_package_lock_held=0
vibepollo_package_transaction_is_safe() {
  [ -d "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_package_transaction")" = '0:0:700:directory' ]
}

vibepollo_package_record_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:600:1:regular file' ] &&
    [ "$(stat -c '%%s' -- "$1")" -le 4096 ]
}

vibepollo_package_write_record() (
  vibepollo_package_transaction_is_safe || exit 1
  vibepollo_package_temporary=$(mktemp "$vibepollo_package_transaction/.record.XXXXXX") || exit 1
  trap 'rm -f -- "$vibepollo_package_temporary"' 0
  chmod 0600 -- "$vibepollo_package_temporary" || exit 1
  printf '%%s\n' "$2" >"$vibepollo_package_temporary" || exit 1
  mv -fT -- "$vibepollo_package_temporary" "$vibepollo_package_transaction/$1"
)

vibepollo_package_path_identity() {
  if [ ! -e "$1" ] && [ ! -L "$1" ]; then
    printf 'absent\n'
  elif [ -L "$1" ] && [ "$(readlink -- "$1")" = /dev/null ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  elif [ "$1" = "$vibepollo_host_upgrade_dropin" ] &&
       [ -f "$1" ] && [ ! -L "$1" ] &&
       [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:644:1:regular file' ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  else
    return 1
  fi
}

vibepollo_package_snapshot_path() {
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  case "$vibepollo_package_identity" in
    absent | *:0:0:'symbolic link' | *:0:0:'regular file') ;;
    *) return 1 ;;
  esac
  vibepollo_package_write_record "$1.before" "$vibepollo_package_identity"
}

vibepollo_package_cleanup_record_temporaries() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_has_temporary=0
  # First validate the complete set, so one malformed entry prevents every
  # deletion. Writers inherit fd 8; a fresh caller cannot hold the lock while
  # an interrupted operation's surviving writer is still publishing a record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    vibepollo_package_has_temporary=1
    vibepollo_package_suffix=${vibepollo_package_path##*/}
    case "$vibepollo_package_suffix" in
      .record.*) vibepollo_package_suffix=${vibepollo_package_suffix#.record.} ;;
      .mask.*) vibepollo_package_suffix=${vibepollo_package_suffix#.mask.} ;;
      *) return 1 ;;
    esac
    [ "${#vibepollo_package_suffix}" -eq 6 ] || return 1
    case "$vibepollo_package_suffix" in *[!A-Za-z0-9]*) return 1 ;; esac
    if [ -L "$vibepollo_package_path" ]; then
      case "${vibepollo_package_path##*/}" in .mask.*) ;; *) return 1 ;; esac
      [ "$(readlink -- "$vibepollo_package_path")" = /dev/null ] || return 1
      vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%h:%%F' -- "$vibepollo_package_path") || return 1
      case "$vibepollo_package_temporary_attributes" in
        '0:0:1:symbolic link' | '0:0:2:symbolic link') continue ;;
        *) return 1 ;;
      esac
    fi
    [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
    vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path") || return 1
    case "$vibepollo_package_temporary_attributes" in
      '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;;
      *) return 1 ;;
    esac
  done
  [ "$vibepollo_package_has_temporary" -eq 1 ] || return 0
  [ "${vibepollo_package_lock_held:-0}" -eq 1 ] || return 1
  flock --nonblock 8 || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
}

vibepollo_package_snapshot_host_executables() {
  if [ -e "$vibepollo_package_transaction/host-executables.before" ] || [ -L "$vibepollo_package_transaction/host-executables.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before"
    return $?
  fi
  vibepollo_package_executables=$(vibepollo_package_capture_host_executables) || return 1
  vibepollo_package_write_record host-executables.before "$vibepollo_package_executables"
}

vibepollo_package_lock() {
  [ "${vibepollo_package_lock_held:-0}" -eq 0 ] || return 0
  vibepollo_package_transaction_is_safe || return 1
  if [ -e "$vibepollo_package_transaction/lock" ] || [ -L "$vibepollo_package_transaction/lock" ]; then
    [ -f "$vibepollo_package_transaction/lock" ] && [ ! -L "$vibepollo_package_transaction/lock" ] || return 1
    vibepollo_package_lock_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_transaction/lock") || return 1
    case "$vibepollo_package_lock_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  else
    (umask 077; set -C; printf 'package-lock\n' >"$vibepollo_package_transaction/lock") || return 1
  fi
  exec 8<>"$vibepollo_package_transaction/lock" || return 1
  flock --nonblock 8 || return 1
  vibepollo_package_lock_held=1
  vibepollo_package_cleanup_record_temporaries
}

vibepollo_package_properties_are_unique() {
  vibepollo_package_property_output=$1
  shift
  [ "$(printf '%%s\n' "$vibepollo_package_property_output" | wc -l | tr -d ' ')" = "$#" ] || return 1
  for vibepollo_package_property do
    [ "$(printf '%%s\n' "$vibepollo_package_property_output" | grep -c "^$vibepollo_package_property=" || true)" = 1 ] || return 1
  done
}

vibepollo_package_controller_identity_is_safe() {
  vibepollo_package_properties_are_unique "$1" MainPID ControlGroup FreezerState InvocationID || return 1
  vibepollo_package_identity_pid=$(printf '%%s\n' "$1" | sed -n 's/^MainPID=//p')
  vibepollo_package_identity_group=$(printf '%%s\n' "$1" | sed -n 's/^ControlGroup=//p')
  vibepollo_package_identity_invocation=$(printf '%%s\n' "$1" | sed -n 's/^InvocationID=//p')
  vibepollo_package_identity_freezer=$(printf '%%s\n' "$1" | sed -n 's/^FreezerState=//p')
  case "$vibepollo_package_identity_pid" in '' | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_package_identity_group" in '' | /*) ;; *) return 1 ;; esac
  case "$vibepollo_package_identity_group" in */../* | */.. | *'
'*) return 1 ;; esac
  case "$vibepollo_package_identity_freezer" in '' | running | frozen) ;; *) return 1 ;; esac
  if [ "$vibepollo_package_identity_pid" != 0 ]; then
    [ -n "$vibepollo_package_identity_group" ] && [ -n "$vibepollo_package_identity_freezer" ] || return 1
  fi
  if [ "$vibepollo_package_identity_pid" = 0 ] && [ -z "$vibepollo_package_identity_invocation" ]; then return 0; fi
  [ "${#vibepollo_package_identity_invocation}" -eq 32 ] || return 1
  case "$vibepollo_package_identity_invocation" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_record_directory_is_safe() {
  vibepollo_package_transaction_is_safe || return 1
  # Refuse unknown files rather than recursively deleting a damaged record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/* "$vibepollo_package_transaction"/.[!.]* "$vibepollo_package_transaction"/..?*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    case "${vibepollo_package_path##*/}" in
      lock | phase | controller | handoff.before | host-executables.before | host-stop.before | run-id | failure-class | legacy-adopted | fence.before | fence.owned | vibepollo.service.before | vibepollo.service.owned | vibepollo-session-exec.socket.before | vibepollo-session-exec.socket.owned | vibeshine-vkms-control.socket.before | vibeshine-vkms-control.socket.owned | vibepollo-session-restore@.service.before | vibepollo-session-restore@.service.owned) ;;
      *) return 1 ;;
    esac
    if [ "${vibepollo_package_path##*/}" = lock ] && [ ! -s "$vibepollo_package_path" ]; then
      [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] &&
        [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path")" = '0:0:600:1:regular empty file' ] || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_path" || return 1
    fi
  done
}

vibepollo_package_run_id_is_safe() {
  [ "${#1}" -eq 32 ] || return 1
  case "$1" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_preserve_unowned_host_mask() {
  vibepollo_package_preserve_host_mask=0
  for vibepollo_package_mask_path in /etc/systemd/system/vibepollo.service /run/systemd/system/vibepollo.service; do
    [ -L "$vibepollo_package_mask_path" ] || continue
    [ "$(readlink -- "$vibepollo_package_mask_path")" = /dev/null ] || continue
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    case "$vibepollo_package_mask_identity" in
      *:0:0:'symbolic link') ;;
      *) echo "error: unsafe host mask at $vibepollo_package_mask_path; package operation is blocked." >&2; return 1 ;;
    esac
    if [ "$vibepollo_package_mask_path" = /run/systemd/system/vibepollo.service ] &&
       { [ -e "$vibepollo_package_transaction/vibepollo.service.owned" ] || [ -L "$vibepollo_package_transaction/vibepollo.service.owned" ]; }; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/vibepollo.service.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/vibepollo.service.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    vibepollo_package_preserve_host_mask=1
  done
  [ "$vibepollo_package_preserve_host_mask" -eq 1 ] || return 0
  vibepollo_package_mask_state=$(timeout --signal=KILL 5 systemctl is-enabled vibepollo.service 2>/dev/null || true)
  vibepollo_package_mask_load=$(timeout --signal=KILL 5 systemctl show vibepollo.service --property=LoadState 2>/dev/null) || vibepollo_package_mask_load=uncertain
  vibepollo_package_mask_verified=0
  case "$vibepollo_package_mask_state:$vibepollo_package_mask_load" in
    masked:LoadState=masked | masked-runtime:LoadState=masked) vibepollo_package_mask_verified=1 ;;
  esac
  if [ "$vibepollo_package_mask_verified" -ne 1 ] ||
     ! vibepollo_unit_is_quiescent vibepollo.service ||
     ! vibepollo_package_no_jobs; then
    echo 'error: an unowned Vibepollo host mask exists, but the host is active, populated, has a pending job, or its state is uncertain; package replacement is blocked. Force cannot remove an administrator or legacy mask without proving its activation ownership.' >&2
    return 1
  fi
  # Preserve the actual mask and any unowned fence byte-for-byte. The mask
  # supplies no-start admission while the full generation proof still runs.
  return 0
}

vibepollo_package_prepare_postinstall_start() {
  vibepollo_package_start_deferred=0
  vibepollo_package_preserve_unowned_host_mask || return 1
  if [ "$vibepollo_package_preserve_host_mask" -eq 1 ]; then
    vibepollo_package_start_deferred=1
  fi
  for vibepollo_package_start_unit in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    for vibepollo_package_start_directory in /etc/systemd/system /run/systemd/system; do
      vibepollo_package_start_path=$vibepollo_package_start_directory/$vibepollo_package_start_unit
      [ -L "$vibepollo_package_start_path" ] || continue
      [ "$(readlink -- "$vibepollo_package_start_path")" = /dev/null ] || continue
      vibepollo_package_start_identity=$(vibepollo_package_path_identity "$vibepollo_package_start_path") || return 1
      case "$vibepollo_package_start_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      if [ "$vibepollo_package_start_directory" = /run/systemd/system ] &&
         { [ -e "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ]; }; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" || return 1
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned")" = "$vibepollo_package_start_identity" ] || return 1
        continue
      fi
      vibepollo_package_start_deferred=1
    done
  done
  if [ -e "$vibepollo_package_transaction/fence.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
    vibepollo_package_prior_fence=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
    if [ "$vibepollo_package_prior_fence" != absent ]; then
      [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_prior_fence" ] || return 1
      vibepollo_package_start_deferred=1
    fi
  fi
  [ "$vibepollo_package_start_deferred" -eq 1 ] || return 0
  # This must be the full actual generation proof (controller, broker/apps,
  # restore/control instances, session record and retained executable inodes).
  # The local host-empty + no-jobs classifier is insufficient for release.
  if ! vibepollo_package_complete_quiescence_is_safe || ! vibepollo_package_release; then
    echo 'error: safe package shutdown recovery could not finish; automatic service activation remains deferred. Resolve the preceding check and retry package configuration; administrator masks were preserved.' >&2
    return 1
  fi
  echo 'package: preserved administrator host mask or start fence; configuration completed and automatic service activation is deferred.' >&2
  return 0
}

vibepollo_package_bind_run() {
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    vibepollo_package_write_record run-id "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
  elif [ ! -e "$vibepollo_package_transaction/run-id" ] && [ ! -L "$vibepollo_package_transaction/run-id" ]; then
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record failure-class none
}

vibepollo_package_run_is_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
  vibepollo_package_record_run=$(cat -- "$vibepollo_package_transaction/run-id") || return 1
  vibepollo_package_run_id_is_safe "$vibepollo_package_record_run" || return 1
  [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ] || return 0
  vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" &&
    [ "$vibepollo_package_record_run" = "$VIBEPOLLO_PACKAGE_RUN_ID" ]
}

vibepollo_package_force_is_applicable() (
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" &&
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/failure-class" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = late ] &&
    [ "$(cat -- "$vibepollo_package_transaction/failure-class")" = host-stop-deadline ]
)

vibepollo_package_refresh_missing_owned() {
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || continue
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    if [ "$vibepollo_package_current" = absent ]; then
      rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    else
      [ "$vibepollo_package_current" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" ] || return 1
    fi
  done
}

vibepollo_package_cleanup_retired() (
  vibepollo_package_original_root=$vibepollo_package_transaction
  vibepollo_package_retired_root=$vibepollo_package_original_root.retired
  [ -e "$vibepollo_package_retired_root" ] || [ -L "$vibepollo_package_retired_root" ] || return 0
  # This independent root-owned lock survives removal of the retired record's
  # own lock inode, including SIGKILL between the final unlink and rmdir.
  vibepollo_package_gc_lock=$vibepollo_package_original_root.gc-lock
  if [ ! -e "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ]; then
    (umask 077; set -C; printf 'package-gc\n' >"$vibepollo_package_gc_lock") || return 1
  fi
  [ -f "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ] || return 1
  vibepollo_package_gc_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_gc_lock") || return 1
  case "$vibepollo_package_gc_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  exec 7<>"$vibepollo_package_gc_lock" || return 1
  timeout --signal=KILL 5 flock --exclusive 7 || return 1
  vibepollo_package_transaction=$vibepollo_package_retired_root
  [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ] || return 0
  vibepollo_package_transaction_is_safe && vibepollo_package_record_directory_is_safe || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/*; do
    [ -e "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
  rmdir -- "$vibepollo_package_transaction" || return 1
)

vibepollo_package_adopt_legacy_barriers() {
  # Only the exact old package signature, with every admission path closed
  # and the complete old generation drained, permits ownership migration.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
  vibepollo_package_adoption_pending=0
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    vibepollo_package_adoption_pending=1
    # The marker is published only after the exact old fence, all original
    # mask identities and full generation proof were verified. An interrupted
    # conversion may leave some .before snapshots unconverted when old package
    # unwind removes the fence. Resume only those original masks, never a
    # replacement path or an unrecorded new mask.
    if [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ]; then
      vibepollo_package_complete_quiescence_is_safe || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      if [ -e "$vibepollo_package_transaction/fence.owned" ] || [ -L "$vibepollo_package_transaction/fence.owned" ]; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.owned" || return 1
      fi
      # Validate every path before changing any record. Repeat the identity
      # checks during publication so interruption leaves resumable metadata.
      for vibepollo_package_adoption_pass in validate commit; do
        for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
          vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
          if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
            vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
            vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
          fi
          vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
          if [ "$vibepollo_package_current" != absent ]; then
            case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
            [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
            [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
          fi
          [ "$vibepollo_package_adoption_pass" = commit ] || continue
          if [ "$vibepollo_package_current" = absent ]; then
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
            rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          else
            vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
          fi
        done
      done
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      vibepollo_package_write_record fence.before absent || return 1
      rm -f -- "$vibepollo_package_transaction/fence.owned" || return 1
      vibepollo_package_write_record phase preflight || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      rm -f -- "$vibepollo_package_transaction/legacy-adopted" || return 1
      return 0
    fi
  fi
  [ "$vibepollo_package_before" != absent ] || [ "$vibepollo_package_adoption_pending" -eq 1 ] || return 0
  [ -f "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = '0:0:644:1:regular file' ] || return 1
  vibepollo_package_legacy_contents=$(cat -- "$vibepollo_host_upgrade_dropin") || return 1
  case "$vibepollo_package_legacy_contents" in
    '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=mixed
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no') ;;
    *) [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1; return 0 ;;
  esac
  [ "$(stat -c '%%s' -- "$vibepollo_host_upgrade_dropin")" = "$(printf '%%s\n' "$vibepollo_package_legacy_contents" | wc -c | tr -d ' ')" ] || {
    [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1
    return 0
  }
  vibepollo_package_complete_quiescence_is_safe || {
    echo 'error: an earlier package fence remains, but the old host or session has not fully drained; it cannot be adopted safely.' >&2
    return 1
  }
  # Generation inspection can take time. An administrator may edit the same
  # inode while it runs; do not convert authority for changed fence bytes.
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
  done
  if [ -e "$vibepollo_package_transaction/fence.owned" ]; then
    vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.owned") || return 1
  fi
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_before" ] || return 1
  vibepollo_package_adoption_fence_identity=$vibepollo_package_before
  vibepollo_package_write_record legacy-adopted yes || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    # Recheck each original identity at publication, rather than claiming a
    # replacement that appeared after the inventory pass.
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
    if [ "$vibepollo_package_name" != fence ]; then
      [ "$(readlink -- "$vibepollo_package_owned_path")" = /dev/null ] || return 1
    fi
    vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
    vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
  done
  vibepollo_package_write_record phase preflight || return 1
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_adoption_fence_identity" ] || return 1
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  # Every owned identity and before snapshot is now durable; the pending
  # adoption marker is no longer needed by later release/retry operations.
  rm -f -- "$vibepollo_package_transaction/legacy-adopted"
}

vibepollo_package_rebaseline_unwound_run() {
  # Pending legacy authority is not a wholly unwound transaction even when
  # no .owned file has been published yet. Reconcile it before a new target.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    return 0
  fi
  # A native package retry, or a fresh explicit installer operation, may
  # select the current healthy invocation after every owned barrier unwound.
  # This grants a fresh target, never proof that old process resources vanished.
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    [ -e "$vibepollo_package_transaction/run-id" ] || return 0
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
    # A same-run force retry must retain its original invocation identity.
    [ "$(cat -- "$vibepollo_package_transaction/run-id")" != "$VIBEPOLLO_PACKAGE_RUN_ID" ] || return 0
  fi
  for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
    [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 0
  done
  vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    [ "$(vibepollo_package_path_identity "$vibepollo_package_owned_path")" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" ] || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  if [ -z "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    # A native retry is a new operation. Retire any earlier wrapper's nonce
    # atomically before refreshing phase or discarding its saved stop identity.
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  rm -f -- "$vibepollo_package_transaction/host-stop.before" || return 1
  vibepollo_package_prior_phase=early
}

vibepollo_package_begin() {
  vibepollo_package_cleanup_retired || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_record_directory_is_safe || return 1
    vibepollo_package_refresh_missing_owned || return 1
    if [ ! -e "$vibepollo_package_transaction/phase" ] && [ ! -L "$vibepollo_package_transaction/phase" ]; then
      for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
        [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
      done
      vibepollo_package_forget || return 1
      vibepollo_package_begin
      return $?
    fi
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
    vibepollo_package_prior_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
    case "$vibepollo_package_prior_phase" in early | late | preflight) ;; *) return 1 ;; esac
    for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    done
    vibepollo_package_snapshot_host_executables || return 1
    vibepollo_package_rebaseline_unwound_run || return 1
    vibepollo_package_bind_run || return 1
    vibepollo_package_adopt_legacy_barriers || return 1
    return 0
  fi
  # mkdir is exclusive; a competing package operation may not share ownership.
  (umask 077; mkdir -- "$vibepollo_package_transaction") || return 1
  vibepollo_package_transaction_created=1
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_lock || return 1
  vibepollo_package_snapshot_host_executables || return 1
  vibepollo_package_prior_phase=early
  vibepollo_package_snapshot_path fence "$vibepollo_host_upgrade_dropin" || return 1
  vibepollo_package_handoff=absent
  if [ -e "${vibepollo_legacy_handoff:-/usr/libexec/vibeshine/vibepollo-session-handoff}" ] || [ -L "$vibepollo_legacy_handoff" ]; then
    [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] || return 1
    vibepollo_package_handoff=$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff") || return 1
    case "$vibepollo_package_handoff" in *:0:0:755:1 | *:0:0:0:1) ;; *) return 1 ;; esac
  fi
  vibepollo_package_write_record handoff.before "$vibepollo_package_handoff" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_snapshot_path "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  vibepollo_package_bind_run || return 1
  vibepollo_package_adopt_legacy_barriers
}

vibepollo_package_record_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 0
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_identity" != absent ] || return 1
  if [ -e "$vibepollo_package_transaction/$1.owned" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_identity" ]
  else
    vibepollo_package_write_record "$1.owned" "$vibepollo_package_identity"
  fi
}

vibepollo_package_mask() (
  vibepollo_package_mask_stage=
  trap '[ -z "$vibepollo_package_mask_stage" ] || rm -f -- "$vibepollo_package_mask_stage"' 0
  for vibepollo_package_name do
    case "$vibepollo_package_name" in
      vibepollo.service | vibepollo-session-exec.socket | vibeshine-vkms-control.socket | vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = absent ] &&
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    if [ "$vibepollo_package_mask_identity" = absent ]; then
      # Keep a private inode as creation evidence. Publish its ownership intent
      # before linking it, so interruption cannot strand our published mask.
      # ln -P -T never replaces an administrator's competing path.
      vibepollo_package_mask_stage=$(mktemp "$vibepollo_package_transaction/.mask.XXXXXX") || return 1
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      ln -s /dev/null "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_created_mask=$(vibepollo_package_path_identity "$vibepollo_package_mask_stage") || return 1
      case "$vibepollo_package_created_mask" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
      vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_created_mask" || return 1
      if ! ln -P -T -- "$vibepollo_package_mask_stage" "$vibepollo_package_mask_path" 2>/dev/null; then
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        if [ "$vibepollo_package_mask_identity" != "$vibepollo_package_created_mask" ]; then
          # An exclusive-link failure published none of our inode. Drop only
          # our exact intent, then preserve a valid competing external mask.
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_created_mask" ] || return 1
          rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$vibepollo_package_mask_identity" != absent ] || return 1
        fi
      else
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        [ "$vibepollo_package_mask_identity" = "$vibepollo_package_created_mask" ] || return 1
      fi
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_mask_stage=
      if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then continue; fi
    fi
    # Existing unowned masks, including ones added since a failed attempt's
    # snapshot, remain policy. Refresh their preserved baseline, never claim.
    case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_write_record "$vibepollo_package_name.before" "$vibepollo_package_mask_identity" || return 1
    [ "$(vibepollo_package_path_identity "$vibepollo_package_mask_path")" = "$vibepollo_package_mask_identity" ] || return 1
  done
  timeout --signal=KILL 15 systemctl mask --runtime "$@" 2>/dev/null
  vibepollo_package_mask_status=$?
  # The manager can reload our prepublished masks, but cannot establish their
  # ownership. Even a timed-out client must leave every recorded inode intact.
  for vibepollo_package_name do
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
    else
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = "$vibepollo_package_mask_identity" ] || return 1
    fi
  done
  return "$vibepollo_package_mask_status"
)

vibepollo_package_mark_late() {
  vibepollo_package_write_record phase late
}

vibepollo_package_remove_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/$1.before") || return 1
  [ "$vibepollo_package_before" = absent ] || return 0
  vibepollo_package_current=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_current" != absent ] || return 0
  if [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ]; then
    # An idle Arch retry can skip mask creation. Reconcile an external mask
    # added since the snapshot without claiming it or reopening its admission.
    # New package masks publish intent first, so a missing record is not ours.
    case "$1:$2" in
      vibepollo.service:/run/systemd/system/vibepollo.service | vibepollo-session-exec.socket:/run/systemd/system/vibepollo-session-exec.socket | vibeshine-vkms-control.socket:/run/systemd/system/vibeshine-vkms-control.socket | vibepollo-session-restore@.service:/run/systemd/system/vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_lock || return 1
    vibepollo_package_complete_quiescence_is_safe || return 1
    [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ] || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    vibepollo_package_write_record "$1.before" "$vibepollo_package_current" || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    return 0
  fi
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_current" ] || return 1
  rm -f -- "$2"
}

vibepollo_package_no_jobs() {
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_package_job=$(timeout --signal=KILL 5 systemctl show "$vibepollo_package_name" --property=Job 2>/dev/null) || return 1
    [ "$vibepollo_package_job" = 'Job=' ] || return 1
  done
}

vibepollo_package_restore_barriers() {
  vibepollo_package_keep_legacy=${1:-0}
  vibepollo_package_transaction_is_safe || return 1
  # An interrupted ownership conversion must finish (or refuse a changed
  # legacy fence) before release can remove any path by its saved inode.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_adopt_legacy_barriers || return 1
  fi
  vibepollo_package_remove_owned fence "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_keep_legacy" -eq 1 ] && [ "$vibepollo_package_name" = 'vibepollo-session-restore@.service' ]; then continue; fi
    vibepollo_package_remove_owned "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  timeout --signal=KILL 15 systemctl daemon-reload || return 1
}

vibepollo_package_forget() {
  # Failed initial adoption can already own barriers. Retain its authority
  # until a proved resume completes; a created-directory flag cannot erase it.
  [ ! -e "$vibepollo_package_transaction/legacy-adopted" ] &&
    [ ! -L "$vibepollo_package_transaction/legacy-adopted" ] || return 1
  vibepollo_package_transaction_is_safe && vibepollo_package_cleanup_record_temporaries &&
    vibepollo_package_record_directory_is_safe || return 1
  vibepollo_package_forget_root=$vibepollo_package_transaction
  [ ! -e "$vibepollo_package_forget_root.retired" ] && [ ! -L "$vibepollo_package_forget_root.retired" ] || return 1
  mv -T -- "$vibepollo_package_forget_root" "$vibepollo_package_forget_root.retired" || return 1
  exec 8>&-
  vibepollo_package_lock_held=0
  vibepollo_package_cleanup_retired
}

vibepollo_package_restore_legacy_handoff() {
  # Only failed/cancelled transactions restore the same installed old inode.
  # Successful replacement deliberately retires this obsolete admission path.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/handoff.before" || return 1
  vibepollo_package_handoff=$(cat -- "$vibepollo_package_transaction/handoff.before") || return 1
  case "$vibepollo_package_handoff" in
    *:0:0:755:1)
      vibepollo_package_handoff_expected=${vibepollo_package_handoff%%:755:1}:0:1
      if [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] &&
         [ "$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff")" = "$vibepollo_package_handoff_expected" ]; then
        chmod 0755 -- "$vibepollo_legacy_handoff" || return 1
      fi
      ;;
    absent | *:0:0:0:1) ;;
    *) return 1 ;;
  esac
}

vibepollo_package_restore_early() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = early ] || return 1
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/controller" || return 1
    vibepollo_package_controller=$(cat -- "$vibepollo_package_transaction/controller") || return 1
    vibepollo_package_current_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" &&
      vibepollo_package_controller_identity_is_safe "$vibepollo_package_current_controller" || return 1
    # Thaw only our original exact unit identity, never an administrator's freeze.
    vibepollo_package_original_identity=$(printf '%%s\n' "$vibepollo_package_controller" | sed '/^FreezerState=/d')
    vibepollo_package_current_identity=$(printf '%%s\n' "$vibepollo_package_current_controller" | sed '/^FreezerState=/d')
    [ "$vibepollo_package_original_identity" = "$vibepollo_package_current_identity" ] || return 1
    vibepollo_package_restore_barriers || return 1
    if printf '%%s\n' "$vibepollo_package_controller" | grep -qx 'FreezerState=running' &&
       printf '%%s\n' "$vibepollo_package_current_controller" | grep -qx 'FreezerState=frozen'; then
      timeout --signal=KILL 15 systemctl thaw vibepollo-session-controller.service 2>/dev/null || return 1
    fi
    vibepollo_controller_was_frozen=0
  fi
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget
}

vibepollo_package_release() {
  # Called only after the old generation has completely drained.
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  [ ! -e /run/vibepollo/session.env ] && [ ! -L /run/vibepollo/session.env ] || return 1
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_restore_barriers 1 || return 1
    vibepollo_package_forget || return 1
  fi
}

vibepollo_package_recover_failed_transaction() (
  # The caller must wait for its package-manager child before invoking this.
  # Releasing a preflight while replacement is still executing is forbidden.
  if [ ! -e "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ]; then return 0; fi
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  vibepollo_package_recovery_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
  case "$vibepollo_package_recovery_phase" in
    early) vibepollo_package_restore_early; return $? ;;
    late | preflight) ;;
    *) return 1 ;;
  esac
  vibepollo_package_complete_quiescence_is_safe || return 1
  vibepollo_package_restore_barriers || return 1
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget || return 1
  echo 'package recovery: old host and session cleanup verified; transaction-owned admission fences released without starting services.' >&2
)








vibepollo_package_complete_quiescence_is_safe() {
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket vibepollo-prelogin.service vibepollo-machine-prepare.service; do
    vibepollo_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  vibepollo_control_instances_are_quiescent &&
    vibepollo_brokers_are_quiescent &&
    vibepollo_restore_instances_are_quiescent || return 1
  for vibepollo_package_runtime_path in /run/vibepollo/session.env "$vibepollo_broker_socket" "$vibepollo_control_socket" "$vibepollo_legacy_handoff_directory" "$vibepollo_legacy_restore_directory" "$vibepollo_legacy_transition_lock" "$vibepollo_legacy_prelogin_marker"; do
    [ -n "$vibepollo_package_runtime_path" ] && [ ! -e "$vibepollo_package_runtime_path" ] && [ ! -L "$vibepollo_package_runtime_path" ] || return 1
  done
}

vibepollo_package_preserved_preflight_is_safe() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = preflight ] || return 1
  vibepollo_restore_template_is_masked && vibepollo_control_socket_is_masked &&
    vibepollo_broker_socket_is_masked && vibepollo_host_unit_is_masked || return 1
  vibepollo_package_complete_quiescence_is_safe
}

vibepollo_package_open_owned_host_for_stop() {
  if [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ]; then return 0; fi
  # A prior late stop may finish after its client timed out. Re-expose the
  # installed unit only when the old cgroup is empty, retaining its start fence.
  vibepollo_unit_is_quiescent vibepollo.service && vibepollo_package_no_jobs || return 1
  vibepollo_package_remove_owned vibepollo.service /run/systemd/system/vibepollo.service || return 1
  [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ] || return 1
  rm -f -- "$vibepollo_package_transaction/vibepollo.service.owned"
}









vibepollo_restore_template_is_masked() {
  vibepollo_restore_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    'vibepollo-session-restore@.service' 2>/dev/null || true)
  vibepollo_restore_load=$(timeout --signal=KILL 5 systemctl show \
    'vibepollo-session-restore@.service' --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_restore_mask" in
    masked | masked-runtime) [ "$vibepollo_restore_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}

vibepollo_host_unit_is_masked() {
  vibepollo_host_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibepollo.service 2>/dev/null || true)
  vibepollo_host_load=$(timeout --signal=KILL 5 systemctl show \
    vibepollo.service --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_host_mask" in
    masked | masked-runtime) [ "$vibepollo_host_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}

vibepollo_broker_socket_is_masked() {
  vibepollo_socket_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibepollo-session-exec.socket 2>/dev/null || true)
  vibepollo_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-exec.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_socket_mask" in
    masked | masked-runtime) [ "$vibepollo_socket_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}

vibepollo_control_socket_is_masked() {
  vibepollo_socket_mask=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibeshine-vkms-control.socket 2>/dev/null || true)
  vibepollo_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibeshine-vkms-control.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_socket_mask" in
    masked | masked-runtime) [ "$vibepollo_socket_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}

vibepollo_restore_unit_is_safe() {
  vibepollo_restore_unit=$1
  case "$vibepollo_restore_unit" in
    vibepollo-session-restore@*.service) ;;
    *) return 1 ;;
  esac
  vibepollo_restore_instance=${vibepollo_restore_unit#vibepollo-session-restore@}
  vibepollo_restore_instance=${vibepollo_restore_instance%%.service}
  case "$vibepollo_restore_instance" in
    [1-9]*) ;;
    *) return 1 ;;
  esac
  case "$vibepollo_restore_instance" in
    *[!0-9]*) return 1 ;;
  esac
}

vibepollo_control_unit_is_safe() {
  vibepollo_control_unit=$1
  case "$vibepollo_control_unit" in
    vibeshine-vkms-control@*.service) ;;
    *) return 1 ;;
  esac
  vibepollo_control_instance=${vibepollo_control_unit#vibeshine-vkms-control@}
  vibepollo_control_instance=${vibepollo_control_instance%%.service}
  [ -n "$vibepollo_control_instance" ] || return 1
  case "$vibepollo_control_instance" in
    *[!A-Za-z0-9_.:-]*) return 1 ;;
  esac
}

vibepollo_capture_unit_list() {
  vibepollo_unit_list_path=$1
  vibepollo_unit_list_pattern=$2
  [ -f "$vibepollo_unit_list_path" ] && [ ! -L "$vibepollo_unit_list_path" ] || return 1
  chmod 0600 "$vibepollo_unit_list_path" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units \
      --all --plain --no-legend --no-pager --full \
      "$vibepollo_unit_list_pattern" >"$vibepollo_unit_list_path" 2>/dev/null
  ) || return 1
  vibepollo_unit_list_size=$(stat -c '%%s' -- "$vibepollo_unit_list_path") || return 1
  case "$vibepollo_unit_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_unit_list_size" -le 65536 ]
}

vibepollo_control_instances_are_quiescent() (
  vibepollo_control_list=$(mktemp /run/vibepollo-control-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_control_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_control_attempt=0
  vibepollo_control_clean_passes=0
  while [ "$vibepollo_control_attempt" -lt 100 ]; do
    vibepollo_control_dirty=0
    vibepollo_capture_unit_list "$vibepollo_control_list" \
      'vibeshine-vkms-control@*.service' || return 1
    vibepollo_seen_units='
'
    vibepollo_seen_count=0
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *"\r"*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1
      vibepollo_load=$2
      vibepollo_state=$3
      vibepollo_substate=$4
      vibepollo_control_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      vibepollo_seen_count=$((vibepollo_seen_count + 1))
      [ "$vibepollo_seen_count" -le 256 ] || return 1
      if ! vibepollo_unit_is_quiescent "$vibepollo_unit"; then
        vibepollo_control_dirty=1
      fi
    done <"$vibepollo_control_list"
    if [ "$vibepollo_control_dirty" -eq 0 ]; then
      vibepollo_control_clean_passes=$((vibepollo_control_clean_passes + 1))
      [ "$vibepollo_control_clean_passes" -lt 5 ] || {
        [ ! -e "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ] || return 1
        return 0
      }
    else
      vibepollo_control_clean_passes=0
    fi
    vibepollo_control_attempt=$((vibepollo_control_attempt + 1))
    sleep 0.1
  done
  return 1
)

vibepollo_select_upgrade_kill_mode() {
  # A stopped, empty host has no signal contract to preserve. In particular,
  # disabled installations must not depend on obsolete helper source text.
  if vibepollo_unit_is_quiescent vibepollo.service; then
    vibepollo_upgrade_kill_mode=control-group
    return 0
  fi
  vibepollo_privileged_helper_is_safe "$vibepollo_legacy_host" || return 1
  if grep -Fqx '  trap request_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=mixed
  elif grep -Fqx '  trap mark_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=control-group
  elif grep -Fqx "  trap 'forward_host_signal TERM' TERM" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal INT' INT" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal HUP' HUP" "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=process
  else
    # Unknown trusted supervisors get TERM first, without cancelling their
    # clients early. Bounded exact-unit shutdown still must prove an empty
    # cgroup before the package manager can replace any files.
    vibepollo_upgrade_kill_mode=mixed
  fi
}

vibepollo_prepare_host_upgrade_fence() (
  vibepollo_package_preserve_unowned_host_mask || exit 1
  [ "$vibepollo_package_preserve_host_mask" -eq 0 ] || exit 0
  [ ! -L "$vibepollo_host_upgrade_dropin_dir" ] && \
    [ ! -L "$vibepollo_host_upgrade_dropin" ] || exit 1
  install -d -o root -g root -m 0755 -- "$vibepollo_host_upgrade_dropin_dir" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_host_upgrade_dropin_dir")" = \
    '0:0:755:directory' ] || exit 1
  vibepollo_upgrade_temporary=$(mktemp /run/vibepollo-host-upgrade.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_upgrade_temporary"' 0
  case "$vibepollo_upgrade_kill_mode" in mixed | process | control-group) ;; *) exit 1 ;; esac
  printf '[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=%%s\nSendSIGKILL=no\nRestart=no\n' \
    "$vibepollo_upgrade_kill_mode" >"$vibepollo_upgrade_temporary" || exit 1
  chmod 0600 -- "$vibepollo_upgrade_temporary" || exit 1
  if [ -e "$vibepollo_host_upgrade_dropin" ]; then
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || exit 1
    if cmp -s -- "$vibepollo_upgrade_temporary" "$vibepollo_host_upgrade_dropin"; then
      vibepollo_package_record_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
      exit 0
    fi
    # A retry may select a new stop mode after the old host finishes. Only an
    # unchanged owned fence can be replaced, and only with an empty old host.
    vibepollo_unit_is_quiescent vibepollo.service && vibepollo_package_no_jobs || exit 1
    vibepollo_package_remove_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
    [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || exit 1
    rm -f -- "$vibepollo_package_transaction/fence.owned" || exit 1
  fi
  install -o root -g root -m 0644 -- "$vibepollo_upgrade_temporary" \
    "$vibepollo_host_upgrade_dropin" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
    '0:0:644:1:regular file' ] || exit 1
  vibepollo_package_record_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
)

vibepollo_activate_host_upgrade_fence() {
  vibepollo_package_preserve_unowned_host_mask || return 1
  [ "$vibepollo_package_preserve_host_mask" -eq 0 ] || return 0
  vibepollo_package_open_owned_host_for_stop || exit 1
  systemctl daemon-reload || exit 1
  vibepollo_host_stop_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=LoadState --property=RefuseManualStart --property=KillMode --property=SendSIGKILL --property=Restart 2>/dev/null) || exit 1
  if printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'LoadState=not-found'; then
    vibepollo_unit_is_quiescent vibepollo.service || exit 1
    return 0
  fi
  vibepollo_package_properties_are_unique "$vibepollo_host_stop_properties" LoadState RefuseManualStart KillMode SendSIGKILL Restart || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'RefuseManualStart=yes' || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | \
    grep -qx "KillMode=$vibepollo_upgrade_kill_mode" || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'SendSIGKILL=no' || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'Restart=no' || exit 1
}

vibepollo_host_is_stable_or_quiescent() {
  vibepollo_host_state=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup --property=Job 2>/dev/null) || return 1
  vibepollo_host_active=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^ActiveState=//p')
  vibepollo_host_substate=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^SubState=//p')
  vibepollo_host_pid=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^MainPID=//p')
  vibepollo_host_cgroup=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^ControlGroup=//p')
  vibepollo_host_job=$(printf '%%s\n' "$vibepollo_host_state" | sed -n 's/^Job=//p')
  [ -z "$vibepollo_host_job" ] || return 1
  if [ "$vibepollo_host_active" = active ] && [ "$vibepollo_host_substate" = running ]; then
    case "$vibepollo_host_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
    case "$vibepollo_host_cgroup" in /*) return 0 ;; *) return 1 ;; esac
  fi
  case "$vibepollo_host_active:$vibepollo_host_substate:$vibepollo_host_pid" in
    inactive:dead:0 | failed:failed:0) vibepollo_cgroup_is_quiescent "$vibepollo_host_cgroup" ;;
    *) return 1 ;;
  esac
}

vibepollo_freeze_controller() {
  if vibepollo_unit_is_quiescent vibepollo-session-controller.service; then
    vibepollo_controller_was_frozen=0
    return 0
  fi
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  vibepollo_controller_active=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ActiveState=//p')
  vibepollo_controller_substate=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^SubState=//p')
  vibepollo_controller_pid=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^MainPID=//p')
  vibepollo_controller_cgroup=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ControlGroup=//p')
  vibepollo_controller_invocation=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^InvocationID=//p')
  [ "${#vibepollo_controller_invocation}" -eq 32 ] || return 1
  case "$vibepollo_controller_invocation" in *[!0-9a-f]*) return 1 ;; esac
  [ "$vibepollo_controller_active" = active ] && \
    [ "$vibepollo_controller_substate" = running ] || return 1
  case "$vibepollo_controller_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in /*) ;; *) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in */../* | */..) return 1 ;; esac
  # A timed-out freeze may have succeeded; the abort path must attempt thaw.
  vibepollo_controller_was_frozen=1
  timeout --signal=TERM --kill-after=2 15 systemctl freeze \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation" || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=frozen' || return 1
  vibepollo_controller_freeze_path=/sys/fs/cgroup$vibepollo_controller_cgroup/cgroup.freeze
  vibepollo_controller_events_path=/sys/fs/cgroup$vibepollo_controller_cgroup/cgroup.events
  [ -f "$vibepollo_controller_freeze_path" ] && [ ! -L "$vibepollo_controller_freeze_path" ] && \
    grep -qx '1' "$vibepollo_controller_freeze_path" && \
    [ -f "$vibepollo_controller_events_path" ] && [ ! -L "$vibepollo_controller_events_path" ] && \
    grep -qx 'populated 1' "$vibepollo_controller_events_path" && \
    grep -qx 'frozen 1' "$vibepollo_controller_events_path" || return 1
}

vibepollo_controller_remains_frozen() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=frozen' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation"
}

vibepollo_thaw_controller() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  vibepollo_controller_remains_frozen || return 1
  timeout --signal=KILL 15 systemctl thaw \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_was_frozen=0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation"
}

vibepollo_stop_restore_instances() (
  vibepollo_restore_list=$(mktemp /run/vibepollo-restore-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_restore_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_capture_unit_list "$vibepollo_restore_list" \
    'vibepollo-session-restore@*.service' || return 1
  vibepollo_seen_units='
'
  vibepollo_seen_count=0
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1
    vibepollo_load=$2
    vibepollo_state=$3
    vibepollo_substate=$4
    vibepollo_restore_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_seen_count=$((vibepollo_seen_count + 1))
    [ "$vibepollo_seen_count" -le 256 ] || return 1
    vibepollo_stop_exact_unit "$vibepollo_unit"
    vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
  done <"$vibepollo_restore_list"
)

vibepollo_restore_instances_are_quiescent() (
  vibepollo_restore_list=$(mktemp /run/vibepollo-restore-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_restore_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_capture_unit_list "$vibepollo_restore_list" \
    'vibepollo-session-restore@*.service' || return 1
  vibepollo_seen_units='
'
  vibepollo_seen_count=0
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1
    vibepollo_load=$2
    vibepollo_state=$3
    vibepollo_substate=$4
    vibepollo_restore_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_seen_count=$((vibepollo_seen_count + 1))
    [ "$vibepollo_seen_count" -le 256 ] || return 1
    vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
  done <"$vibepollo_restore_list"
)

vibepollo_run_optional_legacy_command() {
  if [ ! -e "$vibepollo_legacy_host" ] && [ ! -L "$vibepollo_legacy_host" ]; then
    return 2
  fi
  vibepollo_privileged_helper_is_safe "$vibepollo_legacy_host" || return 1
  timeout --signal=KILL 40 "$vibepollo_legacy_host" "$1" >/dev/null 2>&1
  vibepollo_command_status=$?
  [ "$vibepollo_command_status" -eq 0 ] && return 0
  [ "$vibepollo_command_status" -eq 2 ] && return 2
  return 1
}

vibepollo_runtime_root_is_safe_or_absent() {
  if [ ! -e "$vibepollo_runtime_root" ] && [ ! -L "$vibepollo_runtime_root" ]; then
    return 0
  fi
  [ -d "$vibepollo_runtime_root" ] && [ ! -L "$vibepollo_runtime_root" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_runtime_root")" = '0:0:755:directory' ]
}

vibepollo_legacy_handoff_process_is_running() {
  for vibepollo_proc in /proc/[0-9]*; do
    [ -d "$vibepollo_proc" ] || continue
    if grep -Fzxq -- "$vibepollo_legacy_handoff" "$vibepollo_proc/cmdline" 2>/dev/null; then
      return 0
    fi
  done
  return 1
}

vibepollo_wait_for_legacy_handoff() {
  vibepollo_handoff_attempt=0
  vibepollo_handoff_clean_passes=0
  while [ "$vibepollo_handoff_attempt" -lt 400 ]; do
    if vibepollo_legacy_handoff_process_is_running; then
      vibepollo_handoff_clean_passes=0
    else
      vibepollo_handoff_clean_passes=$((vibepollo_handoff_clean_passes + 1))
      [ "$vibepollo_handoff_clean_passes" -lt 10 ] || return 0
    fi
    vibepollo_handoff_attempt=$((vibepollo_handoff_attempt + 1))
    sleep 0.1
  done
  return 1
}

vibepollo_disable_legacy_handoff() {
  if [ ! -e "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ]; then
    return 0
  fi
  [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] || return 1
  vibepollo_handoff_attributes=$(stat -c '%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff") || return 1
  case "$vibepollo_handoff_attributes" in
    '0:0:755:1' | '0:0:0:1') ;;
    *) return 1 ;;
  esac
  vibepollo_legacy_handoff_identity=$(stat -Lc '%%d:%%i' -- "$vibepollo_legacy_handoff") || return 1
  chmod 000 -- "$vibepollo_legacy_handoff" || return 1
  vibepollo_handoff_attributes=$(stat -Lc '%%d:%%i:%%u:%%g:%%a:%%h' -- \
    "$vibepollo_legacy_handoff") || return 1
  [ "$vibepollo_handoff_attributes" = \
    "$vibepollo_legacy_handoff_identity:0:0:0:1" ] || return 1

  # A PAM module already loaded by the login manager can still fork this exact
  # script. Mode 000 closes new execs; consecutive exact argv scans drain any
  # interpreter that passed exec admission before the mode change.
  vibepollo_wait_for_legacy_handoff
}

vibepollo_legacy_state_file_is_safe() {
  vibepollo_legacy_state_path=$1
  vibepollo_legacy_state_name=${vibepollo_legacy_state_path##*/}
  case "$vibepollo_legacy_state_name" in [1-9]*) ;; *) return 1 ;; esac
  case "$vibepollo_legacy_state_name" in *[!0-9]*) return 1 ;; esac
  [ -f "$vibepollo_legacy_state_path" ] && [ ! -L "$vibepollo_legacy_state_path" ] || return 1
  vibepollo_legacy_state_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%s' -- \
    "$vibepollo_legacy_state_path") || return 1
  case "$vibepollo_legacy_state_attributes" in
    0:0:600:1:* | 0:0:644:1:*) ;;
    *) return 1 ;;
  esac
  vibepollo_legacy_state_size=${vibepollo_legacy_state_attributes##*:}
  case "$vibepollo_legacy_state_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_legacy_state_size" -le 4096 ] || return 1
  {
    IFS= read -r vibepollo_legacy_state_user &&
      IFS= read -r vibepollo_legacy_state_uid &&
      IFS= read -r vibepollo_legacy_state_token &&
      ! IFS= read -r vibepollo_legacy_state_extra
  } <"$vibepollo_legacy_state_path" || return 1
  case "$vibepollo_legacy_state_user" in [a-z_]*) ;; *) return 1 ;; esac
  case "$vibepollo_legacy_state_user" in *[!a-z0-9_-]*) return 1 ;; esac
  [ "$vibepollo_legacy_state_uid" = "$vibepollo_legacy_state_name" ] || return 1
  [ "${#vibepollo_legacy_state_token}" -eq 32 ] || return 1
  case "$vibepollo_legacy_state_token" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_remove_legacy_state_directory() {
  vibepollo_legacy_directory=$1
  if [ ! -e "$vibepollo_legacy_directory" ] && [ ! -L "$vibepollo_legacy_directory" ]; then
    return 0
  fi
  [ -d "$vibepollo_legacy_directory" ] && [ ! -L "$vibepollo_legacy_directory" ] || return 1
  vibepollo_legacy_directory_attributes=$(stat -c '%%u:%%g:%%a' -- \
    "$vibepollo_legacy_directory") || return 1
  case "$vibepollo_legacy_directory_attributes" in
    '0:0:700' | '0:0:755') ;;
    *) return 1 ;;
  esac
  for vibepollo_legacy_state_path in \
    "$vibepollo_legacy_directory"/* \
    "$vibepollo_legacy_directory"/.[!.]* \
    "$vibepollo_legacy_directory"/..?*; do
    if [ ! -e "$vibepollo_legacy_state_path" ] && [ ! -L "$vibepollo_legacy_state_path" ]; then
      continue
    fi
    vibepollo_legacy_state_file_is_safe "$vibepollo_legacy_state_path" || return 1
    rm -f -- "$vibepollo_legacy_state_path" || return 1
  done
  rmdir -- "$vibepollo_legacy_directory" || return 1
  [ ! -e "$vibepollo_legacy_directory" ] && [ ! -L "$vibepollo_legacy_directory" ]
}

vibepollo_remove_legacy_prelogin_marker() {
  if [ ! -e "$vibepollo_legacy_prelogin_marker" ] && [ ! -L "$vibepollo_legacy_prelogin_marker" ]; then
    return 0
  fi
  [ -f "$vibepollo_legacy_prelogin_marker" ] && \
    [ ! -L "$vibepollo_legacy_prelogin_marker" ] || return 1
  vibepollo_prelogin_marker_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%s' -- \
    "$vibepollo_legacy_prelogin_marker") || return 1
  case "$vibepollo_prelogin_marker_attributes" in
    0:0:644:1:*) ;;
    *) return 1 ;;
  esac
  vibepollo_prelogin_marker_size=${vibepollo_prelogin_marker_attributes##*:}
  case "$vibepollo_prelogin_marker_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_prelogin_marker_size" -le 256 ] || return 1
  {
    IFS= read -r vibepollo_prelogin_marker_user &&
      ! IFS= read -r vibepollo_prelogin_marker_extra
  } <"$vibepollo_legacy_prelogin_marker" || return 1
  case "$vibepollo_prelogin_marker_user" in [a-z_]*) ;; *) return 1 ;; esac
  case "$vibepollo_prelogin_marker_user" in *[!a-z0-9_-]*) return 1 ;; esac
  rm -f -- "$vibepollo_legacy_prelogin_marker" || return 1
  [ ! -e "$vibepollo_legacy_prelogin_marker" ] && [ ! -L "$vibepollo_legacy_prelogin_marker" ]
}

vibepollo_cleanup_legacy_transition_state() {
  (
    vibepollo_runtime_root_is_safe_or_absent || exit 1
    if [ -e "$vibepollo_legacy_transition_lock" ] || [ -L "$vibepollo_legacy_transition_lock" ]; then
      [ -f "$vibepollo_legacy_transition_lock" ] && \
        [ ! -L "$vibepollo_legacy_transition_lock" ] || exit 1
      [ "$(stat -c '%%u:%%g:%%a:%%h:%%s' -- "$vibepollo_legacy_transition_lock")" = \
        '0:0:600:1:0' ] || exit 1
      exec 9<>"$vibepollo_legacy_transition_lock" || exit 1
      timeout --signal=KILL 10 flock --exclusive 9 || exit 1
    fi
    vibepollo_wait_for_legacy_handoff || exit 1
    vibepollo_remove_legacy_state_directory "$vibepollo_legacy_handoff_directory" || exit 1
    vibepollo_remove_legacy_state_directory "$vibepollo_legacy_restore_directory" || exit 1
    vibepollo_remove_legacy_prelogin_marker || exit 1
    if [ -e "$vibepollo_legacy_transition_lock" ] || [ -L "$vibepollo_legacy_transition_lock" ]; then
      rm -f -- "$vibepollo_legacy_transition_lock" || exit 1
    fi
    [ ! -e "$vibepollo_legacy_transition_lock" ] && \
      [ ! -L "$vibepollo_legacy_transition_lock" ]
  )
}

vq() {
  vibepollo_quiesce_step=$*
  "$@"
}

vibepollo_abort_quiesce() {
  vibepollo_package_abort_done=1
  trap - HUP INT TERM
  echo "error: could not safely quiesce the existing Vibepollo host (failed step: ${vibepollo_quiesce_step:-unknown}); package replacement is blocked." >&2

  # Before broker admission closes, preserve the existing GPU-owning host.
  # Keep any runtime fence/masks for a deliberate retry or reboot instead of
  # racing a restart against a partially completed shutdown.
  if [ "${vibepollo_shutdown_started:-0}" -eq 0 ]; then
    if vibepollo_package_restore_early; then
      echo 'error: package replacement was aborted; transaction-owned admission fences were restored.' >&2
    else
      echo 'error: shutdown recovery is uncertain; owned fences are retained. Retry this package operation to finish exact host and session cleanup.' >&2
    fi
    return 0
  fi
  # The failed attempt already spent its shutdown budget. Do not signal a
  # changed/new invocation or spend a second grace interval from the abort trap.
  if ! vibepollo_unit_is_quiescent vibepollo.service; then
    echo 'error: the old host still owns resources; its controller and admission fences remain protected. Retry this package operation to finish bounded shutdown.' >&2
    return 1
  fi
  if [ "${vibepollo_controller_was_frozen:-0}" -eq 1 ]; then
    vibepollo_thaw_controller || {
      echo 'error: original controller identity could not be verified for thaw; transaction fences were retained.' >&2
      return 1
    }
  fi
  vibepollo_stop_exact_unit vibepollo-session-controller.service
  # Socket paths that outlived a masked-then-stopped unit would keep the next
  # controller's cleanup failing; drop them only once their unit is stopped.
  if vibepollo_unit_is_quiescent vibepollo-session-exec.socket &&
     [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" 2>/dev/null || true
  fi
  if vibepollo_unit_is_quiescent vibeshine-vkms-control.socket &&
     [ -S "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ]; then
    rm -f -- "$vibepollo_control_socket" 2>/dev/null || true
  fi

  if vibepollo_package_recover_failed_transaction; then
    echo 'error: shutdown failed after the old generation drained; transaction-owned fences were released without restarting services.' >&2
  else
    echo 'error: retained transaction record; retry the package operation to finish exact host and generation cleanup.' >&2
  fi
}

vibepollo_quiesce_or_abort() (
  # Isolate lifecycle signal/EXIT traps from the package-manager shell.
  vibepollo_quiesce_step=''
  vibepollo_package_abort_done=0
  vibepollo_shutdown_started=0
  vibepollo_package_transaction_created=0
  trap 'vibepollo_package_status=$?; if [ "$vibepollo_package_status" -ne 0 ] && [ "$vibepollo_package_abort_done" -eq 0 ]; then vibepollo_abort_quiesce; fi' 0
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
  if ! vq vibepollo_package_begin; then
    if [ "$vibepollo_package_transaction_created" -eq 1 ]; then
      vibepollo_package_forget || true
    fi
    vibepollo_package_abort_done=1
    echo 'error: package transaction ownership record is unsafe or incomplete; no new shutdown was started.' >&2
    return 1
  fi
  if vibepollo_quiesce_machine_host; then
    vq vibepollo_package_unmanaged_host_absent || return 1
    vibepollo_package_write_record phase preflight || return 1
    return 0
  fi
  vibepollo_abort_quiesce
  return 1
)

vibepollo_unmask_host_for_controller() {
  vibepollo_unit_is_quiescent vibepollo-session-controller.service || return 1
  vibepollo_unit_is_quiescent vibepollo-session-exec.socket || return 1
  if [ -e "$vibepollo_host_upgrade_dropin" ] || [ -L "$vibepollo_host_upgrade_dropin" ]; then
    [ -f "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || return 1
    vibepollo_upgrade_contents=$(cat -- "$vibepollo_host_upgrade_dropin") || return 1
    case "$vibepollo_upgrade_contents" in
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no' | \
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no
Restart=no' | \
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no' | \
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no
Restart=no' | \
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=mixed
SendSIGKILL=no' | \
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=mixed
SendSIGKILL=no
Restart=no') ;;
      *) return 1 ;;
    esac
  fi
  vibepollo_package_release || return 1
  for vibepollo_package_name in vibepollo.service vibeshine-vkms-control.socket vibepollo-session-exec.socket; do
    [ ! -L "/run/systemd/system/$vibepollo_package_name" ] || {
      echo 'package recovery: an existing administrator runtime mask was preserved; automatic service start is deferred.' >&2
      return 1
    }
  done
  systemctl daemon-reload || return 1
  systemctl daemon-reload || return 1
  vibepollo_new_host_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=RefuseManualStart --property=KillMode --property=SendSIGKILL 2>/dev/null) || return 1
  printf '%%s\n' "$vibepollo_new_host_properties" | grep -qx 'RefuseManualStart=no' && \
    printf '%%s\n' "$vibepollo_new_host_properties" | grep -qx 'KillMode=mixed' && \
    printf '%%s\n' "$vibepollo_new_host_properties" | grep -qx 'SendSIGKILL=no' || return 1
  systemctl start vibeshine-vkms-control.socket || return 1
  systemctl is-active --quiet vibeshine-vkms-control.socket || return 1
  vibepollo_host_state=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibepollo.service 2>/dev/null || true)
  vibepollo_host_load=$(timeout --signal=KILL 5 systemctl show \
    vibepollo.service --property=LoadState 2>/dev/null) || return 1
  vibepollo_socket_state=$(timeout --signal=KILL 5 systemctl is-enabled \
    vibepollo-session-exec.socket 2>/dev/null || true)
  vibepollo_socket_load=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-exec.socket --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_socket_state" in
    disabled | static | indirect | generated | transient | linked | linked-runtime)
      [ "$vibepollo_socket_load" = 'LoadState=loaded' ] || return 1 ;;
    *) return 1 ;;
  esac
  case "$vibepollo_host_state" in
    disabled | static | indirect | generated | transient | linked | linked-runtime)
      [ "$vibepollo_host_load" = 'LoadState=loaded' ] ;;
    *) return 1 ;;
  esac
}
vibepollo_cgroup_is_quiescent() {
  vibepollo_control_group=$1
  [ -n "$vibepollo_control_group" ] || return 0
  case "$vibepollo_control_group" in
    /*) ;;
    *) return 1 ;;
  esac
  case "$vibepollo_control_group" in
    */../* | */..) return 1 ;;
  esac
  vibepollo_cgroup_path=/sys/fs/cgroup$vibepollo_control_group
  if [ ! -e "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ]; then
    return 0
  fi
  [ -d "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ] && \
    [ -f "$vibepollo_cgroup_path/cgroup.events" ] && \
    [ ! -L "$vibepollo_cgroup_path/cgroup.events" ] && \
    grep -qx 'populated 0' "$vibepollo_cgroup_path/cgroup.events"
}
vibepollo_unit_is_quiescent() {
  vibepollo_properties=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup 2>/dev/null) || return 1
  vibepollo_property_lines=$(printf '%%s\n' "$vibepollo_properties" | wc -l | tr -d ' ')
  case "$1:$vibepollo_property_lines" in
    *.socket:4 | *.socket:5 | *:5) ;;
    *) return 1 ;;
  esac
  for vibepollo_property in LoadState ActiveState SubState ControlGroup; do
    vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | \
      grep -c "^$vibepollo_property=" || true)
    [ "$vibepollo_property_count" = 1 ] || return 1
  done
  vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | grep -c '^MainPID=' || true)
  case "$1:$vibepollo_property_count" in
    *.socket:0 | *.socket:1 | *:1) ;;
    *) return 1 ;;
  esac
  vibepollo_load=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^LoadState=//p')
  vibepollo_state=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ActiveState=//p')
  vibepollo_substate=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^SubState=//p')
  vibepollo_pid=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^MainPID=//p')
  case "$1" in *.socket) [ -n "$vibepollo_pid" ] || vibepollo_pid=0 ;; esac
  vibepollo_control_group=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ControlGroup=//p')
  case "$vibepollo_load:$vibepollo_state:$vibepollo_substate" in
    not-found:inactive:dead | \
    loaded:inactive:dead | loaded:failed:failed | \
    masked:inactive:dead | masked:failed:failed | \
    masked-runtime:inactive:dead | masked-runtime:failed:failed) ;;
    *) return 1 ;;
  esac
  [ "$vibepollo_pid" = 0 ] && vibepollo_cgroup_is_quiescent "$vibepollo_control_group"
}
vibepollo_unit_is_disabled() {
  vibepollo_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  case "$vibepollo_enabled" in
    disabled | masked | masked-runtime | static | indirect | generated | transient | linked | linked-runtime | not-found) return 0 ;;
    *) return 1 ;;
  esac
}
vibepollo_broker_unit_is_safe() {
  vibepollo_broker_unit=$1
  case "$vibepollo_broker_unit" in
    vibepollo-session-exec@*.service) ;;
    *) return 1 ;;
  esac
  vibepollo_broker_instance=${vibepollo_broker_unit#vibepollo-session-exec@}
  vibepollo_broker_instance=${vibepollo_broker_instance%%.service}
  [ -n "$vibepollo_broker_instance" ] || return 1
  case "$vibepollo_broker_instance" in
    *[!A-Za-z0-9_.:-]*) return 1 ;;
  esac
}
# Read the exact fenced host identity. A changed invocation must never inherit
# an earlier upgrade's escalation request, even if Linux has reused its PID.
vibepollo_package_capture_host_executables() {
  if [ ! -e /usr/bin/vibepollo ] && [ ! -L /usr/bin/vibepollo ] &&
     [ ! -e /usr/libexec/vibeshine/vibepollo-host ] && [ ! -L /usr/libexec/vibeshine/vibepollo-host ]; then
    printf '[]\n'
    return 0
  fi
  [ -x /usr/bin/python3 ] || {
    echo 'error: trusted Python pid/process inspection is required before replacing an installed host.' >&2
    return 1
  }
  timeout --signal=KILL 10 /usr/bin/python3 -I - <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
records = []
try:
    for name in paths:
        path = pathlib.Path(name)
        try:
            entry = path.lstat()
        except FileNotFoundError:
            continue
        if entry.st_uid != 0 or (stat.S_ISLNK(entry.st_mode) and name != paths[0]):
            raise ValueError("unsafe installed host executable")
        resolved = path.resolve(strict=True)
        identity = resolved.stat()
        if identity.st_uid != 0 or not stat.S_ISREG(identity.st_mode) or identity.st_mode & 0o022:
            raise ValueError("unsafe installed host executable")
        # A public package entrypoint may be a root-owned symlink; both its
        # original and resolved directory ancestry must remain root-owned.
        for parent in set((path.parent, *path.parent.parents, resolved.parent, *resolved.parent.parents)):
            owner = parent.stat()
            if owner.st_uid != 0 or not stat.S_ISDIR(owner.st_mode) or owner.st_mode & 0o022:
                raise ValueError("unsafe installed host executable ancestry")
        records.append({"path": name, "device": identity.st_dev, "inode": identity.st_ino})
    print(json.dumps(records, separators=(",", ":")))
except (OSError, ValueError) as error:
    print(f"error: installed host identity could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_package_unmanaged_host_absent() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before" || return 1
  vibepollo_package_executable_records=$(cat -- "$vibepollo_package_transaction/host-executables.before") || return 1
  if [ ! -x /usr/bin/python3 ]; then
    [ "$vibepollo_package_executable_records" = '[]' ] || {
      echo 'error: trusted Python process inspection is required for the retained old host inode snapshot.' >&2
      return 1
    }
    # First-install pre-hooks can run before Python dependencies are installed.
    # An empty capture still requires a bounded exact-path scan, including old
    # root-owned deleted executables; it is never treated as an absence proof.
    timeout --signal=KILL 10 /bin/sh -s <<'HOST_PROOF'
command -v readlink >/dev/null 2>&1 && command -v stat >/dev/null 2>&1 || exit 1
for executable in /proc/[0-9]*/exe; do
  target=$(readlink -v -- "$executable" 2>&1)
  status=$?
  if [ "$status" -ne 0 ]; then
    case "$target" in
      *': No such file or directory') continue ;;
      *) echo 'error: executable scope inspection is uncertain; replacement is blocked.' >&2; exit 1 ;;
    esac
  fi
  case "$target" in
    /usr/bin/vibepollo | '/usr/bin/vibepollo (deleted)' | \
    /usr/libexec/vibeshine/vibepollo-host | '/usr/libexec/vibeshine/vibepollo-host (deleted)')
      identity=$(stat -Lc '%%u:%%F' -- "$executable" 2>/dev/null) || exit 1
      if [ "$identity" = '0:regular file' ]; then
        process_id=${executable%/exe}
        process_id=${process_id##*/}
        echo "error: verified old host executable remains in PID $process_id; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership." >&2
        exit 1
      fi
      ;;
  esac
done
HOST_PROOF
    return $?
  fi
  timeout --signal=KILL 10 /usr/bin/python3 -I - "$vibepollo_package_executable_records" <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
try:
    records = json.loads(sys.argv[1])
    if not isinstance(records, list) or len(records) > 2:
        raise ValueError("invalid executable identity snapshot")
    identities = set()
    names = set()
    for record in records:
        if not isinstance(record, dict) or set(record) != {"path", "device", "inode"}:
            raise ValueError("invalid executable identity snapshot")
        if record["path"] not in paths or record["path"] in names:
            raise ValueError("invalid executable identity snapshot")
        if any(type(record[key]) is not int or record[key] <= 0 for key in ("device", "inode")):
            raise ValueError("invalid executable identity snapshot")
        names.add(record["path"])
        identities.add((record["device"], record["inode"]))
    # Linux retains executable inode identity after unlink/replacement. Scan
    # every scope, using no argv/name matching and issuing no process signals.
    for process in pathlib.Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            executable = process / "exe"
            identity = executable.stat()
            target = os.readlink(executable)
        except (FileNotFoundError, ProcessLookupError):
            continue
        def report_old_owner(deleted=False):
            uid, scope = "unknown", "<unavailable>"
            try:
                # Diagnostics must describe the same observed task and inode.
                # PID reuse or disappearing metadata still blocks replacement;
                # never attribute a different process's UID/scope to this owner.
                before = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                status = (process / "status").read_text()
                uid_lines = [line.split()[1:] for line in status.splitlines() if line.startswith("Uid:")]
                if len(uid_lines) != 1 or len(uid_lines[0]) != 4 or not all(value.isdecimal() for value in uid_lines[0]):
                    raise ValueError("uncertain process UID")
                cgroup = (process / "cgroup").read_text()
                if len(cgroup) > 8192:
                    raise ValueError("oversized process scope")
                after = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                confirmed = executable.stat()
                if before != after or (confirmed.st_dev, confirmed.st_ino) != (identity.st_dev, identity.st_ino):
                    raise ValueError("changed process identity")
                uid, scope = uid_lines[0][1], cgroup.rstrip("\n")
            except (OSError, ValueError, IndexError):
                pass
            kind = "deleted old" if deleted else "old"
            print(f"error: verified {kind} host executable remains in PID {process.name}, UID {uid}, scope {json.dumps(scope)}; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership.", file=sys.stderr)
        if (identity.st_dev, identity.st_ino) in identities:
            report_old_owner()
            sys.exit(1)
        # A failed older transaction may lack a retained inode record. A
        # kernel-reported deleted executable at either exact root-owned package
        # path still supplies positive evidence of an old owner, never absence.
        if target in tuple(name + " (deleted)" for name in paths) and identity.st_uid == 0 and stat.S_ISREG(identity.st_mode) and not identity.st_mode & 0o022:
            report_old_owner(deleted=True)
            sys.exit(1)
except (OSError, ValueError, TypeError) as error:
    print(f"error: cross-scope old host absence could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_package_backup_for_force() {
  # Forced termination can interrupt a configuration write. Keep a private
  # snapshot before signalling; never follow service-owned links or mounts.
  # Logs are excluded because an active stream can keep changing them.
  umask 077
  vibepollo_backup_base=/var/lib
  vibepollo_backup_root=$vibepollo_backup_base/vibepollo-package-backups
  [ -d "$vibepollo_backup_base" ] && [ ! -L "$vibepollo_backup_base" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_base")" = '0:0:755:directory' ] || return 1
  if [ ! -e "$vibepollo_backup_root" ] && [ ! -L "$vibepollo_backup_root" ]; then
    mkdir -m 0700 -- "$vibepollo_backup_root" || return 1
  fi
  [ -d "$vibepollo_backup_root" ] && [ ! -L "$vibepollo_backup_root" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_root")" = '0:0:700:directory' ] || return 1
  vibepollo_backup_directory=$(mktemp -d "$vibepollo_backup_root/forced-stop.XXXXXX") || return 1
  [ -d "$vibepollo_backup_directory" ] && [ ! -L "$vibepollo_backup_directory" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_directory")" = '0:0:700:directory' ] || return 1
  set --
  for vibepollo_backup_source in etc/vibepollo var/lib/vibepollo; do
    if [ -e "/$vibepollo_backup_source" ] || [ -L "/$vibepollo_backup_source" ]; then
      [ -d "/$vibepollo_backup_source" ] && [ ! -L "/$vibepollo_backup_source" ] || return 1
      set -- "$@" "$vibepollo_backup_source"
    fi
  done
  command -v tar >/dev/null 2>&1 || return 1
  if [ "$#" -gt 0 ]; then
    timeout --signal=TERM --kill-after=2 30 tar --one-file-system \
      --exclude=var/lib/vibepollo/logs -C / -cf "$vibepollo_backup_directory/machine-state.tar" -- "$@" \
      >"$vibepollo_backup_directory/backup.log" 2>&1 || {
        echo "error: machine-state backup failed; forced shutdown was not attempted. Diagnostics: $vibepollo_backup_directory/backup.log" >&2
        return 1
      }
  else
    timeout --signal=TERM --kill-after=2 30 tar -cf "$vibepollo_backup_directory/machine-state.tar" \
      --files-from=/dev/null >"$vibepollo_backup_directory/backup.log" 2>&1 || return 1
  fi
  chmod 0600 -- "$vibepollo_backup_directory/machine-state.tar" "$vibepollo_backup_directory/backup.log" || return 1
  {
    printf 'product=vibepollo\noperation=forced-package-stop\nsnapshot=machine-state.tar\n'
    printf 'invocation=%%s\ncgroup=%%s\n' "$vibepollo_original_invocation" "$vibepollo_original_cgroup"
    printf 'Warning: snapshot taken while the old host may still be running; logs excluded.\n'
    if command -v pacman >/dev/null 2>&1; then timeout --signal=KILL 5 pacman -Q vibepollo 2>/dev/null || :; fi
    if command -v dpkg-query >/dev/null 2>&1; then timeout --signal=KILL 5 dpkg-query -W vibepollo 2>/dev/null || :; fi
    if command -v rpm >/dev/null 2>&1; then timeout --signal=KILL 5 rpm -q vibepollo 2>/dev/null || :; fi
  } >"$vibepollo_backup_directory/recovery.log" || return 1
  chmod 0600 -- "$vibepollo_backup_directory/recovery.log" || return 1
  echo "warning: forced shutdown will terminate active streams and may leave normal cleanup incomplete. Private machine-state snapshot and recovery log: $vibepollo_backup_directory" >&2
}

vibepollo_package_log_force_outcome() {
  case "$1" in pending | complete | kill-failed | still-populated | identity-changed | graceful-after-backup) ;; *) return 1 ;; esac
  case "${vibepollo_backup_directory:-}" in /var/lib/vibepollo-package-backups/forced-stop.*) ;; *) return 1 ;; esac
  [ -d "$vibepollo_backup_directory" ] && [ ! -L "$vibepollo_backup_directory" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_directory")" = '0:0:700:directory' ] || return 1
  [ -f "$vibepollo_backup_directory/recovery.log" ] && [ ! -L "$vibepollo_backup_directory/recovery.log" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_backup_directory/recovery.log")" = '0:0:600:1:regular file' ] || return 1
  printf 'outcome=%%s\n' "$1" >>"$vibepollo_backup_directory/recovery.log"
}



vibepollo_read_host_stop_identity() {
  if [ "$#" -eq 1 ]; then
    vibepollo_stop_properties=$1
  else
    vibepollo_stop_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
      --property=Id --property=InvocationID --property=MainPID --property=ControlGroup \
      --property=Restart --property=RefuseManualStart 2>/dev/null) || return 1
  fi
  vibepollo_stop_id='' vibepollo_stop_invocation='' vibepollo_stop_pid=''
  vibepollo_stop_cgroup='' vibepollo_stop_restart='' vibepollo_stop_fenced=''
  vibepollo_stop_fields=0
  while IFS= read -r vibepollo_stop_line; do
    vibepollo_stop_fields=$((vibepollo_stop_fields + 1))
    case "$vibepollo_stop_line" in
      Id=*) vibepollo_stop_id=${vibepollo_stop_line#*=} ;;
      InvocationID=*) vibepollo_stop_invocation=${vibepollo_stop_line#*=} ;;
      MainPID=*) vibepollo_stop_pid=${vibepollo_stop_line#*=} ;;
      ControlGroup=*) vibepollo_stop_cgroup=${vibepollo_stop_line#*=} ;;
      Restart=*) vibepollo_stop_restart=${vibepollo_stop_line#*=} ;;
      RefuseManualStart=*) vibepollo_stop_fenced=${vibepollo_stop_line#*=} ;;
      *) return 1 ;;
    esac
  done <<EOF
$vibepollo_stop_properties
EOF
  [ "$vibepollo_stop_fields" -eq 6 ] && [ "$vibepollo_stop_id" = vibepollo.service ] && \
    [ "$vibepollo_stop_restart" = no ] && [ "$vibepollo_stop_fenced" = yes ] || return 1
  [ "${#vibepollo_stop_invocation}" -eq 32 ] || return 1
  case "$vibepollo_stop_invocation" in *[!0-9a-f]*) return 1 ;; esac
  case "$vibepollo_stop_pid" in ''|*[!0-9]*) return 1 ;; esac
  case "$vibepollo_stop_cgroup" in
    */../*|*/./*|*//*|*/..|*/.) return 1 ;;
    /*/vibepollo.service) ;;
    *) return 1 ;;
  esac
}

# Linux exposes a monotonic clock here; wall-clock corrections must not shorten
# the graceful shutdown interval or leave a retry waiting without a deadline.
vibepollo_host_stop_now() {
  IFS=' .' read -r vibepollo_stop_now vibepollo_stop_unused < /proc/uptime || return 1
  case "$vibepollo_stop_now" in ''|*[!0-9]*) return 1 ;; esac
}

vibepollo_stop_host_for_package() (
  vibepollo_unit_is_quiescent vibepollo.service && return 0
  vibepollo_read_host_stop_identity || return 1
  vibepollo_current_stop_properties=$vibepollo_stop_properties
  # Retain the original invocation across a timed-out transaction retry. A
  # replacement owner must not inherit permission to force the former owner.
  if [ -e "$vibepollo_package_transaction/host-stop.before" ] || [ -L "$vibepollo_package_transaction/host-stop.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-stop.before" || return 1
    vibepollo_saved_stop_properties=$(cat -- "$vibepollo_package_transaction/host-stop.before") || return 1
    vibepollo_read_host_stop_identity "$vibepollo_saved_stop_properties" || return 1
  else
    vibepollo_package_write_record host-stop.before "$vibepollo_current_stop_properties" || return 1
  fi
  vibepollo_original_invocation=$vibepollo_stop_invocation
  vibepollo_original_pid=$vibepollo_stop_pid
  vibepollo_original_cgroup=$vibepollo_stop_cgroup
  vibepollo_read_host_stop_identity "$vibepollo_current_stop_properties" || return 1
  [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] && \
    [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] && \
    { [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || [ "$vibepollo_stop_pid" = 0 ]; } || return 1
  vibepollo_host_stop_now || return 1
  vibepollo_host_stop_deadline=$((vibepollo_stop_now + 60))
  # TERM reaches the supervisor first so capture/encoding can drain while its
  # broker clients remain connected. Older hosts may lack an internal deadline.
  timeout --signal=TERM --kill-after=2 60 systemctl stop vibepollo.service \
    2>/dev/null || :
  while :; do
    vibepollo_unit_is_quiescent vibepollo.service && return 0
    vibepollo_read_host_stop_identity || return 1
    [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] && \
      [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] || return 1
    # MainPID can become zero while old subordinate clients remain in the
    # cgroup. A different nonzero owner is never an escalation target.
    [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || \
      [ "$vibepollo_stop_pid" = 0 ] || return 1
    vibepollo_host_stop_now || return 1
    [ "$vibepollo_stop_now" -lt "$vibepollo_host_stop_deadline" ] || break
    sleep 0.1
  done
  if [ "${VIBEPOLLO_PACKAGE_FORCE:-}" != 1 ]; then
    if command -v vibepollo_package_run_is_owned >/dev/null 2>&1 && vibepollo_package_run_is_owned; then
      vibepollo_package_write_record failure-class host-stop-deadline || return 1
    fi
    echo 'error: Vibepollo did not stop within 60 seconds; replacement is blocked. Retry with VIBEPOLLO_PACKAGE_FORCE=1 (or installer --force-stop) to authorize forced shutdown.' >&2
    return 1
  fi
  # Preserve private machine state and the recovery record before the first
  # irreversible forced signal. Missing/failed backup support blocks force.
  command -v vibepollo_package_backup_for_force >/dev/null 2>&1 && \
    vibepollo_package_backup_for_force || return 1
  # Recheck after the potentially slow backup; never inherit an old target.
  if vibepollo_unit_is_quiescent vibepollo.service; then
    vibepollo_package_log_force_outcome graceful-after-backup || return 1
    return 0
  fi
  if ! vibepollo_read_host_stop_identity; then
    vibepollo_package_log_force_outcome identity-changed || :
    return 1
  fi
  if ! { [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] &&
         [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] &&
         { [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || [ "$vibepollo_stop_pid" = 0 ]; }; }; then
    vibepollo_package_log_force_outcome identity-changed || :
    return 1
  fi
  command -v vibepollo_package_log_force_outcome >/dev/null 2>&1 && \
    vibepollo_package_log_force_outcome pending || return 1
  echo 'warning: forced shutdown authorized; terminating the fenced Vibepollo service cgroup after its graceful deadline.' >&2
  timeout --signal=KILL 5 systemctl kill --signal=KILL \
    vibepollo.service 2>/dev/null || {
      vibepollo_package_log_force_outcome kill-failed || :
      return 1
    }
  # Let the manager complete its existing stop job, then require the complete
  # stopped-state and empty-cgroup proof even when force was authorized.
  vibepollo_host_stop_now || return 1
  vibepollo_host_reap_deadline=$((vibepollo_stop_now + 15))
  timeout --signal=TERM --kill-after=2 15 systemctl stop vibepollo.service \
    2>/dev/null || :
  # The kill request can return before the kernel finishes releasing imports.
  # Poll only the existing target; never restart shutdown or adopt a new owner.
  while :; do
    if vibepollo_unit_is_quiescent vibepollo.service; then
      vibepollo_package_log_force_outcome complete || return 1
      echo 'Vibepollo forced shutdown completed; the old service cgroup is empty.' >&2
      return 0
    fi
    vibepollo_read_host_stop_identity || return 1
    if [ "$vibepollo_stop_invocation" != "$vibepollo_original_invocation" ] ||
       [ "$vibepollo_stop_cgroup" != "$vibepollo_original_cgroup" ]; then
      vibepollo_package_log_force_outcome identity-changed || :
      return 1
    fi
    vibepollo_host_stop_now || return 1
    [ "$vibepollo_stop_now" -lt "$vibepollo_host_reap_deadline" ] || break
    sleep 0.1
  done
  vibepollo_package_log_force_outcome still-populated || :
  echo 'error: forced shutdown did not empty the old Vibepollo service cgroup; replacement remains blocked.' >&2
  return 1
)

vibepollo_stop_exact_unit() {
  if [ "$1" = vibepollo.service ]; then
    vibepollo_stop_host_for_package
    return $?
  fi
  timeout --signal=TERM --kill-after=2 60 systemctl stop "$1" 2>/dev/null
}
vibepollo_bounded_broker_list() (
  vibepollo_broker_list=$(mktemp /run/vibepollo-broker-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_broker_list"' 0
  trap 'exit 1' HUP INT TERM
  chmod 0600 "$vibepollo_broker_list" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units --all --plain \
      --no-legend --no-pager --full 'vibepollo-session-exec@*.service' \
      >"$vibepollo_broker_list" 2>/dev/null
  ) || return 1
  vibepollo_broker_list_size=$(stat -c '%%s' -- "$vibepollo_broker_list") || return 1
  case "$vibepollo_broker_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_broker_list_size" -le 65536 ] || return 1
  cat -- "$vibepollo_broker_list"
)
vibepollo_stop_brokers() (
  vibepollo_broker_list=$(mktemp /run/vibepollo-broker-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_broker_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_broker_attempt=0
  vibepollo_broker_clean_passes=0
  while [ "$vibepollo_broker_attempt" -lt 20 ]; do
    vibepollo_broker_dirty=0
    vibepollo_capture_unit_list "$vibepollo_broker_list" \
      'vibepollo-session-exec@*.service' || return 1
    vibepollo_seen_units='
'
    vibepollo_seen_count=0
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *"
"*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1
      vibepollo_load=$2
      vibepollo_state=$3
      vibepollo_substate=$4
      vibepollo_broker_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      vibepollo_seen_count=$((vibepollo_seen_count + 1))
      [ "$vibepollo_seen_count" -le 256 ] || return 1
      if ! vibepollo_unit_is_quiescent "$vibepollo_unit"; then
        vibepollo_broker_dirty=1
        vibepollo_stop_exact_unit "$vibepollo_unit"
        vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
      fi
    done <"$vibepollo_broker_list"
    if [ "$vibepollo_broker_dirty" -eq 0 ]; then
      vibepollo_broker_clean_passes=$((vibepollo_broker_clean_passes + 1))
      [ "$vibepollo_broker_clean_passes" -lt 5 ] || return 0
    else
      vibepollo_broker_clean_passes=0
    fi
    vibepollo_broker_attempt=$((vibepollo_broker_attempt + 1))
    sleep 0.1
  done
  return 1
)
vibepollo_brokers_are_quiescent() {
  vibepollo_stop_brokers
}
vibepollo_quiesce_machine_host() {
  vibepollo_have_systemd=0
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_have_systemd=1
    vibepollo_package_preserved_preflight_is_safe && return 0

    # Stage (but do not load) the old-host stop fence, then freeze the exact old
    # controller cgroup before changing any dependency. This is the transaction
    # barrier which prevents its legacy reconcile path from issuing a
    # concurrent hard kill or connector release.
    vibepollo_shutdown_started=0
    vibepollo_pre_handoff_identity=''
    if [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ]; then
      vibepollo_pre_handoff_identity=$(stat -Lc '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff" 2>/dev/null || true)
    fi
    vq vibepollo_select_upgrade_kill_mode || return 1
    vq vibepollo_prepare_host_upgrade_fence || return 1
    vq vibepollo_freeze_controller || return 1
    if [ "${vibepollo_package_prior_phase:-early}" = early ]; then
      vq vibepollo_host_is_stable_or_quiescent || return 1
    fi
    vq vibepollo_activate_host_upgrade_fence || return 1
    vq vibepollo_controller_remains_frozen || return 1
    if [ "${vibepollo_package_prior_phase:-early}" = early ]; then
      vq vibepollo_host_is_stable_or_quiescent || return 1
    fi

    # An already-loaded legacy PAM handle can start a restore worker that
    # restarts the host. Runtime-mask that template before every other stop and
    # deliberately leave it masked after replacement.
    vq vibepollo_package_mask 'vibepollo-session-restore@.service' 2>/dev/null || return 1
    vq vibepollo_restore_template_is_masked || return 1
    vq vibepollo_disable_legacy_handoff || return 1

    vq vibepollo_package_mask vibeshine-vkms-control.socket 2>/dev/null || return 1
    vq vibepollo_control_socket_is_masked || return 1
    vibepollo_stop_exact_unit vibeshine-vkms-control.socket
    vq vibepollo_unit_is_quiescent vibeshine-vkms-control.socket || return 1
    # Masking the socket unit before stopping it drops its RemoveOnStop=
    # configuration, so the listening path survives the stop. Nothing can
    # accept on it any more; remove it so the instance check sees a clean slate.
    if [ -S "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ]; then
      vibepollo_quiesce_step='remove stale vkms control socket'
      rm -f -- "$vibepollo_control_socket" || return 1
    fi
    vq vibepollo_control_instances_are_quiescent || return 1

    # Closing the required broker socket may start the host stop. The verified
    # process-mode fence directs one TERM to the old wrapper and forbids the
    # service manager from escalating against its GPU-owning child.
    vq vibepollo_package_mark_late || return 1
    vibepollo_shutdown_started=1
    vq vibepollo_package_mask vibepollo-session-exec.socket 2>/dev/null || return 1
    vq vibepollo_broker_socket_is_masked || return 1
    vibepollo_stop_exact_unit vibepollo-session-exec.socket
    vq vibepollo_unit_is_quiescent vibepollo-session-exec.socket || return 1
    # Same RemoveOnStop= loss as the control socket above: a stale broker path
    # keeps the controller's admission closed until it is removed.
    if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
      vibepollo_quiesce_step='remove stale session broker socket'
      rm -f -- "$vibepollo_broker_socket" || return 1
    fi
    vibepollo_stop_exact_unit vibepollo.service
    vq vibepollo_unit_is_quiescent vibepollo.service || return 1
    vq vibepollo_package_mask vibepollo.service 2>/dev/null || return 1
    vq vibepollo_host_unit_is_masked || return 1

    vq vibepollo_stop_restore_instances || return 1

    # Broker admission is already closed. Every remaining worker name is
    # validated and stopped as one exact unit; no wildcard stop is trusted.
    vq vibepollo_stop_brokers || return 1
    vq vibepollo_controller_remains_frozen || return 1
    # systemd refuses StopUnit for a frozen service. Admission and the host are
    # already masked here, so thaw the controller only for its ordered stop.
    vq vibepollo_thaw_controller || return 1
    vibepollo_stop_exact_unit vibepollo-session-controller.service
    vq vibepollo_unit_is_quiescent vibepollo-session-controller.service || return 1
    vibepollo_stop_exact_unit vibepollo-prelogin.service
    vibepollo_stop_exact_unit vibepollo-machine-prepare.service
  fi

  if [ "$vibepollo_have_systemd" -eq 0 ]; then
    vibepollo_disable_legacy_handoff || return 1
  fi
  vibepollo_runtime_root_is_safe_or_absent || return 1

  if [ "$vibepollo_have_systemd" -eq 0 ] && \
     { [ -e "$vibepollo_controller" ] || [ -L "$vibepollo_controller" ]; }; then
    vibepollo_privileged_helper_is_safe "$vibepollo_controller" || return 1
    timeout --signal=KILL 40 "$vibepollo_controller" cleanup || return 1
  elif [ "$vibepollo_have_systemd" -eq 0 ]; then
    vibepollo_run_optional_legacy_command cleanup
    vibepollo_legacy_status=$?
    case "$vibepollo_legacy_status" in
      0 | 2) ;;
      *) return 1 ;;
    esac
  fi

  # Both the immediately prior helper and the current helper support this
  # migration. Exit 2 alone means an older helper did not implement the verb.
  vibepollo_run_optional_legacy_command remove-pam
  vibepollo_legacy_status=$?
  case "$vibepollo_legacy_status" in
    0 | 2) ;;
    *) return 1 ;;
  esac

  if [ "$vibepollo_have_systemd" -eq 1 ]; then
    # Repeat after cleanup to catch a worker accepted just before admission
    # closed, then prove every old/new cgroup is empty before replacement.
    vibepollo_restore_template_is_masked || return 1
    vibepollo_host_unit_is_masked || return 1
    vibepollo_broker_socket_is_masked || return 1
    vibepollo_stop_restore_instances || return 1
    vibepollo_stop_exact_unit vibepollo.service
    vibepollo_unit_is_quiescent vibepollo.service || return 1
    vibepollo_stop_exact_unit vibepollo-session-exec.socket
    vibepollo_stop_brokers || return 1
    for vibepollo_unit in \
      vibepollo-session-exec.socket \
      vibepollo-session-controller.service \
      vibepollo.service \
      vibepollo-prelogin.service \
      vibepollo-machine-prepare.service; do
      vibepollo_stop_exact_unit "$vibepollo_unit"
      vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
    done
    vibepollo_brokers_are_quiescent || return 1
    vibepollo_restore_instances_are_quiescent || return 1
  fi

  # Only after PAM admission and the directly executed legacy helper are both
  # closed may obsolete transition state be validated and removed.
  vibepollo_cleanup_legacy_transition_state || return 1

  # A stopped socket may leave only its inert filesystem node on older units.
  # Remove that exact socket after every owning cgroup has been proven empty.
  if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" || return 1
  fi
    [ ! -e "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ] && \
    [ ! -e "$vibepollo_legacy_handoff_directory" ] && \
    [ ! -L "$vibepollo_legacy_handoff_directory" ] && \
    [ ! -e "$vibepollo_legacy_restore_directory" ] && \
    [ ! -L "$vibepollo_legacy_restore_directory" ] && \
    [ ! -e "$vibepollo_legacy_transition_lock" ] && \
    [ ! -L "$vibepollo_legacy_transition_lock" ] && \
    [ ! -e "$vibepollo_legacy_prelogin_marker" ] && \
    [ ! -L "$vibepollo_legacy_prelogin_marker" ] || return 1
  [ ! -e /run/vibepollo/session.env ] && [ ! -L /run/vibepollo/session.env ] || return 1

}
if ! vibepollo_quiesce_or_abort; then
  echo "error: could not quiesce the machine host after package replacement; transaction fences are retained if shutdown could not be proved." >&2
  exit 1
fi

if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  timeout --signal=KILL 15 systemctl disable vibepollo.service vibepollo-prelogin.service vibepollo-machine-prepare.service 2>/dev/null || true
fi

for vibepollo_executable in \
  %{_bindir}/vibepollo \
  %{_prefix}/libexec/vibeshine/vibepollo-session-exec; do
  if [ ! -f "$vibepollo_executable" ] || [ -L "$vibepollo_executable" ]; then
    echo "error: native executable is missing or unsafe: $vibepollo_executable" >&2
    exit 1
  fi
  chown root:root "$vibepollo_executable" || exit 1
  chmod 0755 "$vibepollo_executable" || exit 1
  setcap -r "$vibepollo_executable" 2>/dev/null || true
  if [ -n "$(getcap "$vibepollo_executable" 2>/dev/null)" ]; then
    echo "error: unsafe capabilities remain on $vibepollo_executable." >&2
    exit 1
  fi
done

# Load uhid (DS5 emulation)
echo "Loading uhid kernel module for DS5 emulation."
modprobe uhid

# Check if we're in an rpm-ostree environment
if [ ! -x "$(command -v rpm-ostree)" ]; then
  echo "Not in an rpm-ostree environment, proceeding with post install steps."

  systemd-sysusers %{_prefix}/lib/sysusers.d/vibeshine-vkms.conf || \
    echo "warning: could not create the dedicated vibeshine-vkms control group."
  systemd-sysusers %{_prefix}/lib/sysusers.d/vibepollo.conf || {
    echo "error: could not create the dedicated Vibepollo service account." >&2
    exit 1
  }
  vibepollo_session_broker=%{_prefix}/libexec/vibeshine/vibepollo-session-broker
  if [ ! -f "$vibepollo_session_broker" ] || [ -L "$vibepollo_session_broker" ]; then
    echo "error: root-only session broker is missing or unsafe." >&2
    exit 1
  fi
  chown root:root "$vibepollo_session_broker" || exit 1
  chmod 0700 "$vibepollo_session_broker" || exit 1
  setcap cap_kill,cap_setgid,cap_setuid=p "$vibepollo_session_broker" || exit 1
  if [ "$(stat -c '%%U:%%G:%%a:%%F' -- "$vibepollo_session_broker")" != \
       'root:root:700:regular file' ] ||
     [ "$(getcap "$vibepollo_session_broker" 2>/dev/null)" != \
       "$vibepollo_session_broker cap_kill,cap_setgid,cap_setuid=p" ]; then
    echo "error: root-only session broker permissions or capabilities are unsafe." >&2
    exit 1
  fi

  vibepollo_private_host=%{_prefix}/libexec/vibeshine/vibepollo-host
  if [ ! -f "$vibepollo_private_host" ] || [ -L "$vibepollo_private_host" ]; then
    echo "error: private Vibepollo host is missing or unsafe." >&2
    exit 1
  fi
  vibepollo_public_identity=$(stat -Lc '%%d:%%i' -- %{_bindir}/vibepollo) || exit 1
  vibepollo_broker_identity=$(stat -Lc '%%d:%%i' -- "$vibepollo_session_broker") || exit 1
  vibepollo_private_identity=$(stat -Lc '%%d:%%i' -- "$vibepollo_private_host") || exit 1
  if [ "$vibepollo_public_identity" = "$vibepollo_private_identity" ] ||
     [ "$vibepollo_broker_identity" = "$vibepollo_private_identity" ]; then
    echo "error: privileged broker and private/public hosts must be distinct inodes." >&2
    exit 1
  fi
  chown root:vibepollo "$vibepollo_private_host" || exit 1
  chmod 0750 "$vibepollo_private_host" || exit 1
  setcap cap_sys_admin,cap_sys_nice=p "$vibepollo_private_host" || exit 1
  if [ "$(stat -c '%%U:%%G:%%a:%%F' -- "$vibepollo_private_host")" != \
       'root:vibepollo:750:regular file' ] ||
     [ "$(getcap "$vibepollo_private_host" 2>/dev/null)" != \
       "$vibepollo_private_host cap_sys_admin,cap_sys_nice=p" ]; then
    echo "error: private Vibepollo host permissions or capabilities are unsafe." >&2
    exit 1
  fi
  if [ "$(getcap "$vibepollo_session_broker" 2>/dev/null)" != \
       "$vibepollo_session_broker cap_kill,cap_setgid,cap_setuid=p" ]; then
    echo "error: private-host setup altered the session broker capabilities." >&2
    exit 1
  fi
  for vibepollo_executable in \
    %{_bindir}/vibepollo \
    %{_prefix}/libexec/vibeshine/vibepollo-session-exec; do
    if [ -n "$(getcap "$vibepollo_executable" 2>/dev/null)" ]; then
      echo "error: a public Vibepollo entrypoint gained file capabilities: $vibepollo_executable" >&2
      exit 1
    fi
  done

  # Trigger udev rule reload for /dev/uinput and /dev/uhid
  path_to_udevadm=$(command -v udevadm 2>/dev/null || true)
  if [ -x "$path_to_udevadm" ]; then
    echo "Reloading udev rules."
    $path_to_udevadm control --reload-rules
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uinput
    $path_to_udevadm trigger --property-match=DEVNAME=/dev/uhid
    echo "Udev rules reloaded successfully."
  else
    echo "error: udevadm not found or not executable."
  fi

  vibepollo_restore_kwin_capability() {
    # Earlier Vibepollo builds removed cap_sys_nice from the distro KWin binary
    # so the GPU bridge could be preloaded. The bridge is now a trusted
    # set-user-ID library, so give KWin its realtime capability back.
    kwin=/usr/bin/kwin_wayland
    marker=user.vibeshine.cap_sys_nice_removed
    timeout --signal=KILL 15 systemctl disable vibepollo-kwin-capability.path vibeshine-kwin-capability.path --now 2>/dev/null || true
    rm -f /etc/systemd/system/multi-user.target.wants/vibepollo-kwin-capability.path /etc/systemd/system/multi-user.target.wants/vibeshine-kwin-capability.path
    [ -f "$kwin" ] && [ ! -L "$kwin" ] || return 0
    if command -v getfattr >/dev/null 2>&1; then
      getfattr -n "$marker" --only-values "$kwin" >/dev/null 2>&1 || return 0
    elif command -v python3 >/dev/null 2>&1; then
      python3 -c 'import os, sys; os.getxattr(sys.argv[1], sys.argv[2])' "$kwin" "$marker" 2>/dev/null || return 0
    else
      return 0
    fi
    if setcap cap_sys_nice=ep "$kwin"; then
      setfattr -x "$marker" "$kwin" 2>/dev/null || \
        python3 -c 'import os, sys; os.removexattr(sys.argv[1], sys.argv[2])' "$kwin" "$marker" 2>/dev/null || true
      echo "restored cap_sys_nice on $kwin (removed by an earlier Vibepollo build)"
    else
      echo "warning: could not restore cap_sys_nice on $kwin; reinstall the kwin package." >&2
    fi
  }
  vibepollo_restore_kwin_capability || true

  if %{_prefix}/libexec/vibeshine/vibeshine-drm-install install; then
    :
  else
    vibeshine_drm_rc=$?
    if [ "$vibeshine_drm_rc" -eq 4 ]; then
      echo "warning: Vibepollo DRM was updated, but the loaded module is stale; reboot before using managed virtual displays."
    else
      echo "warning: Vibepollo DRM installation failed; managed virtual displays are unavailable."
    fi
  fi
  if ! %{_prefix}/libexec/vibeshine/vibeshine-ds5-install install; then
    echo "warning: DualSense USB haptics are unavailable or require a reboot; see the module error above." >&2
  fi
  vibepollo_machine_helper=%{_prefix}/libexec/vibeshine/vibepollo-machine-host
  vibepollo_privileged_helper_is_safe "$vibepollo_machine_helper" || {
    echo "error: installed Vibepollo machine helper is unsafe." >&2
    exit 1
  }
  "$vibepollo_machine_helper" remove-pam || \
    echo "warning: could not remove the obsolete Plasma Login Manager handoff hook."
  systemctl disable --now vibepollo-prelogin.service 2>/dev/null || true
  systemctl daemon-reload || exit 1
  if "$vibepollo_machine_helper" configure-auto; then
    vibepollo_package_prepare_postinstall_start || exit 1
    if [ "$vibepollo_package_start_deferred" -eq 0 ]; then
      if ! systemctl enable vibepollo-session-controller.service; then
        if vibepollo_quiesce_or_abort; then
          vibepollo_package_recover_failed_transaction ||
            echo 'error: failed-start recovery could not prove a safe release; owned fences were retained for retry.' >&2
        fi
        echo "error: could not enable the Vibepollo controller safely; transaction recovery was attempted; inspect the preceding shutdown result." >&2
        exit 1
      fi
      if ! vibepollo_unmask_host_for_controller || \
         ! systemctl start vibepollo-session-controller.service; then
          if vibepollo_quiesce_or_abort; then
            vibepollo_package_recover_failed_transaction ||
              echo 'error: failed-start recovery could not prove a safe release; owned fences were retained for retry.' >&2
          fi
        echo "error: could not start the Vibepollo controller safely; transaction recovery was attempted; inspect the preceding shutdown result." >&2
        exit 1
      fi
    fi
  else
    if ! vibepollo_package_complete_quiescence_is_safe ||
       ! vibepollo_package_release; then
      echo 'error: machine configuration failed and safe release could not be proved; retry package configuration to finish owned shutdown recovery.' >&2
      exit 1
    fi
    echo "==> ACTION REQUIRED: Vibepollo could not prepare the machine profile; review the preceding setup or migration error." >&2
    echo "    Run:  sudo vibepollo configure USER" >&2
    echo "    then: sudo systemctl enable --now vibepollo-session-controller.service" >&2
  fi
else
  echo "rpm-ostree environment detected, skipping post install steps. Restart to apply the changes."
fi

%preun
export LC_ALL=C

vibepollo_controller="%{_prefix}/libexec/vibeshine/vibepollo-session-controller"
vibepollo_machine_host="%{_prefix}/libexec/vibeshine/vibepollo-machine-host"
vibepollo_session_record=/run/vibepollo/session.env
vibepollo_legacy_acl=/run/vibepollo/runtime-acl
vibepollo_broker_socket=/run/vibepollo/session-broker.sock
vibepollo_legacy_host=$vibepollo_machine_host
vibepollo_legacy_handoff="%{_prefix}/libexec/vibeshine/vibepollo-session-handoff"
vibepollo_host_upgrade_dropin_dir=/run/systemd/system/vibepollo.service.d
vibepollo_host_upgrade_dropin=$vibepollo_host_upgrade_dropin_dir/90-vibepollo-safe-upgrade.conf
vibepollo_controller_was_frozen=0
vibepollo_control_socket=/run/vibeshine/vkms-control.sock
vibepollo_upgrade_kill_mode=
vibepollo_legacy_handoff_directory=/run/vibepollo/session-handoffs
vibepollo_legacy_restore_directory=/run/vibepollo/session-restores
vibepollo_legacy_transition_lock=/run/vibepollo/session-handoff.lock
vibepollo_legacy_prelogin_marker=/run/vibepollo/prelogin-handoff-complete
vibepollo_carriage_return=$(printf '\r')

vibepollo_preun_privileged_helper_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] && [ -x "$1" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = \
    '0:0:755:1:regular file' ]
}

vibepollo_preun_cgroup_is_quiescent() {
  vibepollo_control_group=$1
  [ -n "$vibepollo_control_group" ] || return 0
  case "$vibepollo_control_group" in /*) ;; *) return 1 ;; esac
  case "$vibepollo_control_group" in */../* | */..) return 1 ;; esac
  vibepollo_cgroup_path=/sys/fs/cgroup$vibepollo_control_group
  if [ ! -e "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ]; then return 0; fi
  [ -d "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ] && \
    [ -f "$vibepollo_cgroup_path/cgroup.events" ] && \
    [ ! -L "$vibepollo_cgroup_path/cgroup.events" ] && \
    grep -qx 'populated 0' "$vibepollo_cgroup_path/cgroup.events"
}

vibepollo_preun_unit_is_quiescent() {
  vibepollo_properties=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup 2>/dev/null) || return 1
  vibepollo_property_lines=$(printf '%%s\n' "$vibepollo_properties" | wc -l | tr -d ' ')
  case "$1:$vibepollo_property_lines" in
    *.socket:4 | *.socket:5 | *:5) ;;
    *) return 1 ;;
  esac
  for vibepollo_property in LoadState ActiveState SubState ControlGroup; do
    vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | \
      grep -c "^$vibepollo_property=" || true)
    [ "$vibepollo_property_count" = 1 ] || return 1
  done
  vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | grep -c '^MainPID=' || true)
  case "$1:$vibepollo_property_count" in
    *.socket:0 | *.socket:1 | *:1) ;;
    *) return 1 ;;
  esac
  vibepollo_load=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^LoadState=//p')
  vibepollo_state=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ActiveState=//p')
  vibepollo_substate=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^SubState=//p')
  vibepollo_pid=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^MainPID=//p')
  case "$1" in *.socket) [ -n "$vibepollo_pid" ] || vibepollo_pid=0 ;; esac
  vibepollo_control_group=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ControlGroup=//p')
  case "$vibepollo_load:$vibepollo_state:$vibepollo_substate" in
    not-found:inactive:dead | \
    loaded:inactive:dead | loaded:failed:failed | \
    masked:inactive:dead | masked:failed:failed | \
    masked-runtime:inactive:dead | masked-runtime:failed:failed) ;;
    *) return 1 ;;
  esac
  [ "$vibepollo_pid" = 0 ] && vibepollo_preun_cgroup_is_quiescent "$vibepollo_control_group"
}

vibepollo_preun_unit_is_disabled() {
  vibepollo_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  case "$vibepollo_enabled" in
    disabled | masked | masked-runtime | static | indirect | generated | transient | linked | linked-runtime | not-found) return 0 ;;
    *) return 1 ;;
  esac
}

vibepollo_preun_unit_is_masked() {
  vibepollo_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  vibepollo_load=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_enabled" in
    masked | masked-runtime) [ "$vibepollo_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}

vibepollo_preun_broker_unit_is_safe() {
  vibepollo_broker_unit=$1
  case "$vibepollo_broker_unit" in vibepollo-session-exec@*.service) ;; *) return 1 ;; esac
  vibepollo_broker_instance=${vibepollo_broker_unit#vibepollo-session-exec@}
  vibepollo_broker_instance=${vibepollo_broker_instance%%.service}
  [ -n "$vibepollo_broker_instance" ] || return 1
  case "$vibepollo_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}

vibepollo_package_capture_host_executables() {
  if [ ! -e /usr/bin/vibepollo ] && [ ! -L /usr/bin/vibepollo ] &&
     [ ! -e /usr/libexec/vibeshine/vibepollo-host ] && [ ! -L /usr/libexec/vibeshine/vibepollo-host ]; then
    printf '[]\n'
    return 0
  fi
  [ -x /usr/bin/python3 ] || {
    echo 'error: trusted Python pid/process inspection is required before replacing an installed host.' >&2
    return 1
  }
  timeout --signal=KILL 10 /usr/bin/python3 -I - <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
records = []
try:
    for name in paths:
        path = pathlib.Path(name)
        try:
            entry = path.lstat()
        except FileNotFoundError:
            continue
        if entry.st_uid != 0 or (stat.S_ISLNK(entry.st_mode) and name != paths[0]):
            raise ValueError("unsafe installed host executable")
        resolved = path.resolve(strict=True)
        identity = resolved.stat()
        if identity.st_uid != 0 or not stat.S_ISREG(identity.st_mode) or identity.st_mode & 0o022:
            raise ValueError("unsafe installed host executable")
        # A public package entrypoint may be a root-owned symlink; both its
        # original and resolved directory ancestry must remain root-owned.
        for parent in set((path.parent, *path.parent.parents, resolved.parent, *resolved.parent.parents)):
            owner = parent.stat()
            if owner.st_uid != 0 or not stat.S_ISDIR(owner.st_mode) or owner.st_mode & 0o022:
                raise ValueError("unsafe installed host executable ancestry")
        records.append({"path": name, "device": identity.st_dev, "inode": identity.st_ino})
    print(json.dumps(records, separators=(",", ":")))
except (OSError, ValueError) as error:
    print(f"error: installed host identity could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_package_unmanaged_host_absent() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before" || return 1
  vibepollo_package_executable_records=$(cat -- "$vibepollo_package_transaction/host-executables.before") || return 1
  if [ ! -x /usr/bin/python3 ]; then
    [ "$vibepollo_package_executable_records" = '[]' ] || {
      echo 'error: trusted Python process inspection is required for the retained old host inode snapshot.' >&2
      return 1
    }
    # First-install pre-hooks can run before Python dependencies are installed.
    # An empty capture still requires a bounded exact-path scan, including old
    # root-owned deleted executables; it is never treated as an absence proof.
    timeout --signal=KILL 10 /bin/sh -s <<'HOST_PROOF'
command -v readlink >/dev/null 2>&1 && command -v stat >/dev/null 2>&1 || exit 1
for executable in /proc/[0-9]*/exe; do
  target=$(readlink -v -- "$executable" 2>&1)
  status=$?
  if [ "$status" -ne 0 ]; then
    case "$target" in
      *': No such file or directory') continue ;;
      *) echo 'error: executable scope inspection is uncertain; replacement is blocked.' >&2; exit 1 ;;
    esac
  fi
  case "$target" in
    /usr/bin/vibepollo | '/usr/bin/vibepollo (deleted)' | \
    /usr/libexec/vibeshine/vibepollo-host | '/usr/libexec/vibeshine/vibepollo-host (deleted)')
      identity=$(stat -Lc '%%u:%%F' -- "$executable" 2>/dev/null) || exit 1
      if [ "$identity" = '0:regular file' ]; then
        process_id=${executable%/exe}
        process_id=${process_id##*/}
        echo "error: verified old host executable remains in PID $process_id; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership." >&2
        exit 1
      fi
      ;;
  esac
done
HOST_PROOF
    return $?
  fi
  timeout --signal=KILL 10 /usr/bin/python3 -I - "$vibepollo_package_executable_records" <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
try:
    records = json.loads(sys.argv[1])
    if not isinstance(records, list) or len(records) > 2:
        raise ValueError("invalid executable identity snapshot")
    identities = set()
    names = set()
    for record in records:
        if not isinstance(record, dict) or set(record) != {"path", "device", "inode"}:
            raise ValueError("invalid executable identity snapshot")
        if record["path"] not in paths or record["path"] in names:
            raise ValueError("invalid executable identity snapshot")
        if any(type(record[key]) is not int or record[key] <= 0 for key in ("device", "inode")):
            raise ValueError("invalid executable identity snapshot")
        names.add(record["path"])
        identities.add((record["device"], record["inode"]))
    # Linux retains executable inode identity after unlink/replacement. Scan
    # every scope, using no argv/name matching and issuing no process signals.
    for process in pathlib.Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            executable = process / "exe"
            identity = executable.stat()
            target = os.readlink(executable)
        except (FileNotFoundError, ProcessLookupError):
            continue
        def report_old_owner(deleted=False):
            uid, scope = "unknown", "<unavailable>"
            try:
                # Diagnostics must describe the same observed task and inode.
                # PID reuse or disappearing metadata still blocks replacement;
                # never attribute a different process's UID/scope to this owner.
                before = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                status = (process / "status").read_text()
                uid_lines = [line.split()[1:] for line in status.splitlines() if line.startswith("Uid:")]
                if len(uid_lines) != 1 or len(uid_lines[0]) != 4 or not all(value.isdecimal() for value in uid_lines[0]):
                    raise ValueError("uncertain process UID")
                cgroup = (process / "cgroup").read_text()
                if len(cgroup) > 8192:
                    raise ValueError("oversized process scope")
                after = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                confirmed = executable.stat()
                if before != after or (confirmed.st_dev, confirmed.st_ino) != (identity.st_dev, identity.st_ino):
                    raise ValueError("changed process identity")
                uid, scope = uid_lines[0][1], cgroup.rstrip("\n")
            except (OSError, ValueError, IndexError):
                pass
            kind = "deleted old" if deleted else "old"
            print(f"error: verified {kind} host executable remains in PID {process.name}, UID {uid}, scope {json.dumps(scope)}; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership.", file=sys.stderr)
        if (identity.st_dev, identity.st_ino) in identities:
            report_old_owner()
            sys.exit(1)
        # A failed older transaction may lack a retained inode record. A
        # kernel-reported deleted executable at either exact root-owned package
        # path still supplies positive evidence of an old owner, never absence.
        if target in tuple(name + " (deleted)" for name in paths) and identity.st_uid == 0 and stat.S_ISREG(identity.st_mode) and not identity.st_mode & 0o022:
            report_old_owner(deleted=True)
            sys.exit(1)
except (OSError, ValueError, TypeError) as error:
    print(f"error: cross-scope old host absence could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_package_backup_for_force() {
  # Forced termination can interrupt a configuration write. Keep a private
  # snapshot before signalling; never follow service-owned links or mounts.
  # Logs are excluded because an active stream can keep changing them.
  umask 077
  vibepollo_backup_base=/var/lib
  vibepollo_backup_root=$vibepollo_backup_base/vibepollo-package-backups
  [ -d "$vibepollo_backup_base" ] && [ ! -L "$vibepollo_backup_base" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_base")" = '0:0:755:directory' ] || return 1
  if [ ! -e "$vibepollo_backup_root" ] && [ ! -L "$vibepollo_backup_root" ]; then
    mkdir -m 0700 -- "$vibepollo_backup_root" || return 1
  fi
  [ -d "$vibepollo_backup_root" ] && [ ! -L "$vibepollo_backup_root" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_root")" = '0:0:700:directory' ] || return 1
  vibepollo_backup_directory=$(mktemp -d "$vibepollo_backup_root/forced-stop.XXXXXX") || return 1
  [ -d "$vibepollo_backup_directory" ] && [ ! -L "$vibepollo_backup_directory" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_directory")" = '0:0:700:directory' ] || return 1
  set --
  for vibepollo_backup_source in etc/vibepollo var/lib/vibepollo; do
    if [ -e "/$vibepollo_backup_source" ] || [ -L "/$vibepollo_backup_source" ]; then
      [ -d "/$vibepollo_backup_source" ] && [ ! -L "/$vibepollo_backup_source" ] || return 1
      set -- "$@" "$vibepollo_backup_source"
    fi
  done
  command -v tar >/dev/null 2>&1 || return 1
  if [ "$#" -gt 0 ]; then
    timeout --signal=TERM --kill-after=2 30 tar --one-file-system \
      --exclude=var/lib/vibepollo/logs -C / -cf "$vibepollo_backup_directory/machine-state.tar" -- "$@" \
      >"$vibepollo_backup_directory/backup.log" 2>&1 || {
        echo "error: machine-state backup failed; forced shutdown was not attempted. Diagnostics: $vibepollo_backup_directory/backup.log" >&2
        return 1
      }
  else
    timeout --signal=TERM --kill-after=2 30 tar -cf "$vibepollo_backup_directory/machine-state.tar" \
      --files-from=/dev/null >"$vibepollo_backup_directory/backup.log" 2>&1 || return 1
  fi
  chmod 0600 -- "$vibepollo_backup_directory/machine-state.tar" "$vibepollo_backup_directory/backup.log" || return 1
  {
    printf 'product=vibepollo\noperation=forced-package-stop\nsnapshot=machine-state.tar\n'
    printf 'invocation=%%s\ncgroup=%%s\n' "$vibepollo_original_invocation" "$vibepollo_original_cgroup"
    printf 'Warning: snapshot taken while the old host may still be running; logs excluded.\n'
    if command -v pacman >/dev/null 2>&1; then timeout --signal=KILL 5 pacman -Q vibepollo 2>/dev/null || :; fi
    if command -v dpkg-query >/dev/null 2>&1; then timeout --signal=KILL 5 dpkg-query -W vibepollo 2>/dev/null || :; fi
    if command -v rpm >/dev/null 2>&1; then timeout --signal=KILL 5 rpm -q vibepollo 2>/dev/null || :; fi
  } >"$vibepollo_backup_directory/recovery.log" || return 1
  chmod 0600 -- "$vibepollo_backup_directory/recovery.log" || return 1
  echo "warning: forced shutdown will terminate active streams and may leave normal cleanup incomplete. Private machine-state snapshot and recovery log: $vibepollo_backup_directory" >&2
}

vibepollo_package_log_force_outcome() {
  case "$1" in pending | complete | kill-failed | still-populated | identity-changed | graceful-after-backup) ;; *) return 1 ;; esac
  case "${vibepollo_backup_directory:-}" in /var/lib/vibepollo-package-backups/forced-stop.*) ;; *) return 1 ;; esac
  [ -d "$vibepollo_backup_directory" ] && [ ! -L "$vibepollo_backup_directory" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_backup_directory")" = '0:0:700:directory' ] || return 1
  [ -f "$vibepollo_backup_directory/recovery.log" ] && [ ! -L "$vibepollo_backup_directory/recovery.log" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_backup_directory/recovery.log")" = '0:0:600:1:regular file' ] || return 1
  printf 'outcome=%%s\n' "$1" >>"$vibepollo_backup_directory/recovery.log"
}



vibepollo_read_host_stop_identity() {
  if [ "$#" -eq 1 ]; then
    vibepollo_stop_properties=$1
  else
    vibepollo_stop_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
      --property=Id --property=InvocationID --property=MainPID --property=ControlGroup \
      --property=Restart --property=RefuseManualStart 2>/dev/null) || return 1
  fi
  vibepollo_stop_id='' vibepollo_stop_invocation='' vibepollo_stop_pid=''
  vibepollo_stop_cgroup='' vibepollo_stop_restart='' vibepollo_stop_fenced=''
  vibepollo_stop_fields=0
  while IFS= read -r vibepollo_stop_line; do
    vibepollo_stop_fields=$((vibepollo_stop_fields + 1))
    case "$vibepollo_stop_line" in
      Id=*) vibepollo_stop_id=${vibepollo_stop_line#*=} ;;
      InvocationID=*) vibepollo_stop_invocation=${vibepollo_stop_line#*=} ;;
      MainPID=*) vibepollo_stop_pid=${vibepollo_stop_line#*=} ;;
      ControlGroup=*) vibepollo_stop_cgroup=${vibepollo_stop_line#*=} ;;
      Restart=*) vibepollo_stop_restart=${vibepollo_stop_line#*=} ;;
      RefuseManualStart=*) vibepollo_stop_fenced=${vibepollo_stop_line#*=} ;;
      *) return 1 ;;
    esac
  done <<EOF
$vibepollo_stop_properties
EOF
  [ "$vibepollo_stop_fields" -eq 6 ] && [ "$vibepollo_stop_id" = vibepollo.service ] && \
    [ "$vibepollo_stop_restart" = no ] && [ "$vibepollo_stop_fenced" = yes ] || return 1
  [ "${#vibepollo_stop_invocation}" -eq 32 ] || return 1
  case "$vibepollo_stop_invocation" in *[!0-9a-f]*) return 1 ;; esac
  case "$vibepollo_stop_pid" in ''|*[!0-9]*) return 1 ;; esac
  case "$vibepollo_stop_cgroup" in
    */../*|*/./*|*//*|*/..|*/.) return 1 ;;
    /*/vibepollo.service) ;;
    *) return 1 ;;
  esac
}

# Linux exposes a monotonic clock here; wall-clock corrections must not shorten
# the graceful shutdown interval or leave a retry waiting without a deadline.
vibepollo_host_stop_now() {
  IFS=' .' read -r vibepollo_stop_now vibepollo_stop_unused < /proc/uptime || return 1
  case "$vibepollo_stop_now" in ''|*[!0-9]*) return 1 ;; esac
}

vibepollo_stop_host_for_package() (
  vibepollo_preun_unit_is_quiescent vibepollo.service && return 0
  vibepollo_read_host_stop_identity || return 1
  vibepollo_current_stop_properties=$vibepollo_stop_properties
  # Retain the original invocation across a timed-out transaction retry. A
  # replacement owner must not inherit permission to force the former owner.
  if [ -e "$vibepollo_package_transaction/host-stop.before" ] || [ -L "$vibepollo_package_transaction/host-stop.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-stop.before" || return 1
    vibepollo_saved_stop_properties=$(cat -- "$vibepollo_package_transaction/host-stop.before") || return 1
    vibepollo_read_host_stop_identity "$vibepollo_saved_stop_properties" || return 1
  else
    vibepollo_package_write_record host-stop.before "$vibepollo_current_stop_properties" || return 1
  fi
  vibepollo_original_invocation=$vibepollo_stop_invocation
  vibepollo_original_pid=$vibepollo_stop_pid
  vibepollo_original_cgroup=$vibepollo_stop_cgroup
  vibepollo_read_host_stop_identity "$vibepollo_current_stop_properties" || return 1
  [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] && \
    [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] && \
    { [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || [ "$vibepollo_stop_pid" = 0 ]; } || return 1
  vibepollo_host_stop_now || return 1
  vibepollo_host_stop_deadline=$((vibepollo_stop_now + 60))
  # TERM reaches the supervisor first so capture/encoding can drain while its
  # broker clients remain connected. Older hosts may lack an internal deadline.
  timeout --signal=TERM --kill-after=2 60 systemctl stop vibepollo.service \
    2>/dev/null || :
  while :; do
    vibepollo_preun_unit_is_quiescent vibepollo.service && return 0
    vibepollo_read_host_stop_identity || return 1
    [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] && \
      [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] || return 1
    # MainPID can become zero while old subordinate clients remain in the
    # cgroup. A different nonzero owner is never an escalation target.
    [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || \
      [ "$vibepollo_stop_pid" = 0 ] || return 1
    vibepollo_host_stop_now || return 1
    [ "$vibepollo_stop_now" -lt "$vibepollo_host_stop_deadline" ] || break
    sleep 0.1
  done
  if [ "${VIBEPOLLO_PACKAGE_FORCE:-}" != 1 ]; then
    if command -v vibepollo_package_run_is_owned >/dev/null 2>&1 && vibepollo_package_run_is_owned; then
      vibepollo_package_write_record failure-class host-stop-deadline || return 1
    fi
    echo 'error: Vibepollo did not stop within 60 seconds; replacement is blocked. Retry with VIBEPOLLO_PACKAGE_FORCE=1 (or installer --force-stop) to authorize forced shutdown.' >&2
    return 1
  fi
  # Preserve private machine state and the recovery record before the first
  # irreversible forced signal. Missing/failed backup support blocks force.
  command -v vibepollo_package_backup_for_force >/dev/null 2>&1 && \
    vibepollo_package_backup_for_force || return 1
  # Recheck after the potentially slow backup; never inherit an old target.
  if vibepollo_preun_unit_is_quiescent vibepollo.service; then
    vibepollo_package_log_force_outcome graceful-after-backup || return 1
    return 0
  fi
  if ! vibepollo_read_host_stop_identity; then
    vibepollo_package_log_force_outcome identity-changed || :
    return 1
  fi
  if ! { [ "$vibepollo_stop_invocation" = "$vibepollo_original_invocation" ] &&
         [ "$vibepollo_stop_cgroup" = "$vibepollo_original_cgroup" ] &&
         { [ "$vibepollo_stop_pid" = "$vibepollo_original_pid" ] || [ "$vibepollo_stop_pid" = 0 ]; }; }; then
    vibepollo_package_log_force_outcome identity-changed || :
    return 1
  fi
  command -v vibepollo_package_log_force_outcome >/dev/null 2>&1 && \
    vibepollo_package_log_force_outcome pending || return 1
  echo 'warning: forced shutdown authorized; terminating the fenced Vibepollo service cgroup after its graceful deadline.' >&2
  timeout --signal=KILL 5 systemctl kill --signal=KILL \
    vibepollo.service 2>/dev/null || {
      vibepollo_package_log_force_outcome kill-failed || :
      return 1
    }
  # Let the manager complete its existing stop job, then require the complete
  # stopped-state and empty-cgroup proof even when force was authorized.
  vibepollo_host_stop_now || return 1
  vibepollo_host_reap_deadline=$((vibepollo_stop_now + 15))
  timeout --signal=TERM --kill-after=2 15 systemctl stop vibepollo.service \
    2>/dev/null || :
  # The kill request can return before the kernel finishes releasing imports.
  # Poll only the existing target; never restart shutdown or adopt a new owner.
  while :; do
    if vibepollo_preun_unit_is_quiescent vibepollo.service; then
      vibepollo_package_log_force_outcome complete || return 1
      echo 'Vibepollo forced shutdown completed; the old service cgroup is empty.' >&2
      return 0
    fi
    vibepollo_read_host_stop_identity || return 1
    if [ "$vibepollo_stop_invocation" != "$vibepollo_original_invocation" ] ||
       [ "$vibepollo_stop_cgroup" != "$vibepollo_original_cgroup" ]; then
      vibepollo_package_log_force_outcome identity-changed || :
      return 1
    fi
    vibepollo_host_stop_now || return 1
    [ "$vibepollo_stop_now" -lt "$vibepollo_host_reap_deadline" ] || break
    sleep 0.1
  done
  vibepollo_package_log_force_outcome still-populated || :
  echo 'error: forced shutdown did not empty the old Vibepollo service cgroup; replacement remains blocked.' >&2
  return 1
)

vibepollo_preun_stop_exact_unit() {
  if [ "$1" = vibepollo.service ]; then
    vibepollo_stop_host_for_package
    return $?
  fi
  timeout --signal=TERM --kill-after=2 60 systemctl stop "$1" 2>/dev/null
}

vibepollo_preun_select_upgrade_kill_mode() {
  # A stopped, empty host has no signal contract to preserve. In particular,
  # disabled installations must not depend on obsolete helper source text.
  if vibepollo_preun_unit_is_quiescent vibepollo.service; then
    vibepollo_upgrade_kill_mode=control-group
    return 0
  fi
  vibepollo_preun_privileged_helper_is_safe "$vibepollo_legacy_host" || return 1
  if grep -Fqx '  trap request_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=mixed
  elif grep -Fqx '  trap mark_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=control-group
  elif grep -Fqx "  trap 'forward_host_signal TERM' TERM" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal INT' INT" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal HUP' HUP" "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=process
  else
    # Unknown trusted supervisors get TERM first, without cancelling their
    # clients early. Bounded exact-unit shutdown still must prove an empty
    # cgroup before the package manager can replace any files.
    vibepollo_upgrade_kill_mode=mixed
  fi
}

vibepollo_prepare_host_upgrade_fence() (
  vibepollo_package_preserve_unowned_host_mask || exit 1
  [ "$vibepollo_package_preserve_host_mask" -eq 0 ] || exit 0
  [ ! -L "$vibepollo_host_upgrade_dropin_dir" ] && \
    [ ! -L "$vibepollo_host_upgrade_dropin" ] || exit 1
  install -d -o root -g root -m 0755 -- "$vibepollo_host_upgrade_dropin_dir" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_host_upgrade_dropin_dir")" = \
    '0:0:755:directory' ] || exit 1
  vibepollo_upgrade_temporary=$(mktemp /run/vibepollo-host-upgrade.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_upgrade_temporary"' 0
  case "$vibepollo_upgrade_kill_mode" in mixed | process | control-group) ;; *) exit 1 ;; esac
  printf '[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=%%s\nSendSIGKILL=no\nRestart=no\n' \
    "$vibepollo_upgrade_kill_mode" >"$vibepollo_upgrade_temporary" || exit 1
  chmod 0600 -- "$vibepollo_upgrade_temporary" || exit 1
  if [ -e "$vibepollo_host_upgrade_dropin" ]; then
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || exit 1
    if cmp -s -- "$vibepollo_upgrade_temporary" "$vibepollo_host_upgrade_dropin"; then
      vibepollo_package_record_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
      exit 0
    fi
    # A retry may select a new stop mode after the old host finishes. Only an
    # unchanged owned fence can be replaced, and only with an empty old host.
    vibepollo_preun_unit_is_quiescent vibepollo.service && vibepollo_package_no_jobs || exit 1
    vibepollo_package_remove_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
    [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || exit 1
    rm -f -- "$vibepollo_package_transaction/fence.owned" || exit 1
  fi
  install -o root -g root -m 0644 -- "$vibepollo_upgrade_temporary" \
    "$vibepollo_host_upgrade_dropin" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
    '0:0:644:1:regular file' ] || exit 1
  vibepollo_package_record_owned fence "$vibepollo_host_upgrade_dropin" || exit 1
)

vibepollo_activate_host_upgrade_fence() {
  vibepollo_package_preserve_unowned_host_mask || return 1
  [ "$vibepollo_package_preserve_host_mask" -eq 0 ] || return 0
  vibepollo_package_open_owned_host_for_stop || exit 1
  systemctl daemon-reload || exit 1
  vibepollo_host_stop_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=LoadState --property=RefuseManualStart --property=KillMode --property=SendSIGKILL --property=Restart 2>/dev/null) || exit 1
  if printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'LoadState=not-found'; then
    vibepollo_preun_unit_is_quiescent vibepollo.service || exit 1
    return 0
  fi
  vibepollo_package_properties_are_unique "$vibepollo_host_stop_properties" LoadState RefuseManualStart KillMode SendSIGKILL Restart || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'RefuseManualStart=yes' || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | \
    grep -qx "KillMode=$vibepollo_upgrade_kill_mode" || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'SendSIGKILL=no' || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'Restart=no' || exit 1
}

vibepollo_host_is_stable_or_quiescent() {
  vibepollo_host_state=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup --property=Job 2>/dev/null) || return 1
  vibepollo_host_active=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^ActiveState=//p')
  vibepollo_host_substate=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^SubState=//p')
  vibepollo_host_pid=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^MainPID=//p')
  vibepollo_host_cgroup=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^ControlGroup=//p')
  vibepollo_host_job=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^Job=//p')
  [ -z "$vibepollo_host_job" ] || return 1
  if [ "$vibepollo_host_active" = active ] && [ "$vibepollo_host_substate" = running ]; then
    case "$vibepollo_host_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
    case "$vibepollo_host_cgroup" in /*) return 0 ;; *) return 1 ;; esac
  fi
  case "$vibepollo_host_active:$vibepollo_host_substate:$vibepollo_host_pid" in
    inactive:dead:0 | failed:failed:0) vibepollo_preun_cgroup_is_quiescent "$vibepollo_host_cgroup" ;;
    *) return 1 ;;
  esac
}

vibepollo_freeze_controller() {
  if vibepollo_preun_unit_is_quiescent vibepollo-session-controller.service; then
    vibepollo_controller_was_frozen=0
    return 0
  fi
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  vibepollo_controller_active=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ActiveState=//p')
  vibepollo_controller_substate=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^SubState=//p')
  vibepollo_controller_pid=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^MainPID=//p')
  vibepollo_controller_cgroup=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ControlGroup=//p')
  vibepollo_controller_invocation=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^InvocationID=//p')
  [ "${#vibepollo_controller_invocation}" -eq 32 ] || return 1
  case "$vibepollo_controller_invocation" in *[!0-9a-f]*) return 1 ;; esac
  [ "$vibepollo_controller_active" = active ] && \
    [ "$vibepollo_controller_substate" = running ] || return 1
  case "$vibepollo_controller_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in /*) ;; *) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in */../* | */..) return 1 ;; esac
  # A timed-out freeze may have succeeded; the abort path must attempt thaw.
  vibepollo_controller_was_frozen=1
  timeout --signal=TERM --kill-after=2 15 systemctl freeze \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation" || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=frozen' || return 1
  vibepollo_controller_freeze_path=/sys/fs/cgroup$vibepollo_controller_cgroup/cgroup.freeze
  vibepollo_controller_events_path=/sys/fs/cgroup$vibepollo_controller_cgroup/cgroup.events
  [ -f "$vibepollo_controller_freeze_path" ] && [ ! -L "$vibepollo_controller_freeze_path" ] && \
    grep -qx '1' "$vibepollo_controller_freeze_path" && \
    [ -f "$vibepollo_controller_events_path" ] && [ ! -L "$vibepollo_controller_events_path" ] && \
    grep -qx 'populated 1' "$vibepollo_controller_events_path" && \
    grep -qx 'frozen 1' "$vibepollo_controller_events_path" || return 1
}

vibepollo_controller_remains_frozen() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=frozen' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation"
}

vibepollo_thaw_controller() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  vibepollo_controller_remains_frozen || return 1
  timeout --signal=KILL 15 systemctl thaw \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_was_frozen=0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
  vibepollo_package_properties_are_unique "$vibepollo_controller_state" ActiveState SubState MainPID ControlGroup FreezerState InvocationID || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "InvocationID=$vibepollo_controller_invocation"
}

vibepollo_capture_unit_list() {
  vibepollo_unit_list_path=$1
  vibepollo_unit_list_pattern=$2
  [ -f "$vibepollo_unit_list_path" ] && [ ! -L "$vibepollo_unit_list_path" ] || return 1
  chmod 0600 "$vibepollo_unit_list_path" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units \
      --all --plain --no-legend --no-pager --full \
      "$vibepollo_unit_list_pattern" >"$vibepollo_unit_list_path" 2>/dev/null
  ) || return 1
  vibepollo_unit_list_size=$(stat -c '%%s' -- "$vibepollo_unit_list_path") || return 1
  case "$vibepollo_unit_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_unit_list_size" -le 65536 ]
}

vibepollo_control_unit_is_safe() {
  vibepollo_control_unit=$1
  case "$vibepollo_control_unit" in
    vibeshine-vkms-control@*.service) ;;
    *) return 1 ;;
  esac
  vibepollo_control_instance=${vibepollo_control_unit#vibeshine-vkms-control@}
  vibepollo_control_instance=${vibepollo_control_instance%%.service}
  [ -n "$vibepollo_control_instance" ] || return 1
  case "$vibepollo_control_instance" in
    *[!A-Za-z0-9_.:-]*) return 1 ;;
  esac
}

vibepollo_control_instances_are_quiescent() (
  vibepollo_control_list=$(mktemp /run/vibepollo-control-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_control_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_control_attempt=0
  vibepollo_control_clean_passes=0
  while [ "$vibepollo_control_attempt" -lt 100 ]; do
    vibepollo_control_dirty=0
    vibepollo_capture_unit_list "$vibepollo_control_list" \
      'vibeshine-vkms-control@*.service' || return 1
    vibepollo_seen_units='
'
    vibepollo_seen_count=0
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *"$vibepollo_carriage_return"*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1
      vibepollo_load=$2
      vibepollo_state=$3
      vibepollo_substate=$4
      vibepollo_control_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      vibepollo_seen_count=$((vibepollo_seen_count + 1))
      [ "$vibepollo_seen_count" -le 256 ] || return 1
      if ! vibepollo_preun_unit_is_quiescent "$vibepollo_unit"; then
        vibepollo_control_dirty=1
      fi
    done <"$vibepollo_control_list"
    if [ "$vibepollo_control_dirty" -eq 0 ]; then
      vibepollo_control_clean_passes=$((vibepollo_control_clean_passes + 1))
      [ "$vibepollo_control_clean_passes" -lt 5 ] || {
        [ ! -e "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ] || return 1
        return 0
      }
    else
      vibepollo_control_clean_passes=0
    fi
    vibepollo_control_attempt=$((vibepollo_control_attempt + 1))
    sleep 0.1
  done
  return 1
)

vibepollo_capture_broker_list() {
  vibepollo_broker_list_path=$1
  [ -f "$vibepollo_broker_list_path" ] && [ ! -L "$vibepollo_broker_list_path" ] || return 1
  chmod 0600 "$vibepollo_broker_list_path" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units --all --plain \
      --no-legend --no-pager --full 'vibepollo-session-exec@*.service' \
      >"$vibepollo_broker_list_path" 2>/dev/null
  ) || return 1
  vibepollo_broker_list_size=$(stat -c '%%s' -- "$vibepollo_broker_list_path") || return 1
  case "$vibepollo_broker_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_broker_list_size" -le 65536 ]
}

vibepollo_preun_stop_brokers() (
  vibepollo_broker_list=$(mktemp /run/vibepollo-broker-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_broker_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_broker_attempt=0
  vibepollo_broker_clean_passes=0
  while [ "$vibepollo_broker_attempt" -lt 20 ]; do
    vibepollo_broker_dirty=0
    vibepollo_capture_broker_list "$vibepollo_broker_list" || return 1
    vibepollo_seen_units='
'
    vibepollo_seen_count=0
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *"
"*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1
      vibepollo_load=$2
      vibepollo_state=$3
      vibepollo_substate=$4
      vibepollo_preun_broker_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      vibepollo_seen_count=$((vibepollo_seen_count + 1))
      [ "$vibepollo_seen_count" -le 256 ] || return 1
      if ! vibepollo_preun_unit_is_quiescent "$vibepollo_unit"; then
        vibepollo_broker_dirty=1
        vibepollo_preun_stop_exact_unit "$vibepollo_unit"
        vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
      fi
    done <"$vibepollo_broker_list"
    if [ "$vibepollo_broker_dirty" -eq 0 ]; then
      vibepollo_broker_clean_passes=$((vibepollo_broker_clean_passes + 1))
      [ "$vibepollo_broker_clean_passes" -lt 5 ] || return 0
    else
      vibepollo_broker_clean_passes=0
    fi
    vibepollo_broker_attempt=$((vibepollo_broker_attempt + 1))
    sleep 0.1
  done
  return 1
)

vibepollo_disable_legacy_handoff() {
  if [ ! -e "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ]; then
    return 0
  fi
  [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] || return 1
  vibepollo_handoff_attributes=$(stat -c '%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff") || return 1
  case "$vibepollo_handoff_attributes" in
    '0:0:755:1' | '0:0:0:1') ;;
    *) return 1 ;;
  esac
  vibepollo_legacy_handoff_identity=$(stat -Lc '%%d:%%i' -- "$vibepollo_legacy_handoff") || return 1
  chmod 000 -- "$vibepollo_legacy_handoff" || return 1
  vibepollo_handoff_attributes=$(stat -Lc '%%d:%%i:%%u:%%g:%%a:%%h' -- \
    "$vibepollo_legacy_handoff") || return 1
  [ "$vibepollo_handoff_attributes" = \
    "$vibepollo_legacy_handoff_identity:0:0:0:1" ] || return 1

  # A PAM module already loaded by the login manager can still fork this exact
  # script. Mode 000 closes new execs; consecutive exact argv scans drain any
  # interpreter that passed exec admission before the mode change.
  vibepollo_wait_for_legacy_handoff
}

vibepollo_wait_for_legacy_handoff() {
  vibepollo_handoff_attempt=0
  vibepollo_handoff_clean_passes=0
  while [ "$vibepollo_handoff_attempt" -lt 400 ]; do
    if vibepollo_legacy_handoff_process_is_running; then
      vibepollo_handoff_clean_passes=0
    else
      vibepollo_handoff_clean_passes=$((vibepollo_handoff_clean_passes + 1))
      [ "$vibepollo_handoff_clean_passes" -lt 10 ] || return 0
    fi
    vibepollo_handoff_attempt=$((vibepollo_handoff_attempt + 1))
    sleep 0.1
  done
  return 1
}

vibepollo_legacy_handoff_process_is_running() {
  for vibepollo_proc in /proc/[0-9]*; do
    [ -d "$vibepollo_proc" ] || continue
    if grep -Fzxq -- "$vibepollo_legacy_handoff" "$vibepollo_proc/cmdline" 2>/dev/null; then
      return 0
    fi
  done
  return 1
}

vibepollo_stop_restore_instances() (
  vibepollo_restore_list=$(mktemp /run/vibepollo-restore-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_restore_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_capture_unit_list "$vibepollo_restore_list" \
    'vibepollo-session-restore@*.service' || return 1
  vibepollo_seen_units='
'
  vibepollo_seen_count=0
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1
    vibepollo_load=$2
    vibepollo_state=$3
    vibepollo_substate=$4
    vibepollo_restore_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_seen_count=$((vibepollo_seen_count + 1))
    [ "$vibepollo_seen_count" -le 256 ] || return 1
    vibepollo_preun_stop_exact_unit "$vibepollo_unit"
    vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
  done <"$vibepollo_restore_list"
)

vibepollo_preun_remove_pam() {
  if [ ! -e "$vibepollo_machine_host" ] && [ ! -L "$vibepollo_machine_host" ]; then return 0; fi
  vibepollo_preun_privileged_helper_is_safe "$vibepollo_machine_host" || return 1
  timeout --signal=KILL 40 "$vibepollo_machine_host" remove-pam >/dev/null 2>&1
}

vibepollo_restore_unit_is_safe() {
  vibepollo_restore_unit=$1
  case "$vibepollo_restore_unit" in
    vibepollo-session-restore@*.service) ;;
    *) return 1 ;;
  esac
  vibepollo_restore_instance=${vibepollo_restore_unit#vibepollo-session-restore@}
  vibepollo_restore_instance=${vibepollo_restore_instance%%.service}
  case "$vibepollo_restore_instance" in
    [1-9]*) ;;
    *) return 1 ;;
  esac
  case "$vibepollo_restore_instance" in
    *[!0-9]*) return 1 ;;
  esac
}

vibepollo_restore_instances_are_quiescent() (
  vibepollo_restore_list=$(mktemp /run/vibepollo-restore-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_restore_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_capture_unit_list "$vibepollo_restore_list" \
    'vibepollo-session-restore@*.service' || return 1
  vibepollo_seen_units='
'
  vibepollo_seen_count=0
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1
    vibepollo_load=$2
    vibepollo_state=$3
    vibepollo_substate=$4
    vibepollo_restore_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_seen_count=$((vibepollo_seen_count + 1))
    [ "$vibepollo_seen_count" -le 256 ] || return 1
    vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
  done <"$vibepollo_restore_list"
)

vibepollo_brokers_are_quiescent() (
  vibepollo_broker_list=$(mktemp /run/vibepollo-restore-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_broker_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_capture_unit_list "$vibepollo_broker_list" \
    'vibepollo-session-exec@*.service' || return 1
  vibepollo_seen_units='
'
  vibepollo_seen_count=0
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1
    vibepollo_load=$2
    vibepollo_state=$3
    vibepollo_substate=$4
    vibepollo_preun_broker_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_seen_count=$((vibepollo_seen_count + 1))
    [ "$vibepollo_seen_count" -le 256 ] || return 1
    vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
  done <"$vibepollo_broker_list"
)

vibepollo_restore_template_is_masked() { vibepollo_preun_unit_is_masked 'vibepollo-session-restore@.service'; }
vibepollo_control_socket_is_masked() { vibepollo_preun_unit_is_masked vibeshine-vkms-control.socket; }
vibepollo_broker_socket_is_masked() { vibepollo_preun_unit_is_masked vibepollo-session-exec.socket; }
vibepollo_host_unit_is_masked() { vibepollo_preun_unit_is_masked vibepollo.service; }

# Package barriers are a transaction-owned resource, not administrator policy.
# Never source this record: fixed files contain only validated identities.
vibepollo_package_transaction=/run/vibepollo-package-transaction
vibepollo_package_lock_held=0
vibepollo_package_transaction_is_safe() {
  [ -d "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_package_transaction")" = '0:0:700:directory' ]
}

vibepollo_package_record_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:600:1:regular file' ] &&
    [ "$(stat -c '%%s' -- "$1")" -le 4096 ]
}

vibepollo_package_write_record() (
  vibepollo_package_transaction_is_safe || exit 1
  vibepollo_package_temporary=$(mktemp "$vibepollo_package_transaction/.record.XXXXXX") || exit 1
  trap 'rm -f -- "$vibepollo_package_temporary"' 0
  chmod 0600 -- "$vibepollo_package_temporary" || exit 1
  printf '%%s\n' "$2" >"$vibepollo_package_temporary" || exit 1
  mv -fT -- "$vibepollo_package_temporary" "$vibepollo_package_transaction/$1"
)

vibepollo_package_path_identity() {
  if [ ! -e "$1" ] && [ ! -L "$1" ]; then
    printf 'absent\n'
  elif [ -L "$1" ] && [ "$(readlink -- "$1")" = /dev/null ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  elif [ "$1" = "$vibepollo_host_upgrade_dropin" ] &&
       [ -f "$1" ] && [ ! -L "$1" ] &&
       [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:644:1:regular file' ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  else
    return 1
  fi
}

vibepollo_package_snapshot_path() {
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  case "$vibepollo_package_identity" in
    absent | *:0:0:'symbolic link' | *:0:0:'regular file') ;;
    *) return 1 ;;
  esac
  vibepollo_package_write_record "$1.before" "$vibepollo_package_identity"
}

vibepollo_package_cleanup_record_temporaries() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_has_temporary=0
  # First validate the complete set, so one malformed entry prevents every
  # deletion. Writers inherit fd 8; a fresh caller cannot hold the lock while
  # an interrupted operation's surviving writer is still publishing a record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    vibepollo_package_has_temporary=1
    vibepollo_package_suffix=${vibepollo_package_path##*/}
    case "$vibepollo_package_suffix" in
      .record.*) vibepollo_package_suffix=${vibepollo_package_suffix#.record.} ;;
      .mask.*) vibepollo_package_suffix=${vibepollo_package_suffix#.mask.} ;;
      *) return 1 ;;
    esac
    [ "${#vibepollo_package_suffix}" -eq 6 ] || return 1
    case "$vibepollo_package_suffix" in *[!A-Za-z0-9]*) return 1 ;; esac
    if [ -L "$vibepollo_package_path" ]; then
      case "${vibepollo_package_path##*/}" in .mask.*) ;; *) return 1 ;; esac
      [ "$(readlink -- "$vibepollo_package_path")" = /dev/null ] || return 1
      vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%h:%%F' -- "$vibepollo_package_path") || return 1
      case "$vibepollo_package_temporary_attributes" in
        '0:0:1:symbolic link' | '0:0:2:symbolic link') continue ;;
        *) return 1 ;;
      esac
    fi
    [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
    vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path") || return 1
    case "$vibepollo_package_temporary_attributes" in
      '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;;
      *) return 1 ;;
    esac
  done
  [ "$vibepollo_package_has_temporary" -eq 1 ] || return 0
  [ "${vibepollo_package_lock_held:-0}" -eq 1 ] || return 1
  flock --nonblock 8 || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
}

vibepollo_package_snapshot_host_executables() {
  if [ -e "$vibepollo_package_transaction/host-executables.before" ] || [ -L "$vibepollo_package_transaction/host-executables.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before"
    return $?
  fi
  vibepollo_package_executables=$(vibepollo_package_capture_host_executables) || return 1
  vibepollo_package_write_record host-executables.before "$vibepollo_package_executables"
}

vibepollo_package_lock() {
  [ "${vibepollo_package_lock_held:-0}" -eq 0 ] || return 0
  vibepollo_package_transaction_is_safe || return 1
  if [ -e "$vibepollo_package_transaction/lock" ] || [ -L "$vibepollo_package_transaction/lock" ]; then
    [ -f "$vibepollo_package_transaction/lock" ] && [ ! -L "$vibepollo_package_transaction/lock" ] || return 1
    vibepollo_package_lock_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_transaction/lock") || return 1
    case "$vibepollo_package_lock_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  else
    (umask 077; set -C; printf 'package-lock\n' >"$vibepollo_package_transaction/lock") || return 1
  fi
  exec 8<>"$vibepollo_package_transaction/lock" || return 1
  flock --nonblock 8 || return 1
  vibepollo_package_lock_held=1
  vibepollo_package_cleanup_record_temporaries
}

vibepollo_package_properties_are_unique() {
  vibepollo_package_property_output=$1
  shift
  [ "$(printf '%%s\n' "$vibepollo_package_property_output" | wc -l | tr -d ' ')" = "$#" ] || return 1
  for vibepollo_package_property do
    [ "$(printf '%%s\n' "$vibepollo_package_property_output" | grep -c "^$vibepollo_package_property=" || true)" = 1 ] || return 1
  done
}

vibepollo_package_controller_identity_is_safe() {
  vibepollo_package_properties_are_unique "$1" MainPID ControlGroup FreezerState InvocationID || return 1
  vibepollo_package_identity_pid=$(printf '%%s\n' "$1" | sed -n 's/^MainPID=//p')
  vibepollo_package_identity_group=$(printf '%%s\n' "$1" | sed -n 's/^ControlGroup=//p')
  vibepollo_package_identity_invocation=$(printf '%%s\n' "$1" | sed -n 's/^InvocationID=//p')
  vibepollo_package_identity_freezer=$(printf '%%s\n' "$1" | sed -n 's/^FreezerState=//p')
  case "$vibepollo_package_identity_pid" in '' | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_package_identity_group" in '' | /*) ;; *) return 1 ;; esac
  case "$vibepollo_package_identity_group" in */../* | */.. | *'
'*) return 1 ;; esac
  case "$vibepollo_package_identity_freezer" in '' | running | frozen) ;; *) return 1 ;; esac
  if [ "$vibepollo_package_identity_pid" != 0 ]; then
    [ -n "$vibepollo_package_identity_group" ] && [ -n "$vibepollo_package_identity_freezer" ] || return 1
  fi
  if [ "$vibepollo_package_identity_pid" = 0 ] && [ -z "$vibepollo_package_identity_invocation" ]; then return 0; fi
  [ "${#vibepollo_package_identity_invocation}" -eq 32 ] || return 1
  case "$vibepollo_package_identity_invocation" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_record_directory_is_safe() {
  vibepollo_package_transaction_is_safe || return 1
  # Refuse unknown files rather than recursively deleting a damaged record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/* "$vibepollo_package_transaction"/.[!.]* "$vibepollo_package_transaction"/..?*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    case "${vibepollo_package_path##*/}" in
      lock | phase | controller | handoff.before | host-executables.before | host-stop.before | run-id | failure-class | legacy-adopted | fence.before | fence.owned | vibepollo.service.before | vibepollo.service.owned | vibepollo-session-exec.socket.before | vibepollo-session-exec.socket.owned | vibeshine-vkms-control.socket.before | vibeshine-vkms-control.socket.owned | vibepollo-session-restore@.service.before | vibepollo-session-restore@.service.owned) ;;
      *) return 1 ;;
    esac
    if [ "${vibepollo_package_path##*/}" = lock ] && [ ! -s "$vibepollo_package_path" ]; then
      [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] &&
        [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path")" = '0:0:600:1:regular empty file' ] || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_path" || return 1
    fi
  done
}

vibepollo_package_run_id_is_safe() {
  [ "${#1}" -eq 32 ] || return 1
  case "$1" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_preserve_unowned_host_mask() {
  vibepollo_package_preserve_host_mask=0
  for vibepollo_package_mask_path in /etc/systemd/system/vibepollo.service /run/systemd/system/vibepollo.service; do
    [ -L "$vibepollo_package_mask_path" ] || continue
    [ "$(readlink -- "$vibepollo_package_mask_path")" = /dev/null ] || continue
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    case "$vibepollo_package_mask_identity" in
      *:0:0:'symbolic link') ;;
      *) echo "error: unsafe host mask at $vibepollo_package_mask_path; package operation is blocked." >&2; return 1 ;;
    esac
    if [ "$vibepollo_package_mask_path" = /run/systemd/system/vibepollo.service ] &&
       { [ -e "$vibepollo_package_transaction/vibepollo.service.owned" ] || [ -L "$vibepollo_package_transaction/vibepollo.service.owned" ]; }; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/vibepollo.service.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/vibepollo.service.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    vibepollo_package_preserve_host_mask=1
  done
  [ "$vibepollo_package_preserve_host_mask" -eq 1 ] || return 0
  vibepollo_package_mask_state=$(timeout --signal=KILL 5 systemctl is-enabled vibepollo.service 2>/dev/null || true)
  vibepollo_package_mask_load=$(timeout --signal=KILL 5 systemctl show vibepollo.service --property=LoadState 2>/dev/null) || vibepollo_package_mask_load=uncertain
  vibepollo_package_mask_verified=0
  case "$vibepollo_package_mask_state:$vibepollo_package_mask_load" in
    masked:LoadState=masked | masked-runtime:LoadState=masked) vibepollo_package_mask_verified=1 ;;
  esac
  if [ "$vibepollo_package_mask_verified" -ne 1 ] ||
     ! vibepollo_preun_unit_is_quiescent vibepollo.service ||
     ! vibepollo_package_no_jobs; then
    echo 'error: an unowned Vibepollo host mask exists, but the host is active, populated, has a pending job, or its state is uncertain; package replacement is blocked. Force cannot remove an administrator or legacy mask without proving its activation ownership.' >&2
    return 1
  fi
  # Preserve the actual mask and any unowned fence byte-for-byte. The mask
  # supplies no-start admission while the full generation proof still runs.
  return 0
}

vibepollo_package_prepare_postinstall_start() {
  vibepollo_package_start_deferred=0
  vibepollo_package_preserve_unowned_host_mask || return 1
  if [ "$vibepollo_package_preserve_host_mask" -eq 1 ]; then
    vibepollo_package_start_deferred=1
  fi
  for vibepollo_package_start_unit in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    for vibepollo_package_start_directory in /etc/systemd/system /run/systemd/system; do
      vibepollo_package_start_path=$vibepollo_package_start_directory/$vibepollo_package_start_unit
      [ -L "$vibepollo_package_start_path" ] || continue
      [ "$(readlink -- "$vibepollo_package_start_path")" = /dev/null ] || continue
      vibepollo_package_start_identity=$(vibepollo_package_path_identity "$vibepollo_package_start_path") || return 1
      case "$vibepollo_package_start_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      if [ "$vibepollo_package_start_directory" = /run/systemd/system ] &&
         { [ -e "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ]; }; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" || return 1
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned")" = "$vibepollo_package_start_identity" ] || return 1
        continue
      fi
      vibepollo_package_start_deferred=1
    done
  done
  if [ -e "$vibepollo_package_transaction/fence.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
    vibepollo_package_prior_fence=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
    if [ "$vibepollo_package_prior_fence" != absent ]; then
      [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_prior_fence" ] || return 1
      vibepollo_package_start_deferred=1
    fi
  fi
  [ "$vibepollo_package_start_deferred" -eq 1 ] || return 0
  # This must be the full actual generation proof (controller, broker/apps,
  # restore/control instances, session record and retained executable inodes).
  # The local host-empty + no-jobs classifier is insufficient for release.
  if ! vibepollo_package_complete_quiescence_is_safe || ! vibepollo_package_release; then
    echo 'error: safe package shutdown recovery could not finish; automatic service activation remains deferred. Resolve the preceding check and retry package configuration; administrator masks were preserved.' >&2
    return 1
  fi
  echo 'package: preserved administrator host mask or start fence; configuration completed and automatic service activation is deferred.' >&2
  return 0
}

vibepollo_package_bind_run() {
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    vibepollo_package_write_record run-id "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
  elif [ ! -e "$vibepollo_package_transaction/run-id" ] && [ ! -L "$vibepollo_package_transaction/run-id" ]; then
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record failure-class none
}

vibepollo_package_run_is_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
  vibepollo_package_record_run=$(cat -- "$vibepollo_package_transaction/run-id") || return 1
  vibepollo_package_run_id_is_safe "$vibepollo_package_record_run" || return 1
  [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ] || return 0
  vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" &&
    [ "$vibepollo_package_record_run" = "$VIBEPOLLO_PACKAGE_RUN_ID" ]
}

vibepollo_package_force_is_applicable() (
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" &&
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/failure-class" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = late ] &&
    [ "$(cat -- "$vibepollo_package_transaction/failure-class")" = host-stop-deadline ]
)

vibepollo_package_refresh_missing_owned() {
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || continue
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    if [ "$vibepollo_package_current" = absent ]; then
      rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    else
      [ "$vibepollo_package_current" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" ] || return 1
    fi
  done
}

vibepollo_package_cleanup_retired() (
  vibepollo_package_original_root=$vibepollo_package_transaction
  vibepollo_package_retired_root=$vibepollo_package_original_root.retired
  [ -e "$vibepollo_package_retired_root" ] || [ -L "$vibepollo_package_retired_root" ] || return 0
  # This independent root-owned lock survives removal of the retired record's
  # own lock inode, including SIGKILL between the final unlink and rmdir.
  vibepollo_package_gc_lock=$vibepollo_package_original_root.gc-lock
  if [ ! -e "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ]; then
    (umask 077; set -C; printf 'package-gc\n' >"$vibepollo_package_gc_lock") || return 1
  fi
  [ -f "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ] || return 1
  vibepollo_package_gc_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_gc_lock") || return 1
  case "$vibepollo_package_gc_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  exec 7<>"$vibepollo_package_gc_lock" || return 1
  timeout --signal=KILL 5 flock --exclusive 7 || return 1
  vibepollo_package_transaction=$vibepollo_package_retired_root
  [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ] || return 0
  vibepollo_package_transaction_is_safe && vibepollo_package_record_directory_is_safe || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/*; do
    [ -e "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
  rmdir -- "$vibepollo_package_transaction" || return 1
)

vibepollo_package_adopt_legacy_barriers() {
  # Only the exact old package signature, with every admission path closed
  # and the complete old generation drained, permits ownership migration.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
  vibepollo_package_adoption_pending=0
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    vibepollo_package_adoption_pending=1
    # The marker is published only after the exact old fence, all original
    # mask identities and full generation proof were verified. An interrupted
    # conversion may leave some .before snapshots unconverted when old package
    # unwind removes the fence. Resume only those original masks, never a
    # replacement path or an unrecorded new mask.
    if [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ]; then
      vibepollo_package_complete_quiescence_is_safe || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      if [ -e "$vibepollo_package_transaction/fence.owned" ] || [ -L "$vibepollo_package_transaction/fence.owned" ]; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.owned" || return 1
      fi
      # Validate every path before changing any record. Repeat the identity
      # checks during publication so interruption leaves resumable metadata.
      for vibepollo_package_adoption_pass in validate commit; do
        for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
          vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
          if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
            vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
            vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
          fi
          vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
          if [ "$vibepollo_package_current" != absent ]; then
            case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
            [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
            [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
          fi
          [ "$vibepollo_package_adoption_pass" = commit ] || continue
          if [ "$vibepollo_package_current" = absent ]; then
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
            rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          else
            vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
          fi
        done
      done
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      vibepollo_package_write_record fence.before absent || return 1
      rm -f -- "$vibepollo_package_transaction/fence.owned" || return 1
      vibepollo_package_write_record phase preflight || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      rm -f -- "$vibepollo_package_transaction/legacy-adopted" || return 1
      return 0
    fi
  fi
  [ "$vibepollo_package_before" != absent ] || [ "$vibepollo_package_adoption_pending" -eq 1 ] || return 0
  [ -f "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = '0:0:644:1:regular file' ] || return 1
  vibepollo_package_legacy_contents=$(cat -- "$vibepollo_host_upgrade_dropin") || return 1
  case "$vibepollo_package_legacy_contents" in
    '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=mixed
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no') ;;
    *) [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1; return 0 ;;
  esac
  [ "$(stat -c '%%s' -- "$vibepollo_host_upgrade_dropin")" = "$(printf '%%s\n' "$vibepollo_package_legacy_contents" | wc -c | tr -d ' ')" ] || {
    [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1
    return 0
  }
  vibepollo_package_complete_quiescence_is_safe || {
    echo 'error: an earlier package fence remains, but the old host or session has not fully drained; it cannot be adopted safely.' >&2
    return 1
  }
  # Generation inspection can take time. An administrator may edit the same
  # inode while it runs; do not convert authority for changed fence bytes.
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
  done
  if [ -e "$vibepollo_package_transaction/fence.owned" ]; then
    vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.owned") || return 1
  fi
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_before" ] || return 1
  vibepollo_package_adoption_fence_identity=$vibepollo_package_before
  vibepollo_package_write_record legacy-adopted yes || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    # Recheck each original identity at publication, rather than claiming a
    # replacement that appeared after the inventory pass.
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
    if [ "$vibepollo_package_name" != fence ]; then
      [ "$(readlink -- "$vibepollo_package_owned_path")" = /dev/null ] || return 1
    fi
    vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
    vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
  done
  vibepollo_package_write_record phase preflight || return 1
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_adoption_fence_identity" ] || return 1
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  # Every owned identity and before snapshot is now durable; the pending
  # adoption marker is no longer needed by later release/retry operations.
  rm -f -- "$vibepollo_package_transaction/legacy-adopted"
}

vibepollo_package_rebaseline_unwound_run() {
  # Pending legacy authority is not a wholly unwound transaction even when
  # no .owned file has been published yet. Reconcile it before a new target.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    return 0
  fi
  # A native package retry, or a fresh explicit installer operation, may
  # select the current healthy invocation after every owned barrier unwound.
  # This grants a fresh target, never proof that old process resources vanished.
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    [ -e "$vibepollo_package_transaction/run-id" ] || return 0
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
    # A same-run force retry must retain its original invocation identity.
    [ "$(cat -- "$vibepollo_package_transaction/run-id")" != "$VIBEPOLLO_PACKAGE_RUN_ID" ] || return 0
  fi
  for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
    [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 0
  done
  vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    [ "$(vibepollo_package_path_identity "$vibepollo_package_owned_path")" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" ] || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  if [ -z "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    # A native retry is a new operation. Retire any earlier wrapper's nonce
    # atomically before refreshing phase or discarding its saved stop identity.
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  rm -f -- "$vibepollo_package_transaction/host-stop.before" || return 1
  vibepollo_package_prior_phase=early
}

vibepollo_package_begin() {
  vibepollo_package_cleanup_retired || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_record_directory_is_safe || return 1
    vibepollo_package_refresh_missing_owned || return 1
    if [ ! -e "$vibepollo_package_transaction/phase" ] && [ ! -L "$vibepollo_package_transaction/phase" ]; then
      for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
        [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
      done
      vibepollo_package_forget || return 1
      vibepollo_package_begin
      return $?
    fi
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
    vibepollo_package_prior_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
    case "$vibepollo_package_prior_phase" in early | late | preflight) ;; *) return 1 ;; esac
    for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    done
    vibepollo_package_snapshot_host_executables || return 1
    vibepollo_package_rebaseline_unwound_run || return 1
    vibepollo_package_bind_run || return 1
    vibepollo_package_adopt_legacy_barriers || return 1
    return 0
  fi
  # mkdir is exclusive; a competing package operation may not share ownership.
  (umask 077; mkdir -- "$vibepollo_package_transaction") || return 1
  vibepollo_package_transaction_created=1
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_lock || return 1
  vibepollo_package_snapshot_host_executables || return 1
  vibepollo_package_prior_phase=early
  vibepollo_package_snapshot_path fence "$vibepollo_host_upgrade_dropin" || return 1
  vibepollo_package_handoff=absent
  if [ -e "${vibepollo_legacy_handoff:-/usr/libexec/vibeshine/vibepollo-session-handoff}" ] || [ -L "$vibepollo_legacy_handoff" ]; then
    [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] || return 1
    vibepollo_package_handoff=$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff") || return 1
    case "$vibepollo_package_handoff" in *:0:0:755:1 | *:0:0:0:1) ;; *) return 1 ;; esac
  fi
  vibepollo_package_write_record handoff.before "$vibepollo_package_handoff" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_snapshot_path "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  vibepollo_package_bind_run || return 1
  vibepollo_package_adopt_legacy_barriers
}

vibepollo_package_record_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 0
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_identity" != absent ] || return 1
  if [ -e "$vibepollo_package_transaction/$1.owned" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_identity" ]
  else
    vibepollo_package_write_record "$1.owned" "$vibepollo_package_identity"
  fi
}

vibepollo_package_mask() (
  vibepollo_package_mask_stage=
  trap '[ -z "$vibepollo_package_mask_stage" ] || rm -f -- "$vibepollo_package_mask_stage"' 0
  for vibepollo_package_name do
    case "$vibepollo_package_name" in
      vibepollo.service | vibepollo-session-exec.socket | vibeshine-vkms-control.socket | vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = absent ] &&
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    if [ "$vibepollo_package_mask_identity" = absent ]; then
      # Keep a private inode as creation evidence. Publish its ownership intent
      # before linking it, so interruption cannot strand our published mask.
      # ln -P -T never replaces an administrator's competing path.
      vibepollo_package_mask_stage=$(mktemp "$vibepollo_package_transaction/.mask.XXXXXX") || return 1
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      ln -s /dev/null "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_created_mask=$(vibepollo_package_path_identity "$vibepollo_package_mask_stage") || return 1
      case "$vibepollo_package_created_mask" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
      vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_created_mask" || return 1
      if ! ln -P -T -- "$vibepollo_package_mask_stage" "$vibepollo_package_mask_path" 2>/dev/null; then
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        if [ "$vibepollo_package_mask_identity" != "$vibepollo_package_created_mask" ]; then
          # An exclusive-link failure published none of our inode. Drop only
          # our exact intent, then preserve a valid competing external mask.
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_created_mask" ] || return 1
          rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$vibepollo_package_mask_identity" != absent ] || return 1
        fi
      else
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        [ "$vibepollo_package_mask_identity" = "$vibepollo_package_created_mask" ] || return 1
      fi
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_mask_stage=
      if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then continue; fi
    fi
    # Existing unowned masks, including ones added since a failed attempt's
    # snapshot, remain policy. Refresh their preserved baseline, never claim.
    case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_write_record "$vibepollo_package_name.before" "$vibepollo_package_mask_identity" || return 1
    [ "$(vibepollo_package_path_identity "$vibepollo_package_mask_path")" = "$vibepollo_package_mask_identity" ] || return 1
  done
  timeout --signal=KILL 15 systemctl mask --runtime "$@" 2>/dev/null
  vibepollo_package_mask_status=$?
  # The manager can reload our prepublished masks, but cannot establish their
  # ownership. Even a timed-out client must leave every recorded inode intact.
  for vibepollo_package_name do
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
    else
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = "$vibepollo_package_mask_identity" ] || return 1
    fi
  done
  return "$vibepollo_package_mask_status"
)

vibepollo_package_mark_late() {
  vibepollo_package_write_record phase late
}

vibepollo_package_remove_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/$1.before") || return 1
  [ "$vibepollo_package_before" = absent ] || return 0
  vibepollo_package_current=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_current" != absent ] || return 0
  if [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ]; then
    # An idle Arch retry can skip mask creation. Reconcile an external mask
    # added since the snapshot without claiming it or reopening its admission.
    # New package masks publish intent first, so a missing record is not ours.
    case "$1:$2" in
      vibepollo.service:/run/systemd/system/vibepollo.service | vibepollo-session-exec.socket:/run/systemd/system/vibepollo-session-exec.socket | vibeshine-vkms-control.socket:/run/systemd/system/vibeshine-vkms-control.socket | vibepollo-session-restore@.service:/run/systemd/system/vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_lock || return 1
    vibepollo_package_complete_quiescence_is_safe || return 1
    [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ] || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    vibepollo_package_write_record "$1.before" "$vibepollo_package_current" || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    return 0
  fi
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_current" ] || return 1
  rm -f -- "$2"
}

vibepollo_package_no_jobs() {
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_package_job=$(timeout --signal=KILL 5 systemctl show "$vibepollo_package_name" --property=Job 2>/dev/null) || return 1
    [ "$vibepollo_package_job" = 'Job=' ] || return 1
  done
}

vibepollo_package_restore_barriers() {
  vibepollo_package_keep_legacy=${1:-0}
  vibepollo_package_transaction_is_safe || return 1
  # An interrupted ownership conversion must finish (or refuse a changed
  # legacy fence) before release can remove any path by its saved inode.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_adopt_legacy_barriers || return 1
  fi
  vibepollo_package_remove_owned fence "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_keep_legacy" -eq 1 ] && [ "$vibepollo_package_name" = 'vibepollo-session-restore@.service' ]; then continue; fi
    vibepollo_package_remove_owned "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  timeout --signal=KILL 15 systemctl daemon-reload || return 1
}

vibepollo_package_forget() {
  # Failed initial adoption can already own barriers. Retain its authority
  # until a proved resume completes; a created-directory flag cannot erase it.
  [ ! -e "$vibepollo_package_transaction/legacy-adopted" ] &&
    [ ! -L "$vibepollo_package_transaction/legacy-adopted" ] || return 1
  vibepollo_package_transaction_is_safe && vibepollo_package_cleanup_record_temporaries &&
    vibepollo_package_record_directory_is_safe || return 1
  vibepollo_package_forget_root=$vibepollo_package_transaction
  [ ! -e "$vibepollo_package_forget_root.retired" ] && [ ! -L "$vibepollo_package_forget_root.retired" ] || return 1
  mv -T -- "$vibepollo_package_forget_root" "$vibepollo_package_forget_root.retired" || return 1
  exec 8>&-
  vibepollo_package_lock_held=0
  vibepollo_package_cleanup_retired
}

vibepollo_package_restore_legacy_handoff() {
  # Only failed/cancelled transactions restore the same installed old inode.
  # Successful replacement deliberately retires this obsolete admission path.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/handoff.before" || return 1
  vibepollo_package_handoff=$(cat -- "$vibepollo_package_transaction/handoff.before") || return 1
  case "$vibepollo_package_handoff" in
    *:0:0:755:1)
      vibepollo_package_handoff_expected=${vibepollo_package_handoff%%:755:1}:0:1
      if [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] &&
         [ "$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff")" = "$vibepollo_package_handoff_expected" ]; then
        chmod 0755 -- "$vibepollo_legacy_handoff" || return 1
      fi
      ;;
    absent | *:0:0:0:1) ;;
    *) return 1 ;;
  esac
}

vibepollo_package_restore_early() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = early ] || return 1
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/controller" || return 1
    vibepollo_package_controller=$(cat -- "$vibepollo_package_transaction/controller") || return 1
    vibepollo_package_current_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" &&
      vibepollo_package_controller_identity_is_safe "$vibepollo_package_current_controller" || return 1
    # Thaw only our original exact unit identity, never an administrator's freeze.
    vibepollo_package_original_identity=$(printf '%%s\n' "$vibepollo_package_controller" | sed '/^FreezerState=/d')
    vibepollo_package_current_identity=$(printf '%%s\n' "$vibepollo_package_current_controller" | sed '/^FreezerState=/d')
    [ "$vibepollo_package_original_identity" = "$vibepollo_package_current_identity" ] || return 1
    vibepollo_package_restore_barriers || return 1
    if printf '%%s\n' "$vibepollo_package_controller" | grep -qx 'FreezerState=running' &&
       printf '%%s\n' "$vibepollo_package_current_controller" | grep -qx 'FreezerState=frozen'; then
      timeout --signal=KILL 15 systemctl thaw vibepollo-session-controller.service 2>/dev/null || return 1
    fi
    vibepollo_controller_was_frozen=0
  fi
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget
}

vibepollo_package_release() {
  # Called only after the old generation has completely drained.
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_preun_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  [ ! -e /run/vibepollo/session.env ] && [ ! -L /run/vibepollo/session.env ] || return 1
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_restore_barriers 1 || return 1
    vibepollo_package_forget || return 1
  fi
}

vibepollo_package_recover_failed_transaction() (
  # The caller must wait for its package-manager child before invoking this.
  # Releasing a preflight while replacement is still executing is forbidden.
  if [ ! -e "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ]; then return 0; fi
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  vibepollo_package_recovery_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
  case "$vibepollo_package_recovery_phase" in
    early) vibepollo_package_restore_early; return $? ;;
    late | preflight) ;;
    *) return 1 ;;
  esac
  vibepollo_package_complete_quiescence_is_safe || return 1
  vibepollo_package_restore_barriers || return 1
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget || return 1
  echo 'package recovery: old host and session cleanup verified; transaction-owned admission fences released without starting services.' >&2
)








vibepollo_package_complete_quiescence_is_safe() {
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket vibepollo-prelogin.service vibepollo-machine-prepare.service; do
    vibepollo_preun_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  vibepollo_control_instances_are_quiescent &&
    vibepollo_brokers_are_quiescent &&
    vibepollo_restore_instances_are_quiescent || return 1
  for vibepollo_package_runtime_path in /run/vibepollo/session.env "$vibepollo_broker_socket" "$vibepollo_control_socket" "$vibepollo_legacy_handoff_directory" "$vibepollo_legacy_restore_directory" "$vibepollo_legacy_transition_lock" "$vibepollo_legacy_prelogin_marker"; do
    [ -n "$vibepollo_package_runtime_path" ] && [ ! -e "$vibepollo_package_runtime_path" ] && [ ! -L "$vibepollo_package_runtime_path" ] || return 1
  done
}

vibepollo_package_preserved_preflight_is_safe() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = preflight ] || return 1
  vibepollo_restore_template_is_masked && vibepollo_control_socket_is_masked &&
    vibepollo_broker_socket_is_masked && vibepollo_host_unit_is_masked || return 1
  vibepollo_package_complete_quiescence_is_safe
}

vibepollo_package_open_owned_host_for_stop() {
  if [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ]; then return 0; fi
  # A prior late stop may finish after its client timed out. Re-expose the
  # installed unit only when the old cgroup is empty, retaining its start fence.
  vibepollo_preun_unit_is_quiescent vibepollo.service && vibepollo_package_no_jobs || return 1
  vibepollo_package_remove_owned vibepollo.service /run/systemd/system/vibepollo.service || return 1
  [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ] || return 1
  rm -f -- "$vibepollo_package_transaction/vibepollo.service.owned"
}











vibepollo_preun_quiesce() {
  vibepollo_have_systemd=0
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_have_systemd=1
    # Preserve the loaded host's TERM contract while preventing new starts.
    # Freeze the exact controller so it cannot race this ordered host stop.
    vibepollo_preun_select_upgrade_kill_mode || return 1
    vibepollo_prepare_host_upgrade_fence || return 1
    vibepollo_freeze_controller || return 1
    vibepollo_activate_host_upgrade_fence || return 1
    vibepollo_controller_remains_frozen || return 1
    # Older installed PAM code can execute this exact root helper directly.
    # Close only that known admission path before any host shutdown begins.
    if [ -e "$vibepollo_legacy_handoff" ] || [ -L "$vibepollo_legacy_handoff" ]; then
      vibepollo_package_mask 'vibepollo-session-restore@.service' || return 1
      vibepollo_restore_template_is_masked || return 1
      vibepollo_disable_legacy_handoff || return 1
    fi
    vibepollo_package_mask vibeshine-vkms-control.socket || return 1
    vibepollo_preun_unit_is_masked vibeshine-vkms-control.socket || return 1
    vibepollo_preun_stop_exact_unit vibeshine-vkms-control.socket
    vibepollo_preun_unit_is_quiescent vibeshine-vkms-control.socket || return 1
    if [ -S "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ]; then
      rm -f -- "$vibepollo_control_socket" || return 1
    fi
    vibepollo_control_instances_are_quiescent || return 1
    vibepollo_package_mark_late || return 1
    vibepollo_shutdown_started=1
    vibepollo_package_mask vibepollo-session-exec.socket || return 1
    vibepollo_preun_unit_is_masked vibepollo-session-exec.socket || return 1
    vibepollo_preun_stop_exact_unit vibepollo-session-exec.socket
    vibepollo_preun_unit_is_quiescent vibepollo-session-exec.socket || return 1
    if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
      rm -f -- "$vibepollo_broker_socket" || return 1
    fi
    vibepollo_preun_stop_exact_unit vibepollo.service
    vibepollo_preun_unit_is_quiescent vibepollo.service || return 1
    # Mask only after the old GPU owner has drained: masking earlier discards
    # the loaded service's shutdown settings and defeats identity verification.
    vibepollo_package_mask vibepollo.service || return 1
    vibepollo_preun_unit_is_masked vibepollo.service || return 1
    vibepollo_stop_restore_instances || return 1
    vibepollo_preun_stop_brokers || return 1
    vibepollo_controller_remains_frozen || return 1
    vibepollo_thaw_controller || return 1
    for vibepollo_unit in vibepollo-session-controller.service \
      vibepollo-prelogin.service vibepollo-machine-prepare.service; do
      vibepollo_preun_stop_exact_unit "$vibepollo_unit"
      vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
    done
  fi

  if [ -e "$vibepollo_controller" ] || [ -L "$vibepollo_controller" ]; then
    vibepollo_preun_privileged_helper_is_safe "$vibepollo_controller" || return 1
    [ "$vibepollo_have_systemd" -eq 1 ] || return 1
    timeout --signal=KILL 40 "$vibepollo_controller" cleanup || return 1
  fi
  vibepollo_disable_legacy_handoff || return 1
  vibepollo_preun_remove_pam || return 1

  if [ "$vibepollo_have_systemd" -eq 1 ]; then
    vibepollo_preun_unit_is_masked vibepollo.service || return 1
    vibepollo_preun_unit_is_masked vibepollo-session-exec.socket || return 1
    vibepollo_preun_stop_exact_unit vibepollo-session-exec.socket
    vibepollo_preun_stop_exact_unit vibepollo.service
    vibepollo_preun_unit_is_quiescent vibepollo.service || return 1
    vibepollo_preun_stop_brokers || return 1
    for vibepollo_unit in vibepollo-session-exec.socket vibepollo-session-controller.service \
      vibepollo.service vibepollo-prelogin.service vibepollo-machine-prepare.service; do
      vibepollo_preun_stop_exact_unit "$vibepollo_unit"
      vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
    done
    vibepollo_preun_stop_brokers || return 1
  fi
  if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" || return 1
  fi
  [ ! -e "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ] && \
    [ ! -e "$vibepollo_session_record" ] && [ ! -L "$vibepollo_session_record" ] && \
    [ ! -e "$vibepollo_legacy_acl" ] && [ ! -L "$vibepollo_legacy_acl" ] || return 1
  for vibepollo_removal_runtime_path in "$vibepollo_legacy_handoff_directory" "$vibepollo_legacy_restore_directory" "$vibepollo_legacy_transition_lock" "$vibepollo_legacy_prelogin_marker"; do
    [ ! -e "$vibepollo_removal_runtime_path" ] && [ ! -L "$vibepollo_removal_runtime_path" ] || return 1
  done
  if [ "$vibepollo_have_systemd" -eq 1 ]; then
    vibepollo_restore_instances_are_quiescent || return 1
    vibepollo_package_no_jobs || return 1
  fi
}

vibepollo_abort_removal() {
  vibepollo_removal_abort_done=1
  trap - HUP INT TERM
  if [ "${vibepollo_shutdown_started:-0}" -eq 0 ] &&
     [ "${vibepollo_package_prior_phase:-early}" = early ]; then
    vibepollo_package_restore_early && return 0
  else
    # Finish the bounded shutdown before releasing admission. If proof remains
    # uncertain, retain only owned barriers and allow the next removal to retry.
    if vibepollo_preun_unit_is_quiescent vibepollo.service; then
      vibepollo_thaw_controller || return 1
      vibepollo_preun_stop_brokers || return 1
      if vibepollo_preun_unit_is_quiescent vibepollo-session-exec.socket &&
         [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
        rm -f -- "$vibepollo_broker_socket" || return 1
      fi
      vibepollo_preun_stop_exact_unit vibepollo-session-controller.service || true
      vibepollo_preun_unit_is_quiescent vibepollo-session-controller.service || return 1
      if [ -e "$vibepollo_controller" ] || [ -L "$vibepollo_controller" ]; then
        vibepollo_preun_privileged_helper_is_safe "$vibepollo_controller" || return 1
        timeout --signal=KILL 40 "$vibepollo_controller" cleanup || return 1
      fi
      vibepollo_package_complete_quiescence_is_safe || return 1
      vibepollo_package_restore_legacy_handoff || return 1
      vibepollo_package_restore_barriers || return 1
      vibepollo_package_forget || return 1
      return 0
    fi
  fi
  echo 'error: removal recovery remains uncertain; retry removal to finish exact host and session cleanup.' >&2
  return 1
}

vibepollo_remove_or_abort() (
  # Package-manager signals must reach one transaction owner, including nested
  # temporary-file helpers. SIGKILL is recovered by the next invocation.
  vibepollo_removal_abort_done=0
  vibepollo_shutdown_started=0
  vibepollo_package_transaction_created=0
  trap 'vibepollo_removal_status=$?; if [ "$vibepollo_removal_status" -ne 0 ] && [ "$vibepollo_removal_abort_done" -eq 0 ]; then vibepollo_abort_removal; fi' 0
  trap 'exit 129' HUP
  trap 'exit 130' INT
  trap 'exit 143' TERM
  if ! vibepollo_package_begin; then
    if [ "$vibepollo_package_transaction_created" -eq 1 ]; then
      vibepollo_package_forget || true
    fi
    vibepollo_removal_abort_done=1
    echo 'error: removal transaction ownership record is unsafe or incomplete.' >&2
    return 1
  fi
  vibepollo_preun_quiesce || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  # Keep start/admission barriers until postrm/%%postun has removed the files.
  vibepollo_package_write_record phase preflight || return 1
  if [ "$vibepollo_have_systemd" -eq 1 ]; then
    # All old resources are proven empty. Disable obsolete boot links only
    # after quiescence, while owned admission barriers remain in place.
    for vibepollo_unit in vibepollo-session-controller.service vibepollo.service \
      vibepollo-prelogin.service vibepollo-machine-prepare.service; do
      timeout --signal=KILL 15 systemctl disable "$vibepollo_unit" 2>/dev/null || true
    done
  fi
)


if [ "$1" -eq 0 ]; then
  vibepollo_remove_or_abort || {
    echo "error: refusing to uninstall while Vibepollo cgroups or session state remain." >&2
    exit 1
  }
  timeout --signal=KILL 30 systemctl stop vibeshine-vkms.service 2>/dev/null || true
  timeout --signal=KILL 30 systemctl stop vibeshine-drm-setup.service 2>/dev/null || true
  %{_prefix}/libexec/vibeshine/vibeshine-ds5-install remove || \
    echo "warning: could not remove the DualSense USB module cleanly."
  %{_prefix}/libexec/vibeshine/vibeshine-drm-install remove || \
    echo "warning: could not remove the Vibepollo HDR DRM module cleanly."
fi

%postun
export LC_ALL=C
vibepollo_controller="%{_prefix}/libexec/vibeshine/vibepollo-session-controller"
vibepollo_machine_host="%{_prefix}/libexec/vibeshine/vibepollo-machine-host"
vibepollo_session_record=/run/vibepollo/session.env
vibepollo_legacy_acl=/run/vibepollo/runtime-acl
vibepollo_broker_socket=/run/vibepollo/session-broker.sock
vibepollo_legacy_host=$vibepollo_machine_host
vibepollo_legacy_handoff="%{_prefix}/libexec/vibeshine/vibepollo-session-handoff"
vibepollo_host_upgrade_dropin_dir=/run/systemd/system/vibepollo.service.d
vibepollo_host_upgrade_dropin=$vibepollo_host_upgrade_dropin_dir/90-vibepollo-safe-upgrade.conf
vibepollo_controller_was_frozen=0
vibepollo_control_socket=/run/vibeshine/vkms-control.sock
vibepollo_upgrade_kill_mode=
vibepollo_legacy_handoff_directory=/run/vibepollo/session-handoffs
vibepollo_legacy_restore_directory=/run/vibepollo/session-restores
vibepollo_legacy_transition_lock=/run/vibepollo/session-handoff.lock
vibepollo_legacy_prelogin_marker=/run/vibepollo/prelogin-handoff-complete
vibepollo_carriage_return=$(printf '\r')



vibepollo_cgroup_is_quiescent() {
  vibepollo_control_group=$1
  [ -n "$vibepollo_control_group" ] || return 0
  case "$vibepollo_control_group" in /*) ;; *) return 1 ;; esac
  case "$vibepollo_control_group" in */../* | */..) return 1 ;; esac
  vibepollo_cgroup_path=/sys/fs/cgroup$vibepollo_control_group
  if [ ! -e "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ]; then return 0; fi
  [ -d "$vibepollo_cgroup_path" ] && [ ! -L "$vibepollo_cgroup_path" ] && \
    [ -f "$vibepollo_cgroup_path/cgroup.events" ] && \
    [ ! -L "$vibepollo_cgroup_path/cgroup.events" ] && \
    grep -qx 'populated 0' "$vibepollo_cgroup_path/cgroup.events"
}

vibepollo_unit_is_quiescent() {
  vibepollo_properties=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup 2>/dev/null) || return 1
  vibepollo_property_lines=$(printf '%%s\n' "$vibepollo_properties" | wc -l | tr -d ' ')
  case "$1:$vibepollo_property_lines" in
    *.socket:4 | *.socket:5 | *:5) ;;
    *) return 1 ;;
  esac
  for vibepollo_property in LoadState ActiveState SubState ControlGroup; do
    vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | \
      grep -c "^$vibepollo_property=" || true)
    [ "$vibepollo_property_count" = 1 ] || return 1
  done
  vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | grep -c '^MainPID=' || true)
  case "$1:$vibepollo_property_count" in
    *.socket:0 | *.socket:1 | *:1) ;;
    *) return 1 ;;
  esac
  vibepollo_load=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^LoadState=//p')
  vibepollo_state=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ActiveState=//p')
  vibepollo_substate=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^SubState=//p')
  vibepollo_pid=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^MainPID=//p')
  case "$1" in *.socket) [ -n "$vibepollo_pid" ] || vibepollo_pid=0 ;; esac
  vibepollo_control_group=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ControlGroup=//p')
  case "$vibepollo_load:$vibepollo_state:$vibepollo_substate" in
    not-found:inactive:dead | \
    loaded:inactive:dead | loaded:failed:failed | \
    masked:inactive:dead | masked:failed:failed | \
    masked-runtime:inactive:dead | masked-runtime:failed:failed) ;;
    *) return 1 ;;
  esac
  [ "$vibepollo_pid" = 0 ] && vibepollo_cgroup_is_quiescent "$vibepollo_control_group"
}

vibepollo_broker_unit_is_safe() {
  vibepollo_broker_unit=$1
  case "$vibepollo_broker_unit" in vibepollo-session-exec@*.service) ;; *) return 1 ;; esac
  vibepollo_broker_instance=${vibepollo_broker_unit#vibepollo-session-exec@}
  vibepollo_broker_instance=${vibepollo_broker_instance%%.service}
  [ -n "$vibepollo_broker_instance" ] || return 1
  case "$vibepollo_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}

vibepollo_restore_unit_is_safe() {
  vibepollo_restore_unit=$1
  case "$vibepollo_restore_unit" in
    vibepollo-session-restore@*.service) ;;
    *) return 1 ;;
  esac
  vibepollo_restore_instance=${vibepollo_restore_unit#vibepollo-session-restore@}
  vibepollo_restore_instance=${vibepollo_restore_instance%%.service}
  case "$vibepollo_restore_instance" in
    [1-9]*) ;;
    *) return 1 ;;
  esac
  case "$vibepollo_restore_instance" in
    *[!0-9]*) return 1 ;;
  esac
}

vibepollo_control_unit_is_safe() {
  vibepollo_control_unit=$1
  case "$vibepollo_control_unit" in
    vibeshine-vkms-control@*.service) ;;
    *) return 1 ;;
  esac
  vibepollo_control_instance=${vibepollo_control_unit#vibeshine-vkms-control@}
  vibepollo_control_instance=${vibepollo_control_instance%%.service}
  [ -n "$vibepollo_control_instance" ] || return 1
  case "$vibepollo_control_instance" in
    *[!A-Za-z0-9_.:-]*) return 1 ;;
  esac
}

vibepollo_capture_unit_list() {
  vibepollo_unit_list_path=$1
  vibepollo_unit_list_pattern=$2
  [ -f "$vibepollo_unit_list_path" ] && [ ! -L "$vibepollo_unit_list_path" ] || return 1
  chmod 0600 "$vibepollo_unit_list_path" || return 1
  (
    ulimit -f 128 || exit 1
    timeout --signal=KILL 5 systemctl list-units \
      --all --plain --no-legend --no-pager --full \
      "$vibepollo_unit_list_pattern" >"$vibepollo_unit_list_path" 2>/dev/null
  ) || return 1
  vibepollo_unit_list_size=$(stat -c '%%s' -- "$vibepollo_unit_list_path") || return 1
  case "$vibepollo_unit_list_size" in '' | *[!0-9]*) return 1 ;; esac
  [ "$vibepollo_unit_list_size" -le 65536 ]
}

vibepollo_control_instances_are_quiescent() (
  vibepollo_control_list=$(mktemp /run/vibepollo-control-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_control_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_control_attempt=0
  vibepollo_control_clean_passes=0
  while [ "$vibepollo_control_attempt" -lt 100 ]; do
    vibepollo_control_dirty=0
    vibepollo_capture_unit_list "$vibepollo_control_list" \
      'vibeshine-vkms-control@*.service' || return 1
    vibepollo_seen_units='
'
    vibepollo_seen_count=0
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *"$vibepollo_carriage_return"*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1
      vibepollo_load=$2
      vibepollo_state=$3
      vibepollo_substate=$4
      vibepollo_control_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      vibepollo_seen_count=$((vibepollo_seen_count + 1))
      [ "$vibepollo_seen_count" -le 256 ] || return 1
      if ! vibepollo_unit_is_quiescent "$vibepollo_unit"; then
        vibepollo_control_dirty=1
      fi
    done <"$vibepollo_control_list"
    if [ "$vibepollo_control_dirty" -eq 0 ]; then
      vibepollo_control_clean_passes=$((vibepollo_control_clean_passes + 1))
      [ "$vibepollo_control_clean_passes" -lt 5 ] || {
        [ ! -e "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ] || return 1
        return 0
      }
    else
      vibepollo_control_clean_passes=0
    fi
    vibepollo_control_attempt=$((vibepollo_control_attempt + 1))
    sleep 0.1
  done
  return 1
)

vibepollo_brokers_are_quiescent() (
  vibepollo_broker_list=$(mktemp /run/vibepollo-restore-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_broker_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_capture_unit_list "$vibepollo_broker_list" \
    'vibepollo-session-exec@*.service' || return 1
  vibepollo_seen_units='
'
  vibepollo_seen_count=0
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1
    vibepollo_load=$2
    vibepollo_state=$3
    vibepollo_substate=$4
    vibepollo_broker_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_seen_count=$((vibepollo_seen_count + 1))
    [ "$vibepollo_seen_count" -le 256 ] || return 1
    vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
  done <"$vibepollo_broker_list"
)

vibepollo_restore_instances_are_quiescent() (
  vibepollo_restore_list=$(mktemp /run/vibepollo-restore-units.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_restore_list"' 0
  trap 'exit 1' HUP INT TERM
  vibepollo_capture_unit_list "$vibepollo_restore_list" \
    'vibepollo-session-restore@*.service' || return 1
  vibepollo_seen_units='
'
  vibepollo_seen_count=0
  while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
    [ -n "$vibepollo_line" ] || continue
    case "$vibepollo_line" in *"
"*) return 1 ;; esac
    set -f
    set -- $vibepollo_line
    [ "${1:-}" = '●' ] && shift
    [ "$#" -ge 4 ] || return 1
    vibepollo_unit=$1
    vibepollo_load=$2
    vibepollo_state=$3
    vibepollo_substate=$4
    vibepollo_restore_unit_is_safe "$vibepollo_unit" || return 1
    case "$vibepollo_load" in loaded | masked) ;; *) return 1 ;; esac
    case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
    case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
    vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
    vibepollo_seen_count=$((vibepollo_seen_count + 1))
    [ "$vibepollo_seen_count" -le 256 ] || return 1
    vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
  done <"$vibepollo_restore_list"
)

vibepollo_package_unmanaged_host_absent() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before" || return 1
  vibepollo_package_executable_records=$(cat -- "$vibepollo_package_transaction/host-executables.before") || return 1
  if [ ! -x /usr/bin/python3 ]; then
    [ "$vibepollo_package_executable_records" = '[]' ] || {
      echo 'error: trusted Python process inspection is required for the retained old host inode snapshot.' >&2
      return 1
    }
    # First-install pre-hooks can run before Python dependencies are installed.
    # An empty capture still requires a bounded exact-path scan, including old
    # root-owned deleted executables; it is never treated as an absence proof.
    timeout --signal=KILL 10 /bin/sh -s <<'HOST_PROOF'
command -v readlink >/dev/null 2>&1 && command -v stat >/dev/null 2>&1 || exit 1
for executable in /proc/[0-9]*/exe; do
  target=$(readlink -v -- "$executable" 2>&1)
  status=$?
  if [ "$status" -ne 0 ]; then
    case "$target" in
      *': No such file or directory') continue ;;
      *) echo 'error: executable scope inspection is uncertain; replacement is blocked.' >&2; exit 1 ;;
    esac
  fi
  case "$target" in
    /usr/bin/vibepollo | '/usr/bin/vibepollo (deleted)' | \
    /usr/libexec/vibeshine/vibepollo-host | '/usr/libexec/vibeshine/vibepollo-host (deleted)')
      identity=$(stat -Lc '%%u:%%F' -- "$executable" 2>/dev/null) || exit 1
      if [ "$identity" = '0:regular file' ]; then
        process_id=${executable%/exe}
        process_id=${process_id##*/}
        echo "error: verified old host executable remains in PID $process_id; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership." >&2
        exit 1
      fi
      ;;
  esac
done
HOST_PROOF
    return $?
  fi
  timeout --signal=KILL 10 /usr/bin/python3 -I - "$vibepollo_package_executable_records" <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
try:
    records = json.loads(sys.argv[1])
    if not isinstance(records, list) or len(records) > 2:
        raise ValueError("invalid executable identity snapshot")
    identities = set()
    names = set()
    for record in records:
        if not isinstance(record, dict) or set(record) != {"path", "device", "inode"}:
            raise ValueError("invalid executable identity snapshot")
        if record["path"] not in paths or record["path"] in names:
            raise ValueError("invalid executable identity snapshot")
        if any(type(record[key]) is not int or record[key] <= 0 for key in ("device", "inode")):
            raise ValueError("invalid executable identity snapshot")
        names.add(record["path"])
        identities.add((record["device"], record["inode"]))
    # Linux retains executable inode identity after unlink/replacement. Scan
    # every scope, using no argv/name matching and issuing no process signals.
    for process in pathlib.Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            executable = process / "exe"
            identity = executable.stat()
            target = os.readlink(executable)
        except (FileNotFoundError, ProcessLookupError):
            continue
        def report_old_owner(deleted=False):
            uid, scope = "unknown", "<unavailable>"
            try:
                # Diagnostics must describe the same observed task and inode.
                # PID reuse or disappearing metadata still blocks replacement;
                # never attribute a different process's UID/scope to this owner.
                before = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                status = (process / "status").read_text()
                uid_lines = [line.split()[1:] for line in status.splitlines() if line.startswith("Uid:")]
                if len(uid_lines) != 1 or len(uid_lines[0]) != 4 or not all(value.isdecimal() for value in uid_lines[0]):
                    raise ValueError("uncertain process UID")
                cgroup = (process / "cgroup").read_text()
                if len(cgroup) > 8192:
                    raise ValueError("oversized process scope")
                after = (process / "stat").read_text().rpartition(") ")[2].split()[19]
                confirmed = executable.stat()
                if before != after or (confirmed.st_dev, confirmed.st_ino) != (identity.st_dev, identity.st_ino):
                    raise ValueError("changed process identity")
                uid, scope = uid_lines[0][1], cgroup.rstrip("\n")
            except (OSError, ValueError, IndexError):
                pass
            kind = "deleted old" if deleted else "old"
            print(f"error: verified {kind} host executable remains in PID {process.name}, UID {uid}, scope {json.dumps(scope)}; replacement is blocked. Force cannot safely stop another scope without its activation authority and session cleanup ownership.", file=sys.stderr)
        if (identity.st_dev, identity.st_ino) in identities:
            report_old_owner()
            sys.exit(1)
        # A failed older transaction may lack a retained inode record. A
        # kernel-reported deleted executable at either exact root-owned package
        # path still supplies positive evidence of an old owner, never absence.
        if target in tuple(name + " (deleted)" for name in paths) and identity.st_uid == 0 and stat.S_ISREG(identity.st_mode) and not identity.st_mode & 0o022:
            report_old_owner(deleted=True)
            sys.exit(1)
except (OSError, ValueError, TypeError) as error:
    print(f"error: cross-scope old host absence could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_package_capture_host_executables() {
  if [ ! -e /usr/bin/vibepollo ] && [ ! -L /usr/bin/vibepollo ] &&
     [ ! -e /usr/libexec/vibeshine/vibepollo-host ] && [ ! -L /usr/libexec/vibeshine/vibepollo-host ]; then
    printf '[]\n'
    return 0
  fi
  [ -x /usr/bin/python3 ] || {
    echo 'error: trusted Python pid/process inspection is required before replacing an installed host.' >&2
    return 1
  }
  timeout --signal=KILL 10 /usr/bin/python3 -I - <<'PY'
import json
import os
import pathlib
import stat
import sys

paths = ("/usr/bin/vibepollo", "/usr/libexec/vibeshine/vibepollo-host")
records = []
try:
    for name in paths:
        path = pathlib.Path(name)
        try:
            entry = path.lstat()
        except FileNotFoundError:
            continue
        if entry.st_uid != 0 or (stat.S_ISLNK(entry.st_mode) and name != paths[0]):
            raise ValueError("unsafe installed host executable")
        resolved = path.resolve(strict=True)
        identity = resolved.stat()
        if identity.st_uid != 0 or not stat.S_ISREG(identity.st_mode) or identity.st_mode & 0o022:
            raise ValueError("unsafe installed host executable")
        # A public package entrypoint may be a root-owned symlink; both its
        # original and resolved directory ancestry must remain root-owned.
        for parent in set((path.parent, *path.parent.parents, resolved.parent, *resolved.parent.parents)):
            owner = parent.stat()
            if owner.st_uid != 0 or not stat.S_ISDIR(owner.st_mode) or owner.st_mode & 0o022:
                raise ValueError("unsafe installed host executable ancestry")
        records.append({"path": name, "device": identity.st_dev, "inode": identity.st_ino})
    print(json.dumps(records, separators=(",", ":")))
except (OSError, ValueError) as error:
    print(f"error: installed host identity could not be verified: {error}", file=sys.stderr)
    sys.exit(1)
PY
}

vibepollo_host_is_stable_or_quiescent() {
  vibepollo_host_state=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=ActiveState --property=SubState --property=MainPID \
    --property=ControlGroup --property=Job 2>/dev/null) || return 1
  vibepollo_host_active=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^ActiveState=//p')
  vibepollo_host_substate=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^SubState=//p')
  vibepollo_host_pid=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^MainPID=//p')
  vibepollo_host_cgroup=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^ControlGroup=//p')
  vibepollo_host_job=$(printf '%%s
' "$vibepollo_host_state" | sed -n 's/^Job=//p')
  [ -z "$vibepollo_host_job" ] || return 1
  if [ "$vibepollo_host_active" = active ] && [ "$vibepollo_host_substate" = running ]; then
    case "$vibepollo_host_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
    case "$vibepollo_host_cgroup" in /*) return 0 ;; *) return 1 ;; esac
  fi
  case "$vibepollo_host_active:$vibepollo_host_substate:$vibepollo_host_pid" in
    inactive:dead:0 | failed:failed:0) vibepollo_cgroup_is_quiescent "$vibepollo_host_cgroup" ;;
    *) return 1 ;;
  esac
}

vibepollo_unit_is_masked() {
  vibepollo_enabled=$(timeout --signal=KILL 5 systemctl is-enabled "$1" 2>/dev/null || true)
  vibepollo_load=$(timeout --signal=KILL 5 systemctl show "$1" \
    --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_enabled" in
    masked | masked-runtime) [ "$vibepollo_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}

vibepollo_restore_template_is_masked() { vibepollo_unit_is_masked 'vibepollo-session-restore@.service'; }
vibepollo_control_socket_is_masked() { vibepollo_unit_is_masked vibeshine-vkms-control.socket; }
vibepollo_broker_socket_is_masked() { vibepollo_unit_is_masked vibepollo-session-exec.socket; }
vibepollo_host_unit_is_masked() { vibepollo_unit_is_masked vibepollo.service; }


# Package barriers are a transaction-owned resource, not administrator policy.
# Never source this record: fixed files contain only validated identities.
vibepollo_package_transaction=/run/vibepollo-package-transaction
vibepollo_package_lock_held=0
vibepollo_package_transaction_is_safe() {
  [ -d "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_package_transaction")" = '0:0:700:directory' ]
}

vibepollo_package_record_is_safe() {
  [ -f "$1" ] && [ ! -L "$1" ] &&
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:600:1:regular file' ] &&
    [ "$(stat -c '%%s' -- "$1")" -le 4096 ]
}

vibepollo_package_write_record() (
  vibepollo_package_transaction_is_safe || exit 1
  vibepollo_package_temporary=$(mktemp "$vibepollo_package_transaction/.record.XXXXXX") || exit 1
  trap 'rm -f -- "$vibepollo_package_temporary"' 0
  chmod 0600 -- "$vibepollo_package_temporary" || exit 1
  printf '%%s\n' "$2" >"$vibepollo_package_temporary" || exit 1
  mv -fT -- "$vibepollo_package_temporary" "$vibepollo_package_transaction/$1"
)

vibepollo_package_path_identity() {
  if [ ! -e "$1" ] && [ ! -L "$1" ]; then
    printf 'absent\n'
  elif [ -L "$1" ] && [ "$(readlink -- "$1")" = /dev/null ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  elif [ "$1" = "$vibepollo_host_upgrade_dropin" ] &&
       [ -f "$1" ] && [ ! -L "$1" ] &&
       [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$1")" = '0:0:644:1:regular file' ]; then
    stat -c '%%d:%%i:%%u:%%g:%%F' -- "$1"
  else
    return 1
  fi
}

vibepollo_package_snapshot_path() {
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  case "$vibepollo_package_identity" in
    absent | *:0:0:'symbolic link' | *:0:0:'regular file') ;;
    *) return 1 ;;
  esac
  vibepollo_package_write_record "$1.before" "$vibepollo_package_identity"
}

vibepollo_package_cleanup_record_temporaries() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_has_temporary=0
  # First validate the complete set, so one malformed entry prevents every
  # deletion. Writers inherit fd 8; a fresh caller cannot hold the lock while
  # an interrupted operation's surviving writer is still publishing a record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    vibepollo_package_has_temporary=1
    vibepollo_package_suffix=${vibepollo_package_path##*/}
    case "$vibepollo_package_suffix" in
      .record.*) vibepollo_package_suffix=${vibepollo_package_suffix#.record.} ;;
      .mask.*) vibepollo_package_suffix=${vibepollo_package_suffix#.mask.} ;;
      *) return 1 ;;
    esac
    [ "${#vibepollo_package_suffix}" -eq 6 ] || return 1
    case "$vibepollo_package_suffix" in *[!A-Za-z0-9]*) return 1 ;; esac
    if [ -L "$vibepollo_package_path" ]; then
      case "${vibepollo_package_path##*/}" in .mask.*) ;; *) return 1 ;; esac
      [ "$(readlink -- "$vibepollo_package_path")" = /dev/null ] || return 1
      vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%h:%%F' -- "$vibepollo_package_path") || return 1
      case "$vibepollo_package_temporary_attributes" in
        '0:0:1:symbolic link' | '0:0:2:symbolic link') continue ;;
        *) return 1 ;;
      esac
    fi
    [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
    vibepollo_package_temporary_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path") || return 1
    case "$vibepollo_package_temporary_attributes" in
      '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;;
      *) return 1 ;;
    esac
  done
  [ "$vibepollo_package_has_temporary" -eq 1 ] || return 0
  [ "${vibepollo_package_lock_held:-0}" -eq 1 ] || return 1
  flock --nonblock 8 || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/.record.* "$vibepollo_package_transaction"/.mask.*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
}

vibepollo_package_snapshot_host_executables() {
  if [ -e "$vibepollo_package_transaction/host-executables.before" ] || [ -L "$vibepollo_package_transaction/host-executables.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/host-executables.before"
    return $?
  fi
  vibepollo_package_executables=$(vibepollo_package_capture_host_executables) || return 1
  vibepollo_package_write_record host-executables.before "$vibepollo_package_executables"
}

vibepollo_package_lock() {
  [ "${vibepollo_package_lock_held:-0}" -eq 0 ] || return 0
  vibepollo_package_transaction_is_safe || return 1
  if [ -e "$vibepollo_package_transaction/lock" ] || [ -L "$vibepollo_package_transaction/lock" ]; then
    [ -f "$vibepollo_package_transaction/lock" ] && [ ! -L "$vibepollo_package_transaction/lock" ] || return 1
    vibepollo_package_lock_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_transaction/lock") || return 1
    case "$vibepollo_package_lock_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  else
    (umask 077; set -C; printf 'package-lock\n' >"$vibepollo_package_transaction/lock") || return 1
  fi
  exec 8<>"$vibepollo_package_transaction/lock" || return 1
  flock --nonblock 8 || return 1
  vibepollo_package_lock_held=1
  vibepollo_package_cleanup_record_temporaries
}

vibepollo_package_properties_are_unique() {
  vibepollo_package_property_output=$1
  shift
  [ "$(printf '%%s\n' "$vibepollo_package_property_output" | wc -l | tr -d ' ')" = "$#" ] || return 1
  for vibepollo_package_property do
    [ "$(printf '%%s\n' "$vibepollo_package_property_output" | grep -c "^$vibepollo_package_property=" || true)" = 1 ] || return 1
  done
}

vibepollo_package_controller_identity_is_safe() {
  vibepollo_package_properties_are_unique "$1" MainPID ControlGroup FreezerState InvocationID || return 1
  vibepollo_package_identity_pid=$(printf '%%s\n' "$1" | sed -n 's/^MainPID=//p')
  vibepollo_package_identity_group=$(printf '%%s\n' "$1" | sed -n 's/^ControlGroup=//p')
  vibepollo_package_identity_invocation=$(printf '%%s\n' "$1" | sed -n 's/^InvocationID=//p')
  vibepollo_package_identity_freezer=$(printf '%%s\n' "$1" | sed -n 's/^FreezerState=//p')
  case "$vibepollo_package_identity_pid" in '' | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_package_identity_group" in '' | /*) ;; *) return 1 ;; esac
  case "$vibepollo_package_identity_group" in */../* | */.. | *'
'*) return 1 ;; esac
  case "$vibepollo_package_identity_freezer" in '' | running | frozen) ;; *) return 1 ;; esac
  if [ "$vibepollo_package_identity_pid" != 0 ]; then
    [ -n "$vibepollo_package_identity_group" ] && [ -n "$vibepollo_package_identity_freezer" ] || return 1
  fi
  if [ "$vibepollo_package_identity_pid" = 0 ] && [ -z "$vibepollo_package_identity_invocation" ]; then return 0; fi
  [ "${#vibepollo_package_identity_invocation}" -eq 32 ] || return 1
  case "$vibepollo_package_identity_invocation" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_record_directory_is_safe() {
  vibepollo_package_transaction_is_safe || return 1
  # Refuse unknown files rather than recursively deleting a damaged record.
  for vibepollo_package_path in "$vibepollo_package_transaction"/* "$vibepollo_package_transaction"/.[!.]* "$vibepollo_package_transaction"/..?*; do
    [ -e "$vibepollo_package_path" ] || [ -L "$vibepollo_package_path" ] || continue
    case "${vibepollo_package_path##*/}" in
      lock | phase | controller | handoff.before | host-executables.before | host-stop.before | run-id | failure-class | legacy-adopted | fence.before | fence.owned | vibepollo.service.before | vibepollo.service.owned | vibepollo-session-exec.socket.before | vibepollo-session-exec.socket.owned | vibeshine-vkms-control.socket.before | vibeshine-vkms-control.socket.owned | vibepollo-session-restore@.service.before | vibepollo-session-restore@.service.owned) ;;
      *) return 1 ;;
    esac
    if [ "${vibepollo_package_path##*/}" = lock ] && [ ! -s "$vibepollo_package_path" ]; then
      [ -f "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] &&
        [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_path")" = '0:0:600:1:regular empty file' ] || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_path" || return 1
    fi
  done
}

vibepollo_package_run_id_is_safe() {
  [ "${#1}" -eq 32 ] || return 1
  case "$1" in *[!0-9a-f]*) return 1 ;; esac
}

vibepollo_package_preserve_unowned_host_mask() {
  vibepollo_package_preserve_host_mask=0
  for vibepollo_package_mask_path in /etc/systemd/system/vibepollo.service /run/systemd/system/vibepollo.service; do
    [ -L "$vibepollo_package_mask_path" ] || continue
    [ "$(readlink -- "$vibepollo_package_mask_path")" = /dev/null ] || continue
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    case "$vibepollo_package_mask_identity" in
      *:0:0:'symbolic link') ;;
      *) echo "error: unsafe host mask at $vibepollo_package_mask_path; package operation is blocked." >&2; return 1 ;;
    esac
    if [ "$vibepollo_package_mask_path" = /run/systemd/system/vibepollo.service ] &&
       { [ -e "$vibepollo_package_transaction/vibepollo.service.owned" ] || [ -L "$vibepollo_package_transaction/vibepollo.service.owned" ]; }; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/vibepollo.service.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/vibepollo.service.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    vibepollo_package_preserve_host_mask=1
  done
  [ "$vibepollo_package_preserve_host_mask" -eq 1 ] || return 0
  vibepollo_package_mask_state=$(timeout --signal=KILL 5 systemctl is-enabled vibepollo.service 2>/dev/null || true)
  vibepollo_package_mask_load=$(timeout --signal=KILL 5 systemctl show vibepollo.service --property=LoadState 2>/dev/null) || vibepollo_package_mask_load=uncertain
  vibepollo_package_mask_verified=0
  case "$vibepollo_package_mask_state:$vibepollo_package_mask_load" in
    masked:LoadState=masked | masked-runtime:LoadState=masked) vibepollo_package_mask_verified=1 ;;
  esac
  if [ "$vibepollo_package_mask_verified" -ne 1 ] ||
     ! vibepollo_unit_is_quiescent vibepollo.service ||
     ! vibepollo_package_no_jobs; then
    echo 'error: an unowned Vibepollo host mask exists, but the host is active, populated, has a pending job, or its state is uncertain; package replacement is blocked. Force cannot remove an administrator or legacy mask without proving its activation ownership.' >&2
    return 1
  fi
  # Preserve the actual mask and any unowned fence byte-for-byte. The mask
  # supplies no-start admission while the full generation proof still runs.
  return 0
}

vibepollo_package_prepare_postinstall_start() {
  vibepollo_package_start_deferred=0
  vibepollo_package_preserve_unowned_host_mask || return 1
  if [ "$vibepollo_package_preserve_host_mask" -eq 1 ]; then
    vibepollo_package_start_deferred=1
  fi
  for vibepollo_package_start_unit in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    for vibepollo_package_start_directory in /etc/systemd/system /run/systemd/system; do
      vibepollo_package_start_path=$vibepollo_package_start_directory/$vibepollo_package_start_unit
      [ -L "$vibepollo_package_start_path" ] || continue
      [ "$(readlink -- "$vibepollo_package_start_path")" = /dev/null ] || continue
      vibepollo_package_start_identity=$(vibepollo_package_path_identity "$vibepollo_package_start_path") || return 1
      case "$vibepollo_package_start_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      if [ "$vibepollo_package_start_directory" = /run/systemd/system ] &&
         { [ -e "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" ]; }; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned" || return 1
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_start_unit.owned")" = "$vibepollo_package_start_identity" ] || return 1
        continue
      fi
      vibepollo_package_start_deferred=1
    done
  done
  if [ -e "$vibepollo_package_transaction/fence.before" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
    vibepollo_package_prior_fence=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
    if [ "$vibepollo_package_prior_fence" != absent ]; then
      [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_prior_fence" ] || return 1
      vibepollo_package_start_deferred=1
    fi
  fi
  [ "$vibepollo_package_start_deferred" -eq 1 ] || return 0
  # This must be the full actual generation proof (controller, broker/apps,
  # restore/control instances, session record and retained executable inodes).
  # The local host-empty + no-jobs classifier is insufficient for release.
  if ! vibepollo_package_complete_quiescence_is_safe || ! vibepollo_package_release; then
    echo 'error: safe package shutdown recovery could not finish; automatic service activation remains deferred. Resolve the preceding check and retry package configuration; administrator masks were preserved.' >&2
    return 1
  fi
  echo 'package: preserved administrator host mask or start fence; configuration completed and automatic service activation is deferred.' >&2
  return 0
}

vibepollo_package_bind_run() {
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    vibepollo_package_write_record run-id "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
  elif [ ! -e "$vibepollo_package_transaction/run-id" ] && [ ! -L "$vibepollo_package_transaction/run-id" ]; then
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record failure-class none
}

vibepollo_package_run_is_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
  vibepollo_package_record_run=$(cat -- "$vibepollo_package_transaction/run-id") || return 1
  vibepollo_package_run_id_is_safe "$vibepollo_package_record_run" || return 1
  [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ] || return 0
  vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" &&
    [ "$vibepollo_package_record_run" = "$VIBEPOLLO_PACKAGE_RUN_ID" ]
}

vibepollo_package_force_is_applicable() (
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" &&
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/failure-class" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = late ] &&
    [ "$(cat -- "$vibepollo_package_transaction/failure-class")" = host-stop-deadline ]
)

vibepollo_package_refresh_missing_owned() {
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || continue
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    if [ "$vibepollo_package_current" = absent ]; then
      rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
    else
      [ "$vibepollo_package_current" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" ] || return 1
    fi
  done
}

vibepollo_package_cleanup_retired() (
  vibepollo_package_original_root=$vibepollo_package_transaction
  vibepollo_package_retired_root=$vibepollo_package_original_root.retired
  [ -e "$vibepollo_package_retired_root" ] || [ -L "$vibepollo_package_retired_root" ] || return 0
  # This independent root-owned lock survives removal of the retired record's
  # own lock inode, including SIGKILL between the final unlink and rmdir.
  vibepollo_package_gc_lock=$vibepollo_package_original_root.gc-lock
  if [ ! -e "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ]; then
    (umask 077; set -C; printf 'package-gc\n' >"$vibepollo_package_gc_lock") || return 1
  fi
  [ -f "$vibepollo_package_gc_lock" ] && [ ! -L "$vibepollo_package_gc_lock" ] || return 1
  vibepollo_package_gc_attributes=$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_package_gc_lock") || return 1
  case "$vibepollo_package_gc_attributes" in '0:0:600:1:regular file' | '0:0:600:1:regular empty file') ;; *) return 1 ;; esac
  exec 7<>"$vibepollo_package_gc_lock" || return 1
  timeout --signal=KILL 5 flock --exclusive 7 || return 1
  vibepollo_package_transaction=$vibepollo_package_retired_root
  [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ] || return 0
  vibepollo_package_transaction_is_safe && vibepollo_package_record_directory_is_safe || return 1
  for vibepollo_package_path in "$vibepollo_package_transaction"/*; do
    [ -e "$vibepollo_package_path" ] || continue
    rm -f -- "$vibepollo_package_path" || return 1
  done
  rmdir -- "$vibepollo_package_transaction" || return 1
)

vibepollo_package_adopt_legacy_barriers() {
  # Only the exact old package signature, with every admission path closed
  # and the complete old generation drained, permits ownership migration.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.before") || return 1
  vibepollo_package_adoption_pending=0
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    vibepollo_package_adoption_pending=1
    # The marker is published only after the exact old fence, all original
    # mask identities and full generation proof were verified. An interrupted
    # conversion may leave some .before snapshots unconverted when old package
    # unwind removes the fence. Resume only those original masks, never a
    # replacement path or an unrecorded new mask.
    if [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ]; then
      vibepollo_package_complete_quiescence_is_safe || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      if [ -e "$vibepollo_package_transaction/fence.owned" ] || [ -L "$vibepollo_package_transaction/fence.owned" ]; then
        vibepollo_package_record_is_safe "$vibepollo_package_transaction/fence.owned" || return 1
      fi
      # Validate every path before changing any record. Repeat the identity
      # checks during publication so interruption leaves resumable metadata.
      for vibepollo_package_adoption_pass in validate commit; do
        for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
          vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
          if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
            vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
            vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
          fi
          vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
          if [ "$vibepollo_package_current" != absent ]; then
            case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
            [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
            [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
          fi
          [ "$vibepollo_package_adoption_pass" = commit ] || continue
          if [ "$vibepollo_package_current" = absent ]; then
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
            rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          else
            vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
            vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
          fi
        done
      done
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      vibepollo_package_write_record fence.before absent || return 1
      rm -f -- "$vibepollo_package_transaction/fence.owned" || return 1
      vibepollo_package_write_record phase preflight || return 1
      [ ! -e "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
      rm -f -- "$vibepollo_package_transaction/legacy-adopted" || return 1
      return 0
    fi
  fi
  [ "$vibepollo_package_before" != absent ] || [ "$vibepollo_package_adoption_pending" -eq 1 ] || return 0
  [ -f "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = '0:0:644:1:regular file' ] || return 1
  vibepollo_package_legacy_contents=$(cat -- "$vibepollo_host_upgrade_dropin") || return 1
  case "$vibepollo_package_legacy_contents" in
    '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=mixed
SendSIGKILL=no' | '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no') ;;
    *) [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1; return 0 ;;
  esac
  [ "$(stat -c '%%s' -- "$vibepollo_host_upgrade_dropin")" = "$(printf '%%s\n' "$vibepollo_package_legacy_contents" | wc -c | tr -d ' ')" ] || {
    [ "$vibepollo_package_adoption_pending" -eq 0 ] || return 1
    return 0
  }
  vibepollo_package_complete_quiescence_is_safe || {
    echo 'error: an earlier package fence remains, but the old host or session has not fully drained; it cannot be adopted safely.' >&2
    return 1
  }
  # Generation inspection can take time. An administrator may edit the same
  # inode while it runs; do not convert authority for changed fence bytes.
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_current=$(vibepollo_package_path_identity "/run/systemd/system/$vibepollo_package_name") || return 1
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    [ "$(readlink -- "/run/systemd/system/$vibepollo_package_name")" = /dev/null ] || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
  done
  if [ -e "$vibepollo_package_transaction/fence.owned" ]; then
    vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/fence.owned") || return 1
  fi
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_before" ] || return 1
  vibepollo_package_adoption_fence_identity=$vibepollo_package_before
  vibepollo_package_write_record legacy-adopted yes || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    # Recheck each original identity at publication, rather than claiming a
    # replacement that appeared after the inventory pass.
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned") || return 1
    else
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
      vibepollo_package_expected=$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before") || return 1
    fi
    vibepollo_package_current=$(vibepollo_package_path_identity "$vibepollo_package_owned_path") || return 1
    [ "$vibepollo_package_current" = "$vibepollo_package_expected" ] || return 1
    if [ "$vibepollo_package_name" != fence ]; then
      [ "$(readlink -- "$vibepollo_package_owned_path")" = /dev/null ] || return 1
    fi
    vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_current" || return 1
    vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
  done
  vibepollo_package_write_record phase preflight || return 1
  [ "$(vibepollo_package_path_identity "$vibepollo_host_upgrade_dropin")" = "$vibepollo_package_adoption_fence_identity" ] || return 1
  printf '%%s\n' "$vibepollo_package_legacy_contents" |
    cmp -s -- - "$vibepollo_host_upgrade_dropin" || return 1
  # Every owned identity and before snapshot is now durable; the pending
  # adoption marker is no longer needed by later release/retry operations.
  rm -f -- "$vibepollo_package_transaction/legacy-adopted"
}

vibepollo_package_rebaseline_unwound_run() {
  # Pending legacy authority is not a wholly unwound transaction even when
  # no .owned file has been published yet. Reconcile it before a new target.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/legacy-adopted" &&
      [ "$(cat -- "$vibepollo_package_transaction/legacy-adopted")" = yes ] || return 1
    return 0
  fi
  # A native package retry, or a fresh explicit installer operation, may
  # select the current healthy invocation after every owned barrier unwound.
  # This grants a fresh target, never proof that old process resources vanished.
  if [ -n "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    vibepollo_package_run_id_is_safe "$VIBEPOLLO_PACKAGE_RUN_ID" || return 1
    [ -e "$vibepollo_package_transaction/run-id" ] || return 0
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/run-id" || return 1
    # A same-run force retry must retain its original invocation identity.
    [ "$(cat -- "$vibepollo_package_transaction/run-id")" != "$VIBEPOLLO_PACKAGE_RUN_ID" ] || return 0
  fi
  for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
    [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 0
  done
  vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
  for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_name" = fence ]; then vibepollo_package_owned_path=$vibepollo_host_upgrade_dropin
    else vibepollo_package_owned_path=/run/systemd/system/$vibepollo_package_name; fi
    [ "$(vibepollo_package_path_identity "$vibepollo_package_owned_path")" = "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" ] || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  if [ -z "${VIBEPOLLO_PACKAGE_RUN_ID:-}" ]; then
    # A native retry is a new operation. Retire any earlier wrapper's nonce
    # atomically before refreshing phase or discarding its saved stop identity.
    vibepollo_package_new_run=$(cat /proc/sys/kernel/random/uuid | tr -d '-') || return 1
    vibepollo_package_run_id_is_safe "$vibepollo_package_new_run" || return 1
    vibepollo_package_write_record run-id "$vibepollo_package_new_run" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  rm -f -- "$vibepollo_package_transaction/host-stop.before" || return 1
  vibepollo_package_prior_phase=early
}

vibepollo_package_begin() {
  vibepollo_package_cleanup_retired || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_record_directory_is_safe || return 1
    vibepollo_package_refresh_missing_owned || return 1
    if [ ! -e "$vibepollo_package_transaction/phase" ] && [ ! -L "$vibepollo_package_transaction/phase" ]; then
      for vibepollo_package_path in "$vibepollo_package_transaction"/*.owned; do
        [ ! -e "$vibepollo_package_path" ] && [ ! -L "$vibepollo_package_path" ] || return 1
      done
      vibepollo_package_forget || return 1
      vibepollo_package_begin
      return $?
    fi
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
    vibepollo_package_prior_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
    case "$vibepollo_package_prior_phase" in early | late | preflight) ;; *) return 1 ;; esac
    for vibepollo_package_name in fence vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    done
    vibepollo_package_snapshot_host_executables || return 1
    vibepollo_package_rebaseline_unwound_run || return 1
    vibepollo_package_bind_run || return 1
    vibepollo_package_adopt_legacy_barriers || return 1
    return 0
  fi
  # mkdir is exclusive; a competing package operation may not share ownership.
  (umask 077; mkdir -- "$vibepollo_package_transaction") || return 1
  vibepollo_package_transaction_created=1
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_lock || return 1
  vibepollo_package_snapshot_host_executables || return 1
  vibepollo_package_prior_phase=early
  vibepollo_package_snapshot_path fence "$vibepollo_host_upgrade_dropin" || return 1
  vibepollo_package_handoff=absent
  if [ -e "${vibepollo_legacy_handoff:-/usr/libexec/vibeshine/vibepollo-session-handoff}" ] || [ -L "$vibepollo_legacy_handoff" ]; then
    [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] || return 1
    vibepollo_package_handoff=$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff") || return 1
    case "$vibepollo_package_handoff" in *:0:0:755:1 | *:0:0:0:1) ;; *) return 1 ;; esac
  fi
  vibepollo_package_write_record handoff.before "$vibepollo_package_handoff" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    vibepollo_package_snapshot_path "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" || return 1
    vibepollo_package_write_record controller "$vibepollo_package_controller" || return 1
  fi
  vibepollo_package_write_record phase early || return 1
  vibepollo_package_bind_run || return 1
  vibepollo_package_adopt_legacy_barriers
}

vibepollo_package_record_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 0
  vibepollo_package_identity=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_identity" != absent ] || return 1
  if [ -e "$vibepollo_package_transaction/$1.owned" ]; then
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_identity" ]
  else
    vibepollo_package_write_record "$1.owned" "$vibepollo_package_identity"
  fi
}

vibepollo_package_mask() (
  vibepollo_package_mask_stage=
  trap '[ -z "$vibepollo_package_mask_stage" ] || rm -f -- "$vibepollo_package_mask_stage"' 0
  for vibepollo_package_name do
    case "$vibepollo_package_name" in
      vibepollo.service | vibepollo-session-exec.socket | vibeshine-vkms-control.socket | vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.before" || return 1
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = absent ] &&
        [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
      continue
    fi
    if [ "$vibepollo_package_mask_identity" = absent ]; then
      # Keep a private inode as creation evidence. Publish its ownership intent
      # before linking it, so interruption cannot strand our published mask.
      # ln -P -T never replaces an administrator's competing path.
      vibepollo_package_mask_stage=$(mktemp "$vibepollo_package_transaction/.mask.XXXXXX") || return 1
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      ln -s /dev/null "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_created_mask=$(vibepollo_package_path_identity "$vibepollo_package_mask_stage") || return 1
      case "$vibepollo_package_created_mask" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
      vibepollo_package_write_record "$vibepollo_package_name.before" absent || return 1
      vibepollo_package_write_record "$vibepollo_package_name.owned" "$vibepollo_package_created_mask" || return 1
      if ! ln -P -T -- "$vibepollo_package_mask_stage" "$vibepollo_package_mask_path" 2>/dev/null; then
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        if [ "$vibepollo_package_mask_identity" != "$vibepollo_package_created_mask" ]; then
          # An exclusive-link failure published none of our inode. Drop only
          # our exact intent, then preserve a valid competing external mask.
          vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_created_mask" ] || return 1
          rm -f -- "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
          [ "$vibepollo_package_mask_identity" != absent ] || return 1
        fi
      else
        vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
        [ "$vibepollo_package_mask_identity" = "$vibepollo_package_created_mask" ] || return 1
      fi
      rm -f -- "$vibepollo_package_mask_stage" || return 1
      vibepollo_package_mask_stage=
      if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then continue; fi
    fi
    # Existing unowned masks, including ones added since a failed attempt's
    # snapshot, remain policy. Refresh their preserved baseline, never claim.
    case "$vibepollo_package_mask_identity" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_write_record "$vibepollo_package_name.before" "$vibepollo_package_mask_identity" || return 1
    [ "$(vibepollo_package_path_identity "$vibepollo_package_mask_path")" = "$vibepollo_package_mask_identity" ] || return 1
  done
  timeout --signal=KILL 15 systemctl mask --runtime "$@" 2>/dev/null
  vibepollo_package_mask_status=$?
  # The manager can reload our prepublished masks, but cannot establish their
  # ownership. Even a timed-out client must leave every recorded inode intact.
  for vibepollo_package_name do
    vibepollo_package_mask_path=/run/systemd/system/$vibepollo_package_name
    vibepollo_package_mask_identity=$(vibepollo_package_path_identity "$vibepollo_package_mask_path") || return 1
    if [ -e "$vibepollo_package_transaction/$vibepollo_package_name.owned" ] || [ -L "$vibepollo_package_transaction/$vibepollo_package_name.owned" ]; then
      vibepollo_package_record_is_safe "$vibepollo_package_transaction/$vibepollo_package_name.owned" || return 1
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.owned")" = "$vibepollo_package_mask_identity" ] || return 1
    else
      [ "$(cat -- "$vibepollo_package_transaction/$vibepollo_package_name.before")" = "$vibepollo_package_mask_identity" ] || return 1
    fi
  done
  return "$vibepollo_package_mask_status"
)

vibepollo_package_mark_late() {
  vibepollo_package_write_record phase late
}

vibepollo_package_remove_owned() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
  vibepollo_package_before=$(cat -- "$vibepollo_package_transaction/$1.before") || return 1
  [ "$vibepollo_package_before" = absent ] || return 0
  vibepollo_package_current=$(vibepollo_package_path_identity "$2") || return 1
  [ "$vibepollo_package_current" != absent ] || return 0
  if [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ]; then
    # An idle Arch retry can skip mask creation. Reconcile an external mask
    # added since the snapshot without claiming it or reopening its admission.
    # New package masks publish intent first, so a missing record is not ours.
    case "$1:$2" in
      vibepollo.service:/run/systemd/system/vibepollo.service | vibepollo-session-exec.socket:/run/systemd/system/vibepollo-session-exec.socket | vibeshine-vkms-control.socket:/run/systemd/system/vibeshine-vkms-control.socket | vibepollo-session-restore@.service:/run/systemd/system/vibepollo-session-restore@.service) ;;
      *) return 1 ;;
    esac
    case "$vibepollo_package_current" in *:0:0:'symbolic link') ;; *) return 1 ;; esac
    vibepollo_package_lock || return 1
    vibepollo_package_complete_quiescence_is_safe || return 1
    [ ! -e "$vibepollo_package_transaction/$1.owned" ] && [ ! -L "$vibepollo_package_transaction/$1.owned" ] || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.before" || return 1
    [ "$(cat -- "$vibepollo_package_transaction/$1.before")" = absent ] || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    vibepollo_package_write_record "$1.before" "$vibepollo_package_current" || return 1
    [ "$(vibepollo_package_path_identity "$2")" = "$vibepollo_package_current" ] || return 1
    return 0
  fi
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/$1.owned" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/$1.owned")" = "$vibepollo_package_current" ] || return 1
  rm -f -- "$2"
}

vibepollo_package_no_jobs() {
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_package_job=$(timeout --signal=KILL 5 systemctl show "$vibepollo_package_name" --property=Job 2>/dev/null) || return 1
    [ "$vibepollo_package_job" = 'Job=' ] || return 1
  done
}

vibepollo_package_restore_barriers() {
  vibepollo_package_keep_legacy=${1:-0}
  vibepollo_package_transaction_is_safe || return 1
  # An interrupted ownership conversion must finish (or refuse a changed
  # legacy fence) before release can remove any path by its saved inode.
  if [ -e "$vibepollo_package_transaction/legacy-adopted" ] || [ -L "$vibepollo_package_transaction/legacy-adopted" ]; then
    vibepollo_package_adopt_legacy_barriers || return 1
  fi
  vibepollo_package_remove_owned fence "$vibepollo_host_upgrade_dropin" || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-exec.socket vibeshine-vkms-control.socket 'vibepollo-session-restore@.service'; do
    if [ "$vibepollo_package_keep_legacy" -eq 1 ] && [ "$vibepollo_package_name" = 'vibepollo-session-restore@.service' ]; then continue; fi
    vibepollo_package_remove_owned "$vibepollo_package_name" "/run/systemd/system/$vibepollo_package_name" || return 1
  done
  timeout --signal=KILL 15 systemctl daemon-reload || return 1
}

vibepollo_package_forget() {
  # Failed initial adoption can already own barriers. Retain its authority
  # until a proved resume completes; a created-directory flag cannot erase it.
  [ ! -e "$vibepollo_package_transaction/legacy-adopted" ] &&
    [ ! -L "$vibepollo_package_transaction/legacy-adopted" ] || return 1
  vibepollo_package_transaction_is_safe && vibepollo_package_cleanup_record_temporaries &&
    vibepollo_package_record_directory_is_safe || return 1
  vibepollo_package_forget_root=$vibepollo_package_transaction
  [ ! -e "$vibepollo_package_forget_root.retired" ] && [ ! -L "$vibepollo_package_forget_root.retired" ] || return 1
  mv -T -- "$vibepollo_package_forget_root" "$vibepollo_package_forget_root.retired" || return 1
  exec 8>&-
  vibepollo_package_lock_held=0
  vibepollo_package_cleanup_retired
}

vibepollo_package_restore_legacy_handoff() {
  # Only failed/cancelled transactions restore the same installed old inode.
  # Successful replacement deliberately retires this obsolete admission path.
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/handoff.before" || return 1
  vibepollo_package_handoff=$(cat -- "$vibepollo_package_transaction/handoff.before") || return 1
  case "$vibepollo_package_handoff" in
    *:0:0:755:1)
      vibepollo_package_handoff_expected=${vibepollo_package_handoff%%:755:1}:0:1
      if [ -f "$vibepollo_legacy_handoff" ] && [ ! -L "$vibepollo_legacy_handoff" ] &&
         [ "$(stat -c '%%d:%%i:%%u:%%g:%%a:%%h' -- "$vibepollo_legacy_handoff")" = "$vibepollo_package_handoff_expected" ]; then
        chmod 0755 -- "$vibepollo_legacy_handoff" || return 1
      fi
      ;;
    absent | *:0:0:0:1) ;;
    *) return 1 ;;
  esac
}

vibepollo_package_restore_early() {
  vibepollo_package_transaction_is_safe || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = early ] || return 1
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_package_no_jobs && vibepollo_host_is_stable_or_quiescent || return 1
    vibepollo_package_record_is_safe "$vibepollo_package_transaction/controller" || return 1
    vibepollo_package_controller=$(cat -- "$vibepollo_package_transaction/controller") || return 1
    vibepollo_package_current_controller=$(timeout --signal=KILL 5 systemctl show \
      vibepollo-session-controller.service --property=MainPID --property=ControlGroup \
      --property=FreezerState --property=InvocationID 2>/dev/null) || return 1
    vibepollo_package_controller_identity_is_safe "$vibepollo_package_controller" &&
      vibepollo_package_controller_identity_is_safe "$vibepollo_package_current_controller" || return 1
    # Thaw only our original exact unit identity, never an administrator's freeze.
    vibepollo_package_original_identity=$(printf '%%s\n' "$vibepollo_package_controller" | sed '/^FreezerState=/d')
    vibepollo_package_current_identity=$(printf '%%s\n' "$vibepollo_package_current_controller" | sed '/^FreezerState=/d')
    [ "$vibepollo_package_original_identity" = "$vibepollo_package_current_identity" ] || return 1
    vibepollo_package_restore_barriers || return 1
    if printf '%%s\n' "$vibepollo_package_controller" | grep -qx 'FreezerState=running' &&
       printf '%%s\n' "$vibepollo_package_current_controller" | grep -qx 'FreezerState=frozen'; then
      timeout --signal=KILL 15 systemctl thaw vibepollo-session-controller.service 2>/dev/null || return 1
    fi
    vibepollo_controller_was_frozen=0
  fi
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget
}

vibepollo_package_release() {
  # Called only after the old generation has completely drained.
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket; do
    vibepollo_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  [ ! -e /run/vibepollo/session.env ] && [ ! -L /run/vibepollo/session.env ] || return 1
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  if [ -e "$vibepollo_package_transaction" ] || [ -L "$vibepollo_package_transaction" ]; then
    vibepollo_package_lock || return 1
    vibepollo_package_restore_barriers 1 || return 1
    vibepollo_package_forget || return 1
  fi
}

vibepollo_package_recover_failed_transaction() (
  # The caller must wait for its package-manager child before invoking this.
  # Releasing a preflight while replacement is still executing is forbidden.
  if [ ! -e "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ]; then return 0; fi
  vibepollo_package_lock || return 1
  vibepollo_package_run_is_owned || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  vibepollo_package_recovery_phase=$(cat -- "$vibepollo_package_transaction/phase") || return 1
  case "$vibepollo_package_recovery_phase" in
    early) vibepollo_package_restore_early; return $? ;;
    late | preflight) ;;
    *) return 1 ;;
  esac
  vibepollo_package_complete_quiescence_is_safe || return 1
  vibepollo_package_restore_barriers || return 1
  vibepollo_package_restore_legacy_handoff || return 1
  vibepollo_package_forget || return 1
  echo 'package recovery: old host and session cleanup verified; transaction-owned admission fences released without starting services.' >&2
)








vibepollo_package_complete_quiescence_is_safe() {
  vibepollo_package_no_jobs || return 1
  vibepollo_package_unmanaged_host_absent || return 1
  for vibepollo_package_name in vibepollo.service vibepollo-session-controller.service vibepollo-session-exec.socket vibeshine-vkms-control.socket vibepollo-prelogin.service vibepollo-machine-prepare.service; do
    vibepollo_unit_is_quiescent "$vibepollo_package_name" || return 1
  done
  vibepollo_control_instances_are_quiescent &&
    vibepollo_brokers_are_quiescent &&
    vibepollo_restore_instances_are_quiescent || return 1
  for vibepollo_package_runtime_path in /run/vibepollo/session.env "$vibepollo_broker_socket" "$vibepollo_control_socket" "$vibepollo_legacy_handoff_directory" "$vibepollo_legacy_restore_directory" "$vibepollo_legacy_transition_lock" "$vibepollo_legacy_prelogin_marker"; do
    [ -n "$vibepollo_package_runtime_path" ] && [ ! -e "$vibepollo_package_runtime_path" ] && [ ! -L "$vibepollo_package_runtime_path" ] || return 1
  done
}

vibepollo_package_preserved_preflight_is_safe() {
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = preflight ] || return 1
  vibepollo_restore_template_is_masked && vibepollo_control_socket_is_masked &&
    vibepollo_broker_socket_is_masked && vibepollo_host_unit_is_masked || return 1
  vibepollo_package_complete_quiescence_is_safe
}

vibepollo_package_open_owned_host_for_stop() {
  if [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ]; then return 0; fi
  # A prior late stop may finish after its client timed out. Re-expose the
  # installed unit only when the old cgroup is empty, retaining its start fence.
  vibepollo_unit_is_quiescent vibepollo.service && vibepollo_package_no_jobs || return 1
  vibepollo_package_remove_owned vibepollo.service /run/systemd/system/vibepollo.service || return 1
  [ ! -e /run/systemd/system/vibepollo.service ] && [ ! -L /run/systemd/system/vibepollo.service ] || return 1
  rm -f -- "$vibepollo_package_transaction/vibepollo.service.owned"
}











vibepollo_finish_removal() (
  if [ ! -e "$vibepollo_package_transaction" ] && [ ! -L "$vibepollo_package_transaction" ]; then return 0; fi
  vibepollo_package_lock || return 1
  vibepollo_package_record_is_safe "$vibepollo_package_transaction/phase" || return 1
  [ "$(cat -- "$vibepollo_package_transaction/phase")" = preflight ] || return 1
  # This runs after removal, when an old executable may already be unlinked.
  # The retained inode snapshot still proves no process in any scope owns it.
  vibepollo_package_unmanaged_host_absent || return 1
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    timeout --signal=KILL 15 systemctl daemon-reload || return 1
    vibepollo_package_complete_quiescence_is_safe || return 1
    vibepollo_package_release || return 1
  else
    for vibepollo_package_runtime_path in /run/vibepollo/session.env "$vibepollo_broker_socket" "$vibepollo_control_socket" "$vibepollo_legacy_acl" "$vibepollo_legacy_handoff_directory" "$vibepollo_legacy_restore_directory" "$vibepollo_legacy_transition_lock" "$vibepollo_legacy_prelogin_marker" "$vibepollo_host_upgrade_dropin" /run/systemd/system/vibepollo.service /run/systemd/system/vibepollo-session-exec.socket /run/systemd/system/vibeshine-vkms-control.socket; do
      [ ! -e "$vibepollo_package_runtime_path" ] && [ ! -L "$vibepollo_package_runtime_path" ] || return 1
    done
    vibepollo_package_forget || return 1
  fi
)

if [ "$1" -eq 0 ]; then
  vibepollo_finish_removal || {
    echo 'error: post-removal cleanup could not prove old host resources empty; transaction barriers are retained for retry.' >&2
    exit 1
  }
fi

%files
# Executables
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-display-power
%{_bindir}/vibepollo
%{_bindir}/vibepollo-mangohud
%{_prefix}/libexec/vibeshine/vibeshine-drm-install
%{_prefix}/libexec/vibeshine/vibeshine-ds5-install
%{_prefix}/libexec/vibeshine/vibeshine-vkms
%{_prefix}/libexec/vibeshine/vibeshine-vkms-quiesce
%{_prefix}/libexec/vibeshine/vibeshine-vkms-peercred
%{_prefix}/libexec/vibeshine/vibepollo-session-controller
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-display-observer
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-touch-map
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-session-exec
%attr(0700,root,root) %caps(cap_kill,cap_setgid,cap_setuid+p) %{_prefix}/libexec/vibeshine/vibepollo-session-broker
%{_prefix}/libexec/vibeshine/vibepollo-provider-scan
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-global-limiter.py
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-steam-launch
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-app-focus
%{_prefix}/libexec/vibeshine/vibepollo-profile-import
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-app-supervisor
%{_prefix}/libexec/vibeshine/vibepollo-machine-host
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-kwin-session-environment
%attr(0750,root,vibepollo) %caps(cap_sys_admin,cap_sys_nice+p) %{_prefix}/libexec/vibeshine/vibepollo-host
%attr(4755,root,root) %{_libdir}/libvibeshine-kwin-gpu.so

# Dedicated access group for the privileged virtual-display control socket
%{_prefix}/lib/sysusers.d/vibeshine-vkms.conf
%{_prefix}/lib/sysusers.d/vibepollo.conf

# Versioned DKMS/direct-build source tree
/usr/src/vibeshine-drm-*
/usr/src/vibeshine-ds5-*

# KWin user-unit drop-ins; Linux does not install the generic app service.
%{_userunitdir}/plasma-kwin_wayland.service.d/vibeshine-kwin-gpu.conf
%{_userunitdir}/plasma-kwin_wayland.service.d/vibepollo-kwin-session-environment.conf
%{_userunitdir}/plasma-login-kwin_wayland.service.d/vibeshine-kwin-gpu.conf
%{_userunitdir}/plasma-login-kwin_wayland.service.d/vibepollo-kwin-session-environment.conf

# Privileged virtual-display provisioning service
%{_unitdir}/vibeshine-drm-setup.service
%{_unitdir}/vibeshine-vkms-control.socket
%{_unitdir}/vibeshine-vkms-control@.service
%{_unitdir}/vibeshine-vkms.service
%{_unitdir}/vibepollo-session-exec.socket
%{_unitdir}/vibepollo-session-exec@.service
%{_unitdir}/vibepollo-session-controller.service
%{_unitdir}/vibepollo.service

# Udev rules
%{_udevrulesdir}/*-sunshine.rules
%{_udevrulesdir}/70-vibepollo-uinput.rules

# Native firewall profiles and PipeWire capture defaults
%{_prefix}/lib/firewalld/services/vibepollo.xml
%{_sysconfdir}/ufw/applications.d/vibepollo
%{_datadir}/pipewire/pipewire.conf.d/50-vibepollo-audio.conf

# Modules-load configuration
%{_modulesloaddir}/*-sunshine.conf
%{_modulesloaddir}/70-vibeshine-ds5.conf

# Desktop entries
%{_datadir}/applications/*.desktop

# Icons
%{_datadir}/icons/hicolor/scalable/apps/apollo.svg
%{_datadir}/icons/hicolor/scalable/status/apollo*.svg

# Metainfo
%{_datadir}/metainfo/*.metainfo.xml

# Assets
%{_datadir}/vibepollo/**

%changelog
