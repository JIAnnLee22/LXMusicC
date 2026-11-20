#ifndef LXM_UI_H
#define LXM_UI_H
#include "render.h"
#include "source.h"
#include "search.h"
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
    SearchItem selected;
    char query[SEARCH_QUERY_BYTES];
    unsigned paste_revision;
    bool input_open, search_paste_requested, has_selected;
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
