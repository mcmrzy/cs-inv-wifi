#include "ota_compat.h"
#include <string.h>
bool direct_ota_compatible(bool production,const char *image_model,const char *image_hw,const char *actual_model,const char *actual_hw){
 if(!production)return true;
 return image_model&&image_hw&&actual_model&&actual_hw&&image_model[0]&&image_hw[0]&&actual_model[0]&&actual_hw[0]&&strcmp(actual_hw,"UNSET")&& !strcmp(image_model,actual_model)&&!strcmp(image_hw,actual_hw);
}
