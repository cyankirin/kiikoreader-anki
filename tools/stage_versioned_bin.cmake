# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 中文：版本号产物拷贝：镜像缺失时先用 esptool 重建，再复制带版本号的副本。
#       由 CMakeLists.txt 里挂到 all 的 anki_versioned_bin 目标每次构建调用，
#       变量经 -D 传入。/ English: Versioned-image staging: regenerate the image
#       with esptool when it is missing, then copy the versioned copy. Invoked on
#       every build by the anki_versioned_bin target hooked to all in
#       CMakeLists.txt; variables arrive via -D.

if(NOT DEFINED BIN OR NOT DEFINED ELF OR NOT DEFINED ESPTOOLPY_CMD
   OR NOT DEFINED ELF2IMAGE_ARGS OR NOT DEFINED VERSIONED)
    message(FATAL_ERROR "stage_versioned_bin.cmake: missing -D variables "
        "(BIN, ELF, ESPTOOLPY_CMD, ELF2IMAGE_ARGS, VERSIONED)")
endif()

# IDF 原生规则只把 .bin_timestamp 记为输出，镜像文件本身是副产品：手动删掉
# 镜像而邮戳仍新时 ninja 不会重建，后续 check_sizes 与拷贝都会失败。这里补上
# 重建，保证构建可自愈。/ IDF's native rule tracks only .bin_timestamp and
# writes the image as a side effect: deleting the image alone leaves the stamp
# fresh, so ninja skips the rebuild and later steps fail. Regenerate here so
# the build self-heals.
if(NOT EXISTS "${BIN}")
    message(STATUS "Regenerating ${BIN} (missing)")
    execute_process(
        COMMAND ${ESPTOOLPY_CMD} elf2image ${ELF2IMAGE_ARGS}
                -o "${BIN}" "${ELF}"
        RESULT_VARIABLE elf2image_result
    )
    if(NOT elf2image_result EQUAL 0)
        message(FATAL_ERROR "elf2image failed for ${BIN}")
    endif()
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BIN}" "${VERSIONED}"
    RESULT_VARIABLE copy_result
)
if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "copy_if_different failed for ${VERSIONED}")
endif()
