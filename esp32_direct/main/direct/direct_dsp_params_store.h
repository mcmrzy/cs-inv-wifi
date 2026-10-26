#ifndef DIRECT_DSP_PARAMS_STORE_H
#define DIRECT_DSP_PARAMS_STORE_H
#include "direct_dsp_params.h"
bool direct_dsp_params_load(const char *sn,direct_dsp_params_t *params);
bool direct_dsp_params_save(const char *sn,const direct_dsp_params_t *params);
/* Recover source defaults for missing/invalid records, including a different SN.
 * Valid settings are preserved; storage failures never enable unsaved defaults. */
bool direct_dsp_params_bootstrap(const char *sn,direct_dsp_params_t *params,bool *defaults_used);
#endif
