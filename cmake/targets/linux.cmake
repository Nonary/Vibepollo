# linux specific target definitions

# Keep the inherited target name stable while exposing the Vibepollo product
# name to Linux users and package managers.
set_target_properties(sunshine PROPERTIES OUTPUT_NAME vibepollo)

# Using newer c++ compilers / features on older distros causes runtime dyn link errors.
# CUDA can require an older host compiler than the system C++ compiler (for
# example, GCC 15 for CUDA with GCC 16 on Arch). In that configuration CUDA's
# library search path can make -static-libstdc++ select the older archive and
# leave symbols from system-compiler objects unresolved. Keep libstdc++ dynamic
# whenever CUDA is part of the target so the final link uses the system ABI.
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        -static-libgcc
)
if(NOT CUDA_FOUND)
    list(APPEND SUNSHINE_EXTERNAL_LIBRARIES -static-libstdc++)
endif()
