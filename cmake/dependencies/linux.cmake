# linux specific dependencies

include("${CMAKE_MODULE_PATH}/dependencies/glad.cmake")

if(SUNSHINE_ENABLE_WEBRTC)
    include("${CMAKE_MODULE_PATH}/dependencies/webrtc.cmake")
endif()
