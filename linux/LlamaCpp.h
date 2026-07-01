#ifndef HEADER_LlamaCpp
#define HEADER_LlamaCpp
/*
htop - LlamaCpp.h
(C) 2026 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "Meter.h"

/* Token throughput and request state of a local llama.cpp server, scraped
 * from its Prometheus /metrics endpoint. Enabled (and located) via the
 * HTOP_LLAMACPP_METRICS_URL environment variable, e.g.
 * http://127.0.0.1:2233/metrics. Renders N/A when unset or unreachable. */
extern const MeterClass LlamaCppMeter_class;

#endif
