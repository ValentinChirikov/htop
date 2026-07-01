/*
htop - LlamaCpp.c
(C) 2026 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "linux/LlamaCpp.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>

#include "CRT.h"
#include "Object.h"
#include "RichString.h"
#include "XUtils.h"


/* Env var carrying the full endpoint URL; its presence enables the meter. */
#define LLAMACPP_ENV_URL           "HTOP_LLAMACPP_METRICS_URL"

/* Bound each network operation so the UI never stalls. */
#define LLAMACPP_TIMEOUT_MS        300

/* Reuse the cached scrape if the last one is younger than this. */
#define LLAMACPP_MIN_INTERVAL_MS   900


/* Parsed endpoint. urlState: 0 unchecked, 1 valid, -1 disabled/unsupported. */
static int urlState = 0;
static char llamaHost[256];
static char llamaPort[16];
static char llamaPath[512];

/* Cached reading. */
static bool haveData = false;
static double valTg = 0.0;      /* generation tokens/s */
static double valPp = 0.0;      /* prompt tokens/s */
static double valProc = 0.0;    /* requests processing */
static double valDef = 0.0;     /* requests deferred */
static double valNTok = 0.0;    /* max tokens seen */
static long long lastQueryMs = -1;


static long long monotonicMs(void) {
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return -1;
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


/* Parse HTOP_LLAMACPP_METRICS_URL once into host/port/path. Only plaintext
 * http:// is supported; anything else disables the meter. */
static void parseUrlOnce(void) {
   if (urlState != 0)
      return;

   const char* url = getenv(LLAMACPP_ENV_URL);
   if (!url || !url[0]) {
      urlState = -1;
      return;
   }

   if (!String_startsWith(url, "http://")) {
      urlState = -1;
      return;
   }
   const char* p = url + strlen("http://");

   const char* slash = strchr(p, '/');
   const char* colon = strchr(p, ':');

   size_t hostLen;
   if (colon && (!slash || colon < slash)) {
      hostLen = (size_t)(colon - p);
      const char* portStart = colon + 1;
      size_t portLen = slash ? (size_t)(slash - portStart) : strlen(portStart);
      if (portLen == 0 || portLen >= sizeof(llamaPort)) {
         urlState = -1;
         return;
      }
      memcpy(llamaPort, portStart, portLen);
      llamaPort[portLen] = '\0';
   } else {
      hostLen = slash ? (size_t)(slash - p) : strlen(p);
      memcpy(llamaPort, "80", sizeof("80"));
   }

   if (hostLen == 0 || hostLen >= sizeof(llamaHost)) {
      urlState = -1;
      return;
   }
   memcpy(llamaHost, p, hostLen);
   llamaHost[hostLen] = '\0';

   String_safeStrncpy(llamaPath, slash ? slash : "/metrics", sizeof(llamaPath));
   urlState = 1;
}


/* Perform a plaintext HTTP/1.0 GET of the endpoint into buf (NUL-terminated).
 * Non-blocking with bounded timeouts. Returns bytes read, or -1 on failure. */
static ssize_t llamaHttpGet(char* buf, size_t cap) {
   struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
   struct addrinfo* res = NULL;
   if (getaddrinfo(llamaHost, llamaPort, &hints, &res) != 0)
      return -1;

   int fd = -1;
   for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
      if (fd < 0)
         continue;

      int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
      if (rc < 0 && errno == EINPROGRESS) {
         struct pollfd pfd = { .fd = fd, .events = POLLOUT };
         rc = -1;
         if (poll(&pfd, 1, LLAMACPP_TIMEOUT_MS) > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
               rc = 0;
         }
      }
      if (rc == 0)
         break;

      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);
   if (fd < 0)
      return -1;

   char req[768];
   int reqLen = xSnprintf(req, sizeof(req),
      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: htop\r\nConnection: close\r\n\r\n",
      llamaPath, llamaHost);

   ssize_t sent = 0;
   while (sent < reqLen) {
      ssize_t w = send(fd, req + sent, (size_t)(reqLen - sent), MSG_NOSIGNAL);
      if (w > 0) {
         sent += w;
         continue;
      }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
         struct pollfd pfd = { .fd = fd, .events = POLLOUT };
         if (poll(&pfd, 1, LLAMACPP_TIMEOUT_MS) <= 0)
            break;
         continue;
      }
      break;
   }
   if (sent < reqLen) {
      close(fd);
      return -1;
   }

   size_t off = 0;
   long long deadline = monotonicMs() + LLAMACPP_TIMEOUT_MS;
   while (off + 1 < cap) {
      long long now = monotonicMs();
      int wait = (int)(deadline - now);
      if (wait <= 0)
         break;

      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      if (poll(&pfd, 1, wait) <= 0)
         break;

      ssize_t r = recv(fd, buf + off, cap - 1 - off, 0);
      if (r > 0) {
         off += (size_t)r;
         continue;
      }
      if (r == 0)
         break;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
         continue;
      break;
   }

   close(fd);
   buf[off] = '\0';
   return (ssize_t)off;
}


/* Extract the value of a Prometheus metric line "<name> <value>". Matches only
 * at the start of a line (so "# HELP <name> ..." comments are ignored) and
 * requires a trailing space (so "<name>" is not confused with "<name>_total"). */
static bool findMetric(const char* body, const char* name, double* out) {
   size_t nlen = strlen(name);
   const char* p = body;
   while ((p = strstr(p, name)) != NULL) {
      bool atLineStart = (p == body) || (p[-1] == '\n');
      if (atLineStart && p[nlen] == ' ') {
         return sscanf(p + nlen + 1, "%lf", out) == 1;
      }
      p += nlen;
   }
   return false;
}


static void llamaRefresh(void) {
   long long now = monotonicMs();
   if (now >= 0 && lastQueryMs >= 0 && now - lastQueryMs < LLAMACPP_MIN_INTERVAL_MS)
      return;
   lastQueryMs = now;

   char buf[8192];
   if (llamaHttpGet(buf, sizeof(buf)) <= 0) {
      haveData = false;
      return;
   }

   double tg, pp;
   /* These two gauges are always present; the rest are best-effort. */
   if (!findMetric(buf, "llamacpp:predicted_tokens_seconds", &tg) ||
       !findMetric(buf, "llamacpp:prompt_tokens_seconds", &pp)) {
      haveData = false;
      return;
   }

   valTg = tg;
   valPp = pp;
   if (!findMetric(buf, "llamacpp:requests_processing", &valProc)) valProc = 0.0;
   if (!findMetric(buf, "llamacpp:requests_deferred", &valDef)) valDef = 0.0;
   if (!findMetric(buf, "llamacpp:n_tokens_max", &valNTok)) valNTok = 0.0;
   haveData = true;
}


static void LlamaCppMeter_updateValues(Meter* this) {
   parseUrlOnce();
   if (urlState != 1) {
      memcpy(this->txtBuffer, "N/A", sizeof("N/A"));
      return;
   }

   llamaRefresh();
   if (!haveData) {
      memcpy(this->txtBuffer, "N/A", sizeof("N/A"));
      return;
   }

   xSnprintf(this->txtBuffer, sizeof(this->txtBuffer),
      "tg: %.1f pp: %.1f t/s  req: %.0f/%.0f  ntok: %.0f",
      valTg, valPp, valProc, valDef, valNTok);
}


static void LlamaCppMeter_display(const Object* cast, RichString* out) {
   const Meter* this = (const Meter*)cast;
   (void)this;

   if (urlState != 1 || !haveData) {
      RichString_appendAscii(out, CRT_colors[METER_SHADOW], "N/A");
      return;
   }

   char buffer[32];
   int written;

   RichString_appendAscii(out, CRT_colors[METER_TEXT], "tg: ");
   written = xSnprintf(buffer, sizeof(buffer), "%.1f", valTg);
   RichString_appendnAscii(out, CRT_colors[METER_VALUE_IOREAD], buffer, written);

   RichString_appendAscii(out, CRT_colors[METER_TEXT], " pp: ");
   written = xSnprintf(buffer, sizeof(buffer), "%.1f", valPp);
   RichString_appendnAscii(out, CRT_colors[METER_VALUE_IOREAD], buffer, written);

   RichString_appendAscii(out, CRT_colors[METER_TEXT], " t/s  req: ");
   written = xSnprintf(buffer, sizeof(buffer), "%.0f", valProc);
   RichString_appendnAscii(out, CRT_colors[METER_VALUE_IOREAD], buffer, written);
   RichString_appendAscii(out, CRT_colors[METER_TEXT], "/");
   written = xSnprintf(buffer, sizeof(buffer), "%.0f", valDef);
   RichString_appendnAscii(out, CRT_colors[METER_VALUE_IOREAD], buffer, written);

   RichString_appendAscii(out, CRT_colors[METER_TEXT], "  ntok: ");
   written = xSnprintf(buffer, sizeof(buffer), "%.0f", valNTok);
   RichString_appendnAscii(out, CRT_colors[METER_VALUE_IOREAD], buffer, written);
}


static const int LlamaCppMeter_attributes[] = { METER_VALUE };

const MeterClass LlamaCppMeter_class = {
   .super = {
      .extends = Class(Meter),
      .delete = Meter_delete,
      .display = LlamaCppMeter_display,
   },
   .updateValues = LlamaCppMeter_updateValues,
   .defaultMode = TEXT_METERMODE,
   .supportedModes = (1 << TEXT_METERMODE),
   .maxItems = 0,
   .total = 0.0,
   .attributes = LlamaCppMeter_attributes,
   .name = "LlamaCpp",
   .uiName = "LLM",
   .caption = "LLM: ",
   .description = "llama.cpp token throughput (HTOP_LLAMACPP_METRICS_URL)",
};
