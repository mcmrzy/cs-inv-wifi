#include "panel_locale.h"
#include <stdint.h>
#include <string.h>
typedef struct {uint16_t zh,en;} translation_t;
#include "panel_locale_terms.inc"

void panel_locale_english(char *text,size_t capacity)
{
    if(!text || !capacity)return;
    char translated[256];size_t used=0;
    const char *source=text;
    while(*source && used+1<sizeof(translated)){
        const char *replacement=NULL;size_t consumed=1;
        if((unsigned char)*source>=0x80 || *source=='B' || *source=='P' || *source=='C' || *source=='M' || *source=='6'){
            size_t matched=0;
            for(size_t i=0;i<sizeof(terms)/sizeof(terms[0]);i++){
                const char *zh=zh_text+terms[i].zh;
                size_t n=strlen(zh);
                if(n>matched && !strncmp(source,zh,n)){
                    replacement=en_text+terms[i].en;consumed=n;matched=n;
                }
            }
        }
        if(replacement){
            size_t n=strlen(replacement);
            if(n>sizeof(translated)-1-used)n=sizeof(translated)-1-used;
            memcpy(translated+used,replacement,n);used+=n;
        }else translated[used++]=*source;
        source+=consumed;
    }
    translated[used]=0;
    if(used>=capacity)used=capacity-1;
    memcpy(text,translated,used);text[used]=0;
}
