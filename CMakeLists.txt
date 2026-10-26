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
    set(PROJECT_VER "0.3.3-rc100.02")
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
#
# 仅覆盖未启用安全启动签名二进制的常规配置（本项目即如此）；签名场景下 PROJECT_BIN 与
# elf2image 产物不同名，交由 IDF 原生链路处理。/ Covers the regular configuration without
# secure-boot signed binaries (this project's); under signed binaries PROJECT_BIN differs
# from the elf2image output and IDF's native chain owns that case.
#
# 实现要点：IDF 原生镜像规则只把 .bin_timestamp 记为输出，镜像文件是副产品——手动删掉
# 镜像而邮戳仍新时 ninja 跳过重建，挂在 gen_project_binary 上的 POST_BUILD 拷贝既会失败
# 也无法自愈（.util 边按日志判定为最新，根本不会执行）。因此用一个自带 COMMAND 的 custom
# target（custom target 每次都执行）经脚本补上「缺失即重建 + 拷贝」，两种场景都覆盖。
# / Implementation notes: IDF's native image rule tracks only .bin_timestamp and the image
# file is a side effect — deleting the image alone leaves the stamp fresh, so a POST_BUILD
# copy hung off gen_project_binary both fails and cannot self-heal (its .util edge counts as
# up to date from the build log and never runs). Use a custom target carrying its own COMMAND
# (custom targets always run) whose script regenerates when missing and then copies, covering
# both scenarios.
if(NOT CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
    idf_build_get_property(build_dir BUILD_DIR)
    idf_build_get_property(project_bin PROJECT_BIN)
    idf_build_get_property(elf_name EXECUTABLE_NAME)
    idf_build_get_property(elf_dir EXECUTABLE_DIR)
    idf_component_get_property(esptool_py_cmd esptool_py ESPTOOLPY_CMD)
    idf_component_get_property(esptool_elf2image_args esptool_py ESPTOOL_PY_ELF2IMAGE_ARGS)
    add_custom_target(anki_versioned_bin ALL
        COMMAND ${CMAKE_COMMAND}
            -D "BIN=${build_dir}/${project_bin}"
            -D "ELF=${elf_dir}/${elf_name}.elf"
            -D "ESPTOOLPY_CMD=${esptool_py_cmd}"
            -D "ELF2IMAGE_ARGS=${esptool_elf2image_args}"
            -D "VERSIONED=${build_dir}/Read_Pico-${PROJECT_VER}.bin"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage_versioned_bin.cmake"
        DEPENDS gen_project_binary
        COMMENT "Staging versioned firmware image: Read_Pico-${PROJECT_VER}.bin"
        VERBATIM
    )
    # 分区大小核对读到镜像文件；排在本目标之后，缺失重建先发生再核对。
    # / The partition size check reads the image; order it after this target so a
    # missing image is regenerated before the check runs.
    if(TARGET app_check_size)
        add_dependencies(app_check_size anki_versioned_bin)
    endif()
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY ADDITIONAL_CLEAN_FILES
        "${build_dir}/Read_Pico-${PROJECT_VER}.bin")
endif()
