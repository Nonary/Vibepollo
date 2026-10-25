# Exact package pins; no mutable latest-release or submodule-tag inference.
# The two nightly targets use native AC3/EAC3 maintenance artifacts.
# Other targets retain their existing packages until replacements are qualified.
set(FFMPEG_GITHUB_REPO "LizardByte/build-deps")
set(FFMPEG_RELEASE_TAG "v2026.516.30821")
set(FFMPEG_BUILD_DEPS_COMMIT "fce763bb09c73b5952ccdcbc90efb8ba8f53a7de")

set(FFMPEG_SHA256_Alpine_aarch64 "668364186a55b9708302ee05073ed80b746b82dc69f4896b473cf25a310f6d38")
set(FFMPEG_SHA256_Alpine_x86_64 "cacaec15498c3a04c64241d6a2ef2e4ffce20145aceda1eff63a8d959f32512d")
set(FFMPEG_SHA256_Darwin_arm64 "707813b81590d80f0f411e05ddbac9a4e2f1c4dbc1e3f61d32d512454317bfa6")
set(FFMPEG_SHA256_Darwin_x86_64 "fce51e3ecb41d60c39b0378a548e786c5e11e6c5834fdb805a6cc531b4458bf0")
set(FFMPEG_SHA256_FreeBSD_aarch64 "7ef5687dadc700f3023b36e46503ebde28dd9d41460e57beb21befffeae423e4")
set(FFMPEG_SHA256_FreeBSD_amd64 "2f1f1510ec121ac8e5aa5ee998bdefecfd09430536c1d5669e29fa2e878621a5")
set(FFMPEG_SHA256_Linux_aarch64 "10b6c9440ba6178ec3b944f6c6f697253497cad5196d23a2d32d335daae0062e")
set(FFMPEG_SHA256_Linux_ppc64le "8b7b0ba5eaa12bc9bfb6e3561fb14a50453ce688f6c8cab81aa70fb85bc4e3fb")
set(FFMPEG_SHA256_Linux_x86_64 "c32319fcc2867befe8ff3ae26b4d3a58378c3a9c1bfef9029c9969321c3bc6ec")
set(FFMPEG_SHA256_Windows_AMD64 "2f7a2c2fc6be9b96de3c6f654389f73a5e5d369d7e802d017894fae96247661d")
set(FFMPEG_SHA256_Windows_ARM64 "0675898a1c7175e133b8f0e9cdb737038c53b9bd86d1a5b63fe2ea88a974d2e6")

if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    set(FFMPEG_GITHUB_REPO "Nonary/build-deps")
    set(FFMPEG_RELEASE_TAG "v2026.516.30821-nonary.1")
    set(FFMPEG_BUILD_DEPS_COMMIT "df4c19e82853e4168ffde8b9cab435449929db5f")
    set(FFMPEG_SHA256_Windows_AMD64 "6afb6b7b913ed608dd8c965569d4e1d7b389fa30b83f52b911afdb400433b791")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(FFMPEG_GITHUB_REPO "Nonary/build-deps")
    set(FFMPEG_RELEASE_TAG "v2026.516.30821-nonary.1")
    set(FFMPEG_BUILD_DEPS_COMMIT "05a7b19772789e5231ec3849b0c02b0ab1fb1d5b")
    set(FFMPEG_SHA256_Linux_x86_64 "c38e1e87d819560aecbc6e3906702c6e49ef71d3a7d26b3a9e24a69f1819536a")
endif()

set(FFMPEG_RELEASE_URL "https://github.com/${FFMPEG_GITHUB_REPO}/releases/download/${FFMPEG_RELEASE_TAG}")
