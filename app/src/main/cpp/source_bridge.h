#ifndef LXM_SOURCE_BRIDGE_H
#define LXM_SOURCE_BRIDGE_H
#include <android/native_activity.h>
#include <stdbool.h>
#include <stdint.h>
bool source_bridge_paste(ANativeActivity *activity,bool search);
void source_bridge_cancel(ANativeActivity *activity);
/* SourceFetch 回调；context 是 ANativeActivity。仅工作线程调用。 */
char *source_bridge_fetch(void *context,const char *url,char error[256]);
/* RuntimeHttp 回调；UTF-8 JSON 请求/响应，仅工作线程调用。 */
char *source_bridge_http(void *context,const char *request,char error[256]);
char *source_bridge_search_http(void *context,const char *request,uint64_t token,char error[256]);
#endif
