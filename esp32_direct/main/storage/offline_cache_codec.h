/* Lossless, batch-local JSON string delta encoding. Old plain batches remain readable.
 * Each delta stores a middle string p, common-prefix k and common-suffix e.
 * Encoding backwards keeps the preceding original payload available. */
#ifndef OFFLINE_CACHE_CODEC_H
#define OFFLINE_CACHE_CODEC_H
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
#define CACHE_PAYLOAD_LIMIT 1024
static bool cache_batch_encode(cJSON *arr)
{
    for (int i=cJSON_GetArraySize(arr)-1;i>0;--i) {
        cJSON *row=cJSON_GetArrayItem(arr,i);
        cJSON *p=cJSON_GetObjectItemCaseSensitive(row,"p");
        cJSON *prev=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr,i-1),"p");
        if (!cJSON_IsString(p)||!cJSON_IsString(prev)) return false;
        size_t n=strlen(p->valuestring),m=strlen(prev->valuestring),k=0,e=0;
        while(k<n && k<m && p->valuestring[k]==prev->valuestring[k]) ++k;
        while(e<n-k && e<m-k && p->valuestring[n-1-e]==prev->valuestring[m-1-e]) ++e;
        /* Retain plain strings when metadata would outweigh saved bytes. */
        if(k+e<=24 || n>=CACHE_PAYLOAD_LIMIT) continue;
        char *middle=cJSON_malloc(n-k-e+1);
        if(!middle) return false;
        memcpy(middle,p->valuestring+k,n-k-e);middle[n-k-e]='\0';
        bool ok=cJSON_AddNumberToObject(row,"k",(double)k) &&
                cJSON_AddNumberToObject(row,"e",(double)e) &&
                cJSON_SetValuestring(p,middle);
        cJSON_free(middle);
        if(!ok) return false;
    }
    return true;
}
static bool cache_batch_decode(cJSON *arr)
{
    for(int i=0;i<cJSON_GetArraySize(arr);++i) {
        cJSON *row=cJSON_GetArrayItem(arr,i);
        cJSON *p=cJSON_GetObjectItemCaseSensitive(row,"p");
        cJSON *t=cJSON_GetObjectItemCaseSensitive(row,"t");
        cJSON *k=cJSON_GetObjectItemCaseSensitive(row,"k");
        cJSON *e=cJSON_GetObjectItemCaseSensitive(row,"e");
        if(!cJSON_IsString(p)||!cJSON_IsString(t)||strlen(p->valuestring)>=CACHE_PAYLOAD_LIMIT) return false;
        if(!k && !e) continue;
        if(!i || !cJSON_IsNumber(k)||!cJSON_IsNumber(e) ||
           k->valuedouble<0 || e->valuedouble<0 ||
           k->valuedouble>=CACHE_PAYLOAD_LIMIT || e->valuedouble>=CACHE_PAYLOAD_LIMIT ||
           k->valuedouble!=(double)k->valueint || e->valuedouble!=(double)e->valueint) return false;
        cJSON *prev=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr,i-1),"p");
        size_t prefix=(size_t)k->valueint,suffix=(size_t)e->valueint;
        size_t n=strlen(p->valuestring),m=strlen(prev->valuestring);
        if(prefix+suffix>m || prefix+n+suffix>=CACHE_PAYLOAD_LIMIT) return false;
        char *plain=cJSON_malloc(prefix+n+suffix+1);
        if(!plain) return false;
        memcpy(plain,prev->valuestring,prefix);
        memcpy(plain+prefix,p->valuestring,n);
        memcpy(plain+prefix+n,prev->valuestring+m-suffix,suffix);
        plain[prefix+n+suffix]='\0';
        bool ok=cJSON_SetValuestring(p,plain)!=NULL;
        cJSON_free(plain);
        if(!ok) return false;
        cJSON_DeleteItemFromObjectCaseSensitive(row,"k");
        cJSON_DeleteItemFromObjectCaseSensitive(row,"e");
    }
    return true;
}
#endif
