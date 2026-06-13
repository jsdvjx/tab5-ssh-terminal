#include "i18n.h"

static lang_t s_lang = LANG_ZH;

// [LANG_COUNT][T_COUNT] string table. Row 0 = Chinese, row 1 = English.
// English kept concise to fit the same boxes as the Chinese source.
static const char *S[LANG_COUNT][T_COUNT] = {
    [LANG_ZH] = {
        [T_SSH_SESSIONS]      = "会话",
        [T_CONNECT]           = "连接",
        [T_DISPLAY]           = "显示",
        [T_SYSTEM]            = "系统",
        [T_IME]               = "输入法",
        [T_MANAGE]            = "管理",
        [T_ADD_TARGET]        = "添加目标",
        [T_EDIT_TARGET]       = "编辑目标",
        [T_NAME]              = "名称",
        [T_HOST]              = "主机",
        [T_PORT]              = "端口",
        [T_USER]              = "用户",
        [T_PASSWORD]          = "密码",
        [T_COMMAND]           = "命令",
        [T_AUTH]              = "认证",
        [T_AUTH_AUTO]         = "自动",
        [T_AUTH_PASSWORD]     = "密码",
        [T_AUTH_CERT]         = "证书",
        [T_SAVE]              = "保存",
        [T_CANCEL]            = "取消",
        [T_DELETE]            = "删除",
        [T_CONFIRM_DELETE]    = "确认删除?",
        [T_SAVE_CONNECT]      = "保存并连接",
        [T_SCAN]              = "扫描",
        [T_SCANNING]          = "扫描中…",
        [T_NEARBY_NETS]       = "附近网络",
        [T_SAVED_NETS]        = "已存网络",
        [T_NO_NETS]           = "未发现网络",
        [T_NO_SAVED_NETS]     = "无已存网络",
        [T_ADD_NET]           = "添加网络",
        [T_FORGET]            = "忘记",
        [T_CONNECTED]         = "已连接",
        [T_DISCONNECTED]      = "未连接",
        [T_CONNECTING]        = "连接中",
        [T_EDIT]              = "编辑",
        [T_BT]                = "蓝牙",
        [T_BT_SWITCH]         = "蓝牙开关",
        [T_BT_OFF]            = "蓝牙已关闭",
        [T_ADVERTISING]       = "正在广播",
        [T_ADV_STOPPED]       = "广播已停止",
        [T_START_ADV]         = "开始广播",
        [T_STOP_ADV]          = "停止广播",
        [T_KEY]               = "密钥",
        [T_DEVICE_KEY]        = "设备密钥",
        [T_KEY_READY]         = "已就绪",
        [T_KEY_GENERATING]    = "生成中…",
        [T_NO_KEY]            = "无密钥",
        [T_REGEN]             = "重新生成",
        [T_CONFIRM_REGEN]     = "确认重新生成?",
        [T_PUBKEY_HINT_AT]    = "完整公钥可在",
        [T_PUBKEY_HINT_COPY]  = "复制或",
        [T_PUBKEY_HINT_RUN]   = "执行",
        [T_PRIVKEY_HINT]      = "自定义私钥通过蓝牙上传",
        [T_WIFI_NOT_CONNECTED]= "Wi-Fi 未连接，请先连接网络",
        [T_LANGUAGE]          = "语言",
        [T_LANG_ZH]           = "中文",
        [T_LANG_EN]           = "English",
    },
    [LANG_EN] = {
        [T_SSH_SESSIONS]      = "Sessions",
        [T_CONNECT]           = "Connect",
        [T_DISPLAY]           = "Display",
        [T_SYSTEM]            = "System",
        [T_IME]               = "Input",
        [T_MANAGE]            = "Manage",
        [T_ADD_TARGET]        = "Add Target",
        [T_EDIT_TARGET]       = "Edit Target",
        [T_NAME]              = "Name",
        [T_HOST]              = "Host",
        [T_PORT]              = "Port",
        [T_USER]              = "User",
        [T_PASSWORD]          = "Password",
        [T_COMMAND]           = "Command",
        [T_AUTH]              = "Auth",
        [T_AUTH_AUTO]         = "Auto",
        [T_AUTH_PASSWORD]     = "Pass",
        [T_AUTH_CERT]         = "Cert",
        [T_SAVE]              = "Save",
        [T_CANCEL]            = "Cancel",
        [T_DELETE]            = "Delete",
        [T_CONFIRM_DELETE]    = "Confirm?",
        [T_SAVE_CONNECT]      = "Save & Connect",
        [T_SCAN]              = "Scan",
        [T_SCANNING]          = "Scanning…",
        [T_NEARBY_NETS]       = "Nearby",
        [T_SAVED_NETS]        = "Saved",
        [T_NO_NETS]           = "No networks",
        [T_NO_SAVED_NETS]     = "No saved networks",
        [T_ADD_NET]           = "Add Network",
        [T_FORGET]            = "Forget",
        [T_CONNECTED]         = "Connected",
        [T_DISCONNECTED]      = "Offline",
        [T_CONNECTING]        = "Connecting",
        [T_EDIT]              = "Edit",
        [T_BT]                = "Bluetooth",
        [T_BT_SWITCH]         = "Bluetooth",
        [T_BT_OFF]            = "Bluetooth off",
        [T_ADVERTISING]       = "Advertising",
        [T_ADV_STOPPED]       = "Adv stopped",
        [T_START_ADV]         = "Start Adv",
        [T_STOP_ADV]          = "Stop Adv",
        [T_KEY]               = "Key",
        [T_DEVICE_KEY]        = "Device Key",
        [T_KEY_READY]         = "Ready",
        [T_KEY_GENERATING]    = "Generating…",
        [T_NO_KEY]            = "No key",
        [T_REGEN]             = "Regenerate",
        [T_CONFIRM_REGEN]     = "Confirm?",
        [T_PUBKEY_HINT_AT]    = "Public key at",
        [T_PUBKEY_HINT_COPY]  = "copy or",
        [T_PUBKEY_HINT_RUN]   = "run",
        [T_PRIVKEY_HINT]      = "Upload key via Bluetooth",
        [T_WIFI_NOT_CONNECTED]= "Wi-Fi not connected - connect first",
        [T_LANGUAGE]          = "Language",
        [T_LANG_ZH]           = "中文",
        [T_LANG_EN]           = "English",
    },
};

const char *T(str_id_t id)
{
    if (id < 0 || id >= T_COUNT) return "";
    const char *s = S[s_lang][id];
    if (!s) s = S[LANG_ZH][id];     // fall back to ZH if an EN entry is missing
    return s ? s : "";
}

void i18n_set_lang(lang_t lang)
{
    if (lang < 0 || lang >= LANG_COUNT) lang = LANG_ZH;
    s_lang = lang;
}

lang_t i18n_lang(void)
{
    return s_lang;
}
