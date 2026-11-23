#ifndef LXM_UI_H
#define LXM_UI_H
#include "render.h"
#include "source.h"
#include "search.h"
#include "playback.h"
#include "cache.h"
#include "library.h"
#include "settings.h"
#define UI_STATE_VERSION 1u
typedef struct {
    unsigned version;
    int tab, library_filter, search_chip, settings_row;
    float scroll[4];
} UiState;
typedef struct { Rect rect; int action, value; } UiHit;
/* 有界返回历史只保留路由与滚动，不复制歌曲或持有工作线程引用。 */
typedef struct {
    int tab, library_filter, library_id, library_page, source_page, queue_page;
    bool source_open, cache_open, queue_open;
    float scroll;
    char library_query[256];
} UiRoute;
typedef struct {
    UiState state;
    UiRoute history[16];
    int history_count, notifications, system_action;
    float transition, back_progress;
    Rect area, body;
    float max_scroll[4], velocity, down_x, down_y, last_y;
    double last_time, toast_until;
    bool dragging, pressed;
    int pointer;
    char toast[160];
    UiHit hits[128];
    int hit_count;
    SourceView sources;
    bool source_open, paste_requested;
    int source_page;
    uint32_t delete_id;
    SearchView search;
    PlaybackView playback;
    bool playback_requested, cover_retry_requested, cover_loading;
    Rect seek_rect, art_rect;
    bool seeking, queue_open;
    int queue_page;
    int64_t seek_position;
    uint64_t seek_generation, player_generation;
    double shown_position;
    float player_appear, cover_fade, queue_mix;
    LibraryView library;
    int library_id,library_page,library_input,library_paste_target;
    unsigned library_paste_revision,library_request;
    uint32_t library_confirm;
    int library_confirm_action;
    bool library_adding,library_paste_requested;
    SearchItem library_candidate;
    char library_query[256],library_name[256];
    SettingsView settings;
    CacheView cache;
    bool cache_open, cache_clear_confirm;
    uint32_t cache_delete_id;
    char query[SEARCH_QUERY_BYTES];
    unsigned paste_revision;
    bool input_open, search_paste_requested;
    char source_url[2048];
    int ime_target, ime_active, ime_session, ime_start, ime_end, ime_cs, ime_ce, ime_bottom, ime_reference_height;
    char ime_mirror[2048];
    Rect ime_rect;
    float ime_offset;
    bool ime_selection_dirty, ime_paste, ime_menu, ime_gesture, ime_selecting, ime_relayout, ime_show;
    int ime_anchor;
    double ime_down_time;
} Ui;
void ui_init(Ui *ui, const UiState *saved);
void ui_layout(Ui *ui, Rect area);
void ui_draw(Ui *ui, Renderer *r, double now);
bool ui_tick(Ui *ui, float dt);
void ui_touch(Ui *ui, int action, float x, float y, double now);
bool ui_back(Ui *ui);
void ui_cancel(Ui *ui);
UiRoute ui_route(const Ui *ui);
void ui_route_record(Ui *ui,UiRoute previous);
bool ui_route_pop(Ui *ui);
bool ui_can_back(const Ui *ui);
void ui_pressed_draw(Ui *ui,Renderer *r,bool navigation);
bool ui_current_song(const Ui *ui,const SearchItem *item);
/* 提交保留在 C：13=搜索/保存；系统 IME 负责普通文本编辑。 */
bool ui_search_key(Ui *ui,int character,double now);
void ui_search_sync(Ui *ui);
#endif
