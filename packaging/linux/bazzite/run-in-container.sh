#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$HOME" local-output
git config --global --add safe.directory /workspace
cmake -S . -B build-local -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ASSETS_DIR=share/vibepollo -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/vibepollo \
  -DBUILD_TESTS=ON -DBUILD_WERROR=ON -DBUILD_DOCS=OFF \
  -DSUNSHINE_ENABLE_DRM=ON -DSUNSHINE_ENABLE_KWIN=ON \
  -DSUNSHINE_ENABLE_PORTAL=ON -DSUNSHINE_ENABLE_WAYLAND=ON -DSUNSHINE_ENABLE_X11=ON \
  -DSUNSHINE_ENABLE_CUDA=ON -DSUNSHINE_REQUIRE_CUDA_PASCAL=ON \
  -DCMAKE_C_COMPILER=/opt/gcc14/bin/gcc-14 \
  -DCMAKE_CXX_COMPILER=/opt/gcc14/bin/g++-14 \
  -DCMAKE_CUDA_HOST_COMPILER=/opt/gcc14/bin/gcc-14 \
  -DCMAKE_CUDA_COMPILER=/opt/vibepollo/build/cuda/bin/nvcc \
  -DSUNSHINE_BUILD_BAZZITE=ON 2>&1 | tee local-output/configure.log
cmake --build build-local --parallel "$BUILD_JOBS" 2>&1 | tee local-output/build.log
python3 tests/unit/test_bazzite_package_profile.py
python3 tests/unit/test_bazzite_runtime_fixes.py
python3 tests/unit/test_bazzite_kernel_update.py
ctest --test-dir build-local --output-on-failure --parallel 2 2>&1 | tee local-output/tests.log
cpack -G RPM --config build-local/CPackConfig.cmake -B /workspace/local-output 2>&1 | tee local-output/package.log
mapfile -t packages < <(find local-output -maxdepth 1 -type f -name '*.rpm')
[[ "${#packages[@]}" == 1 ]]
rpm -qp --scripts "${packages[0]}" > local-output/rpm-scriptlets.txt
test ! -s local-output/rpm-scriptlets.txt
rpm -qp --requires "${packages[0]}" > local-output/rpm-requires.txt
grep -Fx vibepollo-kmod local-output/rpm-requires.txt
sha256sum "${packages[0]}" > local-output/rpm-sha256.txt
echo 'Experimental Bazzite app RPM. Install together with the matching signed driver RPM; see packaging/linux/bazzite/README.md.' > local-output/STATUS.txt
