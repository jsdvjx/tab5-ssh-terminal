# Tab5 SSH Terminal

把 M5Stack Tab5(ESP32-P4)变成一台 vibecoding 终端:SSH 连到开发机跑
Claude Code,实体键盘 + 触摸屏 + 完整的终端体验(真彩色、中文、Nerd Font、
彩色 emoji)。

A hardware SSH terminal for the M5Stack Tab5 — full xterm-256color emulation
with CJK, Nerd Font glyphs and color emoji, built on ESP-IDF.

## 功能

- **终端模拟**:自研 xterm-256color 核心(`main/term/`),支持 256 色/真彩、
  滚动区域、alternate screen、宽字符(汉字占 2 格)、粗体/下划线;Claude
  Code、tmux、htop、vim 正常使用
- **SSH 客户端**:libssh2,多目标管理、断线自动重连、运行时切换
- **双键盘**:USB-A 实体键盘 + M5Stack Tab5 专用键盘(I2C,HID 模式)
- **右侧面板**:Wi-Fi 状态、Web 配置地址、SSH 目标列表(触摸切换);
  `Ctrl+Alt+P` 隐藏面板,终端扩展到全屏 106 列
- **Wi-Fi 引导**:首次开机扫描 → 触摸选 SSID → 键盘输密码,存 NVS
- **Web 配置**:`http://<设备IP>/` 管理 SSH 目标;另有调试接口(见下)
- **SD 资产包**:全量 CJK 字体、Nerd Font 符号、约 1500 个彩色 emoji,
  开机载入 PSRAM;无卡时回退到内置 GB2312 字库

## 硬件

M5Stack Tab5:ESP32-P4(双核 RISC-V 360MHz,32MB PSRAM,16MB flash)+
ESP32-C6(Wi-Fi,SDIO 互联),5 寸 1280×720 MIPI-DSI 触摸屏。

## 构建

```sh
# ESP-IDF v5.5.x
. ~/esp/esp-idf/export.sh
idf.py build flash monitor
```

首次开机按屏幕提示完成 Wi-Fi 配置,然后浏览器打开面板上显示的地址添加
SSH 服务器。

### SD 资产包(可选,强烈推荐)

```sh
python3 tools/make_assets.py        # 需要 node、python3+Pillow
# 方式一:把 assets_sd/tab5/ 拷到 SD 卡根目录
# 方式二:无读卡器,直接通过设备 HTTP 推送(设备会自行格式化空卡):
for f in assets_sd/tab5/*.bin; do
  curl -X POST --data-binary @$f "http://<设备IP>/api/sdput?name=$(basename $f)"
done
curl -X POST http://<设备IP>/api/reboot
```

## Web API

| 接口 | 说明 |
|---|---|
| `GET /` | SSH 目标配置页 |
| `GET/POST /api/targets` | 目标列表读写(密码不回传,留空保持) |
| `POST /api/connect` | `{"index":n}` 切换连接 |
| `POST /api/sdput?name=x` | 写文件到 SD 卡 `/tab5/` |
| `POST /api/reboot` | 重启 |
| `GET /shot` | 终端画面 BMP 截图(远程调试渲染) |
| `GET /debug` | 输入/SSH/渲染各级流水线计数器 |

## 代码结构

```
main/
  term/term.c          终端模拟核心(无渲染依赖)
  term/term_render.c   LVGL canvas 渲染:脏行、字体回退链、powerline 几何绘制
  ssh_client.c         libssh2 会话任务,多目标切换
  hid_keyboard.c       USB HID 键盘 + 键码翻译(两种键盘共用)
  i2c_keyboard.c       Tab5 专用键盘(I2C HID 模式)
  ui_panel.c           右侧 LVGL 面板
  wifi.c               C6 上电 + esp_hosted 延迟初始化 + 扫描/连接
  settings.c           NVS 多目标配置
  setup.c              开机 Wi-Fi 引导(触摸选网 + 终端行编辑)
  web_config.c         HTTP 配置服务 + 调试接口
  assets.c             SD 资产包加载(binfont + emoji 图集)
components/
  m5stack_tab5/        本地化 BSP(新增 ST7121 面板支持,板版本 3 自动识别)
  esp_lcd_st7121/      ST7121 驱动(移植自 M5Tab5-UserDemo)
tools/
  make_assets.py       SD 资产包生成(Nerd Font / 全量 CJK / Twemoji)
  patch_esp_hosted.py  esp_hosted 必要补丁(CMake 配置阶段自动执行)
```

## 已知坑位(换板子/升级依赖前必读)

1. **ST7121 面板**:2026-04-28 之后的批次换用 ST7121,esp-bsp ≤1.2.0 只认
   ST7123,误判后初始化被面板静默忽略 → 黑屏但触摸正常。本仓库的本地 BSP
   通过触摸固件版本(reg 0x0000:1=ST7121,3=ST7123)自动识别三种板版本。
2. **esp_hosted 构造函数启动循环**:`tools/patch_esp_hosted.py` 的注释里有
   完整说明,构建时自动重打,勿删。
3. **Tab5 SDIO 引脚**与 esp_hosted 默认值不同(见 `sdkconfig.defaults`);
   C6 需先经 IO 扩展器上电(`bsp_feature_enable(BSP_FEATURE_WIFI)`)。
4. **LVGL 渲染**:缺字形的绘制任务会死锁 canvas(渲染器已用
   `get_glyph_dsc` 前置检查规避);SD 大字体需要
   `CONFIG_LV_USE_CLIB_MALLOC=y`;字体行高需运行时校正到格高。
5. SD 卡与 C6 共用 SDMMC 外设:资产在 Wi-Fi 启动前读完即卸载。

## 路线图

- [ ] 拼音输入法(键盘滤镜 + LVGL 候选条 + SD 词库,进行中)
- [ ] 语音输入(ES7210 双麦 → 主机 whisper)
- [ ] 面板:电池电量(INA226)、RTC 时钟
- [ ] SSH 公钥认证 + 主机密钥校验
- [ ] 滚动回看(触摸手势)

## License

MIT(`components/esp_lcd_st7121` 来自 M5Stack,Apache-2.0/MIT 按其原始声明)
