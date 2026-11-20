#ifndef LXM_PLAYER_UI_H
#define LXM_PLAYER_UI_H
#include "ui.h"
/* 独立动作区间，不与通用页面动作冲突。 */
enum { PLAYER_TOGGLE_QUEUE=200, PLAYER_QUEUE_PAGE, PLAYER_SELECT,
    PLAYER_MODE, PLAYER_RETRY_COVER };
float ui_player_draw(Ui *u,Renderer *r,float x,float y,float w);
bool ui_player_touch(Ui *u,int action,float x,float y,double now);
void ui_player_action(Ui *u,int action,int value);
void ui_player_sync(Ui *u);
bool ui_player_tick(Ui *u,float dt);
bool ui_player_animating(const Ui *u);
#endif
