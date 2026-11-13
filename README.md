# usb-linkbench

详细用法见 [使用手册](docs/usb-linkbench/usage.md)。

Linux 上的相机 USB 与高速链路测量工具。它面向奥比中光（厂商号 `2BC5`）和 Intel RealSense 这类 UVC 相机，用来回答三件不同的事：

- 口协商到了哪一档速率
- 控制传输的回传时间是多少
- 驱动实际交出的视频载荷有多快

CrystalDiskInfo 看的是磁盘健康，不测量这些。本工具是独立项目。

代码是 C++20，USB 走 libusb，链路状态读 Linux sysfs。视频载荷用 V4L2 mmap，数的是驱动交出的 `bytesused`。`budget` 和 `report` 不打开设备。做不到的测量会失败或跳过，并写明原因。协商到 5 Gbit/s 不会被写成实测吞吐。

## 测试命令含义

| 命令 | 种类 | 实际测到的是什么 |
| --- | --- | --- |
| `list` / `links` | 协商链路 | 操作系统报告的 USB、Thunderbolt、PCIe 已训练速率 |
| `audit` | 描述符上限 | 由端点描述符和服务间隔算出的周期端点预算 |
| `budget` | 帧预算 | `宽 × 高 × 每像素字节 × 帧率`，再和规范上限比较 |
| `rtt` | 控制回传 | 一次 `GET_STATUS` 从提交前到完成的单调时钟间隔 |
| `stream` / `run` | 视频载荷 | Linux V4L2 `bytesused` 除以实测墙钟。`stream --profile quick` 重复短采并取最高；`--profile stress` 用浅队列长时间采，取最低并检查丢帧 |
| `bulk` | Bulk IN | 可占用的 Bulk IN 端点已完成的字节 |
| `storage` | 直接读 | 绕过页缓存的顺序读 |
| `timing-check` | 计时自检 | 时钟地板和一次 50 ms 睡眠，不是 USB 结果 |
| `report` | 测试项 | 上行 / 下行口、相机 640×480 保深度、VUT 120 Hz、PEAK-CAN 60 Hz、供电能力与并发验收各参数的含义 |

## 计时

计时使用 `CLOCK_MONOTONIC_RAW`。预热样本丢弃。吞吐的分母是测到的墙钟，不是请求的秒数。计时开销单独列出，不从样本里扣除。

`rtt` 的每个样本是一笔控制传输，包含一次系统调用。这是应用程序看到的回传，不是协议分析仪上的线级时间。

`stream` 直接对内核 V4L2 做 mmap，不经过 libv4lconvert。`bytesused` 是驱动交出的帧，不含 UVC 包头和总线编码。多路 `/dev/video` 会在预热后同时进入计时窗，合计是各路窗内速率之和。`stream --color-depth` 给彩色和深度各自设格式：深度取驱动列出的未压缩模式里预算最高的一档，彩色用同帧率的 MJPEG 陪跑。奥比中光保留深度时两路必须一起开。结果里的深度速率只算深度那一路。

## 编译

需要 `cmake`、C++20 编译器和 `libusb-1.0`。Linux 用 `pkg-config` 和 `libusb-1.0-dev`。macOS 用 `brew install cmake pkg-config libusb`。Windows 用 vcpkg 的 libusb，或运行 `scripts/package-windows.sh` 打出 `dist/usb-linkbench-windows-x64.zip`。图形看板还要 Qt Widgets，有 `qt6-base-dev` 就用 Qt6，否则用 `qtbase5-dev`。交叉编译 Windows 时默认不编看板。

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

命令行在 `build/usb-linkbench`。装了 Qt 还会生成 `build/usb-linkbench-gui`，不要它就加 `-DBUILD_GUI=OFF`。

```sh
./build/usb-linkbench-gui
```

打开设备节点需要写权限。奥比中光和 RealSense 的 udev 示例在 `contrib/60-usb-linkbench.rules`，用户要在 `plugdev` 组里。视频节点通常还要求 `video` 组。

```sh
sudo cp contrib/60-usb-linkbench.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## 用法

选项以 `usb-linkbench --help` 为准。`--json` 放在命令前面，把这一次的结果打成 JSON。

```sh
# 协商速率。--cameras 只保留奥比中光，以及产品字符串或已知 PID 能确认的 RealSense
usb-linkbench list --cameras

# 一台相机：描述符上限、GET_STATUS 回传、再采集码流
usb-linkbench run --vid 2bc5 --seconds 5 --preset gemini2-depth-1280x800-y16-30

# 只测回传。预热 20 次不入统计
usb-linkbench rtt --vid 8086 --pid 0b3a --count 200

# 已有的 V4L2 节点。不改格式时测当前模式
usb-linkbench stream --device /dev/video0 --seconds 5

# 快速性能：3 次 × 5 s，队列 8，取得分最高的一次
usb-linkbench stream --device /dev/video0 --profile quick

# 压力测试：3 次 × 10 s，队列 2，取得分最低的一次
usb-linkbench stream --device /dev/video0 --profile stress

# 彩色和深度一起开。深度选未压缩预算最高的模式，表上单列深度速率
usb-linkbench stream --vid 2bc5 --color-depth --seconds 5

# 未压缩 1080p30 在 USB 2.0 上等时端点放不下，在 USB 3.0 上放得下
usb-linkbench budget --preset d435-color-1920x1080-yuyv-30 --speed 5000

# U 盘或文件的顺序读。块大小按 4 KiB 对齐
usb-linkbench storage --path /media/usb/sample.bin --seconds 5

# 只有确认端点是 Bulk IN 时才用。默认不卸载内核驱动
usb-linkbench bulk --vid 1234 --pid 5678 --interface 0 --endpoint 0x81 --seconds 2

usb-linkbench links
usb-linkbench timing-check
usb-linkbench report
```

`run` 必须带 `--vid`、`--pid`、`--bus`、`--addr` 或 `--cameras`，避免对所有 USB 设备做回传。

退出码 `1` 是命令没跑成。`2` 是测完了但判定没过：`timing-check` 的睡眠异常，`stream --profile stress` 丢帧超过 1%、出现停滞或错误帧，以及 `stream` / `run --strict` 的帧率或帧字节不合格，都走这个码。

只有一路视频、并且只给了一个预设时，`run` 和 `stream` 会用该预设帧率的 95% 以及未压缩帧字节做判定。多路视频不会把同一个帧大小套到每一路。

`bulk --detach` 会卸掉该接口的内核驱动，结束时尝试挂回。相机视频不要走这条路径。

## 预设

`usb-linkbench budget` 不带参数时列出预设名字。预设是未压缩几何预算，不是某台相机的实测档案。MJPEG 这类变长码流没有固定字节数，所以没有放进预设。

## 许可

MIT OR Apache-2.0，见 `LICENSE-MIT` 和 `LICENSE-APACHE`。提交说明在 `CONTRIBUTING.md`。

## English

usb-linkbench measures negotiated USB link state, USB control round-trip, and video payload goodput on Linux (V4L2), macOS (AVFoundation), and Windows (Media Foundation). Thunderbolt and PCIe link state are read on Linux. Descriptor ceilings and uncompressed frame budgets are planning numbers. The tool does not report a link rate as measured throughput.

Build with CMake, a C++20 compiler, and libusb 1.0. Qt Widgets is optional. `scripts/package-windows.sh` produces a Windows x64 Release zip. `usb-linkbench --help` lists the flags. Exit status `1` is a failed command; `2` means the run finished and a check failed.
