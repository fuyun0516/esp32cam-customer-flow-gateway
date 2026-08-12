# ESP32-CAM 智能客流与延时摄影网关

一个运行在 AI Thinker ESP32-CAM 上的本地客流与延时摄影项目。设备可提供 MJPEG 实时预览、定时保存照片、基于画面变化估算客流、浏览器数据看板和延时回放；配套的电脑端脚本可从局域网下载数据并生成每日竖屏宣传视频。

> 当前客流功能是轻量级运动估算，不是人体检测、人脸识别或身份识别。部署前请阅读下方的“算法限制与隐私”章节。

## 功能概览

- 640×480 MJPEG 实时预览，目标帧率 15 FPS
- 每 5 秒保存一张 640×480 JPEG 到 MicroSD 卡
- 按小时建立目录，已同步时间时使用 `YYYYMMDDHH/HHMMSS.jpg`
- SD 卡空间不足时优先删除最旧照片，并清理空目录
- 当前日期客流计数和按小时柱状图
- 浏览器内按顺序播放延时照片，支持 1×、3×、5×
- AP 配网页面，可加入 2.4 GHz 局域网并通过 `esp32cam.local` 访问
- 日夜光线大幅变化时自动恢复曝光并重新校准运动背景
- Windows/Python 每日视频工具：下载事件附近照片、生成字幕、旁白、背景音乐和高峰时段总结
- 热点主题与灾害提醒分离；灾害信息只可作为结尾安全提醒

## 硬件需求

- AI Thinker ESP32-CAM（带 PSRAM）
- OV2640 摄像头
- FAT32 格式 MicroSD 卡
- 稳定的 5 V 电源
- USB-TTL 烧写器，或具备 USB 转串口功能的底板
- 仅支持 2.4 GHz Wi-Fi

仓库中还保留了项目曾使用的 GC0328 相机驱动，但当前固件配置以 OV2640 为主。

## 仓库结构

```text
.
├─ src/main.cpp                    ESP32 固件、网页和 HTTP API
├─ web/chart.umd.min.js            离线趋势图组件
├─ lib/esp32-camera-gc0328/        相机驱动
├─ scripts/                        数据同步、视频生成和 Windows 辅助脚本
├─ tests/                          Python 自动测试
├─ platformio.ini                  PlatformIO 构建配置
└─ requirements.txt                视频工具的 Python 依赖
```

照片、视频、本地工具和编译缓存分别保存在 `media/`、`output/`、`tmp/`、`.tools/` 和 `.pio/`，这些目录已被 Git 忽略，不会在提交代码时上传。

## 编译和烧写固件

### 使用 PlatformIO CLI

1. 安装 [Python](https://www.python.org/) 3.11 或更高版本。
2. 安装 PlatformIO：

   ```powershell
   python -m pip install platformio
   ```

3. 在项目根目录编译：

   ```powershell
   pio run
   ```

4. 连接 ESP32-CAM，进入烧写模式后上传。把示例端口换成电脑实际端口：

   ```powershell
   pio run --target upload --upload-port COM7
   ```

5. 重启设备并查看 115200 波特率串口日志：

   ```powershell
   pio device monitor --baud 115200 --port COM7
   ```

也可以使用仓库中的 PowerShell 日志脚本：

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\scripts\serial-monitor.ps1 -PortName COM7
```

如果电脑只有一个串口，`-PortName` 可以省略。日志默认写入 `tmp/serial-live.log`。

### 烧写模式提示

常见 ESP32-CAM 烧写方式是将 GPIO0 接地后复位，再开始上传；上传完成后断开 GPIO0 与 GND，并再次复位。不同底板的按键和自动下载电路可能不同，请以硬件说明为准。

## 首次联网配置

1. 插入 FAT32 MicroSD 卡并启动设备。
2. 手机连接热点 `ESP32-CAM-DAY1`。
3. 输入初始密码 `esp32cam`。
4. 如果系统没有自动弹出配网页面，打开 `http://192.168.4.1/network`。
5. 选择附近的 2.4 GHz Wi-Fi，输入密码并保存。
6. 设备会重启。让手机或电脑连接同一个局域网，然后打开 `http://esp32cam.local/`；如果 mDNS 不可用，请使用串口中打印的 LAN IP。

路由器 SSID 和密码保存在 ESP32 的 Preferences 中，不写入源代码或 SD 卡。公开部署前建议修改 `src/main.cpp` 中的默认 AP 名称和密码。

## 浏览器页面与 API

| 地址 | 用途 |
| --- | --- |
| `/` | 实时预览、客流状态、趋势图和延时回放 |
| `:81/stream` | MJPEG 视频流 |
| `/network` | 扫描并配置局域网 Wi-Fi |
| `/api/status` | 设备、SD 卡、客流和曝光状态 |
| `/api/flow.csv` | 历史客流 CSV |
| `/api/timelapse` | 延时照片索引范围 |
| `/api/catalog?after=...` | 照片目录增量数据 |
| `/api/photo?index=...` | 按内部序号读取照片 |

## SD 卡数据结构

浏览器同步正确时间后，照片使用以下结构：

```text
/timelapse/
└─ 2026081214/
   ├─ 140005.jpg
   ├─ 140010.jpg
   └─ 140015.jpg

/data/
├─ flow.csv
├─ photo-index.bin
└─ capture-state.txt
```

设备尚未获得可信时间时，会使用序号小时目录，避免写入错误日期。`photo-index.bin` 用于把内部连续序号映射到实际照片路径。清理旧照片不会删除 `flow.csv`。

## 每日视频工具

电脑和 ESP32-CAM 需要连接同一个局域网。安装 Python 依赖：

```powershell
python -m pip install -r requirements.txt
```

FFmpeg 按以下顺序查找：

1. 环境变量 `FFMPEG_EXE` 指定的文件；
2. 系统 `PATH` 中的 `ffmpeg`；
3. 本地 `.tools/video-deps`；
4. `imageio-ffmpeg` 提供的可执行文件。

生成指定已结束日期的视频：

```powershell
python .\scripts\run_daily_video.py `
  --device http://esp32cam.local `
  --day 20260811 `
  --store-name "示例门店" `
  --store-region "示例省示例市"
```

默认输出文件为：

```text
output/YYYYMMDD/YYYYMMDD_店铺宣传_每日高光版.mp4
```

常用参数：

- `--skip-sync`：使用已经下载到 `media/` 的数据
- `--all-photos`：下载当天全部照片，而不是只下载事件片段和背景样本
- `--skip-hot-trends`：不读取热点，使用通用文案
- `--hot-topic "..."`：使用人工审核后的主题
- `--safety-notice "..."`：使用人工审核后的结尾安全提醒

门店名和地区也可以通过环境变量设置：

```powershell
$env:ESP32_STORE_NAME = '示例门店'
$env:ESP32_STORE_REGION = '示例省示例市'
```

项目不会自动读取 `.env` 文件；根目录的 `.env.example` 仅用于说明可配置项。

### Windows 每日定时任务

以下示例每天 00:20 处理前一天的数据：

```powershell
powershell.exe -ExecutionPolicy Bypass -File .\scripts\install_daily_video_task.ps1 `
  -Device http://esp32cam.local `
  -RunAt 00:20 `
  -StoreName "示例门店" `
  -StoreRegion "示例省示例市" `
  -PythonExe python
```

电脑必须在计划时间开机且未休眠，并能访问 ESP32-CAM。任务日志写入 `output/task-logs/`。

## 算法限制与隐私

- 固件根据远处小范围运动、靠近时覆盖面积增大、随后目标消失的过程估算一次客流。
- 它不能确认目标一定是人，也不能稳定区分并排行走的多人。
- 背景变化、遮挡、摄像头抖动、强逆光和安装角度都会影响结果。
- 需要可靠人体识别、多人跟踪或进出方向判断时，应把视频交给电脑、树莓派或服务器运行专用检测模型。
- 照片和客流数据默认只保存在设备 SD 卡和运行同步脚本的电脑中；项目本身不提供云上传。
- 请勿把真实门店 Wi-Fi 密码、顾客照片、生成视频或日志提交到公开仓库。
- 在公共或商业场所部署摄像头前，应遵守当地隐私、告知、数据保存和肖像权法规。
- 自动热点和安全提醒只能作为辅助素材，发布前应人工核实来源、时效和措辞。

## 测试

运行 Python 测试：

```powershell
python -m unittest discover -s tests -v
```

验证固件：

```powershell
pio run
```

## 参与贡献

提交问题或改动前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。不要在 issue、日志或测试数据中上传 Wi-Fi 密码、真实顾客照片和其他个人信息。

## 许可证

本项目代码使用 [Apache License 2.0](LICENSE)。第三方组件及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
