#include "settings.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

Color settings_accent={.34f,.88f,.72f,1},settings_tint={.08f,.20f,.19f,1};
void settings_palette(int theme) {
    const Color colors[]={{.34f,.88f,.72f,1},{.48f,.74f,1,1},{.80f,.65f,1,1}};
    settings_accent=colors[theme>=0 && theme<3?theme:0];
    settings_tint=(Color){settings_accent.r*.20f,settings_accent.g*.23f,settings_accent.b*.26f,1};
}
static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    bool running,started,pending;
    int action,value;
    char path[1100];
    SettingsView view;
    SourceWake wake;
    void *context;
} prefs={.lock=PTHREAD_MUTEX_INITIALIZER,.cond=PTHREAD_COND_INITIALIZER};
/* 位0=已就绪，位1=仅Wi-Fi；一次原子发布，冷启动未加载时保守拒绝下载。 */
static atomic_uint policy=0;
bool settings_download_allowed(unsigned network) {
    unsigned p=atomic_load(&policy);
    return (p&1) && (!(p&2) || (network&7)==7);
}
static bool load(bool *wifi,int *theme,char error[256]) {
    FILE *f=fopen(prefs.path,"rb");
    if(!f) {
        if(errno==ENOENT) return true;
        snprintf(error,256,"设置读取失败，已使用安全默认值；点击重试"); return false;
    }
    unsigned char data[8];
    bool ok=fread(data,1,8,f)==8 && fgetc(f)==EOF && !ferror(f) &&
        !memcmp(data,"LXSP\1",5) && data[5]<=1 && data[6]<3 && data[7]==0;
    fclose(f);
    if(ok) { *wifi=data[5]; *theme=data[6]; }
    else snprintf(error,256,"设置文件损坏，已使用安全默认值；点击重试可重建");
    return ok;
}
static bool save(bool wifi,int theme,char error[256]) {
    char temp[1150]; snprintf(temp,sizeof(temp),"%s.tmp",prefs.path);
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC,0600);
    unsigned char data[8]={'L','X','S','P',1,wifi,(unsigned char)theme,0};
    bool ok=fd>=0;
    if(ok) { ok=write(fd,data,8)==8 && fsync(fd)==0; if(close(fd)!=0) ok=false; }
    if(ok && rename(temp,prefs.path)==0) return true;
    unlink(temp); snprintf(error,256,"设置保存失败，未更改原设置；请检查空间后重试"); return false;
}
static void publish(bool wifi,int theme,const char *message) {
    pthread_mutex_lock(&prefs.lock);
    prefs.view.ready=true; prefs.view.busy=false;
    prefs.view.wifi_only=wifi; prefs.view.theme=theme;
    snprintf(prefs.view.message,256,"%s",message); ++prefs.view.revision;
    atomic_store(&policy,1u|(wifi?2u:0u));
    if(prefs.wake) prefs.wake(prefs.context);
    pthread_mutex_unlock(&prefs.lock);
}
static void *worker(void *unused) {
    bool wifi=true; int theme=0; char error[256]="";
    load(&wifi,&theme,error);
    publish(wifi,theme,error[0]?error:"设置已加载；修改后自动保存");
    for(;;) {
        pthread_mutex_lock(&prefs.lock);
        while(prefs.running && !prefs.pending) pthread_cond_wait(&prefs.cond,&prefs.lock);
        if(!prefs.running && !prefs.pending) { pthread_mutex_unlock(&prefs.lock); break; }
        int action=prefs.action,value=prefs.value; prefs.pending=false;
        pthread_mutex_unlock(&prefs.lock);
        bool next_wifi=action==SETTINGS_WIFI?value!=0:wifi;
        int next_theme=action==SETTINGS_THEME?value:theme;
        error[0]=0;
        if(save(next_wifi,next_theme,error)) { wifi=next_wifi; theme=next_theme; }
        publish(wifi,theme,error[0]?error:"设置已保存");
    }
    return NULL;
}
bool settings_start(const char *directory,SourceWake wake,void *context) {
    atomic_store(&policy,0);
    pthread_mutex_lock(&prefs.lock);
    prefs.running=true; prefs.pending=false; prefs.wake=wake; prefs.context=context;
    prefs.view=(SettingsView){.busy=true,.wifi_only=true,.revision=prefs.view.revision+1};
    snprintf(prefs.view.message,256,"正在加载设置…");
    snprintf(prefs.path,sizeof(prefs.path),"%s/lx-settings-v1.bin",directory);
    pthread_mutex_unlock(&prefs.lock);
    if(pthread_create(&prefs.thread,NULL,worker,NULL)!=0) {
        prefs.running=false; publish(true,0,"设置线程启动失败，请退出应用后重试"); return false;
    }
    prefs.started=true; return true;
}
void settings_stop(void) {
    if(!prefs.started) return;
    pthread_mutex_lock(&prefs.lock); prefs.running=false;
    pthread_cond_signal(&prefs.cond); pthread_mutex_unlock(&prefs.lock);
    pthread_join(prefs.thread,NULL); prefs.started=false;
}
bool settings_command(int action,int value) {
    if(action<SETTINGS_WIFI || action>SETTINGS_RETRY ||
       (action==SETTINGS_THEME && (value<0 || value>=3))) return false;
    pthread_mutex_lock(&prefs.lock);
    bool ok=prefs.running && prefs.view.ready && !prefs.view.busy;
    if(ok) {
        prefs.action=action; prefs.value=value; prefs.pending=true; prefs.view.busy=true;
        snprintf(prefs.view.message,256,"正在保存设置…"); ++prefs.view.revision;
        pthread_cond_signal(&prefs.cond);
    }
    pthread_mutex_unlock(&prefs.lock); return ok;
}
bool settings_view(SettingsView *view) {
    pthread_mutex_lock(&prefs.lock);
    bool changed=view->revision!=prefs.view.revision;
    if(changed) *view=prefs.view;
    pthread_mutex_unlock(&prefs.lock); return changed;
}
