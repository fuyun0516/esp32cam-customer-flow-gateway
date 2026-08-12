# 参与贡献

感谢你愿意改进这个项目。为了让改动容易复现和审核，请遵循以下约定。

## 开始之前

1. 先搜索已有 issue，避免重复报告。
2. 对较大的功能或硬件兼容改动，建议先建立 issue 说明目标、使用的摄像头和 ESP32 板型。
3. 不要上传 Wi-Fi 密码、局域网地址、串口设备标识、真实顾客照片、视频、日志或门店私有信息。

## 本地验证

```powershell
python -m pip install -r requirements.txt
python -m unittest discover -s tests -v
pio run
```

涉及硬件行为时，请在提交说明中写明：

- ESP32-CAM 板型和摄像头型号
- MicroSD 卡格式与容量
- 是否验证实时预览、照片写入、重启恢复和网页访问
- 串口日志中的关键结果（删除密码、IP 和设备标识后）

## 提交建议

- 每个提交尽量只解决一个问题。
- 保持 Python 脚本兼容 Python 3.11 及以上版本。
- 新增 Python 逻辑时同步添加或更新 `tests/` 中的测试。
- 修改 SD 卡格式或 HTTP API 时同步更新 README。
- 不要提交 `.pio/`、`.tools/`、`media/`、`output/`、`tmp/` 或 Python 缓存。

提交贡献即表示你同意按项目的 Apache License 2.0 提供该贡献。
