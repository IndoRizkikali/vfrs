/*
 * svc_sig_iel.c - Q.933 Information Element Layouts & Cause Codes Table
 * Virtual Frame Relay Switch
 */

#include "svc_sig_iel.h"

static const q850_cause_entry_t g_q850_causes[] = {
    { Q850_CAUSE_UNALLOCATED_NUMBER,         "Unallocated (unassigned) number" },
    { Q850_CAUSE_NO_ROUTE_TO_TRANSIT_NET,     "No route to specified transit network" },
    { Q850_CAUSE_NO_ROUTE_TO_DESTINATION,     "No route to destination" },
    { Q850_CAUSE_CHANNEL_UNACCEPTABLE,        "Channel unacceptable" },
    { Q850_CAUSE_PREEMPTION,                 "Preemption" },
    { Q850_CAUSE_NORMAL_CLEARING,            "Normal call clearing" },
    { Q850_CAUSE_USER_BUSY,                  "User busy" },
    { Q850_CAUSE_NO_USER_RESPONDING,         "No user responding" },
    { Q850_CAUSE_NO_ANSWER_FROM_USER,        "No answer from user (user alerted)" },
    { Q850_CAUSE_CALL_REJECTED,              "Call rejected" },
    { Q850_CAUSE_NUMBER_CHANGED,             "Number changed" },
    { Q850_CAUSE_DESTINATION_OUT_OF_ORDER,   "Destination out of order" },
    { Q850_CAUSE_INVALID_NUMBER_FORMAT,      "Invalid number format (incomplete number)" },
    { Q850_CAUSE_FACILITY_REJECTED,          "Facility rejected" },
    { Q850_CAUSE_RESPONSE_TO_STATUS_ENQ,     "Response to STATUS ENQUIRY" },
    { Q850_CAUSE_NORMAL_UNSPECIFIED,         "Normal, unspecified" },
    { Q850_CAUSE_NO_CIRCUIT_AVAILABLE,       "Circuit/channel congestion / no circuit available" },
    { Q850_CAUSE_NETWORK_OUT_OF_ORDER,       "Network out of order" },
    { Q850_CAUSE_PERM_CONN_OUT_OF_SERVICE,   "Permanent frame mode connection out of service" },
    { Q850_CAUSE_TEMPORARY_FAILURE,          "Temporary failure" },
    { Q850_CAUSE_SWITCHING_EQUIP_CONGESTION, "Switching equipment congestion" },
    { Q850_CAUSE_ACCESS_INFO_DISCARDED,      "Access information discarded" },
    { Q850_CAUSE_CIRCUIT_NOT_AVAILABLE,      "Requested circuit/channel not available" },
    { Q850_CAUSE_RESOURCE_UNAVAILABLE,       "Resource unavailable, unspecified" },
    { Q850_CAUSE_QOS_UNAVAILABLE,            "Quality of service not available" },
    { Q850_CAUSE_SERVICE_NOT_AVAILABLE,      "Service or option not available, unspecified" },
    { Q850_CAUSE_BEARER_CAP_NOT_AUTHORIZED,  "Bearer capability not authorized" },
    { Q850_CAUSE_FACILITY_NOT_IMPLEMENTED,   "Requested facility not implemented" },
    { Q850_CAUSE_INVALID_CALL_REF,           "Invalid call reference value" },
    { Q850_CAUSE_INCOMPATIBLE_DESTINATION,   "Incompatible destination" },
    { Q850_CAUSE_INVALID_TRANSIT_NET_SEL,    "Invalid transit network selection" },
    { Q850_CAUSE_MANDATORY_IE_MISSING,       "Mandatory information element missing" },
    { Q850_CAUSE_MESSAGE_TYPE_NONEXISTENT,   "Message type non-existent or not implemented" },
    { Q850_CAUSE_MSG_NOT_COMPAT_WITH_STATE,  "Message not compatible with call state" },
    { Q850_CAUSE_IE_NONEXISTENT_OR_NOT_IMPL, "Information element/parameter non-existent or not implemented" },
    { Q850_CAUSE_INVALID_IE_CONTENTS,       "Invalid information element contents" },
    { Q850_CAUSE_MSG_INCOMPAT_WITH_STATE,   "Message not compatible with call state" },
    { Q850_CAUSE_RECOVERY_ON_TIMER_EXPIRY,  "Recovery on timer expiry" },
    { Q850_CAUSE_EXCESS_REPETITIONS_OF_IE,  "Excess repetitions of information element" }
};

const char *q850_get_cause_str(u8 cause_val) {
    size_t count = sizeof(g_q850_causes) / sizeof(g_q850_causes[0]);
    for (size_t i = 0; i < count; i++) {
        if (g_q850_causes[i].cause_val == cause_val) {
            return g_q850_causes[i].description;
        }
    }
    return "Unknown cause code";
}

const char *q933_get_msg_name(u8 msg_type) {
    switch (msg_type) {
        case Q933_MSG_SETUP:               return "SETUP";
        case Q933_MSG_CALL_PROCEEDING:     return "CALL PROCEEDING";
        case Q933_MSG_CONNECT:             return "CONNECT";
        case Q933_MSG_CONNECT_ACK:         return "CONNECT ACK";
        case Q933_MSG_DISCONNECT:          return "DISCONNECT";
        case Q933_MSG_RELEASE:             return "RELEASE";
        case Q933_MSG_RELEASE_COMPLETE:    return "RELEASE COMPLETE";
        case Q933_MSG_STATUS_ENQUIRY:      return "STATUS ENQUIRY";
        case Q933_MSG_STATUS:              return "STATUS";
        case Q933_MSG_RESTART:             return "RESTART";
        case Q933_MSG_RESTART_ACK:         return "RESTART ACKNOWLEDGE";
        default:                           return "UNKNOWN MESSAGE";
    }
}

const char *q933_get_state_name(u8 state) {
    switch (state) {
        case SVC_STATE_NULL:               return "Null (N0)";
        case SVC_STATE_CALL_INITIATED:     return "Call Initiated (N1)";
        case SVC_STATE_OUTGOING_PROC:      return "Outgoing Call Proceeding (N3)";
        case SVC_STATE_CALL_PRESENT:       return "Call Present (N6)";
        case SVC_STATE_CONNECT_REQUEST:    return "Connect Request (N8)";
        case SVC_STATE_INCOMING_PROC:      return "Incoming Call Proceeding (N9)";
        case SVC_STATE_ACTIVE:             return "Active (N10)";
        case SVC_STATE_DISCONNECT_REQ:     return "Disconnect Request (N11)";
        case SVC_STATE_DISCONNECT_IND:     return "Disconnect Indication (N12)";
        case SVC_STATE_RELEASE_REQ:        return "Release Request (N19)";
        case SVC_STATE_RESTART_REQ:        return "Restart Request (Rest1)";
        case SVC_STATE_RESTART:            return "Restart (Rest2)";
        default:                           return "Unknown State";
    }
}

const char *q933_get_state_short_str(u8 state) {
    switch (state) {
        case SVC_STATE_NULL:               return "N0";
        case SVC_STATE_CALL_INITIATED:     return "N1";
        case SVC_STATE_OUTGOING_PROC:      return "N3";
        case SVC_STATE_CALL_PRESENT:       return "N6";
        case SVC_STATE_CONNECT_REQUEST:    return "N8";
        case SVC_STATE_INCOMING_PROC:      return "N9";
        case SVC_STATE_ACTIVE:             return "N10";
        case SVC_STATE_DISCONNECT_REQ:     return "N11";
        case SVC_STATE_DISCONNECT_IND:     return "N12";
        case SVC_STATE_RELEASE_REQ:        return "N19";
        case SVC_STATE_RESTART_REQ:        return "Rest1";
        case SVC_STATE_RESTART:            return "Rest2";
        default:                           return "N?";
    }
}
