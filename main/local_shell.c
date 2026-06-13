// Local esp_console shell on the device's own terminal.
//
// Input: hid_keyboard sink (terminal-encoded bytes, same stream SSH would
// get) -> stream buffer -> shell task with its own tiny line editor (echo,
// backspace, 8-entry history, Ctrl+C). No linenoise: it wants a blocking
// stdin and VMIN/VTIME games we don't have.
//
// Output: command handlers print to stdout. While esp_console_run() executes
// we point the task's stdout/stderr at a funopen() FILE whose write fn feeds
// term_feed() (translating \n -> \r\n). funopen() is the lightest way to get
// stock esp_console help/argument errors onto the screen; a VFS device would
// also work but is ~3x the code for the same effect. newlib keeps stdin/out/
// err per task reent, so swapping them here never disturbs other tasks.

#include "local_shell.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_vfs_fat.h"
#include "bsp/m5stack_tab5.h"

#include "hid_keyboard.h"
#include "ssh_client.h"
#include "ssh_keys.h"
#include "power_mgmt.h"
#include "power_mon.h"
#include "rtc_rx8130.h"
#include "status_bar.h"
#include "wifi.h"
#include "ble_prov.h"
#include "ime_filter.h"
#include "sfx.h"
#include "voice_input.h"

static const char *TAG = "local_shell";

#define SHELL_LINE_MAX 256
#define SHELL_HISTORY  8
#define SHELL_PROMPT   "\x1b[36mtab5>\x1b[0m "

static term_t *s_term;
static settings_t *s_cfg;
static StreamBufferHandle_t s_input;       // keyboard bytes while active
static volatile bool s_active;

// Pending y/N confirmation: some commands arm one and the line loop reads
// the answer on the next line.
typedef enum { CONFIRM_NONE, CONFIRM_POWEROFF, CONFIRM_SSHKEY_REGEN,
               CONFIRM_HOSTKEY_CLEAR } confirm_t;
static confirm_t s_confirm;
static int s_confirm_arg;                  // target index for hostkey clear

static char s_history[SHELL_HISTORY][SHELL_LINE_MAX];
static int s_hist_count;                   // entries stored (<= SHELL_HISTORY)

// Shell output follows the user: the active session's terminal (sessions
// can be switched while the shell is up), or the boot term before SSH.
static term_t *cur_term(void)
{
    term_t *t = ssh_active_term();
    return t ? t : s_term;
}

static void out(const char *s)
{
    term_feed(cur_term(), (const uint8_t *)s, strlen(s));
}

// ---------------------------------------------------------------- stdout

// funopen write fn: \n -> \r\n into the terminal.
static int shell_fwrite(void *cookie, const char *buf, int len)
{
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', end - p);
        if (!nl) {
            term_feed(cur_term(), (const uint8_t *)p, end - p);
            break;
        }
        if (nl > p) term_feed(cur_term(), (const uint8_t *)p, nl - p);
        term_feed(cur_term(), (const uint8_t *)"\r\n", 2);
        p = nl + 1;
    }
    return len;
}

// ---------------------------------------------------------------- commands

static int cmd_free(int argc, char **argv)
{
    printf("internal: %7u free, %7u min free\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    printf("psram:    %7u free, %7u min free\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static int task_cmp(const void *a, const void *b)
{
    return (int)((const TaskStatus_t *)a)->xTaskNumber
         - (int)((const TaskStatus_t *)b)->xTaskNumber;
}

static int print_tasks(void)
{
    UBaseType_t n = uxTaskGetNumberOfTasks();
    TaskStatus_t *ts = malloc(n * sizeof(TaskStatus_t));
    if (!ts) { printf("oom\n"); return 1; }
    n = uxTaskGetSystemState(ts, n, NULL);
    qsort(ts, n, sizeof(TaskStatus_t), task_cmp);

    static const char state_ch[] = { 'X', 'R', 'B', 'S', 'D', '?' };  // run/ready/blk/susp/del
    printf("%-16s st pri  stack-hwm\n", "name");
    for (UBaseType_t i = 0; i < n; i++) {
        eTaskState st = ts[i].eCurrentState;
        printf("%-16s  %c %3u  %9u\n", ts[i].pcTaskName,
               state_ch[st <= eDeleted ? st : 5],
               (unsigned)ts[i].uxCurrentPriority,
               (unsigned)ts[i].usStackHighWaterMark);
    }
    free(ts);
    return 0;
}
#endif

static int cmd_ps(int argc, char **argv)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    return print_tasks();
#else
    printf("CONFIG_FREERTOS_USE_TRACE_FACILITY is off\n");
    return 1;
#endif
}

static int cmd_top(int argc, char **argv)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    printf("(per-task runtime unavailable: CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS off)\n");
    return print_tasks();
#else
    printf("CONFIG_FREERTOS_USE_TRACE_FACILITY is off\n");
    return 1;
#endif
}

static int cmd_uptime(int argc, char **argv)
{
    uint64_t s = esp_timer_get_time() / 1000000ULL;
    printf("up %llud %02llu:%02llu:%02llu\n",
           s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
    return 0;
}

static int cmd_version(int argc, char **argv)
{
    const esp_app_desc_t *d = esp_app_get_description();
    printf("%s %s (%s %s)\nidf %s\n",
           d->project_name, d->version, d->date, d->time, d->idf_ver);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("rebooting...\n");
    esp_restart();
    return 0;
}

static int cmd_date(int argc, char **argv)
{
    if (argc == 1) {
        time_t now = time(NULL);
        char buf[40];
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
        printf("%s\n", buf);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        struct tm tm = {0};
        if (sscanf(argv[2], "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3 ||
            sscanf(argv[3], "%d:%d:%d", &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3) {
            printf("usage: date set YYYY-MM-DD HH:MM:SS\n");
            return 1;
        }
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        tm.tm_isdst = -1;
        struct timeval tv = { .tv_sec = mktime(&tm) };
        settimeofday(&tv, NULL);
        if (!rtc_rx8130_write(&tm)) printf("(RTC write failed)\n");
        printf("time set\n");
        return 0;
    }
    printf("usage: date [set YYYY-MM-DD HH:MM:SS]\n");
    return 1;
}

static int cmd_temp(int argc, char **argv)
{
    float c;
    if (!status_bar_get_temp(&c)) { printf("temp sensor unavailable\n"); return 1; }
    printf("%.1f C\n", c);
    return 0;
}

static int cmd_batt(int argc, char **argv)
{
    float v, ma, mw;
    int pct;
    bool chg;
    if (!power_mon_get_ext(&v, &ma, &mw, &chg) || !power_mon_get(NULL, &pct, NULL)) {
        printf("no INA226\n");
        return 1;
    }
    printf("%.3fV  %.1fmA  %.1fmW  %d%%  %s\n",
           v, ma, mw, pct, chg ? "charging" : "discharging");
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    if (argc == 1) {
        power_mgmt_sleep_now();
        return 0;
    }
    int n = atoi(argv[1]);
    if (n < 0 || n > 65535) { printf("usage: sleep [timeout-seconds]\n"); return 1; }
    power_mgmt_set_timeout((uint16_t)n);
    s_cfg->sleep_timeout_s = (uint16_t)n;
    settings_save(s_cfg);
    printf("sleep timeout %s\n", n ? "set" : "disabled");
    return 0;
}

static int cmd_lock(int argc, char **argv)
{
    power_mgmt_lock_now();
    return 0;
}

// poweroff needs a confirm line: arm a flag, the line loop reads the answer.
static int cmd_poweroff(int argc, char **argv)
{
    s_confirm = CONFIRM_POWEROFF;
    printf("really power off? [y/N] ");
    return 0;
}

static int cmd_bl(int argc, char **argv)
{
    if (argc != 2) { printf("usage: bl <0-100>\n"); return 1; }
    int pct = atoi(argv[1]);
    if (pct < 0 || pct > 100) { printf("usage: bl <0-100>\n"); return 1; }
    bsp_display_brightness_set(pct);
    return 0;
}

// ----- SD card: mount around each op, web_config.c sdput style ------------

static int cmd_ls(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/sdcard/tab5";
    if (bsp_sdcard_mount() != ESP_OK) { printf("sd mount failed\n"); return 1; }
    DIR *d = opendir(path);
    if (!d) {
        printf("opendir %s failed\n", path);
        bsp_sdcard_unmount();
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[300];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (stat(full, &st) == 0 && !S_ISDIR(st.st_mode)) {
            printf("%9ld  %s\n", (long)st.st_size, e->d_name);
        } else {
            printf("%9s  %s/\n", "-", e->d_name);
        }
    }
    closedir(d);
    bsp_sdcard_unmount();
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc != 2) { printf("usage: cat <file>\n"); return 1; }
    if (bsp_sdcard_mount() != ESP_OK) { printf("sd mount failed\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("open %s failed\n", argv[1]);
        bsp_sdcard_unmount();
        return 1;
    }
    char buf[256];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, r, stdout);
    }
    fclose(f);
    bsp_sdcard_unmount();
    printf("\n");
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc != 2) { printf("usage: rm <file>\n"); return 1; }
    if (bsp_sdcard_mount() != ESP_OK) { printf("sd mount failed\n"); return 1; }
    int rc = unlink(argv[1]);
    bsp_sdcard_unmount();
    if (rc != 0) { printf("rm %s failed\n", argv[1]); return 1; }
    return 0;
}

static int cmd_df(int argc, char **argv)
{
    if (bsp_sdcard_mount() != ESP_OK) { printf("sd mount failed\n"); return 1; }
    uint64_t total = 0, free_b = 0;
    esp_err_t err = esp_vfs_fat_info("/sdcard", &total, &free_b);
    bsp_sdcard_unmount();
    if (err != ESP_OK) { printf("fat info failed\n"); return 1; }
    printf("/sdcard: %llu MB total, %llu MB free\n",
           total / (1024 * 1024), free_b / (1024 * 1024));
    return 0;
}

// ----- network -------------------------------------------------------------

static int cmd_ip(int argc, char **argv)
{
    char ip[20];
    wifi_ap_record_t ap;
    if (!wifi_get_ip(ip, sizeof(ip))) { printf("no wifi\n"); return 1; }
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        printf("%s  %s %ddBm ch%d\n", ip, (const char *)ap.ssid, ap.rssi, ap.primary);
    } else {
        printf("%s\n", ip);
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "scan") != 0) {
        printf("usage: wifi scan\n");
        return 1;
    }
    enum { MAX_APS = 20 };
    wifi_ap_record_t *recs = malloc(MAX_APS * sizeof(*recs));
    if (!recs) { printf("oom\n"); return 1; }
    printf("scanning...\n");
    int n = wifi_scan(recs, MAX_APS);
    if (n < 0) {
        free(recs);
        printf("scan failed\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        printf("%-32s %4ddBm ch%-2d %s\n", (const char *)recs[i].ssid,
               recs[i].rssi, recs[i].primary,
               recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "");
    }
    free(recs);
    return 0;
}

static int cmd_ble(int argc, char **argv)
{
    if (argc == 1) {
        printf("ble advertising: %s\n", ble_prov_is_advertising() ? "on" : "off");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        ble_prov_start();
        printf("advertising %s\n", ble_prov_is_advertising() ? "on" : "failed (stack down?)");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        ble_prov_stop();
        printf("advertising off\n");
        return 0;
    }
    printf("usage: ble [on|off]\n");
    return 1;
}

static const char *state_str(ssh_state_t st)
{
    return st == SSH_STATE_CONNECTED  ? "connected"
         : st == SSH_STATE_CONNECTING ? "connecting" : "idle";
}

static int cmd_target(int argc, char **argv)
{
    if (argc == 1) {
        printf("sessions (Ctrl+Alt+1..9 opens/switches, Ctrl+Alt+W closes):\n");
        bool none = true;
        for (int id = 0; id < MAX_SSH_SESSIONS; id++) {
            char name[48];
            int tgt;
            ssh_state_t st = ssh_state(id, name, sizeof(name), &tgt);
            if (tgt < 0 && st == SSH_STATE_IDLE) continue;
            printf("%c s%d: %-10s target %d %s\n",
                   id == ssh_active() ? '*' : ' ', id, state_str(st), tgt, name);
            none = false;
        }
        if (none) printf("  (none open)\n");
        printf("targets:\n");
        if (s_cfg->n_targets == 0) { printf("  (none configured)\n"); return 0; }
        for (int i = 0; i < s_cfg->n_targets; i++) {
            const ssh_target_t *t = &s_cfg->targets[i];
            printf("%c %d: %s  %s@%s:%d\n",
                   i == s_cfg->last_target ? '*' : ' ', i,
                   t->name[0] ? t->name : "-", t->user, t->host, t->port);
        }
        return 0;
    }
    int n = atoi(argv[1]);
    if (n < 0 || n >= s_cfg->n_targets) { printf("no such target\n"); return 1; }
    int id = ssh_open(n);
    if (id < 0) { printf("open failed (session limit / low RAM?)\n"); return 1; }
    printf("session %d on target %d — Ctrl+Alt+T to return to SSH\n", id, n);
    return 0;
}

static int cmd_close(int argc, char **argv)
{
    int id = (argc > 1) ? atoi(argv[1]) : ssh_active();
    if (id < 0) { printf("no session\n"); return 1; }
    int left = ssh_close(id);
    printf("closed session %d, %d left\n", id, left);
    return 0;
}

static int cmd_sshkey(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "regen") == 0) {
        if (ssh_keys_status() == SSH_KEYS_GENERATING) {
            printf("still generating, try later\n");
            return 1;
        }
        s_confirm = CONFIRM_SSHKEY_REGEN;
        printf("wipe the device key and generate a new one? [y/N] ");
        return 0;
    }
    if (argc != 1) { printf("usage: sshkey [regen]\n"); return 1; }
    char line[640];
    if (!ssh_keys_public_line(line, sizeof(line))) {
        printf(ssh_keys_status() == SSH_KEYS_GENERATING
               ? "generating...\n" : "no key (generation failed?)\n");
        return 1;
    }
    printf("%s\n", line);
    return 0;
}

static void hostkey_print(const char *target, const uint8_t *fp)
{
    char fp64[48];
    ssh_fp_base64(fp, fp64, sizeof(fp64));
    printf("%-28s SHA256:%s\n", target, fp64);
}

static int cmd_hostkey(int argc, char **argv)
{
    if (argc == 1) {
        ssh_hostkey_list(hostkey_print);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "clear") == 0) {
        int n = atoi(argv[2]);
        if (n < 0 || n >= s_cfg->n_targets) { printf("no such target\n"); return 1; }
        const ssh_target_t *t = &s_cfg->targets[n];
        s_confirm = CONFIRM_HOSTKEY_CLEAR;
        s_confirm_arg = n;
        printf("unpin host key for %s:%d? [y/N] ", t->host, t->port);
        return 0;
    }
    printf("usage: hostkey [clear <target-n>]\n");
    return 1;
}

static int cmd_shot(int argc, char **argv)
{
    char ip[20];
    if (!wifi_get_ip(ip, sizeof(ip))) { printf("no wifi\n"); return 1; }
    printf("http://%s/shot\n", ip);
    return 0;
}

// ----- misc -----------------------------------------------------------------

static int cmd_ime(int argc, char **argv)
{
    if (argc != 2) { printf("usage: ime <pinyin>\n"); return 1; }
    static char cands[10][64];      // shell task only
    int n = ime_filter_debug_query(argv[1], cands, 10);
    if (n < 0) { printf("ime unavailable\n"); return 1; }
    for (int i = 0; i < n; i++) printf("%d: %s\n", i + 1, cands[i]);
    return 0;
}

static int cmd_sfx(int argc, char **argv)
{
    if (argc == 2) {
        if (strcmp(argv[1], "click") == 0) { sfx_play(SFX_CLICK); return 0; }
        if (strcmp(argv[1], "tick") == 0)  { sfx_play(SFX_TICK);  return 0; }
        if (strcmp(argv[1], "ding") == 0)  { sfx_play(SFX_DING);  return 0; }
    }
    printf("usage: sfx <click|tick|ding>\n");
    return 1;
}

static int cmd_voice(int argc, char **argv)
{
    if (argc == 1) {
        printf("voice url: %s\n", s_cfg->voice_url);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "test") == 0) {
        return voice_input_test(3000);          // 3s record + upload
    }
    if (argc == 2 && strncmp(argv[1], "http", 4) == 0) {
        strlcpy(s_cfg->voice_url, argv[1], sizeof(s_cfg->voice_url));
        settings_save(s_cfg);
        printf("voice url set\n");
        return 0;
    }
    printf("usage: voice [test|http://host:port/inference]\n");
    return 1;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "free",     .help = "heap usage (internal + PSRAM)",        .func = cmd_free },
        { .command = "ps",       .help = "task list: state, prio, stack hwm",    .func = cmd_ps },
        { .command = "top",      .help = "task list (no runtime stats)",         .func = cmd_top },
        { .command = "uptime",   .help = "time since boot",                      .func = cmd_uptime },
        { .command = "version",  .help = "firmware build info",                  .func = cmd_version },
        { .command = "reboot",   .help = "restart the device",                   .func = cmd_reboot },
        { .command = "date",     .help = "show time; date set YYYY-MM-DD HH:MM:SS", .func = cmd_date },
        { .command = "temp",     .help = "chip temperature",                     .func = cmd_temp },
        { .command = "batt",     .help = "battery V/mA/mW/%",                    .func = cmd_batt },
        { .command = "sleep",    .help = "sleep now; sleep <s> sets idle timeout", .func = cmd_sleep },
        { .command = "lock",     .help = "lock the screen (Ctrl+Alt+L unlocks)", .func = cmd_lock },
        { .command = "poweroff", .help = "hard power off (asks y/N)",            .func = cmd_poweroff },
        { .command = "bl",       .help = "backlight brightness 0-100",           .func = cmd_bl },
        { .command = "ls",       .help = "list SD files (default /sdcard/tab5)", .func = cmd_ls },
        { .command = "cat",      .help = "print an SD file",                     .func = cmd_cat },
        { .command = "rm",       .help = "delete an SD file",                    .func = cmd_rm },
        { .command = "df",       .help = "SD card capacity",                     .func = cmd_df },
        { .command = "ip",       .help = "IP, SSID and RSSI",                    .func = cmd_ip },
        { .command = "wifi",     .help = "wifi scan",                            .func = cmd_wifi },
        { .command = "target",   .help = "sessions + targets; target <n> opens/switches", .func = cmd_target },
        { .command = "close",    .help = "close SSH session: close [id]",        .func = cmd_close },
        { .command = "sshkey",   .help = "device public key; sshkey regen",      .func = cmd_sshkey },
        { .command = "hostkey",  .help = "pinned host keys; hostkey clear <n>",  .func = cmd_hostkey },
        { .command = "ble",      .help = "BLE provisioning adv: status; ble on|off", .func = cmd_ble },
        { .command = "ime",      .help = "pinyin lookup: ime nihao",             .func = cmd_ime },
        { .command = "sfx",      .help = "play a sound: sfx <click|tick|ding>",  .func = cmd_sfx },
        { .command = "voice",    .help = "whisper url: voice [test|<url>]",      .func = cmd_voice },
        { .command = "shot",     .help = "screenshot URL",                       .func = cmd_shot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_ERROR_CHECK(esp_console_register_help_command());
}

// ---------------------------------------------------------------- line loop

static void history_push(const char *line)
{
    if (!line[0]) return;
    if (s_hist_count > 0 && strcmp(s_history[0], line) == 0) return;   // dup
    memmove(&s_history[1], &s_history[0],
            (SHELL_HISTORY - 1) * SHELL_LINE_MAX);
    strlcpy(s_history[0], line, SHELL_LINE_MAX);
    if (s_hist_count < SHELL_HISTORY) s_hist_count++;
}

static void run_line(FILE *fp, char *line)
{
    if (s_confirm != CONFIRM_NONE) {
        confirm_t c = s_confirm;
        s_confirm = CONFIRM_NONE;
        if (line[0] != 'y' && line[0] != 'Y') {
            out("cancelled\r\n");
            return;
        }
        switch (c) {
        case CONFIRM_POWEROFF:
            out("powering off\r\n");
            power_mgmt_poweroff();
            break;
        case CONFIRM_SSHKEY_REGEN:
            ssh_keys_regen();
            out("regenerating (run `sshkey` to check)\r\n");
            break;
        case CONFIRM_HOSTKEY_CLEAR: {
            const ssh_target_t *t = &s_cfg->targets[s_confirm_arg];
            out(ssh_hostkey_clear(t->host, t->port)
                    ? "unpinned\r\n" : "no pin found\r\n");
            break;
        }
        default:
            break;
        }
        return;
    }
    if (!line[0]) return;
    history_push(line);

    // Route stdout/stderr (per-task in newlib) into the terminal for the
    // duration of the command, so esp_console's own messages land too.
    FILE *saved_out = stdout, *saved_err = stderr;
    stdout = fp;
    stderr = fp;

    int ret = 0;
    esp_err_t err = esp_console_run(line, &ret);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("%s: command not found (try 'help')\n", line);
    } else if (err == ESP_OK && ret != 0) {
        printf("(exit %d)\n", ret);
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        printf("error: %s\n", esp_err_to_name(err));
    }
    fflush(fp);

    stdout = saved_out;
    stderr = saved_err;
}

static void shell_task(void *arg)
{
    FILE *fp = funopen(NULL, NULL, shell_fwrite, NULL, NULL);
    assert(fp);
    setvbuf(fp, NULL, _IOFBF, 512);

    char line[SHELL_LINE_MAX];
    int len = 0;
    int esc = 0;            // 0 none, 1 got ESC, 2 got ESC [
    int hist_pos = -1;      // -1 = editing a fresh line

    while (true) {
        uint8_t c;
        if (xStreamBufferReceive(s_input, &c, 1, portMAX_DELAY) != 1) continue;
        if (!s_active) { len = 0; esc = 0; continue; }   // drain stale bytes

        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
        if (esc == 2) {
            esc = 0;
            if (c == 'A' && s_hist_count > 0) {              // up: older
                if (hist_pos < s_hist_count - 1) hist_pos++;
                while (len--) out("\b \b");
                strlcpy(line, s_history[hist_pos], sizeof(line));
                len = strlen(line);
                out(line);
            } else if (c == 'B') {                           // down: newer
                while (len--) out("\b \b");
                len = 0;
                if (hist_pos > 0) {
                    hist_pos--;
                    strlcpy(line, s_history[hist_pos], sizeof(line));
                    len = strlen(line);
                    out(line);
                } else {
                    hist_pos = -1;
                }
            }
            if (len < 0) len = 0;
            continue;
        }

        switch (c) {
        case 0x1b:
            esc = 1;
            break;
        case '\r':
        case '\n':
            out("\r\n");
            line[len] = 0;
            run_line(fp, line);
            len = 0;
            hist_pos = -1;
            if (s_active) out(SHELL_PROMPT);
            break;
        case 0x7f:
        case 0x08:
            if (len > 0) { len--; out("\b \b"); }
            break;
        case 0x03:                                           // Ctrl+C
            out("^C\r\n");
            len = 0;
            hist_pos = -1;
            s_confirm = CONFIRM_NONE;
            out(SHELL_PROMPT);
            break;
        default:
            if (c >= 0x20 && c < 0x7f && len < SHELL_LINE_MAX - 1) {
                line[len++] = c;
                term_feed(cur_term(), &c, 1);                // echo
            }
            break;
        }
    }
}

// ---------------------------------------------------------------- toggling

static void shell_sink(const uint8_t *data, size_t len)
{
    if (s_input) xStreamBufferSend(s_input, data, len, 0);
}

void local_shell_enter(void)
{
    if (s_active) return;
    s_active = true;
    hid_keyboard_set_sink(shell_sink);
    out("\r\n\x1b[36m[tab5] local shell — Ctrl+Alt+T to return; 'help' for commands\x1b[0m\r\n");
    out(SHELL_PROMPT);
}

static void shell_exit(void)
{
    s_active = false;
    s_confirm = CONFIRM_NONE;
    hid_keyboard_set_sink(NULL);     // back to SSH; remote redraws on output
}

// Public: an SSH session took over the keyboard (called from ssh_set_active).
void local_shell_leave(void)
{
    if (s_active) shell_exit();
}

bool local_shell_active(void) { return s_active; }

// Ctrl+Alt+T (runs on the HID task — term_feed and the sink swap are safe).
static void shell_toggle(void)
{
    if (s_active) shell_exit();
    else local_shell_enter();
}

void local_shell_init(term_t *t, settings_t *s)
{
    s_term = t;
    s_cfg = s;

    esp_console_config_t cfg = ESP_CONSOLE_CONFIG_DEFAULT();
    cfg.max_cmdline_length = SHELL_LINE_MAX;
    ESP_ERROR_CHECK(esp_console_init(&cfg));
    register_commands();

    s_input = xStreamBufferCreate(512, 1);
    xTaskCreate(shell_task, "local_shell", 10240, NULL, 4, NULL);
    hid_keyboard_set_shell_cb(shell_toggle);
    ESP_LOGI(TAG, "ready — Ctrl+Alt+T");
}
