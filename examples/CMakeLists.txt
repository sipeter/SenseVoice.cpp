add_library(common STATIC
        common-ggml.h
        common-ggml.cc
)

find_package(SDL2 REQUIRED CONFIG)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(ExampleLibs ggml)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    list(APPEND ExampleLibs stdc++fs)
endif()
if(SDL2_FOUND)
    include_directories(${SDL2_INCLUDE_DIRS})
    add_library(streamSDL STATIC common-sdl.h common-sdl.cpp)
    list(APPEND ExampleLibs ${SDL2_LIBRARIES})
endif()
target_link_libraries(common ${ExampleLibs})
add_subdirectory(quantize)

# 原有的独立可执行目标已合并为 ZianCore
# add_subdirectory(zcr_main)
# if(SDL2_FOUND)
#     add_subdirectory(stream)
# endif()

# ZianCore 统一入口 - 合并 stream 和 zcr_main
if(SDL2_FOUND)
    add_executable(ZianCore 
        ZianEntry.cpp 
        zcr_main/main.cc 
        stream/stream.cc
        audio_pipe.cpp
        audio_socket.cpp
    )
    # 添加 include 目录（从原 stream 和 zcr_main 的 CMakeLists.txt 复制）
    target_include_directories(ZianCore PRIVATE 
        ${PROJECT_SOURCE_DIR}/sense-voice/csrc
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(ZianCore PRIVATE sense-voice-core ggml common streamSDL)

    if(MSVC)
        target_compile_options(ZianCore PRIVATE 
            /wd4820 # 'bytes' bytes padding added after data member 'member'
            /wd4242 # 'identifier': conversion from 'type1' to 'type2', possible loss of data
            /wd4710 # function not inlined
            /wd4711 # function selected for automatic inline
            /wd4061 # enumerator 'identifier' in switch of enum 'identifier' is not explicitly handled by a case label
            /wd4514 # 'function' : unreferenced inline function has been removed
            /wd5045 # Compiler will insert Spectre mitigation for memory load if /Qspectre switch specified
            /wd4668 # 'symbol' is not defined as a preprocessor macro, replacing with '0' for 'directives'
            /wd4365 # 'action' : conversion from 'type_1' to 'type_2', signed/unsigned mismatch
            /wd4244 # 'argument' : conversion from 'type1' to 'type2', possible loss of data
            /wd4267 # 'var' : conversion from 'size_t' to 'type', possible loss of data
            /utf-8  # Set source and execution character sets to UTF-8
        )
    endif()
endif()

