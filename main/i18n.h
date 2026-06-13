// Central runtime-switchable string table for the home launcher (ui_home.c).
// Two languages: Chinese (default) and English. T(id) returns the string in
// the active language; i18n_set_lang() flips the active language. The active
// language is also persisted in settings_t.lang (see settings.h / app_main).
//
// NOTE: the IME mode indicator ("中"/"EN" in the status bar, set from
// ime_filter via status_bar_set_ime) reflects the *pinyin input mode*, not the
// UI language — it is intentionally NOT part of this table and stays as-is.
#pragma once

typedef enum {
    LANG_ZH = 0,    // 中文 (default, == 0 so a fresh/old settings blob is ZH)
    LANG_EN,        // English
    LANG_COUNT,
} lang_t;

// One ID per user-facing string in ui_home.c. Keep in sync with the S[][]
// table in i18n.c (a missing entry returns "" rather than crashing).
typedef enum {
    T_SSH_SESSIONS = 0, // 会话
    T_CONNECT,          // 连接 (action / "Connectivity" card title share this)
    T_DISPLAY,          // 显示
    T_SYSTEM,           // 系统
    T_IME,              // 输入法
    T_MANAGE,           // 管理
    T_ADD_TARGET,       // 添加目标
    T_EDIT_TARGET,      // 编辑目标
    T_NAME,             // 名称
    T_HOST,             // 主机
    T_PORT,             // 端口
    T_USER,             // 用户
    T_PASSWORD,         // 密码
    T_COMMAND,          // 命令
    T_AUTH,             // 认证
    T_AUTH_AUTO,        // 自动
    T_AUTH_PASSWORD,    // 密码 (auth picker)
    T_AUTH_CERT,        // 证书
    T_SAVE,             // 保存
    T_CANCEL,           // 取消
    T_DELETE,           // 删除
    T_CONFIRM_DELETE,   // 确认删除?
    T_SAVE_CONNECT,     // 保存并连接
    T_SCAN,             // 扫描
    T_SCANNING,         // 扫描中…
    T_NEARBY_NETS,      // 附近网络
    T_SAVED_NETS,       // 已存网络
    T_NO_NETS,          // 未发现网络
    T_NO_SAVED_NETS,    // 无已存网络
    T_ADD_NET,          // 添加网络
    T_FORGET,           // 忘记
    T_CONNECTED,        // 已连接
    T_DISCONNECTED,     // 未连接
    T_CONNECTING,       // 连接中
    T_EDIT,             // 编辑 (action sheet)
    T_BT,               // 蓝牙
    T_BT_SWITCH,        // 蓝牙开关
    T_BT_OFF,           // 蓝牙已关闭
    T_ADVERTISING,      // 正在广播
    T_ADV_STOPPED,      // 广播已停止
    T_START_ADV,        // 开始广播
    T_STOP_ADV,         // 停止广播
    T_KEY,              // 密钥
    T_DEVICE_KEY,       // 设备密钥
    T_KEY_READY,        // 已就绪
    T_KEY_GENERATING,   // 生成中…
    T_NO_KEY,           // 无密钥
    T_REGEN,            // 重新生成
    T_CONFIRM_REGEN,    // 确认重新生成?
    T_PUBKEY_HINT_AT,   // 完整公钥可在
    T_PUBKEY_HINT_COPY, // 复制或
    T_PUBKEY_HINT_RUN,  // 执行
    T_PRIVKEY_HINT,     // 自定义私钥通过蓝牙上传
    T_WIFI_NOT_CONNECTED, // Wi-Fi 未连接，请先连接网络
    T_LANGUAGE,         // 语言
    T_LANG_ZH,          // 中文 (toggle option label)
    T_LANG_EN,          // English (toggle option label)
    T_COUNT,
} str_id_t;

const char *T(str_id_t id);
void  i18n_set_lang(lang_t lang);
lang_t i18n_lang(void);
