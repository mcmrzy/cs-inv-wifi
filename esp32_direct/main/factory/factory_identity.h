#pragma once
#include "factory_record.h"
factory_result_t factory_identity_init(void);
factory_result_t factory_identity_read(factory_record_t *);
factory_result_t factory_identity_provision(const factory_record_t *);
factory_result_t factory_identity_lock(void);
bool factory_identity_ready(void);
/* Boot revision is immutable. Stored revisions can change before a restart. */
uint32_t factory_identity_runtime_generation(void);
uint32_t factory_identity_revision(void);
bool factory_identity_admin_sn(char *out,size_t capacity);
factory_result_t factory_identity_set_admin_sn(const char *sn,bool allow_unprovisioned);

/* Pairing requires a persisted identity SN, never an unpersisted development
 * fallback. Caller supplies the runtime SN; mismatch keeps the QR unavailable. */
bool factory_identity_pairing(const char *sn,char pin[7]);
factory_result_t factory_identity_set_pairing(const char *sn,const char *pin,bool allow_unprovisioned);
