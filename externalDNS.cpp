/* Query external DNS
 *
 * Architecture (single-task design):
 *   prepDNS()  : opens ONE UDP socket bound to :53 and spawns 'dnsTask'.
 *   dnsTask()  : recvfrom -> processDNSquery -> sendto (same socket, so replies
 *                carry source port 53 like any real resolver).
 *   resolveDomainStatus(): synchronous rcode-aware query to the configured
 *                upstreams; distinguishes NXDOMAIN (RCODE 3) from SERVFAIL
 *                (timeout / RCODE 2,4,5), which lwIP's gethostbyname cannot.
 * Concurrency: only 'dnsTask' talks to dnsSock; upstream probes use short-lived
 * ephemeral sockets, so upstream replies can never be confused with queries. */
//
// dateno1 2026
// s60sc 2026

#include "appGlobals.h"
#include <lwip/sockets.h>   // socket/sendto/recvfrom/setsockopt/close

#define DNS_DEFAULT_PORT   53    // listening port (also reply source port)
//#define CACHE_SIZE         20    // positive-response cache slots (round-robin)
#define CACHE_SIZE         256    // positive-response cache slots (round-robin), allocated in PSRAM (~270 bytes each)
#define PEND_MAX           64     // client queries waiting on upstream at once (~570 bytes each, PSRAM)
#define DEFAULT_TTL        300000 // cache lifetime, ms
#define MAX_HOSTNAME       256    // longest name we accept from clients
//#define RESOLVE_TIMEOUT_MS 1500   // per-upstream-server attempt
#define RESOLVE_TIMEOUT_MS 800   // per-upstream-server attempt

typedef struct {
    uint16_t id;       // transaction ID (echoed in reply)
    uint16_t flags;    // QR/opcode/TC/RD/RA + RCODE live here in replies
    uint16_t qdcount;  // questions
    uint16_t ancount;  // answers
    uint16_t nscount;  // authority
    uint16_t arcount;  // additional
} __attribute__((packed)) dns_header_t;

/* Parse the QNAME starting at 'offset' into dotted text form.
 * Returns offset just past the terminating root label, or -1 on malformed
 * input (overrun, compression pointer, empty). Bounds-checked throughout -
 * never trust packet contents. */
int parseDNSname(uint8_t *packet, int offset, char *out, int outSize, int pktLen) {
  int i = 0;
  while (offset < pktLen && packet[offset] != 0) {
    int len = packet[offset++];
    if ((len & 0xC0) == 0xC0) return -1;      // pointers illegal in question section
    if (offset + len > pktLen) return -1;
    if (i + len + 2 >= outSize) return -1;    // reserve room for '.' + NUL
    for (int j = 0; j < len; j++) out[i++] = packet[offset++];
    out[i++] = '.';
  }
  if (i == 0 || offset >= pktLen) return -1;  // empty/unterminated name
  out[i - 1] = '\0';                          // strip trailing dot
  return offset + 1;
}

/* Family-tagged positive cache: fam 1 = A (4B), 28 = AAAA (16B).
 * Negative results are never cached (fast recovery after WAN outage). */
struct CacheEntry {
  char hostname[MAX_HOSTNAME] = {0};
  uint8_t fam;                                // 1 or 28
  uint8_t addr[16];                           // 4 or 16 bytes used
  uint32_t expiry;
};
static CacheEntry *dnsCache = NULL;   // allocated by prepDNS(); NULL = caching off

static bool cacheGet(const char* host, uint8_t fam, uint8_t* addr);
static void cachePut(const char* host, uint8_t fam, const uint8_t* addr);
static bool isLocalName(const char* host);

/* Reply kinds for buildReply() */
enum ReplyKind : uint8_t { R_NODATA, R_NXDOMAIN, R_SERVFAIL, R_A, R_AAAA };

/* Copy header+question (qlen bytes) from q into tx, then append the answer
 * for 'kind' (addr = 4 or 16 bytes for R_A/R_AAAA). Returns reply length. */
static int buildReply(uint8_t *tx, int txSize, const uint8_t *q, int qlen,
                      ReplyKind kind, const uint8_t *addr) {
  int alen = (kind == R_A) ? 4 : (kind == R_AAAA) ? 16 : 0;
  if (qlen + (alen ? 12 + alen : 0) > txSize) return 0;
  memcpy(tx, q, qlen);                        // echo header + question verbatim
  dns_header_t *res = (dns_header_t *)tx;
  res->qdcount = htons(1);
  res->nscount = 0;
  res->arcount = 0;                           // no OPT/EDNS downstream
  res->ancount = htons(alen ? 1 : 0);
  uint16_t rcode = (kind == R_NXDOMAIN) ? 3 : (kind == R_SERVFAIL) ? 2 : 0;
  res->flags = htons(0x8180 | rcode);
  int pos = qlen;
  if (alen) {
    tx[pos++] = 0xC0; tx[pos++] = 0x0C;                   // name pointer
    tx[pos++] = 0x00; tx[pos++] = (kind == R_A) ? 0x01 : 0x1C; // TYPE
    tx[pos++] = 0x00; tx[pos++] = 0x01;                   // CLASS IN
    tx[pos++] = 0x00; tx[pos++] = 0x00;                   // TTL 60s
    tx[pos++] = 0x00; tx[pos++] = 0x3C;
    tx[pos++] = 0x00; tx[pos++] = alen;                   // RDLENGTH
    memcpy(&tx[pos], addr, alen);
    pos += alen;
  }
  return pos;
}

/* One client query waiting for an upstream answer. Header+question of the client
 * query are kept so the reply (and any failover re-query) can be built later. */
struct Pending {
  bool used;
  uint8_t srv;                  // index of upstream server currently being tried
  uint16_t qtype;               // 1 = A, 28 = AAAA
  uint16_t qlen;                // header + question length
  uint16_t upId;                // transaction ID sent upstream
  uint32_t deadline;            // millis() when current attempt times out
  struct sockaddr_in cli;       // client to reply to
  uint8_t q[272];               // client header+question (12 + 255 + 4 max)
  char name[MAX_HOSTNAME];      // lowercase query name, for the cache
};

/* Core decision engine. Returns reply length (>0), 0 to drop silently (malformed
 * query), or -1 if an upstream lookup is needed - then 'p' (which must be non-NULL
 * to get -1) has been filled in and the caller forwards it asynchronously. */
static int processDNSquery(const uint8_t *rx, int len, uint8_t *tx, int txSize, Pending *p) {
  int offset = sizeof(dns_header_t);
  if (len < offset + 5) return 0;             // need header + minimal question
  char domain[MAX_HOSTNAME];
  int new_offset = parseDNSname((uint8_t *)rx, offset, domain, sizeof(domain), len);
  if (new_offset < 0 || new_offset + 4 > len) return 0;

  uint16_t qtype = ((uint16_t)rx[new_offset] << 8) | rx[new_offset + 1];
  int qlen = new_offset + 4;                  // header + question, skipping QTYPE + QCLASS
  if (qlen > (int)sizeof(p->q)) return 0;

  /* Policy:
   *   BLOCKED  -> sinkhole 0.0.0.0 (A) or :: (AAAA), NODATA for other types
   *   other non-A/AAAA types -> NODATA at once (never worth an upstream lookup)
   *   A/AAAA   -> cache, else async upstream lookup for that same type
   *               (RCODE 3 = NXDOMAIN, RCODE 2 = SERVFAIL, never cached) */
  static const uint8_t zeros[16] = {0};
  IPAddress ansIP;
  DnsResult r = checkBlocklist(domain, ansIP, false);   // blocklist only, no upstream
  LOG_VRB("Q '%s' type=%u -> %d", domain, qtype, (int)r);
  bool isA = (qtype == 0x0001), isAAAA = (qtype == 0x001C);

  if (r == DNS_SERVFAIL) return buildReply(tx, txSize, rx, qlen, R_SERVFAIL, NULL);
  if (r == DNS_BLOCKED)
    return buildReply(tx, txSize, rx, qlen, isA ? R_A : isAAAA ? R_AAAA : R_NODATA, zeros);
  if (!isA && !isAAAA) return buildReply(tx, txSize, rx, qlen, R_NODATA, NULL);

  char name[MAX_HOSTNAME];                    // same normalisation as checkBlocklist
  size_t n = 0;
  for (; domain[n]; n++) name[n] = (char)tolower((unsigned char)domain[n]);
  name[n] = 0;

  if (!strcmp(name, "localhost")) {           // RFC 6761: A 127.0.0.1, AAAA ::1
    uint8_t lo[16] = {0}; lo[15] = 1;
    static const uint8_t lo4[4] = {127, 0, 0, 1};
    return buildReply(tx, txSize, rx, qlen, isA ? R_A : R_AAAA, isA ? lo4 : lo);
  }
  if (isLocalName(name)) return buildReply(tx, txSize, rx, qlen, R_NXDOMAIN, NULL);

  uint8_t addr[16];
  if (cacheGet(name, isA ? 1 : 28, addr))
    return buildReply(tx, txSize, rx, qlen, isA ? R_A : R_AAAA, addr);

  if (!p) return buildReply(tx, txSize, rx, qlen, R_SERVFAIL, NULL);   // no free slot
  memcpy(p->q, rx, qlen);
  p->qlen = qlen;
  p->qtype = qtype;
  strcpy(p->name, name);
  return -1;
}

/* DNS server: one task multiplexes the client socket and the upstream socket with
 * select(), so a slow upstream never blocks reading new queries (lwIP only queues
 * a few datagrams per socket, the rest are dropped). */
static int dnsSock = -1;
static int upSock = -1;
static Pending *pend = NULL;
static uint8_t rxbuf[512];
static uint8_t txbuf[512];

static void completePending(Pending &p, ReplyKind kind, const uint8_t *addr) {
  int txLen = buildReply(txbuf, sizeof(txbuf), p.q, p.qlen, kind, addr);
  if (txLen > 0) sendto(dnsSock, txbuf, txLen, 0, (struct sockaddr *)&p.cli, sizeof(p.cli));
  p.used = false;
}

/* Send (or resend to the next server) the upstream query for p. On running out of
 * servers, answers the client with SERVFAIL. */
static void sendUpstream(Pending &p) {
  const char* servers[] = {ST_ns1, ST_ns2};
  for (; p.srv < 2; p.srv++) {
    IPAddress srv;
    if (!servers[p.srv][0] || !srv.fromString(servers[p.srv])) continue;
    uint8_t qbuf[280];
    uint16_t id;
    bool clash;
    do {                                       // unique among in-flight upstream queries
      id = (uint16_t)esp_random();
      clash = false;
      for (int i = 0; i < PEND_MAX; i++) if (pend[i].used && &pend[i] != &p && pend[i].upId == id) clash = true;
    } while (clash);
    p.upId = id;
    memcpy(qbuf, p.q, p.qlen);                 // question section reused as is
    qbuf[0] = id >> 8; qbuf[1] = id & 0xFF;
    qbuf[2] = 0x01; qbuf[3] = 0x00;            // RD=1
    qbuf[4] = 0; qbuf[5] = 1;                  // QDCOUNT=1
    memset(qbuf + 6, 0, 6);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DNS_DEFAULT_PORT);
    dst.sin_addr.s_addr = srv;
    if (sendto(upSock, qbuf, p.qlen, 0, (struct sockaddr *)&dst, sizeof(dst)) == p.qlen) {
      p.deadline = millis() + RESOLVE_TIMEOUT_MS;
      return;
    }
  }
  completePending(p, R_SERVFAIL, NULL);
}

static void nextServer(Pending &p) {
  p.srv++;
  sendUpstream(p);
}

/* Handle one datagram from an upstream server. Matched by transaction ID and
 * the server address currently being tried. */
static void handleUpstreamReply(const uint8_t *rbuf, int rxLen, const struct sockaddr_in &from) {
  if (rxLen < 12 || !(rbuf[2] & 0x80)) return;             // too short / not a response
  uint16_t id = ((uint16_t)rbuf[0] << 8) | rbuf[1];
  const char* servers[] = {ST_ns1, ST_ns2};
  for (int i = 0; i < PEND_MAX; i++) {
    Pending &p = pend[i];
    if (!p.used || p.upId != id) continue;
    IPAddress srv;
    if (!srv.fromString(servers[p.srv]) || from.sin_addr.s_addr != (uint32_t)srv) return; // spoof/stale
    const uint8_t wantLen = (p.qtype == 28) ? 16 : 4;
    uint8_t rcode = rbuf[3] & 0x0F;
    if (rcode == 3) { completePending(p, R_NXDOMAIN, NULL); return; }
    if (rcode != 0) { nextServer(p); return; }              // SERVFAIL etc: try the backup
    size_t q = 12;
    bool cmp = false;
    while (q < (size_t)rxLen && rbuf[q] != 0) {            // skip question
      if ((rbuf[q] & 0xC0) == 0xC0) { q += 2; cmp = true; break; }
      q += 1 + rbuf[q];
    }
    if (!cmp) q++;                                         // root byte
    q += 4;                                                // QTYPE+QCLASS
    uint16_t ancount = ((uint16_t)rbuf[6] << 8) | rbuf[7];
    for (uint16_t a = 0; a < ancount && q + 10 <= (size_t)rxLen; a++) {
      if ((rbuf[q] & 0xC0) == 0xC0) q += 2;                // compressed owner
      else { while (q < (size_t)rxLen && rbuf[q]) q += 1 + rbuf[q]; q++; }
      if (q + 10 > (size_t)rxLen) break;
      uint16_t rt  = ((uint16_t)rbuf[q] << 8) | rbuf[q + 1];
      uint16_t rdl = ((uint16_t)rbuf[q + 8] << 8) | rbuf[q + 9];
      q += 10;                                             // TYPE+CLASS+TTL+RDLEN
      if (rt == p.qtype && rdl == wantLen && q + rdl <= (size_t)rxLen) {
        uint8_t addr[16];
        memcpy(addr, rbuf + q, rdl);
        cachePut(p.name, p.qtype == 28 ? 28 : 1, addr);
        completePending(p, p.qtype == 28 ? R_AAAA : R_A, addr);
        return;
      }
      q += rdl;                                            // CNAME/others: skip
    }
    completePending(p, R_NODATA, NULL);                    // name exists, no record of this type
    return;
  }
}

static void dnsTask(void *parameter) {
  struct sockaddr_in cli, from;
  socklen_t clilen, fl;
  uint8_t rbuf[512];
  for (;;) {
    // sleep until traffic or the earliest upstream deadline
    uint32_t now = millis();
    int32_t waitMs = 1000;
    for (int i = 0; i < PEND_MAX; i++)
      if (pend[i].used) {
        int32_t d = (int32_t)(pend[i].deadline - now);
        if (d < waitMs) waitMs = d;
      }
    if (waitMs < 0) waitMs = 0;
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(dnsSock, &rs);
    FD_SET(upSock, &rs);
    struct timeval tv;
    tv.tv_sec = waitMs / 1000;
    tv.tv_usec = (waitMs % 1000) * 1000;
    select((dnsSock > upSock ? dnsSock : upSock) + 1, &rs, NULL, NULL, &tv);

    // client queries: drain the socket queue before doing anything slow
    for (int n = 0; n < 32; n++) {
      clilen = sizeof(cli);
      int len = recvfrom(dnsSock, rxbuf, sizeof(rxbuf), MSG_DONTWAIT, (struct sockaddr *)&cli, &clilen);
      if (len < 0) break;
      if (len < (int)sizeof(dns_header_t)) continue;
      Pending *slot = NULL;
      for (int i = 0; i < PEND_MAX; i++) if (!pend[i].used) { slot = &pend[i]; break; }
      int txLen = processDNSquery(rxbuf, len, txbuf, sizeof(txbuf), slot);
      if (txLen < 0) {                         // needs upstream: park it
        slot->used = true;
        slot->cli = cli;
        slot->srv = 0;
        sendUpstream(*slot);
      } else if (txLen > 0) {
        sendto(dnsSock, txbuf, txLen, 0, (struct sockaddr *)&cli, clilen);
      }
    }

    // upstream replies
    for (int n = 0; n < 32; n++) {
      fl = sizeof(from);
      int len = recvfrom(upSock, rbuf, sizeof(rbuf), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
      if (len < 0) break;
      handleUpstreamReply(rbuf, len, from);
    }

    // timeouts: try the backup server, else SERVFAIL
    now = millis();
    for (int i = 0; i < PEND_MAX; i++)
      if (pend[i].used && (int32_t)(now - pend[i].deadline) >= 0) nextServer(pend[i]);
  }
}

static void dnsStartFail(const char* msg) {
  snprintf(startupFailure, SF_LEN, STARTUP_FAIL "%s", msg);
  LOG_WRN("%s", startupFailure);
  if (dnsSock >= 0) { close(dnsSock); dnsSock = -1; }
  if (upSock >= 0) { close(upSock); upSock = -1; }
}

void prepDNS() {
  pend = (Pending*)(psramFound() ? ps_calloc(PEND_MAX, sizeof(Pending)) : calloc(PEND_MAX, sizeof(Pending)));
  dnsCache = (CacheEntry*)(psramFound() ? ps_calloc(CACHE_SIZE, sizeof(CacheEntry)) : calloc(CACHE_SIZE, sizeof(CacheEntry)));
  if (!pend) return dnsStartFail("DNS pending table not allocated");
  dnsSock = socket(AF_INET, SOCK_DGRAM, 0);
  upSock = socket(AF_INET, SOCK_DGRAM, 0);
  if (dnsSock < 0 || upSock < 0) return dnsStartFail("DNS socket not created");
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DNS_DEFAULT_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;          // answers on STA and AP alike
  if (bind(dnsSock, (struct sockaddr *)&addr, sizeof(addr)) < 0) return dnsStartFail("DNS port 53 bind failed");
  if (xTaskCreatePinnedToCore(dnsTask, "dnsTask", 6144, NULL, 5, NULL, 1) != pdPASS)
    return dnsStartFail("DNS worker not started");
  LOG_INF("AdBlocker DNS Server started on %s:%d", formatIPstr(), DNS_DEFAULT_PORT);
}

/************************ DNS Forwarder **************************/

/* Tiny positive-only cache. Negative results (NXDOMAIN/SERVFAIL) are never
 * cached so recovery after a WAN outage is immediate. */

static bool cacheGet(const char* host, uint8_t fam, uint8_t* addr) {
  if (!dnsCache) return false;
  uint32_t now = millis();
  for (int i = 0; i < CACHE_SIZE; i++) {
    if (dnsCache[i].hostname[0] && dnsCache[i].fam == fam &&
        !strcmp(dnsCache[i].hostname, host)) {
      if ((int32_t)(now - dnsCache[i].expiry) >= 0) dnsCache[i].hostname[0] = 0;
      else { memcpy(addr, dnsCache[i].addr, fam == 1 ? 4 : 16); return true; }
    }
  }
  return false;
}

static void cachePut(const char* host, uint8_t fam, const uint8_t* addr) {
  if (!dnsCache) return;
  static int ci = 0;
  strncpy(dnsCache[ci].hostname, host, MAX_HOSTNAME - 1);
  dnsCache[ci].hostname[MAX_HOSTNAME - 1] = 0;
  dnsCache[ci].fam = fam;
  memcpy(dnsCache[ci].addr, addr, fam == 1 ? 4 : 16);
  dnsCache[ci].expiry = millis() + DEFAULT_TTL;
  ci = (ci + 1) % CACHE_SIZE;
}

static bool isLocalName(const char* host) {
  size_t n = strlen(host);
  if (strstr(host, "wpad") == host) return true;
  if (n >= 5 && !strcmp(host + n - 5, ".home")) return true;
  if (n >= 6 && !strcmp(host + n - 6, ".local")) return true;
  return false;
}

/* Query upstreams for 'qtype' (1=A, 28=AAAA). On DNS_RESOLVED copies the
 * record's RDATA (4/16 bytes) to rdata and sets *rlen. Walks primary->backup;
 * any definitive RCODE stops the walk. */
static DnsResult queryUpstream(const char* host, uint16_t qtype,
                               uint8_t* rdata, size_t rcap, size_t* rlen) {
  *rlen = 0;
  const uint8_t wantLen = (qtype == 28) ? 16 : 4;
  const char* servers[] = {ST_ns1, ST_ns2};
  for (int s = 0; s < 2; s++) {
    if (!servers[s][0]) continue;
    IPAddress srv;
    if (!srv.fromString(servers[s])) continue;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) break;
    struct timeval tv;
    tv.tv_sec = RESOLVE_TIMEOUT_MS / 1000;
    tv.tv_usec = (RESOLVE_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint16_t qid = (uint16_t)esp_random();
    uint8_t qbuf[280], rbuf[512];
    size_t pos = 12;
    qbuf[0] = qid >> 8; qbuf[1] = qid & 0xFF;
    qbuf[2] = 0x01; qbuf[3] = 0x00;              // RD=1
    qbuf[4] = 0; qbuf[5] = 1;                    // QDCOUNT=1
    memset(qbuf + 6, 0, 6);
    const char* hp = host;
    bool ok = true;
    while (*hp) {
      const char* dot = strchr(hp, '.');
      size_t lbl = dot ? (size_t)(dot - hp) : strlen(hp);
      if (!lbl || lbl > 63 || pos + lbl + 5 > sizeof(qbuf)) { ok = false; break; }
      qbuf[pos++] = (uint8_t)lbl;
      memcpy(qbuf + pos, hp, lbl);
      pos += lbl;
      hp += lbl + (dot ? 1 : 0);
    }
    if (!ok) { close(fd); return DNS_SERVFAIL; }
    qbuf[pos++] = 0;
    qbuf[pos++] = (uint8_t)(qtype >> 8); qbuf[pos++] = (uint8_t)qtype;
    qbuf[pos++] = 0; qbuf[pos++] = 1;            // QCLASS = IN

    DnsResult res = DNS_SERVFAIL;
    bool haveAnswer = false;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DNS_DEFAULT_PORT);
    dst.sin_addr.s_addr = srv;

        if (sendto(fd, qbuf, pos, 0, (struct sockaddr*)&dst, sizeof(dst)) == (ssize_t)pos) {
      struct sockaddr_in from;
      socklen_t fl = sizeof(from);
      ssize_t rxLen = recvfrom(fd, rbuf, sizeof(rbuf), 0, (struct sockaddr*)&from, &fl);
      /* Accept only: long enough, matching transaction ID, QR=1 (response) */
      if (rxLen >= 12 && ((uint16_t)rbuf[0] << 8 | rbuf[1]) == qid && (rbuf[2] & 0x80)) {
        uint8_t rcode = rbuf[3] & 0x0F;
        if (rcode == 3) {
          res = DNS_NXDOMAIN;
        } else if (rcode == 0) {
          size_t q = 12;
          bool cmp = false;
          while (q < (size_t)rxLen && rbuf[q] != 0) {          // skip question
            if ((rbuf[q] & 0xC0) == 0xC0) { q += 2; cmp = true; break; }
            q += 1 + rbuf[q];
          }
          if (!cmp) q++;                                       // root byte
          q += 4;                                              // QTYPE+QCLASS
          uint16_t ancount = ((uint16_t)rbuf[6] << 8) | rbuf[7];
          for (uint16_t a = 0; a < ancount && q + 10 <= (size_t)rxLen; a++) {
            if ((rbuf[q] & 0xC0) == 0xC0) q += 2;              // compressed owner
            else { while (q < (size_t)rxLen && rbuf[q]) q += 1 + rbuf[q]; q++; }
            if (q + 10 > (size_t)rxLen) break;
            uint16_t rt  = ((uint16_t)rbuf[q] << 8) | rbuf[q + 1];
            uint16_t rdl = ((uint16_t)rbuf[q + 8] << 8) | rbuf[q + 9];
            q += 10;                                           // TYPE+CLASS+TTL+RDLEN
            if (rt == qtype && rdl == wantLen && q + rdl <= (size_t)rxLen) {
              memcpy(rdata, rbuf + q, rdl);
              *rlen = rdl;                                     // OUT param - now unambiguous
              res = DNS_RESOLVED;
              break;
            }
            q += rdl;                          // CNAME/others: skip, keep scanning
          }
          if (res != DNS_RESOLVED) res = DNS_NXDOMAIN;   // NODATA ~= NXDOMAIN
        }
        haveAnswer = true;                     // definitive reply - stop failover
      }
    }
    close(fd);
    if (haveAnswer) return res;
  }
  return DNS_SERVFAIL;                         // no upstream answered in time
}

DnsResult resolveDomainStatus(const char* host, IPAddress& retIP) {
  retIP = IPAddress(0, 0, 0, 0);
  if (isLocalName(host)) { LOG_VRB("Ignore internal discovery: %s", host); return DNS_NXDOMAIN; }
  uint8_t a[4];
  if (cacheGet(host, 1, a)) {
    retIP = IPAddress(a[0], a[1], a[2], a[3]);
    LOG_VRB("Resolved %s (cache)", host);
    return DNS_RESOLVED;
  }
  uint8_t a4[4]; size_t rl = 0;
  DnsResult r = queryUpstream(host, 1, a4, sizeof(a4), &rl);
  if (r == DNS_RESOLVED && rl == 4) {
    retIP = IPAddress(a4[0], a4[1], a4[2], a4[3]);
    cachePut(host, 1, a4);
  }
  return r;
}

bool resolveAAAA(const char* host, uint8_t out[16]) {
  if (isLocalName(host)) return false;
  if (cacheGet(host, 28, out)) return true;
  size_t rl = 0;
  DnsResult r = queryUpstream(host, 28, out, 16, &rl);
  if (r == DNS_RESOLVED && rl == 16) { cachePut(host, 28, out); return true; }
  return false;
}

/* Legacy synchronous wrapper retained for checkDomain()'s "must resolve
 * before adding" validation. Returns 0.0.0.0 on anything but a clean hit. */
IPAddress resolveDomain(const char* host) {
  IPAddress ip;
  return resolveDomainStatus(host, ip) == DNS_RESOLVED ? ip : IPAddress(0, 0, 0, 0);
}
