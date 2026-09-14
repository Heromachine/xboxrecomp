# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

add_library(xbox_nv2a STATIC
    nv2a_core.c
    nv2a_pgraph_d3d11.c
    nv2a_texconv.c
    nv2a_vtxarray.c
    nv2a_pb_replay.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
)

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# nv2a_pgraph_d3d11.c rasterizes through the D3D8 layer's device
# (xbox_GetD3DDevice), so this is a real dependency, not link-order tidiness.
# It stayed invisible while nothing pulled pgraph into a link; the moment a
# title calls nv2a_hook_init() it surfaces as an undefined reference.
target_link_libraries(xbox_nv2a PRIVATE platform xbox_d3d8)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
endif()
