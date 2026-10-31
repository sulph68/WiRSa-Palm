// caldav_diag.cpp - raw TLS diagnostic. Part of the CalDAV Relay mode; include/caldav_internal.h says how the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

// ---- Diagnostic path: BitWarden's raw-socket pattern (WiFiClientSecure directly, not HTTPClient), to tell whether a connectivity problem is in HTTPClient's wrapper or in the WiFiClientSecure/mbedTLS
// handshake. It never touches the real cddvHttpRequestOnce/cddvHttpRequest path (gated by CDDV_DIAGNOSTIC_RAW_TLS at its call site) and emits no events. Kept for reuse. ----

bool cddvRawTlsDiagnostic(const String &url, const char *user, const char *pass,
                                  const String &reportBody, String &resultOut) {
  String host = cddvUrlHost(url);
  String path = cddvUrlPath(url);
  unsigned long t0 = millis();

  // network.cpp's maintainWiFi() treats all of MODE_CALDAV_RELAY as transferActive, so its background reconnect is off while this mode is up. cddvHttpRequest's retry loop compensates with
  // WiFi.reconnect() on failure; mirrored here.
  if (WiFi.status() != WL_CONNECTED) {
    logPrintf(LOG_VERBOSE, "CalDAV: [raw-diag] WiFi is down - reconnecting before attempt...\r\n");
    WiFi.reconnect();
    unsigned long waitStart = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - waitStart) < 8000) {
      delay(200);
      yield();
    }
    logPrintf(LOG_VERBOSE, "CalDAV: [raw-diag] WiFi %s after %lums\r\n",
            (WiFi.status() == WL_CONNECTED) ? "reconnected" : "still down",
            (unsigned long) (millis() - waitStart));
    if (WiFi.status() != WL_CONNECTED) {
      resultOut = "raw-diag: WiFi was down and did not reconnect within 8s";
      return false;
    }
  }

  IPAddress ip;
  if (!WiFi.hostByName(host.c_str(), ip)) {
    resultOut = "raw-diag: DNS lookup failed for " + host;
    return false;
  }
  LOGV("CalDAV: [raw-diag] resolved %s -> %s, heap free=%u rssi=%d\r\n",
                               host.c_str(), ip.toString().c_str(),
                               (unsigned) ESP.getFreeHeap(), (int) WiFi.RSSI());

  // Both client objects are declared here so the one used lives to the end of the function; `client` serves everything scheme-agnostic below (connect, print, read, stop are common Client/Stream),
  // and a plain-http target must never attempt TLS on port 443.
  bool isHttps = url.startsWith("https://");
  uint16_t port = cddvUrlPort(url, isHttps);
  WiFiClientSecure tls;
  WiFiClient plain;
  Client *client;
  if (isHttps) {
    tls.setInsecure();
    tls.setTimeout(60);  // seconds - matches cddvHttpRequestOnce's own budget
    tls.setHandshakeTimeout(60);  // seconds; see cddvHttpRequestOnce for this separate field
    // WiFiClientSecure and WiFiClient each declare a private `int _timeout` that shadows the inherited protected Stream::_timeout (hardcoded 1000 ms, what readStringUntil()/timedRead() read). Set
    // Stream::_timeout through this upcast, as cddvHttpRequestOnce does.
    ((Stream &) tls).setTimeout(60000);  // milliseconds - Stream::setTimeout's own unit, not seconds
    client = &tls;
  } else {
    ((Stream &) plain).setTimeout(60000);  // the shadowing applies to a plain WiFiClient too: set explicitly
    client = &plain;
  }

  unsigned long tConnectStart = millis();
  bool connected = client->connect(ip, port);
  unsigned long connectMs = millis() - tConnectStart;
  if (!connected) {
    resultOut = "raw-diag: connect() failed after " + String(connectMs) +
                "ms (" + (isHttps ? "TLS" : "plain") + ", port " + String(port) +
                ", WiFi.status()=" + String((int) WiFi.status()) + ")";
    LOGV("CalDAV: [raw-diag] %s\r\n", resultOut.c_str());
    return false;
  }
  LOGV("CalDAV: [raw-diag] %s connected (port %u) in %lums, heap free=%u rssi=%d\r\n",
                               isHttps ? "TLS" : "plain", (unsigned) port,
                               connectMs, (unsigned) ESP.getFreeHeap(), (int) WiFi.RSSI());

  // Basic Auth header via mbedtls_base64_encode directly (as bitwarden_relay.cpp's bwBase64Encode), not HTTPClient's encoder: the point here is no HTTPClient at all.
  String credPlain = String(user) + ":" + String(pass);
  size_t b64Needed = 0;
  mbedtls_base64_encode(nullptr, 0, &b64Needed, (const unsigned char *) credPlain.c_str(), credPlain.length());
  uint8_t *b64Buf = (uint8_t *) malloc(b64Needed + 1);
  String credB64;
  if (b64Buf) {
    size_t b64Actual = 0;
    mbedtls_base64_encode(b64Buf, b64Needed, &b64Actual,
                           (const unsigned char *) credPlain.c_str(), credPlain.length());
    b64Buf[b64Actual] = 0;
    credB64 = String((char *) b64Buf);
    free(b64Buf);
  }

  // The header set of cddvHttpRequestOnce's browserHeaders=false branch (EDS/Evolution-matching), sent as one write, like bitwarden_relay.cpp's request building.
  String req = "REPORT " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: Evolution/3.52.3\r\n";
  req += "Connection: close\r\n";
  req += "Cache-Control: no-cache\r\n";
  req += "Pragma: no-cache\r\n";
  req += "Content-Type: application/xml; charset=\"utf-8\"\r\n";
  req += "Depth: 1\r\n";
  req += "Authorization: Basic " + credB64 + "\r\n";
  req += "Content-Length: " + String(reportBody.length()) + "\r\n";
  req += "\r\n";
  req += reportBody;

  unsigned long tSendStart = millis();
  size_t written = client->print(req);
  unsigned long sendMs = millis() - tSendStart;
  LOGV("CalDAV: [raw-diag] wrote %u/%u request bytes in %lums (WiFi.status()=%d)\r\n",
                               (unsigned) written, (unsigned) req.length(), sendMs, (int) WiFi.status());
  if (WiFi.status() != WL_CONNECTED) {
    resultOut = "raw-diag: WiFi dropped during/after request write (wrote " + String((unsigned) written) +
                "/" + String((unsigned) req.length()) + " bytes, sent in " + String(sendMs) + "ms)";
    client->stop();
    return false;
  }

  String statusLine = client->readStringUntil('\n');
  if (statusLine.length() == 0) {
    resultOut = "raw-diag: no response after " + String(millis() - t0) +
                "ms total (WiFi.status()=" + String((int) WiFi.status()) + ")";
    client->stop();
    return false;
  }

  // Drain the headers, then enough of the body to confirm real CalDAV XML came back: a connectivity and stability probe, not a sync, so no full buffering or parsing.
  bool inHeaders = true;
  unsigned bodyBytes = 0;
  bool sawMultistatus = false;
  unsigned long tDrainStart = millis();
  while (millis() - tDrainStart < 5000) {
    if (inHeaders) {
      String line = client->readStringUntil('\n');
      if (line.length() == 0 && !client->connected() && !client->available()) break;
      if (line.length() <= 1) inHeaders = false;
      continue;
    }
    int avail = client->available();
    if (avail <= 0) {
      if (!client->connected()) break;
      yield();
      continue;
    }
    uint8_t buf[256];
    int n = client->read(buf, sizeof(buf));
    if (n <= 0) break;
    bodyBytes += (unsigned) n;
    if (!sawMultistatus && String((char *) buf, (unsigned) n).indexOf("multistatus") >= 0) sawMultistatus = true;
    yield();
  }
  client->stop();

  statusLine.trim();
  resultOut = "raw-diag OK: " + statusLine + ", " + String(bodyBytes) +
              " body bytes, multistatus=" + (sawMultistatus ? "yes" : "no") +
              ", total " + String(millis() - t0) + "ms";
  LOGV("CalDAV: [raw-diag] %s\r\n", resultOut.c_str());
  return true;
}

}  // namespace CalDavRelay
