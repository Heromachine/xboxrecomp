# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_mmio_hook.c
    apu_xaudio2.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    # xaudio2_8 / ole32: apu_xaudio2.c asks for these with #pragma comment(lib),
    # which only MSVC honours. Under mingw nothing supplied them, so the moment
    # any consumer actually referenced an APU symbol the archive member got
    # pulled in and the link failed on XAudio2Create. It never showed up before
    # because nothing referenced the library at all. xaudio2_8 is the Win8+
    # import lib and matches the XAudio2Create the file calls; it resolves
    # under Wine, which ships xaudio2_8.dll as a builtin.
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm xaudio2_8 ole32)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
