// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one seam ipmi_handler.c needs into the environment service. That file
// owns the vtable machinery and knows nothing about what the methods mean.
//
// Separate from sm_env_ipmi.h on purpose: that header is the wire contract,
// copied verbatim into every client, and must stay free of anything internal to
// this payload.

#pragma once

#include "ipmi.h"
#include "sm_env_ipmi.h"

#include <stdint.h>

// Serves one sync dispatch. -> 0 on success, negative on refusal. Every path
// out must leave a truthful out[i].written: the framework reuses the out
// buffer, so a stale length gets the client killed for the mismatch.
int sm_env_ipmi_dispatch(IpmiSession *session, uint32_t method,
                         const IpmiDataInfo *in, uint32_t inCount,
                         IpmiOutBuffer *out, uint32_t outCount);

// What an unknown or unserviceable method answers with; the value is ours to
// pick because both ends of this service are ours.
#define SM_ENV_IPMI_ENOTSUP (-1)
