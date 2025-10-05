# Set build variables if env variables are defined
# These are used in configured files such as manifests for different packages
if(DEFINED ENV{BRANCH})
    set(GITHUB_BRANCH $ENV{BRANCH})
endif()
if(DEFINED ENV{BUILD_VERSION})  # cmake-lint: disable=W0106
    set(BUILD_VERSION $ENV{BUILD_VERSION})
endif()
if(DEFINED ENV{CLONE_URL})
    set(GITHUB_CLONE_URL $ENV{CLONE_URL})
endif()
if(DEFINED ENV{COMMIT})
    set(GITHUB_COMMIT $ENV{COMMIT})
endif()
if(DEFINED ENV{TAG})
    set(GITHUB_TAG $ENV{TAG})
endif()

# Allow forks / CI to override which GitHub repo is used for update checks.
# Cache variables can be provided via -DSUNSHINE_REPO_OWNER=... or -DSUNSHINE_REPO_NAME=...
set(SUNSHINE_REPO_OWNER "Nonary" CACHE STRING "GitHub repo owner for update checks")
set(SUNSHINE_REPO_NAME "vibepollo" CACHE STRING "GitHub repo name for update checks")

# Allow environment variables to override the cache values (useful in CI)
if(DEFINED ENV{SUNSHINE_REPO_OWNER})
    set(SUNSHINE_REPO_OWNER $ENV{SUNSHINE_REPO_OWNER})
endif()
if(DEFINED ENV{SUNSHINE_REPO_NAME})
    set(SUNSHINE_REPO_NAME $ENV{SUNSHINE_REPO_NAME})
endif()

# Try to infer owner/name from the clone URL when available and the defaults are still in use
if(DEFINED GITHUB_CLONE_URL AND (SUNSHINE_REPO_OWNER STREQUAL "Nonary" OR SUNSHINE_REPO_NAME STREQUAL "vibepollo"))
    string(REGEX MATCH "github.com[:/]+([^/]+)/([^/]+)(\\.git)?$" _match "${GITHUB_CLONE_URL}")
    if(_match)
        set(SUNSHINE_REPO_OWNER "${CMAKE_MATCH_1}")
        string(REGEX REPLACE "\\.git$" "" _repo_name "${CMAKE_MATCH_2}")
        set(SUNSHINE_REPO_NAME "${_repo_name}")
        message(STATUS "Inferred GitHub repo: ${SUNSHINE_REPO_OWNER}/${SUNSHINE_REPO_NAME} from ${GITHUB_CLONE_URL}")
    endif()
endif()

# Check if env vars are defined before attempting to access them, variables will be defined even if blank
if((DEFINED ENV{BRANCH}) AND (DEFINED ENV{BUILD_VERSION}))  # cmake-lint: disable=W0106
    if((DEFINED ENV{BRANCH}) AND (NOT $ENV{BUILD_VERSION} STREQUAL ""))
        # If BRANCH is defined and BUILD_VERSION is not empty, then we are building from CI
        # If BRANCH is master we are building a push/release build
        MESSAGE("Got from CI '$ENV{BRANCH}' branch and version '$ENV{BUILD_VERSION}'")
        set(PROJECT_VERSION $ENV{BUILD_VERSION})
        string(REGEX REPLACE "^v" "" PROJECT_VERSION ${PROJECT_VERSION})  # remove the v prefix if it exists
        set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})  # cpack will use this to set the binary versions
    endif()
else()
    # Resolve version from environment tag or git tags
    find_package(Git)
    set(_VER_FROM_ENV "${GITHUB_TAG}")
    if(DEFINED ENV{TAG} AND NOT $ENV{TAG} STREQUAL "")
        set(_VER_FROM_ENV "$ENV{TAG}")
    endif()

    if(NOT _VER_FROM_ENV STREQUAL "")
        set(PROJECT_VERSION "${_VER_FROM_ENV}")
        string(REGEX REPLACE "^v" "" PROJECT_VERSION "${PROJECT_VERSION}")
        set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})
        message(STATUS "Using version from TAG: ${PROJECT_VERSION}")
    elseif(GIT_EXECUTABLE)
        # Current branch name
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE GIT_DESCRIBE_BRANCH
            RESULT_VARIABLE GIT_BRANCH_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        # Nearest tag (annotated or lightweight)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
            OUTPUT_VARIABLE GIT_NEAREST_TAG_RAW
            RESULT_VARIABLE GIT_TAG_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        # Short commit for logging
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            OUTPUT_VARIABLE GIT_SHORT
            RESULT_VARIABLE GIT_SHORT_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        # Dirty state
        execute_process(
            COMMAND ${GIT_EXECUTABLE} diff --quiet --exit-code
            RESULT_VARIABLE GIT_IS_DIRTY
            OUTPUT_STRIP_TRAILING_WHITESPACE)

        if(NOT GIT_TAG_ERROR)
            set(PROJECT_VERSION "${GIT_NEAREST_TAG_RAW}")
            string(REGEX REPLACE "^v" "" PROJECT_VERSION "${PROJECT_VERSION}")
            set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})
            message(STATUS "Detected git tag version: ${PROJECT_VERSION}")
        else()
            # Fallback when no tags: leave PROJECT_VERSION as-is (from project())
            message(WARNING "No git tags found; using default PROJECT_VERSION=${PROJECT_VERSION}")
        endif()

        if(NOT GIT_BRANCH_ERROR)
            message(STATUS "Git branch: ${GIT_DESCRIBE_BRANCH}")
        endif()
        if(NOT GIT_SHORT_ERROR)
            message(STATUS "Git short commit: ${GIT_SHORT}")
        endif()
        if(GIT_IS_DIRTY)
            message(STATUS "Git tree is dirty")
        endif()
    else()
        message(WARNING "Git not found; using default PROJECT_VERSION=${PROJECT_VERSION}")
    endif()
endif()

# Enforce numeric Major.Minor.Patch versioning for this build by stripping any
# prerelease (e.g., -beta.1) or build metadata (e.g., +20240101) suffixes that
# may appear on tags or CI-provided versions.
if(DEFINED PROJECT_VERSION)
    string(REGEX REPLACE "[-+].*$" "" PROJECT_VERSION "${PROJECT_VERSION}")
    set(CMAKE_PROJECT_VERSION ${PROJECT_VERSION})
endif()

# Propagate branch information as a compile definition if available.
# CI builds define BRANCH env var; local builds derive GIT_DESCRIBE_BRANCH above.
if(DEFINED GIT_DESCRIBE_BRANCH AND NOT GIT_DESCRIBE_BRANCH STREQUAL "")
    set(PROJECT_VERSION_BRANCH "${GIT_DESCRIBE_BRANCH}")
elseif(DEFINED GITHUB_BRANCH AND NOT GITHUB_BRANCH STREQUAL "")
    set(PROJECT_VERSION_BRANCH "${GITHUB_BRANCH}")
elseif(DEFINED ENV{BRANCH} AND NOT $ENV{BRANCH} STREQUAL "")
    set(PROJECT_VERSION_BRANCH "$ENV{BRANCH}")
else()
    set(PROJECT_VERSION_BRANCH "unknown")
endif()

# Ensure we always have a commit hash for comparison logic in the UI.
# Prefer the CI provided env COMMIT. If not present, fall back to querying git directly.
if((NOT DEFINED GITHUB_COMMIT) OR (GITHUB_COMMIT STREQUAL ""))
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            OUTPUT_VARIABLE GIT_FULL_COMMIT
            RESULT_VARIABLE GIT_FULL_COMMIT_ERROR_CODE
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT GIT_FULL_COMMIT_ERROR_CODE)
            set(GITHUB_COMMIT "${GIT_FULL_COMMIT}")
        endif()
    endif()
endif()

# set date variables (defaults; will be auto-resolved below)
set(PROJECT_YEAR "1990")
set(PROJECT_MONTH "01")
set(PROJECT_DAY "01")

# Extract year, month, and day (do this AFTER version parsing)
# Note: Cmake doesn't support "{}" regex syntax
if(PROJECT_VERSION MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9][0-9][0-9][0-9]?)\\.([0-9]+)$")
    message("Extracting year and month/day from PROJECT_VERSION: ${PROJECT_VERSION}")
    # First capture group is the year
    set(PROJECT_YEAR "${CMAKE_MATCH_1}")

    # Second capture group contains month and day
    set(MONTH_DAY "${CMAKE_MATCH_2}")

    # Extract month (first 1-2 digits) and day (last 2 digits)
    string(LENGTH "${MONTH_DAY}" MONTH_DAY_LENGTH)
    if(MONTH_DAY_LENGTH EQUAL 3)
        # Format: MDD (e.g., 703 = month 7, day 03)
        string(SUBSTRING "${MONTH_DAY}" 0 1 PROJECT_MONTH)
        string(SUBSTRING "${MONTH_DAY}" 1 2 PROJECT_DAY)
    elseif(MONTH_DAY_LENGTH EQUAL 4)
        # Format: MMDD (e.g., 1203 = month 12, day 03)
        string(SUBSTRING "${MONTH_DAY}" 0 2 PROJECT_MONTH)
        string(SUBSTRING "${MONTH_DAY}" 2 2 PROJECT_DAY)
    endif()

    # Ensure month is two digits
    if(PROJECT_MONTH LESS 10 AND NOT PROJECT_MONTH MATCHES "^0")
        set(PROJECT_MONTH "0${PROJECT_MONTH}")
    endif()
    # Ensure day is two digits
    if(PROJECT_DAY LESS 10 AND NOT PROJECT_DAY MATCHES "^0")
        set(PROJECT_DAY "0${PROJECT_DAY}")
    endif()
endif()

# Parse PROJECT_VERSION to extract major, minor, and patch components
if(PROJECT_VERSION MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(CMAKE_PROJECT_VERSION_MAJOR "${CMAKE_MATCH_1}")

    set(PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(CMAKE_PROJECT_VERSION_MINOR "${CMAKE_MATCH_2}")

    set(PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(CMAKE_PROJECT_VERSION_PATCH "${CMAKE_MATCH_3}")
endif()

message("PROJECT_NAME: ${PROJECT_NAME}")
message("PROJECT_VERSION: ${PROJECT_VERSION}")
message("PROJECT_VERSION_MAJOR: ${PROJECT_VERSION_MAJOR}")
message("PROJECT_VERSION_MINOR: ${PROJECT_VERSION_MINOR}")
message("PROJECT_VERSION_PATCH: ${PROJECT_VERSION_PATCH}")
message("CMAKE_PROJECT_VERSION: ${CMAKE_PROJECT_VERSION}")
message("CMAKE_PROJECT_VERSION_MAJOR: ${CMAKE_PROJECT_VERSION_MAJOR}")
message("CMAKE_PROJECT_VERSION_MINOR: ${CMAKE_PROJECT_VERSION_MINOR}")
message("CMAKE_PROJECT_VERSION_PATCH: ${CMAKE_PROJECT_VERSION_PATCH}")

list(APPEND SUNSHINE_DEFINITIONS PROJECT_NAME="${PROJECT_NAME}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION="${PROJECT_VERSION}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_MAJOR="${PROJECT_VERSION_MAJOR}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_MINOR="${PROJECT_VERSION_MINOR}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_PATCH="${PROJECT_VERSION_PATCH}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_COMMIT="${GITHUB_COMMIT}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_VERSION_BRANCH="${PROJECT_VERSION_BRANCH}")

# ------------------------------------------------------------
# Release date (ISO 8601) for update checks
# Prefer explicit env RELEASE_DATE, else commit date, else YYYY-MM-DD from
# parsed PROJECT_VERSION (falls back to 1990-01-01 if unknown).
# ------------------------------------------------------------
set(PROJECT_RELEASE_DATE_ISO "")

# 1) If provided by CI/environment, use that verbatim
if(DEFINED ENV{RELEASE_DATE} AND NOT $ENV{RELEASE_DATE} STREQUAL "")
    set(PROJECT_RELEASE_DATE_ISO "$ENV{RELEASE_DATE}")
endif()

# 2) If not provided, try getting the last commit date in ISO 8601 from git
if(PROJECT_RELEASE_DATE_ISO STREQUAL "")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} log -1 --format=%cI
            OUTPUT_VARIABLE GIT_COMMIT_DATE_ISO
            RESULT_VARIABLE GIT_COMMIT_DATE_ISO_ERROR_CODE
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT GIT_COMMIT_DATE_ISO_ERROR_CODE AND NOT "${GIT_COMMIT_DATE_ISO}" STREQUAL "")
            set(PROJECT_RELEASE_DATE_ISO "${GIT_COMMIT_DATE_ISO}")
        endif()
    endif()
endif()

# 3) If git is unavailable, fall back to current UTC time
if(PROJECT_RELEASE_DATE_ISO STREQUAL "")
    string(TIMESTAMP _NOW_ISO "%Y-%m-%dT%H:%M:%SZ" UTC)
    set(PROJECT_RELEASE_DATE_ISO "${_NOW_ISO}")
endif()

message("PROJECT_RELEASE_DATE_ISO: ${PROJECT_RELEASE_DATE_ISO}")
list(APPEND SUNSHINE_DEFINITIONS PROJECT_RELEASE_DATE="${PROJECT_RELEASE_DATE_ISO}")

# Derive PROJECT_YEAR/MONTH/DAY from ISO date if available
if(PROJECT_RELEASE_DATE_ISO MATCHES "^([0-9][0-9][0-9][0-9])-([0-9][0-9])-([0-9][0-9])")
    set(PROJECT_YEAR  "${CMAKE_MATCH_1}")
    set(PROJECT_MONTH "${CMAKE_MATCH_2}")
    set(PROJECT_DAY   "${CMAKE_MATCH_3}")
endif()

message("PROJECT_YEAR: ${PROJECT_YEAR}")
message("PROJECT_MONTH: ${PROJECT_MONTH}")
message("PROJECT_DAY: ${PROJECT_DAY}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_REPO_OWNER="${SUNSHINE_REPO_OWNER}")
list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_REPO_NAME="${SUNSHINE_REPO_NAME}")
