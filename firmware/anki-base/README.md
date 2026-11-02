# anki-base — 网页刷机用的三个基础部件

`firmware/Read_Pico-<版本>.bin` 只是**应用分区镜像**（烧到 `0x10000`），不能单靠它完成一次网页刷机：
`esp-web-install-button` 还需要引导程序、分区表与启动槽标记。CI 里没有 ESP-IDF 构建产物，
但用户刷到的字节必须与本地验证过的一模一样，因此把这三个部件固定入库一份。

`tools/stage_anki_flash.py` 的取源优先级是：**构建包 → 本目录 → `--base-dir`**；
`--check` 会核对基础部件必须来自「构建包」或本目录，取到别的来源（例如上游 Release 的
bootloader）就直接失败，不允许静默替换。

## 来源 / Provenance

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `bootloader.bin` | 21232 B | `2e06dc60d4c9b7730b696349019fec80620dabc8ac893af81e6754008f77e353` |
| `partitions.bin` | 3072 B | `edba9d92e5fc51c706c12f4b4a5d5d23728ba8fb0fbc00be654f1eccef404bda` |
| `ota_data_initial.bin` | 8192 B | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |

- 版本：`0.3.3-rc104.01`（上游 `v0.3.3-rc104` 基线 + 本 fork 的 Anki 与字体改动）
- 生成命令：

  ```bash
  export IDF_TOOLS_PATH="C:/Espressif/tools" ESP_ROM_ELF_DIR="C:/Espressif/tools/esp-rom-elfs/20241011/" CCACHE_DISABLE=1
  python tools/build_firmware.py --board pico --python "<idf venv>/python.exe" --idf-pico "<esp-idf>"
  # 产物在 build/boards/packages/pico/：bootloader.bin / partitions.bin /
  # firmware.bin / ota_data_initial.bin / manifest.json / verification.json
  ```

- 复制自 `build/boards/packages/pico/`（同一次构建的 `firmware.bin` 已入库为
  `firmware/Read_Pico-0.3.3-rc104.01.bin`，SHA-256 `8935ebd0bf64b78309139113d864623c727040100632b4030993e70cff447109`）。

## 为什么 bootloader 与上游 Release 不同

本 fork 把控制台日志裁到 ERROR（`boards/pico/sdkconfig.defaults` 的
`CONFIG_LOG_DEFAULT_LEVEL_ERROR` / `CONFIG_LOG_MAXIMUM_LEVEL_ERROR`），bootloader 也会跟着重编，
所以这里是 `2e06dc60…`，上游 `v0.3.3-rc104` Release 里的 `bootloader-0.3.3-rc104.bin` 是 `539efd5c…`。
两者对本应用都能正常启动（`requires_bootloader_update: false`），但只保留一份、并且是本地验证过的那一份，
才能保证「本地跑过的 == CI 发布的 == 用户刷进去的」。分区表与 otadata 与上游逐字节相同。

## 更新时机

重新构建并升版本时：

1. 用新构建包里的同名文件覆盖本目录三个文件；
2. 更新上表的 SHA-256 与本页版本号；
3. 跑 `python tools/stage_anki_flash.py`、`python tools/stage_anki_flash.py --check`，
   确认 `flash/anki/manifest.json` 里的哈希与这里一致。
