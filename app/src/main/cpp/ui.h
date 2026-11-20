#ifndef LXM_UI_H
#define LXM_UI_H
#include "render.h"
#include "source.h"
#include "search.h"
#include "playback.h"
#include "cache.h"
#include "library.h"
#define UI_STATE_VERSION 1u
typedef struct {
    unsigned version;
    int tab, library_filter, search_chip, settings_row;
    float scroll[4];
} UiState;
typedef struct { Rect rect; int action, value; } UiHit;
typedef struct {
    UiState state;
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
    CacheView cache;
    bool cache_open, cache_clear_confirm;
    uint32_t cache_delete_id;
    char query[SEARCH_QUERY_BYTES];
    unsigned paste_revision;
    bool input_open, search_paste_requested;
} Ui;
void ui_init(Ui *ui, const UiState *saved);
void ui_layout(Ui *ui, Rect area);
void ui_draw(Ui *ui, Renderer *r, double now);
bool ui_tick(Ui *ui, float dt);
void ui_touch(Ui *ui, int action, float x, float y, double now);
bool ui_back(Ui *ui);
void ui_cancel(Ui *ui);
/* C 软键盘与硬件键盘共用文本编辑；8=删字，13=提交。 */
bool ui_search_key(Ui *ui,int character,double now);
void ui_search_sync(Ui *ui);
#endif
