# usb-linkbench

Linux 上的相机 USB 与高速链路测量工具。它面向奥比中光（厂商号 `2BC5`）和 Intel RealSense 这类 UVC 相机，用来回答三件不同的事：

- 口协商到了哪一档速率
- 控制传输的回传时间是多少
- 驱动实际交出的视频载荷有多快

CrystalDiskInfo 看的是磁盘健康，不测量这些。本工具是独立项目。

实现使用 C++20、libusb 和 Linux sysfs。视频载荷通过 V4L2 mmap 统计驱动交出的 `bytesused`。预算和 `report` 打印的测试项说明不需要打开设备。

## 数字不要混读

| 命令 | 种类 | 实际测到的是什么 |
| --- | --- | --- |
| `list` / `links` | 协商链路 | 操作系统报告的 USB、Thunderbolt、PCIe 已训练速率 |
| `audit` | 描述符上限 | 由端点描述符和服务间隔算出的周期端点预算 |
| `budget` | 帧预算 | `宽 × 高 × 每像素字节 × 帧率`，再和规范上限比较 |
| `rtt` | 控制回传 | 一次 `GET_STATUS` 从提交前到完成的单调时钟间隔 |
| `stream` / `run` | 视频载荷 | Linux V4L2 `bytesused` 除以实测墙钟 |
| `bulk` | Bulk IN | 可占用的 Bulk IN 端点已完成的字节 |
| `storage` | 直接读 | 绕过页缓存的顺序读 |
| `timing-check` | 计时自检 | 时钟地板和一次 50 ms 睡眠，不是 USB 结果 |
| `report` | 测试项 | 上行 / 下行口、相机 640×480 保深度、VUT 120 Hz、PEAK-CAN 60 Hz、供电能力与并发验收各参数的含义 |

做不到时命令会失败或跳过，并写明原因。不会把 5 Gbit/s 填成实测吞吐。

## 计时

计时使用 `CLOCK_MONOTONIC_RAW`。预热样本丢弃。吞吐的分母是测到的墙钟，不是请求的秒数。计时开销单独列出，不从样本里扣除。

`rtt` 的每个样本是一笔控制传输，包含一次系统调用。这是应用程序看到的回传，不是协议分析仪上的线级时间。

`stream` 直接对内核 V4L2 做 mmap，不经过 libv4lconvert。`bytesused` 是驱动交出的帧，不含 UVC 包头和 8b/10b 编码。多路 `/dev/video` 会在预热后同时进入计时窗，合计是各路窗内速率之和。

## 构建

依赖 `cmake`、`pkg-config` 和 `libusb-1.0-dev`。

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

二进制是 `build/usb-linkbench`。

Linux 上打开设备节点需要写权限。奥比中光和 RealSense 的 udev 示例在 `contrib/60-usb-linkbench.rules`。视频节点通常还要求用户在 `video` 组。

## 用法

```sh
# 协商速率。--cameras 只保留奥比中光，以及产品字符串或已知 PID 能确认的 RealSense
usb-linkbench list --cameras

# 一台相机：描述符上限、GET_STATUS 回传、再采集码流
usb-linkbench run --vid 2bc5 --seconds 5 --preset gemini2-depth-1280x800-y16-30

# 只测回传。预热 20 次不入统计
usb-linkbench rtt --vid 8086 --pid 0b3a --count 200

# 已有的 V4L2 节点。不改格式时测当前模式
usb-linkbench stream --device /dev/video0 --seconds 5

# 未压缩 1080p30 在 USB 2.0 上等时端点放不下，在 USB 3.0 上放得下
usb-linkbench budget --preset d435-color-1920x1080-yuyv-30 --speed 5000

# U 盘或文件的顺序读。块大小按 4 KiB 对齐
usb-linkbench storage --path /media/usb/sample.bin --seconds 5

# 只有确认端点是 Bulk IN 时才用。默认不卸载内核驱动
usb-linkbench bulk --vid 1234 --pid 5678 --interface 0 --endpoint 0x81 --seconds 2

usb-linkbench links
usb-linkbench timing-check

# 端口与供电的测试项，以及每个参数的含义
usb-linkbench report
```

`--json` 把同一次结果打成 JSON。退出码 `1` 是命令失败，`2` 是测完了但判定没过，例如 `timing-check` 的睡眠异常，或 `stream` / `run --strict` 的帧率、帧字节不合格。

只有一路视频、并且只给了一个预设时，`run` 和 `stream` 会用该预设帧率的 95% 以及未压缩帧字节做判定。多路视频不会把同一个帧大小套到每一路。

`bulk --detach` 会卸掉该接口的内核驱动，结束时尝试挂回。相机视频不要走这条路径。

## 预设

`usb-linkbench budget` 不带参数时列出预设名字。预设是未压缩几何预算，不是某台相机的实测档案。MJPEG 这类变长码流没有固定字节数，所以没有放进预设。

## 许可

MIT OR Apache-2.0。见 `LICENSE-MIT` 和 `LICENSE-APACHE`。

## English

usb-linkbench measures negotiated USB, Thunderbolt, and PCIe link state, USB control round-trip, and, on Linux, V4L2 payload goodput for Orbbec and RealSense cameras. Descriptor ceilings and uncompressed frame budgets are labeled as planning numbers. The tool does not report a link rate as measured throughput.
# usb-linkbench
