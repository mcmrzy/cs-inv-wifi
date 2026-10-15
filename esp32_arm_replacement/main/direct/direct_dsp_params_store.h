#ifndef DIRECT_DSP_PARAMS_STORE_H
#define DIRECT_DSP_PARAMS_STORE_H
#include "direct_dsp_params.h"
bool direct_dsp_params_load(const char *sn,direct_dsp_params_t *params);
bool direct_dsp_params_save(const char *sn,const direct_dsp_params_t *params);
/* Seed source defaults only for a missing record. Existing invalid records,
 * a different SN and storage failures remain errors without replacement. */
bool direct_dsp_params_bootstrap(const char *sn,direct_dsp_params_t *params,bool *defaults_used);
#endif
