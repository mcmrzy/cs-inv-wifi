#include "panel_key_labels.h"
static panel_weather_snapshot_t s_weather;
static const char *weather_condition(unsigned code,bool zh)
{
    if(code==0)return zh?"晴":"Clear";
    if(code<=2)return zh?"少云":"Part cloudy";
    if(code==3)return zh?"阴":"Cloudy";
    if(code==45 || code==48)return zh?"雾":"Fog";
    if(code>=51 && code<=57)return zh?"毛毛雨":"Drizzle";
    if(code>=61 && code<=67)return zh?"雨":"Rain";
    if((code>=71 && code<=77) || code==85 || code==86)return zh?"雪":"Snow";
    if(code>=80 && code<=82)return zh?"阵雨":"Showers";
    if(code>=95 && code<=99)return zh?"雷雨":"Storm";
    return zh?"未知":"Unknown";
}
static void weather_ui_key(uint8_t key)
{
    s_ui.dirty=true;s_ui.message[0]=0;
    if(s_ui.editing){
        if(keyboard_key(key,32)){
            if(panel_weather_search(s_wifi_password)){wifi_edit_reset();s_ui.selected=0;}
            else snprintf(s_ui.message,sizeof(s_ui.message),"Check Wi-Fi or city name");
        }
        return;
    }
    if(key==DIRECT_KEY_BACK){
        if(s_ui.detail_page)s_ui.detail_page=0;
        else s_ui.page=UI_HOME;
        return;
    }
    if(key==DIRECT_KEY_RIGHT && s_weather.search_state!=PANEL_CITY_SEARCHING){
        if(s_ui.detail_page){wifi_edit_reset();s_ui.editing=true;}
        s_ui.detail_page=1;s_ui.selected=0;
        return;
    }
    if(!s_ui.detail_page){
        if(key==DIRECT_KEY_ENTER)panel_weather_refresh();
        return;
    }
    if(key==DIRECT_KEY_LEFT || (key==DIRECT_KEY_ENTER && s_ui.selected<s_weather.city_count)){
        if(panel_weather_set_city(key==DIRECT_KEY_LEFT?-1:s_ui.selected))s_ui.detail_page=0;
        else snprintf(s_ui.message,sizeof(s_ui.message),"Wait or retry");
    }else if(key==DIRECT_KEY_UP && s_ui.selected)s_ui.selected--;
    else if(key==DIRECT_KEY_DOWN && s_ui.selected+1<s_weather.city_count)s_ui.selected++;
}
static void weather_ui_view(direct_ui_view_t *v,bool zh)
{
    memset(v,0,sizeof(*v));v->selected=-1;
    v->key_labels[KEY_BACK]=KEY_RETURN;
    if(s_ui.editing){
        keyboard_labels(v->key_labels,KEY_SEARCH);
        snprintf(v->title,sizeof(v->title),"%s",zh?"搜索城市":"Find city");
        snprintf(v->rows[0],sizeof(v->rows[0]),"%s",zh?"输入拼音或英文":"Enter a city name");
        snprintf(v->rows[1],sizeof(v->rows[1]),"%.32s|",s_wifi_password);
        v->selected=(s_ui.draft<<PANEL_WIFI_MODE_SHIFT)|s_ui.selected;
        return;
    }
    if(s_ui.detail_page){
        if(s_weather.search_state!=PANEL_CITY_SEARCHING)v->key_labels[KEY_RIGHT]=KEY_SEARCH;
        v->key_labels[KEY_LEFT]=KEY_AUTO;
        if(s_weather.city_count){
            v->key_labels[KEY_ENTER]=KEY_SAVE;
            if(s_ui.selected)v->key_labels[KEY_UP]=KEY_UPWARD;
            if(s_ui.selected+1<s_weather.city_count)v->key_labels[KEY_DOWN]=KEY_DOWNWARD;
        }
        snprintf(v->title,sizeof(v->title),"%s",zh?"天气城市":"Weather city");
        if(s_weather.city_count){
            for(unsigned i=0;i<s_weather.city_count;i++)panel_weather_city(i,v->rows[i],sizeof(v->rows[i]));
            v->selected=s_ui.selected;
        }else{
            const char *cn[]={"右键搜索城市","正在搜索城市","", "未找到城市","搜索失败"};
            const char *en[]={"Right to search","Searching for city","","No cities found","Search failed"};
            snprintf(v->rows[0],sizeof(v->rows[0]),"%s",zh?cn[s_weather.search_state]:en[s_weather.search_state]);
        }
        return;
    }
    v->key_labels[KEY_ENTER]=KEY_REFRESH;
    if(s_weather.search_state!=PANEL_CITY_SEARCHING)v->key_labels[KEY_RIGHT]=KEY_CITY;
    snprintf(v->title,sizeof(v->title),"%s",zh?"近七天天气":"7-day weather");
    if(s_weather.location_valid){
        const char *status=s_weather.stale?(zh?"上次预报":"Last forecast"):
            s_weather.configured_city?(zh?"手动":"Set"):"IP";
        const char *name=s_weather.city;
        if(zh && (!strcmp(name,"Changsha") || !strcmp(name,"changsha")))name="长沙";
        char city[25];unsigned used=0;
        while(name[used]){
            unsigned char c=(unsigned char)name[used];
            unsigned bytes=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4;
            if(used+bytes>=sizeof(city))break;
            memcpy(city+used,name+used,bytes);used+=bytes;
        }
        city[used]=0;snprintf(v->rows[0],sizeof(v->rows[0]),"%s: %s",city,status);
    }
    if(s_weather.valid){
        for(unsigned i=0;i<s_weather.days && i<PANEL_WEATHER_DAYS;i++){
            const panel_weather_day_t *day=&s_weather.daily[i];
            int low=day->low_10,high=day->high_10;
            snprintf(v->rows[i+1],sizeof(v->rows[i+1]),"%.5s  %s: %d / %d℃",day->date+5,
                weather_condition(day->code,zh),(low+(low<0?-5:5))/10,(high+(high<0?-5:5))/10);
        }
    }else{
        static const char *const cn[]={"等待联网","正在定位城市","正在获取天气","","Wi-Fi 未连接","城市定位失败","天气请求超时","天气获取失败"};
        static const char *const en[]={"Waiting for network","Locating city","Loading forecast","","Wi-Fi disconnected","City lookup failed","Weather timed out","Weather unavailable"};
        snprintf(v->rows[2],sizeof(v->rows[2]),"%s",zh?cn[s_weather.state]:en[s_weather.state]);
    }
}
