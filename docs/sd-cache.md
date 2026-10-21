# ext4 专用 SD 帧缓存

**已通过实际 C 核心的文件镜像测试及 ESP-IDF 5.5.2 构建；尚未完成实卡、功耗或时序验收。**仅用于无灰度时钟恢复旧帧；灰阶不初始化或读写卡。无卡、布局不支持、缓存无效或 CRC 失败时不用旧帧，退回全刷。

## 结构与支持边界

- 保持 **4-bit SDMMC、最高 40MHz**；实际频率由卡和驱动协商。不使用 FatFs 或完整 lwext4。
- 核心 portable API：[components/sdcard_bsp/ext4_cache.h](../components/sdcard_bsp/ext4_cache.h)，以 512 字节扇区为单位回调读写，帧数据每次最多合并 8 个扇区（4KiB）；板级适配负责 SDMMC 初始化和释放。
- ext4 可位于整卡（whole-device / superfloppy）或 **MBR Linux 分区**，不支持 GPT。要求 **4KiB block、256 字节 inode**，根目录为简单、非 indexed 目录。
- 根目录专用普通文件 **`epaper-cache.bin`**，大小恰为 **65536 字节（64KiB）**；extent 树 **depth=0**，单个已初始化 extent 从逻辑块 0 起覆盖 16 块。不接受 sparse hole、unwritten extent、碎片化或更深的 extent 树。
- 支持常见默认特性 `64bit`、`flex_bg`、`metadata_csum`、`csum_seed`，以及干净、无待处理孤儿的 `orphan_file`；不接受 pending journal recovery 或 `orphan_present`。加密、verity、inline data 等不支持的特性或布局会被拒绝；这不是通用 ext4 驱动，也不是任意 ext4 卡都兼容。

设备只读定位 superblock、分区、目录、inode 与 extent 等元数据，随后只原位覆盖该文件的数据区。**不创建、截断或删除文件，不更新 inode、mtime、bitmap 或 journal，不格式化，不回放 journal，也不修复文件系统。**设备使用期间不要热插拔卡，也不能由电脑同时挂载或修改文件系统；电脑若移动/替换文件或改变布局，设备下次必须重新定位且仍须满足约束。

## 电脑准备

使用 Linux、Python 3 和 util-linux `findmnt`。固件下载包包含匹配版本的 `sd-cache/prepare_ext4_cache.py`；仓库中的路径为 `tools/prepare_ext4_cache.py`。先由用户准备并挂载满足上述约束的 ext4 文件系统，再把**已挂载的 ext4 根目录**作为唯一位置参数：

```sh
python3 tools/prepare_ext4_cache.py /media/USER/SDCARD
# 成功后正常卸载；这里不是设备节点，也不是根目录下的子目录
umount /media/USER/SDCARD
```

[准备工具](../tools/prepare_ext4_cache.py)只新建根目录中的专用 64KiB 文件：拒绝覆盖已有路径、拒绝非 ext4 或非挂载根目录；不格式化、不打开块设备、不操作其他文件。它将完整 65536 字节模板实际写入（帧和尾部保留区用非零数据初始化），并对文件及目录 `fsync`；初始 `valid=0`，尚无可读旧帧。**不能用 `truncate` 或仅 `fallocate` 代替实际初始化写入**，否则可能留下空洞或 unwritten extent。

工具还检查 4KiB block、根目录非 indexed/非加密，并在 `fsync` 后用只读 FIEMAP 检查单个已初始化 extent；不保存物理地址供设备使用。这不代替设备对整个 ext4 布局的检查，仍须满足上述 inode、extent depth、特性与干净状态约束。失败时可能留下新建文件，工具不会自动删除或覆盖它。准备后必须正常卸载，再将卡交给设备；首次无有效帧会全刷，显示成功后才保存帧。

## 文件格式与提交

所有多字节整数为 little-endian；CRC32 使用 IEEE / zlib 算法。

| 文件偏移 | 长度 | 内容 |
| --- | ---: | --- |
| 0 | 8 | magic `EPDCACHE` |
| 8 | 4 | version = 1 |
| 12 | 4 | frame_size = 48000 |
| 16 | 4 | valid = 0 / 1 |
| 20 | 4 | frameCRC32，覆盖 48000 字节帧 |
| 24 | 4 | headerCRC32，覆盖头的 `[0,24)` 字节 |
| 28 | 484 | 头内保留，置零；头共 512 字节 |
| 512 | 48000 | 800×480、1bpp，逐行、高位在左，0 黑／1 白 |
| 48512 | 17024 | 容器剩余保留区 |

应用帧仍为 96000 字节 packed 2bpp；仅无灰度缓存读入后展开、写出前压为 1bpp。全刷轮次跳过旧帧读取。保存时**先使头失效，再分批写帧，最后提交有效头**；读取检查 magic、版本、长度、valid、头 CRC 和帧 CRC，任一失败都不用缓存并全刷。显示失败仅尝试将头设为 invalid，不删除容器；失效写入本身也可能因 I/O 故障失败。

头最后提交与双 CRC 是缓存完整性检测，**不等于原子文件系统更新、事务日志或掉电保护**。单份数据原位覆盖不保证保留上一帧；SD 控制器/FTL 可能缓存、重排或撕裂写入，CRC 也不是绝对无碰撞或身份认证。真正掉电、文件系统与卡内部行为仍需实卡验证，不能由镜像测试推断可靠性或节电幅度。

## 测试与待验收

Linux 主机可用 C 编译器和已安装的 `mke2fs`、`debugfs`、`e2fsck` 运行测试：

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -Icomponents/sdcard_bsp \
  tests/ext4_cache_test.c components/sdcard_bsp/ext4_cache.c -o /tmp/ext4-cache-test
python3 tests/ext4_cache_image_test.py /tmp/ext4-cache-test
```

已通过的测试仅使用临时普通文件镜像，不使用 sudo、loop 挂载或真实块设备：完整读写、缓存失效、头/帧 CRC 损坏、错误大小/稀疏/unwritten/待恢复布局拒绝、MBR 分区偏移、模拟中断及重写恢复；全镜像比对确认容器外所有字节未变，无关文件内容不变，`e2fsck -fn` 检查通过。

当前 64MiB 测试镜像中，定位读取 15 个扇区，MBR 包裹时为 17 个；不是任意卡的固定上限。单帧读取 13 个请求／95 扇区，写入 14 个请求／96 扇区（含失效与提交头）。核心约 535 行 C；ESP32-S3 链接 map 中该核心对象的 text/literal/rodata 合计约 4.4KiB（不含对齐与共享 SDK 依赖），缓存上下文为 4,136 字节，包括静态 4KiB 对齐缓冲，不在任务栈分配整块。帧和元数据 CRC 使用小型四位查表，两个表合计 128 字节只读数据。上述不是时延或耗电实测。

仍需实卡验收：电脑准备工具在真实挂载卷上的完整使用流程、SDMMC 时序、显示失败失效、PMU 分钟循环、真实掉电与功耗。镜像测试不验证卡内 FTL、写入原子性或持久化行为。
