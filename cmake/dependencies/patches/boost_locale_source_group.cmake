# Boost 1.89.0 groups locale.hpp outside its declared source_group TREE.
# Keep the pinned dependency and correct only the IDE grouping root before
# FetchContent configures it. This does not alter Boost runtime sources.
set(locale_cmake "${BOOST_SOURCE_DIR}/libs/locale/CMakeLists.txt")
file(READ "${locale_cmake}" contents)
set(original [=[source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/locale" PREFIX "headers" FILES ${sources_inc})]=])
set(corrected [=[source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost" PREFIX "headers" FILES ${sources_inc})]=])
string(FIND "${contents}" "${original}" original_position)
string(FIND "${contents}" "${corrected}" corrected_position)
if(NOT original_position EQUAL -1)
    string(REPLACE "${original}" "${corrected}" contents "${contents}")
    file(WRITE "${locale_cmake}" "${contents}")
elseif(corrected_position EQUAL -1)
    message(FATAL_ERROR "Unexpected Boost.Locale source grouping; review the pinned dependency patch")
endif()
