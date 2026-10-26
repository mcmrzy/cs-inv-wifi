#ifndef DIRECT_UI_LOCALE_H
#define DIRECT_UI_LOCALE_H
#include <stdbool.h>
#include <stddef.h>
/* Returns English unchanged when zh=false. Chinese output uses caller-owned
 * storage; untranslated identifiers/numbers/units are preserved verbatim. */
const char *direct_ui_translate(const char *english,bool zh,char *buffer,size_t capacity);
size_t direct_ui_locale_count(void);
const char *direct_ui_locale_key(size_t index);
const char *direct_ui_locale_zh(size_t index);
#endif
