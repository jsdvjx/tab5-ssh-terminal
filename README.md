# Tab5 SSH Terminal

把 M5Stack Tab5(ESP32-P4)变成一台便携 SSH 终端 / 掌上 vibecoding 设备:
SSH 连到开发机跑 Claude Code、tmux、vim,实体键盘 + 触摸屏 + 完整终端体验
(真彩色、中文、Nerd Font、彩色 emoji),外加一套图形化的启动器界面、
拼音输入法、蓝牙配网、多语言。

A hardware SSH terminal for the M5Stack Tab5 (ESP32-P4): full xterm-256color
emulation with CJK / Nerd Font / color emoji, a touch launcher UI, an on-device
pinyin IME, BLE provisioning, multi-session SSH and key auth — built on ESP-IDF.

> 公开仓库 / repo: <https://github.com/jsdvjx/tab5-ssh-terminal>
> 蓝牙配网页 / BLE setup page: <https://t5.cc.hn>

---

## 功能 / Features

### 终端 Terminal
- 自研 xterm-256color 核心(`main/term/`):256 色 / 真彩、滚动区域、
  alternate screen、宽字符(CJK 占 2 格)、粗体 / 下划线 / 反显;
  Claude Code、tmux、htop、vim 正常使用
- LVGL canvas 渲染:脏行追踪、字体回退链、powerline 分隔符按几何绘制、
  em 宽图标 / emoji 第二遍叠绘
- SD 资产包:全量 CJK 字体 + Nerd Font + ~1500 彩色 Twemoji + 彩色系统 logo
  图集,开机载入 32MB PSRAM;无卡时回退内置字库

### SSH
- libssh2(mbedTLS):**最多 4 路并发会话**,`Ctrl+Alt+1..9` 切换 / 懒加载,
  `Ctrl+Alt+W` 关闭
- **认证可选**:自动(密钥优先) / 仅密码 / 仅证书,逐目标配置
- **设备密钥**:开机自生成 RSA-2048(存 NVS),`sshkey` 查看公钥,可经蓝牙
  上传自定义私钥
- **主机密钥固定**(host key pinning):首连记录指纹,变更则红字告警拒连
- 断线自动重连

### 图形界面 GUI
- 启动器式主界面:点色块卡片展开为 app + 底部 dock,彩色品牌系统图标
- SSH 设备网格:大图标(Apple/Linux/Ubuntu/Debian/Windows/树莓派/服务器)+
  状态点;点设备弹「连接 / 编辑 / 取消」
- 图形化 SSH 目标增删改查(含软键盘,插实体键盘时自动隐藏)
- 顶栏:Wi-Fi 信号强度图标 + IP + 电量 + 时钟,连接中有脉冲动画
- 开机动画(打字机 logo,作为启动屏 hold 到面板就绪)
- **多语言**:中文 / English 运行时切换(`main/i18n.*`)

### 连接 / 输入 / 电源
- **Wi-Fi**:多账号保存、扫描选网、后台无限重试连接
- **蓝牙配网**(NimBLE,HCI over SDIO):手机 / Chrome 经 <https://t5.cc.hn>
  推送 Wi-Fi 凭据 + SSH 私钥;默认关闭,面板里开关
- **拼音输入法**:移植 libgooglepinyin(`components/ime_pinyin`),`Ctrl+Space`
  切换,LVGL 候选条,词库走 SD;任何远程程序里都能打中文
- **本地 shell**(esp_console):`Ctrl+Alt+T` 切换,`free`/`ps`/`batt`/`ls`/
  `sshkey`/`ble`… 一套本机控制台命令
- **电源**:INA226 电量、RX8130 RTC、CPU 温度;空闲灭屏(任意键 / 触摸唤醒)、
  `Ctrl+Alt+L` 锁屏(防误触,仅 `Ctrl+Alt+L` 解锁)、软关机、自动充电使能
- 双键盘:USB-A 实体键盘 + M5Stack Tab5 专用键盘(I2C HID),共用键码管线

---

## 硬件 / Hardware

M5Stack Tab5:ESP32-P4(双核 RISC-V 360MHz,32MB PSRAM,16MB flash)+
ESP32-C6(Wi-Fi / BLE,SDIO 互联),5″ 1280×720 MIPI-DSI 触摸屏,
可拆 NP-F550 电池(2S 7.4V 2000mAh)。

---

## 构建 / Build

```sh
# ESP-IDF v5.5.x
. ~/esp/esp-idf/export.sh
idf.py build flash monitor
```

开机直接进图形界面;Wi-Fi 凭据可经 <https://t5.cc.hn>(蓝牙)、面板里的
「连接 → 添加网络」、或 Web 配置页设置。然后在 SSH 卡里添加目标主机。

### SD 资产包 / SD asset pack(强烈推荐)

```sh
python3 tools/make_assets.py        # 需要 node、python3 + Pillow、rsvg-convert
# 方式一:把 assets_sd/tab5/ 拷到 SD 卡根目录
# 方式二:无读卡器,经设备 HTTP 推送(设备会自行格式化空卡):
for f in assets_sd/tab5/*.bin; do
  curl -X POST --data-binary @"$f" "http://<设备IP>/api/sdput?name=$(basename "$f")"
done
curl -X POST http://<设备IP>/api/reboot
```

资产包内容:`nerd24/cjkfull24/emoji24`(终端字体 + emoji)、`osicons32/64`
(彩色系统 logo)、`dict_pinyin.dat`(拼音词库)。

---

## 远程调试 / Web API

开发期保留的调试基建(纯只读 + 资产推送,改变状态的控制端点已移除):

| 接口 | 说明 |
|---|---|
| `GET /` | SSH 目标配置页 |
| `GET/POST /api/targets` | 目标列表读写(密码不回传,留空保持) |
| `POST /api/sdput?name=x` | 写文件到 SD 卡 `/tab5/` |
| `POST /api/reboot` | 重启 |
| `GET /shot[?full=1&panel=1&…]` | 截图 BMP(`full=1` 整屏含面板;`&edit/&conn/&keys/&sheet/&lang` 为 UI 开发助手) |
| `GET /debug` | 输入 / SSH / 渲染流水线计数器 + 堆 / 电量 |
| `GET /api/ime?py=nihao` | 拼音引擎查询(无键盘验证) |

---

## 代码结构 / Layout

```
main/
  term/                终端模拟核心 + LVGL 渲染(脏行 / 字体回退 / powerline 几何)
  ssh_client.c         多会话 libssh2,逐目标认证,host-key pinning
  ssh_keys.c           设备 RSA 密钥(NVS / 后台生成),指纹固定存储
  hid_keyboard.c       USB HID 键盘 + 键码翻译(两种键盘共用)
  i2c_keyboard.c       Tab5 专用键盘(I2C HID 模式)
  ui_home.c            启动器面板:卡片 / dock / 设备网格 / 各 app / 编辑表单
  i18n.c               多语言字符串表(中 / 英,运行时切换)
  ime_filter.cpp       拼音 IME 状态机(键盘滤镜 → ssh_client_send)
  ime_bar.c            LVGL 候选条
  local_shell.c        esp_console 本地命令行(Ctrl+Alt+T)
  ble_prov.c           NimBLE 配网 + 密钥上传(HCI over SDIO)
  wifi.c               C6 上电 + esp_hosted 延迟初始化 + 扫描 / 连接 / 多账号
  power_mon.c          INA226 电量 / 电流 / 功率
  power_mgmt.c         灭屏 / 锁屏 / 关机 / 充电使能
  rtc_rx8130.c         RX8130 RTC + SNTP
  status_bar.c         顶部状态栏
  boot_anim.c          开机动画 / 启动屏
  assets.c             SD 资产包加载(binfont / emoji 图集 / 系统 icon / 词库)
  settings.c           NVS 配置(Wi-Fi 多账号 / SSH 目标 / 各项偏好)
  web_config.c         HTTP 配置服务 + 调试接口
components/
  ime_pinyin/          libgooglepinyin 移植(ESP-IDF 组件,Apache-2.0)
  m5stack_tab5/        本地化 BSP(ST7121 面板支持,板版本 3 自动识别)
  esp_lcd_st7121/      ST7121 驱动(移植自 M5Tab5-UserDemo)
tools/
  make_assets.py       SD 资产包生成(Nerd / CJK / Twemoji / 彩色 logo / 词库)
  bootanim_preview.py  开机动画设计预览(生成 GIF)
  patch_esp_hosted.py  esp_hosted 必要补丁(CMake 配置阶段自动执行)
  ime_host/            libgooglepinyin 的 Mac 端验证(CLI 测试,克隆不入库)
web/
  t5_site/             蓝牙配网页(部署在 t5.cc.hn,不入库)
```

---

## 已知坑位 / Gotchas(换板子 / 升级依赖前必读)

1. **ST7121 面板**:2026-04-28 之后的批次换用 ST7121,esp-bsp ≤1.2.0 只认
   ST7123,误判后初始化被面板静默忽略 → 黑屏但触摸正常。本仓库本地 BSP
   通过触摸固件版本(reg 0x0000:1=ST7121,3=ST7123)自动识别板版本。
2. **esp_hosted 构造函数启动循环**:自带 `__attribute__((constructor))` 在堆
   就绪前 init 导致 SDIO 内存池分配失败。`tools/patch_esp_hosted.py` 构建时
   自动重打(依赖重新拉取后会丢,勿删脚本)。
3. **PSRAM 栈任务禁碰 flash**:会话任务栈放 PSRAM 省内部 RAM,但其中任何
   NVS / flash 操作会在关 cache 后断言崩溃(boot loop)。host-key pin 用 RAM
   缓存 + 内部栈 worker 落盘解决。
4. **内部 RAM 紧张**:LVGL 对象多,`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`
   从 4096 调到 256,让小对象去 PSRAM,否则 SSH 握手内存不足报错。
5. **C6 固件 0.0.0**:出厂 C6 esp_hosted 固件旧,偶发 RPC 超时致 Wi-Fi 拿不到
   IP;启动用后台无限重试吸收,不阻塞 UI。
6. **新增 bool 设置项**:追加字段在旧 blob 上补零 = false,需按「默认值=0」
   设计标志位(如 `ble_enabled` 默认关)。
7. **LVGL 渲染**:缺字形的绘制任务会死锁 canvas(已用 `get_glyph_dsc` 前置
   检查规避);SD 大字体需 `CONFIG_LV_USE_CLIB_MALLOC=y`;binfont >1MB 字形
   需 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`(否则 20 位 bitmap_index 溢出 → 乱码)。
8. SD 卡与 C6 共用 SDMMC:资产在 Wi-Fi 启动前读完即卸载。

---

## License

MIT。`components/ime_pinyin` 移植自 libgooglepinyin(Apache-2.0);
`components/esp_lcd_st7121` 来自 M5Stack;彩色系统图标来自
[Devicon](https://github.com/devicons/devicon)(MIT)。各依赖以其原始声明为准。
