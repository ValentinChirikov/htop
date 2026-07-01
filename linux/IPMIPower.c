/*
htop - IPMIPower.c
(C) 2026 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/IPMIPower.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/ipmi.h>

#include "CRT.h"
#include "Object.h"
#include "RichString.h"
#include "XUtils.h"


/* DCMI "Get Power Reading" request (DSP0143): group extension 0xDC,
 * mode 0x01 (system power statistics), two reserved bytes. */
#define IPMI_DCMI_NETFN            0x2c
#define IPMI_DCMI_CMD_GET_POWER    0x02
#define IPMI_DCMI_GROUP_EXTENSION  0xdc

/* Bar total (Watts) when no sampling maximum has been observed yet. */
#define IPMI_POWER_DEFAULT_TOTAL   1000.0

/* Do not hammer the BMC: a DCMI query costs tens to hundreds of ms, so reuse
 * the cached reading if the last successful query is younger than this. */
#define IPMI_POWER_MIN_INTERVAL_MS 900

/* Bound the worst-case UI stall while waiting for the BMC response. */
#define IPMI_POWER_POLL_TIMEOUT_MS 400


static int ipmiFd = -1;
static bool ipmiUnavailable = false;
static long ipmiMsgId = 0;

/* Cached reading (Watts); negative means "no valid reading". */
static double cachedWatts = -1.0;
static double cachedMaxWatts = -1.0;
static long long lastQueryMs = -1;


static long long monotonicMs(void) {
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return -1;
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


/* Send the DCMI Get Power Reading command and decode the response.
 * Returns true and fills *watts (and *maxWatts) on success. */
static bool ipmiQueryPower(double* watts, double* maxWatts) {
   if (ipmiUnavailable)
      return false;

   if (ipmiFd < 0) {
      /* IPMI commands require write access, so open read-write. */
      ipmiFd = open("/dev/ipmi0", O_RDWR);
      if (ipmiFd < 0) {
         ipmiUnavailable = true;
         return false;
      }
   }

   struct ipmi_system_interface_addr addr = {
      .addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE,
      .channel = IPMI_BMC_CHANNEL,
      .lun = 0,
   };

   unsigned char reqData[] = { IPMI_DCMI_GROUP_EXTENSION, 0x01, 0x00, 0x00 };
   struct ipmi_req req = {
      .addr = (unsigned char*) &addr,
      .addr_len = sizeof(addr),
      .msgid = ++ipmiMsgId,
      .msg = {
         .netfn = IPMI_DCMI_NETFN,
         .cmd = IPMI_DCMI_CMD_GET_POWER,
         .data = reqData,
         .data_len = sizeof(reqData),
      },
   };

   if (ioctl(ipmiFd, IPMICTL_SEND_COMMAND, &req) < 0)
      return false;

   struct pollfd pfd = { .fd = ipmiFd, .events = POLLIN };
   int ready = poll(&pfd, 1, IPMI_POWER_POLL_TIMEOUT_MS);
   if (ready <= 0)
      return false;

   unsigned char respData[64];
   unsigned char respAddr[IPMI_MAX_ADDR_SIZE];
   struct ipmi_recv recv = {
      .addr = respAddr,
      .addr_len = sizeof(respAddr),
      .msg = {
         .data = respData,
         .data_len = sizeof(respData),
      },
   };

   if (ioctl(ipmiFd, IPMICTL_RECEIVE_MSG_TRUNC, &recv) < 0)
      return false;

   /* Response: [0] completion code, [1] group extension (0xDC),
    * [2..3] current power (LSB first), [6..7] maximum during sampling. */
   if (recv.msg.data_len < 4 || respData[0] != 0x00 || respData[1] != IPMI_DCMI_GROUP_EXTENSION)
      return false;

   *watts = (double)(respData[2] | (respData[3] << 8));
   if (recv.msg.data_len >= 8)
      *maxWatts = (double)(respData[6] | (respData[7] << 8));
   else
      *maxWatts = -1.0;

   return true;
}


/* Refresh the cached reading, throttled to IPMI_POWER_MIN_INTERVAL_MS. */
static void ipmiRefresh(void) {
   long long now = monotonicMs();
   if (now >= 0 && lastQueryMs >= 0 && now - lastQueryMs < IPMI_POWER_MIN_INTERVAL_MS)
      return;

   double watts = -1.0;
   double maxWatts = -1.0;
   if (ipmiQueryPower(&watts, &maxWatts)) {
      cachedWatts = watts;
      if (maxWatts > 0.0)
         cachedMaxWatts = maxWatts;
   } else {
      cachedWatts = -1.0;
   }
   lastQueryMs = now;
}


static void IPMIPowerMeter_updateValues(Meter* this) {
   ipmiRefresh();

   if (cachedWatts < 0.0) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      this->values[0] = NAN;
      return;
   }

   this->total = (cachedMaxWatts > 0.0) ? cachedMaxWatts : IPMI_POWER_DEFAULT_TOTAL;
   this->values[0] = cachedWatts;
   xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.0fW", cachedWatts);
}


static void IPMIPowerMeter_display(const Object* cast, RichString* out) {
   const Meter* this = (const Meter*)cast;

   if (!isNonnegative(this->values[0])) {
      RichString_appendAscii(out, CRT_colors[METER_SHADOW], " N/A");
      return;
   }

   char buffer[16];
   int written;

   RichString_appendAscii(out, CRT_colors[METER_TEXT], ":");
   written = xSnprintf(buffer, sizeof(buffer), "%.0fW", this->values[0]);
   RichString_appendnAscii(out, CRT_colors[METER_VALUE], buffer, written);
}


const MeterClass IPMIPowerMeter_class = {
   .super = {
      .extends = Class(Meter),
      .delete = Meter_delete,
      .display = IPMIPowerMeter_display,
   },
   .updateValues = IPMIPowerMeter_updateValues,
   .defaultMode = BAR_METERMODE,
   .supportedModes = METERMODE_DEFAULT_SUPPORTED,
   .maxItems = 1,
   .total = IPMI_POWER_DEFAULT_TOTAL,
   .attributes = (const int[]) { METER_VALUE_OK },
   .name = "IPMIPower",
   .uiName = "Power (IPMI)",
   .caption = "Power",
   .description = "Whole-server power draw from the BMC via IPMI DCMI",
};
