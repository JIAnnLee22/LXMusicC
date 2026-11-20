#ifndef LXM_LIBRARY_UI_H
#define LXM_LIBRARY_UI_H
#include "ui.h"
/* 100..199 专属库动作，低于播放器动作区间。 */
enum { LIBUI_FILTER=100, LIBUI_PAGE, LIBUI_OPEN, LIBUI_BACK, LIBUI_INPUT, LIBUI_PASTE,
    LIBUI_CREATE, LIBUI_RENAME, LIBUI_SAVE, LIBUI_CANCEL, LIBUI_KEY,
    LIBUI_DELETE, LIBUI_REMOVE, LIBUI_PLAY, LIBUI_FAVORITE, LIBUI_PICK };
float ui_library_draw(Ui *u,Renderer *r,float x,float y,float w);
float ui_library_player(Ui *u,Renderer *r,float x,float y,float w);
void ui_library_action(Ui *u,int action,int value,double now);
bool ui_library_key(Ui *u,int ch,double now);
bool ui_library_back(Ui *u);
void ui_library_sync(Ui *u);
void ui_library_refresh(Ui *u);
#endif
