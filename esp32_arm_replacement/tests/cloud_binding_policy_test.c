#define CLOUD_BINDING_POLICY_ONLY
#include "../main/onboarding/cloud_binding.c"
#include <assert.h>
#include <stdio.h>
int main(void){
 binding_policy_t p={0};
 assert(!policy_due(&p,false,0)); /* No SUBACK means no query. */
 assert(policy_due(&p,true,100));policy_sent(&p,true,100);
 assert(policy_nonce_live(&p,10099));assert(!policy_nonce_live(&p,10100));
 assert(!policy_due(&p,true,10100));assert(p.failures==1);
 assert(!policy_due(&p,true,40099));assert(policy_due(&p,true,40100));
 policy_sent(&p,false,40100);assert(p.failures==2&&!p.pending);
 assert(!policy_due(&p,true,100099));assert(policy_due(&p,true,100100));
 policy_sent(&p,true,100100);assert(!policy_due(&p,true,110100));assert(p.failures==3);
 assert(!policy_due(&p,true,410099));assert(policy_due(&p,true,410100));
 policy_sent(&p,true,410100);policy_answered(&p,410110);
 assert(!p.pending&&!p.failures);assert(!policy_due(&p,true,710109));assert(policy_due(&p,true,710110));
 policy_sent(&p,true,710110);assert(!policy_due(&p,false,710111));assert(!policy_nonce_live(&p,710111));
 assert(policy_due(&p,true,710112)); /* Reconnect gets a new nonce/query. */
 policy_sent(&p,true,UINT32_MAX-100);assert(policy_nonce_live(&p,9898));assert(!policy_nonce_live(&p,9899));
 assert(revision_allowed(false,0,CLOUD_BINDING_UNKNOWN,0,CLOUD_BINDING_UNBOUND));
 assert(!revision_allowed(true,8,CLOUD_BINDING_BOUND,7,CLOUD_BINDING_BOUND));
 assert(!revision_allowed(true,8,CLOUD_BINDING_BOUND,8,CLOUD_BINDING_UNBOUND));
 assert(revision_allowed(true,8,CLOUD_BINDING_BOUND,8,CLOUD_BINDING_BOUND));
 assert(revision_allowed(true,8,CLOUD_BINDING_BOUND,9,CLOUD_BINDING_UNBOUND));
 assert(!public_key_valid(""));assert(!public_key_valid("0000000000000000000000000000000000000000000000000000000000000000"));
 assert(!public_key_valid("z000000000000000000000000000000000000000000000000000000000000000"));
 assert(public_key_valid("a000000000000000000000000000000000000000000000000000000000000000"));
 puts("PASS actual binding policy: SUBACK gate, 10s nonce expiry, 30/60/300 retry, reconnect, wrap, revision conflicts, key validation");
 return 0;
}
