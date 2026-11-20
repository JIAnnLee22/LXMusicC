#ifndef LXM_CACHE_BRIDGE_H
#define LXM_CACHE_BRIDGE_H
#include "cache.h"
bool cache_bridge_io(void *context,int op,const SearchItem *item,const char *uri,
                     uint64_t maximum,char result[CACHE_URI_BYTES],uint64_t *bytes,char error[256]);
#endif
