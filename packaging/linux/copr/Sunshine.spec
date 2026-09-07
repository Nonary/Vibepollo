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
Conflicts: Sunshine sunshine vibeshine

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
Requires: python3
Requires: /usr/bin/pactl
Requires: /usr/bin/parec
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

%pre
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
  [ "$(printf '%%s\n' "$vibepollo_properties" | wc -l | tr -d ' ')" = 5 ] || return 1
  for vibepollo_property in LoadState ActiveState SubState MainPID ControlGroup; do
    vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | \
      grep -c "^$vibepollo_property=" || true)
    [ "$vibepollo_property_count" = 1 ] || return 1
  done
  vibepollo_load=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^LoadState=//p')
  vibepollo_state=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ActiveState=//p')
  vibepollo_substate=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^SubState=//p')
  vibepollo_pid=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^MainPID=//p')
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
vibepollo_stop_exact_unit() {
  # Never bypass a service's ordered resource teardown.  Killing systemctl
  # only abandons the client while its manager job continues; killing the unit
  # cgroup can strand live GPU imports and is therefore forbidden here.
  # Controller cleanup can legitimately spend more than 30 seconds draining
  # the host and GPU bindings; keep waiting for the manager's ordered stop.
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
        [ ! -e "$vibepollo_control_socket" ] && [ ! -L "$vibepollo_control_socket" ]
        return
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
      case "$vibepollo_line" in *""*) return 1 ;; esac
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
    case "$vibepollo_line" in *""*) return 1 ;; esac
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
    case "$vibepollo_line" in *""*) return 1 ;; esac
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
  if [ ! -e "$vibepollo_legacy_host" ] && [ ! -L "$vibepollo_legacy_host" ]; then
    vibepollo_unit_is_quiescent vibepollo.service || return 1
    vibepollo_upgrade_kill_mode=control-group
    return 0
  fi
  vibepollo_privileged_helper_is_safe "$vibepollo_legacy_host" || return 1
  if grep -Fqx '  trap mark_host_shutdown TERM INT HUP' "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=control-group
  elif grep -Fqx "  trap 'forward_host_signal TERM' TERM" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal INT' INT" "$vibepollo_legacy_host" && \
       grep -Fqx "  trap 'forward_host_signal HUP' HUP" "$vibepollo_legacy_host"; then
    vibepollo_upgrade_kill_mode=process
  else
    return 1
  fi
}
vibepollo_prepare_host_upgrade_fence() (
  [ ! -L "$vibepollo_host_upgrade_dropin_dir" ] && \
    [ ! -L "$vibepollo_host_upgrade_dropin" ] || exit 1
  install -d -o root -g root -m 0755 -- "$vibepollo_host_upgrade_dropin_dir" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%F' -- "$vibepollo_host_upgrade_dropin_dir")" = \
    '0:0:755:directory' ] || exit 1
  vibepollo_upgrade_temporary=$(mktemp /run/vibepollo-host-upgrade.XXXXXX) || exit 1
  trap 'rm -f -- "$vibepollo_upgrade_temporary"' 0
  case "$vibepollo_upgrade_kill_mode" in process | control-group) ;; *) exit 1 ;; esac
  printf '[Unit]\nRefuseManualStart=yes\n\n[Service]\nKillMode=%%s\nSendSIGKILL=no\n' \
    "$vibepollo_upgrade_kill_mode" >"$vibepollo_upgrade_temporary" || exit 1
  chmod 0600 -- "$vibepollo_upgrade_temporary" || exit 1
  if [ -e "$vibepollo_host_upgrade_dropin" ]; then
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || exit 1
    cmp -s -- "$vibepollo_upgrade_temporary" "$vibepollo_host_upgrade_dropin" || exit 1
  fi
  install -o root -g root -m 0644 -- "$vibepollo_upgrade_temporary" \
    "$vibepollo_host_upgrade_dropin" || exit 1
  [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
    '0:0:644:1:regular file' ] || exit 1
)
vibepollo_activate_host_upgrade_fence() {
  systemctl daemon-reload || exit 1
  vibepollo_host_stop_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=RefuseManualStart --property=KillMode --property=SendSIGKILL 2>/dev/null) || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'RefuseManualStart=yes' || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | \
    grep -qx "KillMode=$vibepollo_upgrade_kill_mode" || exit 1
  printf '%%s\n' "$vibepollo_host_stop_properties" | grep -qx 'SendSIGKILL=no' || exit 1
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
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
  vibepollo_controller_active=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ActiveState=//p')
  vibepollo_controller_substate=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^SubState=//p')
  vibepollo_controller_pid=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^MainPID=//p')
  vibepollo_controller_cgroup=$(printf '%%s\n' "$vibepollo_controller_state" | sed -n 's/^ControlGroup=//p')
  [ "$vibepollo_controller_active" = active ] && [ "$vibepollo_controller_substate" = running ] || return 1
  case "$vibepollo_controller_pid" in '' | 0 | *[!0-9]*) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in /*) ;; *) return 1 ;; esac
  case "$vibepollo_controller_cgroup" in */../* | */..) return 1 ;; esac
  timeout --signal=TERM --kill-after=2 15 systemctl freeze \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
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
  vibepollo_controller_was_frozen=1
}
vibepollo_controller_remains_frozen() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=frozen'
}
vibepollo_thaw_controller() {
  [ "$vibepollo_controller_was_frozen" -eq 1 ] || return 0
  timeout --signal=KILL 15 systemctl thaw \
    vibepollo-session-controller.service 2>/dev/null || return 1
  vibepollo_controller_was_frozen=0
  vibepollo_controller_state=$(timeout --signal=KILL 5 systemctl show \
    vibepollo-session-controller.service --property=ActiveState --property=SubState \
    --property=MainPID --property=ControlGroup --property=FreezerState 2>/dev/null) || return 1
  printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'ActiveState=active' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'SubState=running' && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "MainPID=$vibepollo_controller_pid" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx "ControlGroup=$vibepollo_controller_cgroup" && \
    printf '%%s\n' "$vibepollo_controller_state" | grep -qx 'FreezerState=running'
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
vibepollo_quiesce_machine_host() {
  vibepollo_have_systemd=0
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_have_systemd=1
    vibepollo_select_upgrade_kill_mode || return 1
    vibepollo_prepare_host_upgrade_fence || return 1
    vibepollo_freeze_controller || return 1
    vibepollo_host_is_stable_or_quiescent || return 1
    vibepollo_activate_host_upgrade_fence || return 1
    vibepollo_controller_remains_frozen || return 1
    vibepollo_host_is_stable_or_quiescent || return 1
    timeout --signal=KILL 15 systemctl mask --runtime \
      'vibepollo-session-restore@.service' 2>/dev/null || return 1
    vibepollo_restore_template_is_masked || return 1
    vibepollo_disable_legacy_handoff || return 1
    timeout --signal=KILL 15 systemctl mask --runtime vibeshine-vkms-control.socket 2>/dev/null || return 1
    vibepollo_control_socket_is_masked || return 1
    vibepollo_stop_exact_unit vibeshine-vkms-control.socket
    vibepollo_unit_is_quiescent vibeshine-vkms-control.socket || return 1
    vibepollo_control_instances_are_quiescent || return 1
    timeout --signal=KILL 15 systemctl mask --runtime vibepollo-session-exec.socket 2>/dev/null || return 1
    vibepollo_broker_socket_is_masked || return 1
    vibepollo_stop_exact_unit vibepollo-session-exec.socket
    vibepollo_unit_is_quiescent vibepollo-session-exec.socket || return 1
    vibepollo_stop_exact_unit vibepollo.service
    vibepollo_unit_is_quiescent vibepollo.service || return 1
    timeout --signal=KILL 15 systemctl mask --runtime vibepollo.service 2>/dev/null || return 1
    vibepollo_host_unit_is_masked || return 1
    vibepollo_stop_restore_instances || return 1
    vibepollo_stop_brokers || return 1
    vibepollo_controller_remains_frozen || return 1
    # systemd refuses StopUnit for a frozen service. Admission and the host are
    # already masked here, so thaw the controller only for its ordered stop.
    vibepollo_thaw_controller || return 1
    vibepollo_stop_exact_unit vibepollo-session-controller.service
    vibepollo_unit_is_quiescent vibepollo-session-controller.service || return 1
    vibepollo_stop_exact_unit vibepollo-prelogin.service
    vibepollo_stop_exact_unit vibepollo-machine-prepare.service
    timeout --signal=KILL 15 systemctl disable vibepollo-session-controller.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibepollo.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibepollo-prelogin.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibepollo-machine-prepare.service --now 2>/dev/null || true
  fi

  if [ "$vibepollo_have_systemd" -eq 0 ]; then vibepollo_disable_legacy_handoff || return 1; fi
  vibepollo_runtime_root_is_safe_or_absent || return 1

  if [ "$vibepollo_have_systemd" -eq 0 ] && \
     { [ -e "$vibepollo_controller" ] || [ -L "$vibepollo_controller" ]; }; then
    vibepollo_privileged_helper_is_safe "$vibepollo_controller" || return 1
    timeout --signal=KILL 40 "$vibepollo_controller" cleanup || return 1
  elif [ "$vibepollo_have_systemd" -eq 0 ]; then
    vibepollo_run_optional_legacy_command cleanup
    vibepollo_legacy_status=$?
    case "$vibepollo_legacy_status" in 0 | 2) ;; *) return 1 ;; esac
  fi

  vibepollo_run_optional_legacy_command remove-pam
  vibepollo_legacy_status=$?
  case "$vibepollo_legacy_status" in 0 | 2) ;; *) return 1 ;; esac

  if [ "$vibepollo_have_systemd" -eq 1 ]; then
    vibepollo_restore_template_is_masked || return 1
    vibepollo_host_unit_is_masked || return 1
    vibepollo_broker_socket_is_masked || return 1
    vibepollo_stop_restore_instances || return 1
    vibepollo_stop_exact_unit vibepollo.service
    vibepollo_unit_is_quiescent vibepollo.service || return 1
    vibepollo_stop_exact_unit vibepollo-session-exec.socket
    vibepollo_stop_brokers || return 1
    for vibepollo_unit in vibepollo-session-exec.socket vibepollo-session-controller.service \
      vibepollo.service vibepollo-prelogin.service vibepollo-machine-prepare.service; do
      vibepollo_stop_exact_unit "$vibepollo_unit"
      vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
      vibepollo_unit_is_disabled "$vibepollo_unit" || return 1
    done
    vibepollo_brokers_are_quiescent || return 1
    vibepollo_restore_instances_are_quiescent || return 1
  fi
  vibepollo_cleanup_legacy_transition_state || return 1
  if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" || return 1
  fi
  [ ! -e "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ] && \
    [ ! -e "$vibepollo_legacy_handoff_directory" ] && [ ! -L "$vibepollo_legacy_handoff_directory" ] && \
    [ ! -e "$vibepollo_legacy_restore_directory" ] && [ ! -L "$vibepollo_legacy_restore_directory" ] && \
    [ ! -e "$vibepollo_legacy_transition_lock" ] && [ ! -L "$vibepollo_legacy_transition_lock" ] && \
    [ ! -e "$vibepollo_legacy_prelogin_marker" ] && [ ! -L "$vibepollo_legacy_prelogin_marker" ]
}
if ! vibepollo_quiesce_machine_host; then
  echo "error: installed Vibepollo services did not quiesce; replacement is blocked and admission remains disabled." >&2
  exit 1
fi

%post
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
vibepollo_unmask_host_for_controller() {
  vibepollo_unit_is_quiescent vibepollo-session-controller.service || return 1
  vibepollo_unit_is_quiescent vibepollo-session-exec.socket || return 1
  if [ -e "$vibepollo_host_upgrade_dropin" ] || [ -L "$vibepollo_host_upgrade_dropin" ]; then
    [ -f "$vibepollo_host_upgrade_dropin" ] && [ ! -L "$vibepollo_host_upgrade_dropin" ] || return 1
    [ "$(stat -c '%%u:%%g:%%a:%%h:%%F' -- "$vibepollo_host_upgrade_dropin")" = \
      '0:0:644:1:regular file' ] || return 1
    vibepollo_upgrade_contents=$(sed -n '1,6p' -- "$vibepollo_host_upgrade_dropin") || return 1
    case "$vibepollo_upgrade_contents" in
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=process
SendSIGKILL=no' | \
      '[Unit]
RefuseManualStart=yes

[Service]
KillMode=control-group
SendSIGKILL=no') ;;
      *) return 1 ;;
    esac
    rm -f -- "$vibepollo_host_upgrade_dropin" || return 1
    rmdir -- "$vibepollo_host_upgrade_dropin_dir" 2>/dev/null || true
  fi
  systemctl daemon-reload || return 1
  timeout --signal=KILL 15 systemctl unmask --runtime vibepollo.service 2>/dev/null || return 1
  systemctl daemon-reload || return 1
  vibepollo_new_host_properties=$(timeout --signal=KILL 5 systemctl show vibepollo.service \
    --property=RefuseManualStart --property=KillMode --property=SendSIGKILL 2>/dev/null) || return 1
  printf '%%s\n' "$vibepollo_new_host_properties" | grep -qx 'RefuseManualStart=no' && \
    printf '%%s\n' "$vibepollo_new_host_properties" | grep -qx 'KillMode=control-group' && \
    printf '%%s\n' "$vibepollo_new_host_properties" | grep -qx 'SendSIGKILL=no' || return 1
  timeout --signal=KILL 15 systemctl unmask --runtime vibeshine-vkms-control.socket 2>/dev/null || return 1
  systemctl start vibeshine-vkms-control.socket || return 1
  systemctl is-active --quiet vibeshine-vkms-control.socket || return 1
  timeout --signal=KILL 15 systemctl unmask --runtime vibepollo-session-exec.socket 2>/dev/null || return 1
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
  [ "$(printf '%%s\n' "$vibepollo_properties" | wc -l | tr -d ' ')" = 5 ] || return 1
  for vibepollo_property in LoadState ActiveState SubState MainPID ControlGroup; do
    vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | \
      grep -c "^$vibepollo_property=" || true)
    [ "$vibepollo_property_count" = 1 ] || return 1
  done
  vibepollo_load=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^LoadState=//p')
  vibepollo_state=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ActiveState=//p')
  vibepollo_substate=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^SubState=//p')
  vibepollo_pid=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^MainPID=//p')
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
  case "$vibepollo_broker_unit" in vibepollo-session-exec@*.service) ;; *) return 1 ;; esac
  vibepollo_broker_instance=${vibepollo_broker_unit#vibepollo-session-exec@}
  vibepollo_broker_instance=${vibepollo_broker_instance%.service}
  [ -n "$vibepollo_broker_instance" ] || return 1
  case "$vibepollo_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
vibepollo_stop_exact_unit() {
  # Never bypass a service's ordered resource teardown.  Killing systemctl
  # only abandons the client while its manager job continues; killing the unit
  # cgroup can strand live GPU imports and is therefore forbidden here.
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
  vibepollo_broker_attempt=0
  vibepollo_broker_clean_passes=0
  while [ "$vibepollo_broker_attempt" -lt 20 ]; do
    vibepollo_broker_dirty=0
    vibepollo_brokers=$(vibepollo_bounded_broker_list) || return 1
    vibepollo_seen_units='
'
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *""*) return 1 ;; esac
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
vibepollo_brokers_are_quiescent() {
  vibepollo_stop_brokers
}
vibepollo_quiesce_machine_host() {
  if ! command -v systemctl >/dev/null 2>&1 || [ ! -d /run/systemd/system ]; then
    [ ! -e "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ] && \
      [ ! -e "$vibepollo_session_record" ] && [ ! -L "$vibepollo_session_record" ] && \
      [ ! -e "$vibepollo_legacy_acl" ] && [ ! -L "$vibepollo_legacy_acl" ]
    return
  fi
  timeout --signal=KILL 15 systemctl mask --runtime vibepollo.service \
    vibepollo-session-exec.socket 2>/dev/null || return 1
  vibepollo_unit_is_masked vibepollo.service || return 1
  vibepollo_unit_is_masked vibepollo-session-exec.socket || return 1
  vibepollo_stop_exact_unit vibepollo-session-exec.socket
  vibepollo_unit_is_quiescent vibepollo-session-exec.socket || return 1
  vibepollo_stop_brokers || return 1
  for vibepollo_unit in vibepollo-session-controller.service vibepollo.service \
    vibepollo-prelogin.service vibepollo-machine-prepare.service; do
    vibepollo_stop_exact_unit "$vibepollo_unit"
  done
  timeout --signal=KILL 15 systemctl disable vibepollo-session-controller.service --now 2>/dev/null || true
  timeout --signal=KILL 15 systemctl disable vibepollo.service --now 2>/dev/null || true
  timeout --signal=KILL 15 systemctl disable vibepollo-prelogin.service --now 2>/dev/null || true
  timeout --signal=KILL 15 systemctl disable vibepollo-machine-prepare.service --now 2>/dev/null || true
  if [ -e "$vibepollo_controller" ] || [ -L "$vibepollo_controller" ]; then
    vibepollo_privileged_helper_is_safe "$vibepollo_controller" || return 1
    timeout --signal=KILL 40 "$vibepollo_controller" cleanup || return 1
  fi
  vibepollo_unit_is_masked vibepollo.service || return 1
  vibepollo_unit_is_masked vibepollo-session-exec.socket || return 1
  vibepollo_stop_exact_unit vibepollo-session-exec.socket
  vibepollo_stop_brokers || return 1
  for vibepollo_unit in vibepollo-session-exec.socket vibepollo-session-controller.service \
    vibepollo.service vibepollo-prelogin.service vibepollo-machine-prepare.service; do
    vibepollo_stop_exact_unit "$vibepollo_unit"
    vibepollo_unit_is_quiescent "$vibepollo_unit" || return 1
    vibepollo_unit_is_disabled "$vibepollo_unit" || return 1
  done
  vibepollo_stop_brokers || return 1
  if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" || return 1
  fi
  [ ! -e "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ] && \
    [ ! -e "$vibepollo_session_record" ] && [ ! -L "$vibepollo_session_record" ] && \
    [ ! -e "$vibepollo_legacy_acl" ] && [ ! -L "$vibepollo_legacy_acl" ]
}
if ! vibepollo_quiesce_machine_host; then
  echo "error: could not quiesce the machine host after package replacement; Vibepollo remains disabled." >&2
  exit 1
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
    if ! systemctl enable vibepollo-session-controller.service; then
      vibepollo_quiesce_machine_host || true
      echo "error: could not enable the Vibepollo controller safely; all machine-host units remain off." >&2
      exit 1
    fi
    if ! vibepollo_unmask_host_for_controller || \
       ! systemctl start vibepollo-session-controller.service; then
      timeout --signal=KILL 15 systemctl mask --runtime vibepollo.service 2>/dev/null || true
      vibepollo_quiesce_machine_host || true
      echo "error: could not start the Vibepollo controller safely; all machine-host units remain off." >&2
      exit 1
    fi
  else
    echo "==> ACTION REQUIRED: Vibepollo could not prepare the machine profile; review the preceding setup or migration error." >&2
    echo "    Run:  sudo vibepollo configure USER" >&2
    echo "    then: sudo systemctl enable --now vibepollo-session-controller.service" >&2
  fi
else
  echo "rpm-ostree environment detected, skipping post install steps. Restart to apply the changes."
fi

%preun
vibepollo_controller=%{_prefix}/libexec/vibeshine/vibepollo-session-controller
vibepollo_machine_host=%{_prefix}/libexec/vibeshine/vibepollo-machine-host
vibepollo_session_record=/run/vibepollo/session.env
vibepollo_legacy_acl=/run/vibepollo/runtime-acl
vibepollo_broker_socket=/run/vibepollo/session-broker.sock
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
  [ "$(printf '%%s\n' "$vibepollo_properties" | wc -l | tr -d ' ')" = 5 ] || return 1
  for vibepollo_property in LoadState ActiveState SubState MainPID ControlGroup; do
    vibepollo_property_count=$(printf '%%s\n' "$vibepollo_properties" | \
      grep -c "^$vibepollo_property=" || true)
    [ "$vibepollo_property_count" = 1 ] || return 1
  done
  vibepollo_load=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^LoadState=//p')
  vibepollo_state=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^ActiveState=//p')
  vibepollo_substate=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^SubState=//p')
  vibepollo_pid=$(printf '%%s\n' "$vibepollo_properties" | sed -n 's/^MainPID=//p')
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
  vibepollo_load=$(timeout --signal=KILL 5 systemctl show "$1" --property=LoadState 2>/dev/null) || return 1
  case "$vibepollo_enabled" in
    masked | masked-runtime) [ "$vibepollo_load" = 'LoadState=masked' ] ;;
    *) return 1 ;;
  esac
}
vibepollo_preun_broker_unit_is_safe() {
  vibepollo_broker_unit=$1
  case "$vibepollo_broker_unit" in vibepollo-session-exec@*.service) ;; *) return 1 ;; esac
  vibepollo_broker_instance=${vibepollo_broker_unit#vibepollo-session-exec@}
  vibepollo_broker_instance=${vibepollo_broker_instance%.service}
  [ -n "$vibepollo_broker_instance" ] || return 1
  case "$vibepollo_broker_instance" in *[!A-Za-z0-9_.:-]*) return 1 ;; esac
}
vibepollo_preun_stop_exact_unit() {
  # Never bypass a service's ordered resource teardown.  Killing systemctl
  # only abandons the client while its manager job continues; killing the unit
  # cgroup can strand live GPU imports and is therefore forbidden here.
  timeout --signal=TERM --kill-after=2 60 systemctl stop "$1" 2>/dev/null
}
vibepollo_preun_bounded_broker_list() (
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
vibepollo_preun_stop_brokers() (
  vibepollo_broker_attempt=0
  vibepollo_broker_clean_passes=0
  while [ "$vibepollo_broker_attempt" -lt 20 ]; do
    vibepollo_broker_dirty=0
    vibepollo_brokers=$(vibepollo_preun_bounded_broker_list) || return 1
    vibepollo_seen_units='
'
    while IFS= read -r vibepollo_line || [ -n "$vibepollo_line" ]; do
      [ -n "$vibepollo_line" ] || continue
      case "$vibepollo_line" in *""*) return 1 ;; esac
      set -f
      set -- $vibepollo_line
      [ "${1:-}" = '●' ] && shift
      [ "$#" -ge 4 ] || return 1
      vibepollo_unit=$1; vibepollo_load=$2
      vibepollo_state=$3; vibepollo_substate=$4
      vibepollo_preun_broker_unit_is_safe "$vibepollo_unit" || return 1
      [ "$vibepollo_load" = loaded ] || return 1
      case "$vibepollo_state:$vibepollo_substate" in *[!a-z:-]*) return 1 ;; esac
      case "$vibepollo_seen_units" in *"
$vibepollo_unit
"*) return 1 ;; esac
      vibepollo_seen_units="$vibepollo_seen_units$vibepollo_unit
"
      if ! vibepollo_preun_unit_is_quiescent "$vibepollo_unit"; then
        vibepollo_broker_dirty=1
        vibepollo_preun_stop_exact_unit "$vibepollo_unit"
        vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
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
vibepollo_preun_remove_pam() {
  if [ ! -e "$vibepollo_machine_host" ] && [ ! -L "$vibepollo_machine_host" ]; then return 0; fi
  vibepollo_preun_privileged_helper_is_safe "$vibepollo_machine_host" || return 1
  timeout --signal=KILL 40 "$vibepollo_machine_host" remove-pam >/dev/null 2>&1
}
vibepollo_preun_quiesce() {
  vibepollo_have_systemd=0
  if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    vibepollo_have_systemd=1
    timeout --signal=KILL 15 systemctl mask --runtime vibepollo.service \
      vibepollo-session-exec.socket 2>/dev/null || return 1
    vibepollo_preun_unit_is_masked vibepollo.service || return 1
    vibepollo_preun_unit_is_masked vibepollo-session-exec.socket || return 1
    vibepollo_preun_stop_exact_unit vibepollo-session-exec.socket
    vibepollo_preun_unit_is_quiescent vibepollo-session-exec.socket || return 1
    vibepollo_preun_stop_brokers || return 1
    for vibepollo_unit in vibepollo-session-controller.service vibepollo.service \
      vibepollo-prelogin.service vibepollo-machine-prepare.service; do
      vibepollo_preun_stop_exact_unit "$vibepollo_unit"
    done
    timeout --signal=KILL 15 systemctl disable vibepollo-session-controller.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibepollo.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibepollo-prelogin.service --now 2>/dev/null || true
    timeout --signal=KILL 15 systemctl disable vibepollo-machine-prepare.service --now 2>/dev/null || true
  fi
  if [ -e "$vibepollo_controller" ] || [ -L "$vibepollo_controller" ]; then
    vibepollo_preun_privileged_helper_is_safe "$vibepollo_controller" || return 1
    [ "$vibepollo_have_systemd" -eq 1 ] || return 1
    timeout --signal=KILL 40 "$vibepollo_controller" cleanup || return 1
  fi
  vibepollo_preun_remove_pam || return 1
  if [ "$vibepollo_have_systemd" -eq 1 ]; then
    vibepollo_preun_unit_is_masked vibepollo.service || return 1
    vibepollo_preun_unit_is_masked vibepollo-session-exec.socket || return 1
    vibepollo_preun_stop_exact_unit vibepollo-session-exec.socket
    vibepollo_preun_stop_brokers || return 1
    for vibepollo_unit in vibepollo-session-exec.socket vibepollo-session-controller.service \
      vibepollo.service vibepollo-prelogin.service vibepollo-machine-prepare.service; do
      vibepollo_preun_stop_exact_unit "$vibepollo_unit"
      vibepollo_preun_unit_is_quiescent "$vibepollo_unit" || return 1
      vibepollo_preun_unit_is_disabled "$vibepollo_unit" || return 1
    done
    vibepollo_preun_stop_brokers || return 1
  fi
  if [ -S "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ]; then
    rm -f -- "$vibepollo_broker_socket" || return 1
  fi
  [ ! -e "$vibepollo_broker_socket" ] && [ ! -L "$vibepollo_broker_socket" ] && \
    [ ! -e "$vibepollo_session_record" ] && [ ! -L "$vibepollo_session_record" ] && \
    [ ! -e "$vibepollo_legacy_acl" ] && [ ! -L "$vibepollo_legacy_acl" ]
}
if [ "$1" -eq 0 ]; then
  vibepollo_preun_quiesce || {
    echo "error: refusing to uninstall while Vibepollo cgroups or session state remain." >&2
    exit 1
  }
  timeout --signal=KILL 30 systemctl stop vibeshine-vkms.service 2>/dev/null || true
  timeout --signal=KILL 30 systemctl stop vibeshine-drm-setup.service 2>/dev/null || true
  %{_prefix}/libexec/vibeshine/vibeshine-drm-install remove || \
    echo "warning: could not remove the Vibepollo HDR DRM module cleanly."
fi

%files
# Executables
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-display-power
%{_bindir}/vibepollo
%{_bindir}/vibepollo-mangohud
%{_prefix}/libexec/vibeshine/vibeshine-drm-install
%{_prefix}/libexec/vibeshine/vibeshine-vkms
%{_prefix}/libexec/vibeshine/vibeshine-vkms-quiesce
%{_prefix}/libexec/vibeshine/vibeshine-vkms-peercred
%{_prefix}/libexec/vibeshine/vibepollo-session-controller
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-session-exec
%attr(0700,root,root) %caps(cap_kill,cap_setgid,cap_setuid+p) %{_prefix}/libexec/vibeshine/vibepollo-session-broker
%{_prefix}/libexec/vibeshine/vibepollo-provider-scan
%attr(0755,root,root) %{_prefix}/libexec/vibeshine/vibepollo-steam-launch
%{_prefix}/libexec/vibeshine/vibepollo-profile-import
%{_prefix}/libexec/vibeshine/vibepollo-profile-normalize.py
%{_prefix}/libexec/vibeshine/pairing_migration.py
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
