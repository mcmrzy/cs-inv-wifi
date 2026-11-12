/* Internal cache route tags preserve the original MQTT envelope version.
 * These tags never appear on the MQTT wire. New c files protect old firmware
 * from replaying tagged V3 records with a V2 decoder. */
#ifndef OFFLINE_CACHE_ROUTE_H
#define OFFLINE_CACHE_ROUTE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static bool cache_route_encode(const char *topic,uint16_t version,char *route,size_t capacity)
{
    if (!topic || (version!=2 && version!=3)) return false;
    int n=snprintf(route,capacity,"%s%s",version==3?"~3/":"",topic);
    return n>=0 && (size_t)n<capacity;
}
static uint16_t cache_route_decode(const char **route)
{
    if (strncmp(*route,"~3/",3)==0) { *route+=3; return 3; }
    return 2;
}
#endif
