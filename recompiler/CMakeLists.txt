# Recompiler tool (gbrecomp)

add_executable(gbrecomp
    src/main.cpp
    src/rom.cpp
    src/decoder.cpp
    src/analyzer.cpp
    src/custom_names.cpp
    src/bank_tracker.cpp
    src/symbol_table.cpp
    src/generator.cpp
    src/ir/ir_builder.cpp
    src/ir/ir_optimizer.cpp
    src/codegen/c_emitter.cpp
)

target_include_directories(gbrecomp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Enable position-independent code for shared library support
set_target_properties(gbrecomp PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(gbrecomp PRIVATE GB_PLATFORM_MACOS)
elseif(WIN32)
    target_compile_definitions(gbrecomp PRIVATE GB_PLATFORM_WINDOWS)
else()
    target_compile_definitions(gbrecomp PRIVATE GB_PLATFORM_LINUX)
endif()
