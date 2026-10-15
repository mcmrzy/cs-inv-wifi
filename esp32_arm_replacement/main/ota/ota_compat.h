#pragma once
#include <stdbool.h>
bool direct_ota_compatible(bool production,const char *image_model,const char *image_hw,const char *actual_model,const char *actual_hw);
