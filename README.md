# 掌上赛道 · Tilt Rally

把 Tikpal Voice 当作方向盘，在 iPad 上跑桌面赛道。可把自己的小车模型放在屏幕上，或开启虚拟车。

1. 让 Tikpal Voice 和 iPad 连接同一个 Wi-Fi，或在圆屏配对页点屏幕切换到设备热点。
2. 用 iPad 相机扫圆屏上的赛道二维码，在 Safari 打开。握稳圆屏，短按侧键开始。
3. 左右倾斜转向，向前倾加速，向后倾刹车；短按侧键重新校准。iPad 左下角的地图可点开放大。

赛道网页由圆屏在局域网内提供，不需要云服务。

```bash
./kit build              # 编译，再跑主机测试
./kit install --slot 2   # USB 写进测试圆屏的位置 2，写完直接打开
./kit logs               # 看日志；./kit snap 截图，./kit heap 看内存
./kit build --release    # 提交用的那份
```

- `devkit/` 是开发包（git 子模块），钉在一个版本上，别改它。`git clone` 这个仓库以后，`./kit` 会自己把它拉下来。
- 应用清单是 `app.json`；逻辑写在 `main/`，能不依赖 ESP-IDF 的部分写成纯 C，`test/run.sh` 在电脑上测。
- 做完、自测过，照 `devkit/AGENTS.md` 第 4 步 `./kit submit` 打提交包，到开发者平台 developer.tikpal.ai 这个应用的「版本」页上传。
