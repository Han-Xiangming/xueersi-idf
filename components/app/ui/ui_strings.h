#pragma once

/* Centralized UI string table.
 *
 * All user-facing text lives here so wording can be tuned in one place and,
 * later, swapped for another language (the macros are the i18n seam). Keep the
 * values short to fit the 320 px screen at 16 px font.
 *
 * NOTE: Bluetooth-related strings (e.g. the "蓝牙" setting row, "关/开/已连接"
 * status, "蓝牙开/关", "扫描中...", the BT page hint) are intentionally NOT
 * here yet — left as literals in ui.c until the BT UI is revisited. */

/* --- Settings menu labels --- */
#define UI_STR_SET_VOL        "音量"
#define UI_STR_SET_GAIN       "总音量补偿"   /* was 总增益 (engineering term) */
#define UI_STR_SET_BACKLIGHT  "背光"
#define UI_STR_SET_STANDBY    "息屏"
#define UI_STR_SET_RESCAN     "刷新播放列表" /* was 重建播放列表 (sounded destructive) */
#define UI_STR_SET_RESET      "恢复出厂设置" /* was 重置NVS (internal jargon) */
#define UI_STR_SET_CLEAR_PROG "清除阅读进度"

/* --- Settings value / action feedback --- */
#define UI_STR_NEVER      "永不"
#define UI_STR_CACHE_YES  "有缓存"
#define UI_STR_CACHE_NO   "无缓存"
#define UI_STR_RESTORE    "按A还原"
#define UI_STR_RESCANNING "刷新中..."   /* was 重建中... */
#define UI_STR_RESETTING  "重置中..."
#define UI_STR_CLEAR_A    "按A清除"
#define UI_STR_CLEARED    "已清除进度"

/* --- Player panel controls --- */
#define UI_STR_PREV        "上一曲"
#define UI_STR_PLAY        "播放"
#define UI_STR_NEXT        "下一曲"
#define UI_STR_STOP        "停止"
#define UI_STR_LOOP        "循环"
#define UI_STR_REPEAT_ONE    "单曲"
#define UI_STR_REPEAT_LIST   "列表"
#define UI_STR_REPEAT_RANDOM "随机"
#define UI_STR_PAUSE       "暂停"
#define UI_STR_RESUME      "继续"

/* --- Player state / action feedback --- */
#define UI_STR_PLAYING       "正在播放"   /* was 播放中 (unify with 已/未 pattern) */
#define UI_STR_PAUSED        "已暂停"
#define UI_STR_NOT_PLAYING   "未播放"
#define UI_STR_STOPPED       "已停止"
#define UI_STR_LOOP_ONE      "已切换为单曲循环" /* was 已切换单曲循环 */
#define UI_STR_LOOP_LIST     "已切换为列表循环" /* was 已切换列表循环 */
#define UI_STR_LOOP_RANDOM   "已切换为随机循环" /* was 已切换随机播放 */

/* --- Source pickers / status --- */
#define UI_STR_SEL_MUSIC  "选择播放列表" /* was 选择来源 / 选择播放来源 */
#define UI_STR_SEL_EBOOK  "选择书籍"     /* was 选择阅读来源 */
#define UI_STR_LOADING    "加载中..."

/* --- Hints (spaced + expanded verbs for readability; no glyphs that may be
 *      missing from the CJK font subset) --- */
#define UI_STR_HINT_NAV        "上/下 选择  A 进入  B 返回"
#define UI_STR_HINT_NAV_LR     "上/下 选择  A 进入  左/右 设置  B 返回"
#define UI_STR_HINT_PANEL      "左/右 选择  A 确认  B 关闭"
/* Player seek-bar hints (左/右切歌, 上/下音量/选择, A播放/暂停/继续, Select循环). */
#define UI_STR_HINT_SEEK_PLAY  "左/右 切歌  上/下 音量  A 暂停  Select 循环"
#define UI_STR_HINT_SEEK_PAUSE "左/右 切歌  上/下 音量  A 继续  Select 循环"
#define UI_STR_HINT_SEEK_IDLE  "左/右 切歌  上/下 选择  A 播放  Select 循环"
#define UI_STR_HINT_EBOOK_OPEN "上/下 选择  A 打开  B 返回"
