#ifndef LXM_IME_UI_H
#define LXM_IME_UI_H
#include "ui.h"
#include <android/native_activity.h>
enum { IME_SEARCH=1, IME_LIBRARY, IME_CREATE, IME_RENAME, IME_SOURCE };
void ui_ime_open(Ui *u,int target);
bool ui_ime_sync(Ui *u,ANativeActivity *activity,double now);
void ui_ime_field(Ui *u,Renderer *r,Rect box,int target,const char *text,const char *hint);
void ui_ime_menu_draw(Ui *u,Renderer *r);
bool ui_ime_touch(Ui *u,int action,float x,float y,double now);
void ui_ime_submit(Ui *u,double now);
#endif
