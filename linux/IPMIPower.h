#ifndef HEADER_IPMIPower
#define HEADER_IPMIPower
/*
htop - IPMIPower.h
(C) 2026 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "Meter.h"

/* Whole-server power draw read from the BMC via IPMI DCMI "Get Power Reading"
 * (netfn 0x2C, cmd 0x02) over /dev/ipmi0. Shows N/A when the device is absent
 * or unreadable (e.g. missing the udev 'ipmi' group grant). */
extern const MeterClass IPMIPowerMeter_class;

#endif
