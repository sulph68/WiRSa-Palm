// caldav_google.cpp - Google Calendar API and the OAuth handoff. Part of the CalDAV Relay mode; include/caldav_internal.h says how the files divide the work.
#include "caldav_internal.h"

namespace CalDavRelay {

// ---- Google Calendar ----
// No uICAL on this path: the Calendar API's events.list expands recurring events and drops cancelled instances server-side (singleEvents=true), so there is no client-side RRULE/EXDATE/RECURRENCE-ID handling.
#if WIRSA_FEATURE_CALDAV_GOOGLE

// RFC 3986 percent-encoding for a URL query or path component, with the alnum and "-_.~" allowlist of bitwarden_relay.cpp's urlEncode().
String cddvUrlEncode(const String &s) {
  String out;
  out.reserve(s.length() * 3);
  static const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t) s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xF]; }
  }
  return out;
}

void cddvFormatRfc3339(time_t t, char *out, size_t outCap) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, outCap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

// Days since the Unix epoch for a proleptic-Gregorian y/m/d (Howard Hinnant's days_from_civil), instead of mktime()/timegm(), so it does not depend on how this toolchain's libc handles UTC and timezone state.
int64_t cddvDaysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned) (y - era * 400);
  unsigned doy = (153 * (unsigned) (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned) d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t) doe - 719468;
}

// Parses an RFC 3339 timestamp as the Calendar API sends it: a timed "2026-09-25T09:00:00-07:00" or "...Z" dateTime, or a bare "2026-09-25" all-day date (midnight UTC, as the CalDAV/iCloud path treats an iCal
// all-day DATE).
// siscanf, not sscanf: newlib's integer-only scanner (already linked for tzset). These were the firmware's only sscanf calls; sscanf pulled in the floating-point scanner and strtod for ~16 KB of flash.
bool cddvParseRfc3339(const String &s, time_t &outEpoch) {
  int y, mo, d, h = 0, mi = 0, sec = 0;
  char offSign = 0; int offH = 0, offM = 0;
  int n = (int) s.length();
  if (n == 10) {
    if (siscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return false;
  } else if (n >= 19) {
    if (siscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return false;
    if (n > 19) {
      char tail = s[19];
      if ((tail == '+' || tail == '-') && n >= 25) {
        offSign = tail;
        siscanf(s.c_str() + 20, "%d:%d", &offH, &offM);
      }
      // 'Z' (or anything else) falls through as UTC (offSign stays 0): Google sends 'Z' or a numeric offset.
    }
  } else {
    return false;
  }
  int64_t secs = cddvDaysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
  // local = UTC + offset, so UTC = local - offset
  if (offSign == '+') secs -= (int64_t) offH * 3600 + offM * 60;
  else if (offSign == '-') secs += (int64_t) offH * 3600 + offM * 60;
  outEpoch = (time_t) secs;
  return true;
}

static String gCddvGoogleAccum;
static bool gCddvGoogleOverflow = false;
static const size_t kCddvGoogleAccumCap = 512 * 1024;

bool cddvOnGoogleChunk(const char *data, size_t len) {
  if (gCddvGoogleOverflow) return false;
  if (gCddvGoogleAccum.length() + len > kCddvGoogleAccumCap ||
      !gCddvGoogleAccum.concat((const uint8_t *) data, (unsigned int) len)) {
    gCddvGoogleOverflow = true;
    LOGW("CalDAV: [google] response body exceeded accumulator cap or ran out of memory");
    return false;
  }
  return true;
}

// Exchanges the long-lived refresh token (kept Sec35-encrypted on the Palm and sent as `secret` in every SYNC_REQUEST) for a short-lived access token. Done fresh every sync, nothing cached on WiRSa,
// as every credential this relay handles.
// clientSecret is required although Google's docs call it optional for "Desktop app" clients: against a real client the token endpoint rejects this grant and the authorization_code grant with "client_secret is missing" without it.
bool cddvGoogleRefreshAccessToken(const String &refreshToken, const String &clientId,
                                          const String &clientSecret,
                                          String &outAccessToken, String &errOut) {
  String form = "client_id=" + cddvUrlEncode(clientId) +
                "&client_secret=" + cddvUrlEncode(clientSecret) +
                "&refresh_token=" + cddvUrlEncode(refreshToken) +
                "&grant_type=refresh_token";
  gCddvGoogleAccum = "";
  gCddvGoogleOverflow = false;
  int code;
  bool ok = cddvHttpRequest("https://oauth2.googleapis.com/token", "POST", form,
                             nullptr, nullptr, "application/x-www-form-urlencoded",
                             nullptr, nullptr, false, cddvOnGoogleChunk, code, errOut);
  if (!ok) return false;
  if (code != 200) {
    // A revoked or expired refresh token (invalid_grant) needs a new sign-in: say so, not a bare HTTP code.
    JsonDocument errDoc;
    if (!deserializeJson(errDoc, gCddvGoogleAccum)) {
      const char *err = errDoc["error"] | "";
      if (err[0]) {
        const char *desc = errDoc["error_description"] | "";
        errOut = String("Google token refresh failed: ") + err + (desc[0] ? (String(" - ") + desc) : String());
        if (strcmp(err, "invalid_grant") == 0)
          errOut += " - sign in with Google again for this calendar";
        return false;
      }
    }
    errOut = String("Google token endpoint returned HTTP ") + code;
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, gCddvGoogleAccum)) {
    errOut = "Malformed JSON from Google token endpoint";
    return false;
  }
  outAccessToken = String((const char *) (doc["access_token"] | (const char *) nullptr));
  if (outAccessToken.length() == 0) {
    errOut = "Google token endpoint response had no access_token";
    return false;
  }
  return true;
}

bool cddvFetchGoogleCalendar(const String &refreshToken, const String &clientId, const String &clientSecret,
                                     const String &calendarId, time_t rangeStart, time_t rangeEnd,
                                     CalDavEventSink sink, String &errOut) {
  String accessToken;
  if (!cddvGoogleRefreshAccessToken(refreshToken, clientId, clientSecret, accessToken, errOut)) return false;
  String authHeader = "Bearer " + accessToken;

  char timeMinBuf[24], timeMaxBuf[24];
  cddvFormatRfc3339(rangeStart, timeMinBuf, sizeof(timeMinBuf));
  cddvFormatRfc3339(rangeEnd, timeMaxBuf, sizeof(timeMaxBuf));

  // Bounded to a sane worst case (250 per page by default x 20 pages = 5000 events), not looping on nextPageToken forever.
  static const int kCddvGoogleMaxPages = 20;
  String pageToken;

  for (int page = 0; page < kCddvGoogleMaxPages; page++) {
    String url = "https://www.googleapis.com/calendar/v3/calendars/" + cddvUrlEncode(calendarId) +
                 "/events?singleEvents=true&orderBy=startTime&maxResults=250" +
                 "&timeMin=" + cddvUrlEncode(String(timeMinBuf)) +
                 "&timeMax=" + cddvUrlEncode(String(timeMaxBuf));
    if (pageToken.length()) url += "&pageToken=" + cddvUrlEncode(pageToken);

    LOGV("CalDAV: [google] fetch page %d: %s\r\n", page, maskUrlForLog(url).c_str());
    gCddvGoogleAccum = "";
    gCddvGoogleOverflow = false;
    int code;
    bool ok = cddvHttpRequest(url, "GET", String(), nullptr, nullptr, nullptr,
                               "Authorization", authHeader.c_str(), false,
                               cddvOnGoogleChunk, code, errOut);
    if (!ok) return false;
    if (code != 200) {
      // The Calendar API (and most Google REST APIs) nest the real reason in error.message, a different shape from the token endpoint's flat error/error_description, so it needs its own parse. Common
      // causes: the Calendar API not enabled for the Cloud project, or the calendar ID not shared with or visible to this account.
      JsonDocument errDoc;
      if (!deserializeJson(errDoc, gCddvGoogleAccum)) {
        const char *msg = errDoc["error"]["message"] | (const char *) nullptr;
        if (msg) {
          errOut = String("Google Calendar API error: ") + msg;
          return false;
        }
      }
      errOut = String("Google Calendar API returned HTTP ") + code;
      return false;
    }
    if (gCddvGoogleOverflow) {
      errOut = "Google Calendar response too large or ran out of memory while reading it";
      return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, gCddvGoogleAccum)) {
      errOut = "Malformed JSON from Google Calendar API";
      return false;
    }

    for (JsonObject item : doc["items"].as<JsonArray>()) {
      const char *status = item["status"] | "confirmed";
      if (strcmp(status, "cancelled") == 0) continue;

      // No icalUnescapeText here (unlike CalDAV/iCloud): these are plain JSON string fields, not iCal TEXT values, so there is no backslash escaping to undo.
      String uid = String((const char *) (item["id"] | (const char *) nullptr));
      String title = String((const char *) (item["summary"] | (const char *) ""));
      String location = String((const char *) (item["location"] | (const char *) ""));
      String description = String((const char *) (item["description"] | (const char *) ""));
      const char *recurringEventId = item["recurringEventId"] | (const char *) nullptr;
      bool isRepeating = (recurringEventId != nullptr);

      const char *startDate = item["start"]["date"] | (const char *) nullptr;
      const char *startDateTime = item["start"]["dateTime"] | (const char *) nullptr;
      const char *endDate = item["end"]["date"] | (const char *) nullptr;
      const char *endDateTime = item["end"]["dateTime"] | (const char *) nullptr;

      bool allDay;
      time_t startEpoch, endEpoch;
      if (startDate && endDate) {
        allDay = true;
        if (!cddvParseRfc3339(String(startDate), startEpoch)) continue;
        if (!cddvParseRfc3339(String(endDate), endEpoch)) continue;
      } else if (startDateTime && endDateTime) {
        allDay = false;
        if (!cddvParseRfc3339(String(startDateTime), startEpoch)) continue;
        if (!cddvParseRfc3339(String(endDateTime), endEpoch)) continue;
      } else {
        continue;  // malformed item - skip rather than abort the whole sync
      }

      uint32_t utcStartPalm = (uint32_t) startEpoch + UNIX_TO_PALM_EPOCH;
      uint32_t utcEndPalm = (uint32_t) endEpoch + UNIX_TO_PALM_EPOCH;

      if (!sink(uid, title, location, utcStartPalm, utcEndPalm, allDay, description, isRepeating)) {
        errOut = "Lost contact with the Palm mid-sync";
        return false;
      }
    }

    const char *nextPageToken = doc["nextPageToken"] | (const char *) nullptr;
    if (!nextPageToken) break;
    pageToken = String(nextPageToken);
  }
  return true;
}
#endif  // WIRSA_FEATURE_CALDAV_GOOGLE

// ---- Google OAuth handoff (CDDV_OAUTH_START/INFO/TOKEN) ----
// google_auth.py, run once by the user on their own computer, does the Google OAuth dance and POSTs the refresh token to a transient, PIN-secured local HTTP listener here, which relays it to the Palm over
// serial and tears itself down. WiRSa never keeps the token: the Palm stores it (Sec35-encrypted) and sends it as `secret` in every SYNC_REQUEST, as a CalDAV/iCloud password.

// cddvOAuthStop()/cddvOAuthPump() are called unconditionally from the shared mode-exit and mode-loop code whichever CalDAV types are built, so a no-op stub pair covers WIRSA_FEATURE_CALDAV_GOOGLE=0
// without guarding those call sites. Everything else in this section (including the CDDV_OAUTH_START frame handler) is Google-only.
#if WIRSA_FEATURE_CALDAV_GOOGLE
// Generous enough for an unfamiliar-app consent-screen click-through in a browser, not only the network round trip.

WebServer gCddvOAuthServer(CDDV_OAUTH_PORT);
bool gCddvOAuthActive = false;
char gCddvOAuthPin[7];  // 6 digits + NUL
String gCddvOAuthRefreshToken;
bool gCddvOAuthTokenReady = false;
unsigned long gCddvOAuthDeadline = 0;

void cddvOAuthStop() {
  if (gCddvOAuthActive) {
    gCddvOAuthServer.stop();
    gCddvOAuthActive = false;
  }
  gCddvOAuthPin[0] = '\0';
  gCddvOAuthRefreshToken = "";
  gCddvOAuthTokenReady = false;
  gCddvOAuthDeadline = 0;
}

// POST body: {"pin": "123456", "refresh_token": "..."} (see apps/caldav_sync/tools/google_auth.py's POST).
void cddvOAuthHandlePost() {
  String body = gCddvOAuthServer.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    gCddvOAuthServer.send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  const char *pin = doc["pin"] | "";
  const char *refreshToken = doc["refresh_token"] | "";
  if (strcmp(pin, gCddvOAuthPin) != 0) {
    LOGW("CalDAV: OAuth callback PIN mismatch - ignored");
    gCddvOAuthServer.send(403, "application/json", "{\"error\":\"pin mismatch\"}");
    return;
  }
  if (!refreshToken[0]) {
    gCddvOAuthServer.send(400, "application/json", "{\"error\":\"missing refresh_token\"}");
    return;
  }
  gCddvOAuthRefreshToken = refreshToken;
  gCddvOAuthTokenReady = true;
  gCddvOAuthServer.send(200, "application/json", "{\"ok\":true}");
  LOGI("CalDAV: OAuth refresh token received from google_auth.py");
}

// Called every caldavRelayLoop tick while a session is active: pumps the listener, relays the token to the Palm when it arrives, and tears the session down on delivery or timeout so the port is not left open.
void cddvOAuthPump() {
  if (!gCddvOAuthActive) return;
  gCddvOAuthServer.handleClient();

  if (gCddvOAuthTokenReady) {
    RelayProto::Writer w(gEventBody, sizeof(gEventBody));
    w.str(gCddvOAuthRefreshToken);
    RelayProto::writeFrame(PhysicalSerial, CDDV_OAUTH_TOKEN, gEventBody, w.len);
    LOGI("CalDAV: OAuth token relayed to Palm");
    cddvOAuthStop();
    cddvDrawStatus("Waiting...");
    return;
  }
  if (millis() > gCddvOAuthDeadline) {
    LOGW("CalDAV: OAuth session timed out waiting for google_auth.py");
    cddvOAuthStop();
    cddvDrawStatus("Waiting...");
  }
}
#else
void cddvOAuthStop() {}
void cddvOAuthPump() {}
#endif  // WIRSA_FEATURE_CALDAV_GOOGLE

}  // namespace CalDavRelay
