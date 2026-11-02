/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：Anki 宿主测试的日志替身；断言失败由测试自行报告。调试构建可用
 *       -DESP_LOGW='printf' 之类预设宏打开输出。
 * English: Log stand-in for Anki host tests; failures are reported by the test.
 * Debug builds may predefine macros such as -DESP_LOGW='printf' to see output.
 */
#pragma once
#ifndef ESP_LOGE
#define ESP_LOGE(tag, ...) ((void)(tag))
#endif
#ifndef ESP_LOGW
#define ESP_LOGW(tag, ...) ((void)(tag))
#endif
#ifndef ESP_LOGI
#define ESP_LOGI(tag, ...) ((void)(tag))
#endif
#ifndef ESP_LOGD
#define ESP_LOGD(tag, ...) ((void)(tag))
#endif
