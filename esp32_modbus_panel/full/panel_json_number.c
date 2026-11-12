/* cJSON's sole scanf use is the %lg round-trip of its own numeric output.
 * strtod is already required by its parser. Avoid linking a second, general
 * floating-point scanf engine just for this call. See the configure-time guard. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int panel_json_scanf(const char *text,const char *format,...)
{
    if(strcmp(format,"%lg"))return 0;
    char *end;
    double value=strtod(text,&end);
    if(end==text)return 0;
    va_list args;
    va_start(args,format);
    *va_arg(args,double *)=value;
    va_end(args);
    return 1;
}
