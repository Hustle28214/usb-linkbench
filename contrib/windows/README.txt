usb-linkbench Windows x64

这是 Release 命令行包。解压后在这个目录里敲命令。

```powershell
.\usb-linkbench.exe timing-check
.\usb-linkbench.exe list --cameras
.\usb-linkbench.exe links
```

`timing-check` 结果要是「通过」。`list --cameras` 打出厂商号、产品号、口位置、协商速率，以及 Media Foundation 的视频链接。链接很长，还含有 `&`，传给 `--device` 时整段用引号包起来。

```powershell
.\usb-linkbench.exe stream --device "\\?\usb#vid_2bc5&pid_....." --seconds 5
```

视频走系统自带的 UVC 驱动，不要用 Zadig 把相机换成 WinUSB，换了之后 `stream` 就看不到这台相机。`rtt` 和 `bulk` 反过来，需要 WinUSB 才能打开设备；那两项测完想再测视频，得把驱动换回去。

`audit` 在没装 WinUSB 时从集线器读描述符，不占用接口。Windows 的集线器接口分不清 10 Gbit/s 和 20 Gbit/s，结果里会写明。

`links` 和 `report --collect` 不读 Thunderbolt、PCIe、CAN、供电轨，那些只在 Linux 的 sysfs 上有。`storage` 用无缓冲读，能认出是不是 USB 盘，认不出协商速率。

图形面板不在这个包里。要面板就在装了 Qt 的机器上自己编，`-DBUILD_GUI=ON`。
