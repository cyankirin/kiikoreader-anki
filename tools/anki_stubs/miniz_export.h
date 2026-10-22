/* 构建系统生成的导出宏垫片 / Build-generated export macro shim.
 * miniz 的 miniz.h 包含 miniz_export.h；上游用 CMake 生成该文件定义
 * MINIZ_EXPORT。宿主测试不需要导出修饰，留空即可。
 * Upstream miniz.h includes miniz_export.h, which the build system generates
 * to define MINIZ_EXPORT. Host tests need no export decoration, so it is empty.
 */
#pragma once
#define MINIZ_EXPORT
