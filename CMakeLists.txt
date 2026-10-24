# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 本地测试沿用当前正式版号；仅正式发布或明确的 OTA 验证指定新版本。
# Local tests keep the current release version; override only for publication or explicit OTA validation.
if(DEFINED ENV{PICO_PROJECT_VER} AND NOT "$ENV{PICO_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{PICO_PROJECT_VER}")
else()
    set(PROJECT_VER "0.3.3-rc100.01")
endif()
project(Read_Pico)

# 固件描述时间统一为 UTC，与概览页标识一致；保留已有编译缓存启动器。
# Compile app-description timestamps in UTC to match Overview; preserve any compiler-cache launcher.
idf_component_get_property(app_desc_lib esp_app_format COMPONENT_LIB)
get_target_property(app_desc_launcher ${app_desc_lib} C_COMPILER_LAUNCHER)
if(NOT app_desc_launcher)
    set(app_desc_launcher "")
endif()
set(app_desc_utc_launcher "${CMAKE_COMMAND}" -E env TZ=UTC)
if(app_desc_launcher)
    list(APPEND app_desc_utc_launcher ${app_desc_launcher})
endif()
set_property(TARGET ${app_desc_lib} PROPERTY C_COMPILER_LAUNCHER "${app_desc_utc_launcher}")

# panic 包装器的链接选项在 main/CMakeLists.txt 里挂到 main 组件上：写在 project() 之后
# 用 idf_build_set_property 设全局链接选项已经太晚，链接器看不到 --wrap。
# The panic wrapper's link option hangs off the main component in main/CMakeLists.txt: setting a
# global link option after project() is too late for the linker to see --wrap.

# 构建成功后再复制一份带版本号的产物，便于发布与 OTA 核对；原 Read_Pico.bin 保持不动，
# flash_args 及既有流程仍依赖它。/ After a successful build, stage a versioned copy of the
# firmware image for release/OTA checks; the original Read_Pico.bin stays untouched because
# flash_args and existing flows depend on it.
idf_build_get_property(build_dir BUILD_DIR)
add_custom_command(TARGET gen_project_binary POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${build_dir}/Read_Pico.bin"
            "${build_dir}/Read_Pico-${PROJECT_VER}.bin"
    COMMENT "Staging versioned firmware image: Read_Pico-${PROJECT_VER}.bin"
    VERBATIM
)
