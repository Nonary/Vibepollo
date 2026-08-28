# linux specific target definitions

# Keep the inherited target name stable while exposing the Vibepollo product
# name to Linux users and package managers.
set_target_properties(sunshine PROPERTIES OUTPUT_NAME vibepollo)

# Using newer c++ compilers / features on older distros causes runtime dyn link errors
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        -static-libgcc
        -static-libstdc++
)
