#ifndef LXM_SETTINGS_H
#define LXM_SETTINGS_H
#include "source.h"
#include "render.h"
typedef struct {
    bool ready,busy,wifi_only;
    int theme;
    unsigned revision;
    char message[256];
} SettingsView;
enum { SETTINGS_WIFI=1, SETTINGS_THEME, SETTINGS_RETRY };
bool settings_start(const char *directory,SourceWake wake,void *context);
void settings_stop(void);
bool settings_command(int action,int value);
bool settings_view(SettingsView *view);
/* 系统网络位：1=Wi-Fi，2=非计费，4=已验证互联网；策略只在 C 判断。 */
bool settings_download_allowed(unsigned network);
/* 仅 GL 线程更新/读取调色板，不与后台设置线程共享可变颜色。 */
extern Color settings_accent,settings_tint;
void settings_palette(int theme);
#define ACCENT settings_accent
#define TINT settings_tint
#endif
