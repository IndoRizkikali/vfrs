/*
 * svc_sig_iel.h - Q.933 Information Element Layouts & Cause Codes
 * Virtual Frame Relay Switch - ITU-T Q.933 / X.36 Clause 10 & Q.850
 */

#ifndef SVC_SIG_IEL_H
#define SVC_SIG_IEL_H

#include "svc_sig_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Q.850 Cause Location Codes */
#define Q850_LOC_USER                  0x00
#define Q850_LOC_PRIVATE_LOCAL_NET    0x01
#define Q850_LOC_PUBLIC_LOCAL_NET     0x02
#define Q850_LOC_TRANSIT_NET          0x03
#define Q850_LOC_PUBLIC_REMOTE_NET    0x04
#define Q850_LOC_PRIVATE_REMOTE_NET   0x05
#define Q850_LOC_INTERNATIONAL_NET    0x07
#define Q850_LOC_BEYOND_INTERWORK     0x0A

/* Q.850 Cause Values */
#define Q850_CAUSE_UNALLOCATED_NUMBER           1
#define Q850_CAUSE_NO_ROUTE_TO_TRANSIT_NET     2
#define Q850_CAUSE_NO_ROUTE_TO_DESTINATION     3
#define Q850_CAUSE_CHANNEL_UNACCEPTABLE        6
#define Q850_CAUSE_PREEMPTION                  8
#define Q850_CAUSE_NORMAL_CLEARING            16
#define Q850_CAUSE_USER_BUSY                  17
#define Q850_CAUSE_NO_USER_RESPONDING         18
#define Q850_CAUSE_NO_ANSWER_FROM_USER        19
#define Q850_CAUSE_CALL_REJECTED              21
#define Q850_CAUSE_NUMBER_CHANGED             22
#define Q850_CAUSE_DESTINATION_OUT_OF_ORDER   27
#define Q850_CAUSE_INVALID_NUMBER_FORMAT      28
#define Q850_CAUSE_FACILITY_REJECTED          29
#define Q850_CAUSE_RESPONSE_TO_STATUS_ENQ     30
#define Q850_CAUSE_NORMAL_UNSPECIFIED         31
#define Q850_CAUSE_NO_CIRCUIT_AVAILABLE       34
#define Q850_CAUSE_NETWORK_OUT_OF_ORDER       38
#define Q850_CAUSE_PERM_CONN_OUT_OF_SERVICE   39
#define Q850_CAUSE_TEMPORARY_FAILURE          41
#define Q850_CAUSE_SWITCHING_EQUIP_CONGESTION 42
#define Q850_CAUSE_ACCESS_INFO_DISCARDED      43
#define Q850_CAUSE_CIRCUIT_NOT_AVAILABLE      44  /* X.76 §10.6.7.2: DLCI collision */
#define Q850_CAUSE_RESOURCE_UNAVAILABLE       47
#define Q850_CAUSE_QOS_UNAVAILABLE            49
#define Q850_CAUSE_SERVICE_NOT_AVAILABLE      63
#define Q850_CAUSE_BEARER_CAP_NOT_AUTHORIZED  65
#define Q850_CAUSE_FACILITY_NOT_IMPLEMENTED   69
#define Q850_CAUSE_INVALID_CALL_REF           81
#define Q850_CAUSE_INCOMPATIBLE_DESTINATION   88
#define Q850_CAUSE_INVALID_TRANSIT_NET_SEL    91
#define Q850_CAUSE_MANDATORY_IE_MISSING       96
#define Q850_CAUSE_MESSAGE_TYPE_NONEXISTENT   97
#define Q850_CAUSE_MSG_NOT_COMPAT_WITH_STATE  98
#define Q850_CAUSE_IE_NONEXISTENT_OR_NOT_IMPL 99
#define Q850_CAUSE_INVALID_IE_CONTENTS       100
#define Q850_CAUSE_MSG_INCOMPAT_WITH_STATE   101
#define Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY  102
#define Q850_CAUSE_EXCESS_REPETITIONS_OF_IE  104

/* Structure describing Q.850 Cause Entry */
typedef struct {
    u8          cause_val;
    const char *description;
} q850_cause_entry_t;

/* Lookup cause description */
const char *q850_get_cause_str(u8 cause_val);

/* Q.933 Message and State String Decoders */
const char *q933_get_msg_name(u8 msg_type);
const char *q933_get_state_name(u8 state);
const char *q933_get_state_short_str(u8 state);

#ifdef __cplusplus
}
#endif

#endif /* SVC_SIG_IEL_H */
