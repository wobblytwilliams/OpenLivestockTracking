#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SdFat.h>

#define GPS_SERIAL Serial1

static const uint8_t kSdCsPin = 10;
static const uint32_t kSdSpiHz = 12000000;
static const char kSbasIndexFilename[] = "SBIDX.TXT";
static const char kSbasCurrentFilename[] = "SBASCUR.TXT";
static const char kSbasLastFilename[] = "SBASLAST.TXT";
static const char kSbasSeenFilename[] = "SBASSEEN.TXT";

static const uint8_t kSouthpanSbasMode = 0x03;      // enabled + allow test mode use
static const uint8_t kSouthpanSbasUsage = 0x02;     // corrections only
static const uint8_t kSouthpanSbasChannels = 3;
static const uint8_t kSouthpanSbasScanmode2 = 0x00;
static const uint32_t kSouthpanSbasScanmode1 = 0x00000004UL; // PRN 122

static uint8_t ubx_ck_a = 0;
static uint8_t ubx_ck_b = 0;
static uint32_t ubx_count = 0;
static uint32_t nmea_count = 0;
static const bool kPrintNmea = false;

static SdFat sd;
static SdFile fSbas;
static bool sd_ok = false;
static uint32_t currentSessionId = 0;
static char currentLogFilename[13] = "SB000000.CSV";

struct SbasStats {
  uint32_t navPolls = 0;
  uint32_t activePolls = 0;
  uint32_t pollsWithTrackedEntries = 0;
  uint32_t pollsWithGeoUsed = 0;
  uint32_t pollsWithCorrections = 0;
  uint32_t pollsWithIntegrity = 0;
  uint32_t firstActiveMs = 0;
  uint32_t lastActiveMs = 0;
  uint32_t firstActiveItow = 0;
  uint32_t lastActiveItow = 0;
  uint8_t maxTrackedEntries = 0;
  uint8_t lastMode = 0;
  int8_t lastSystem = -1;
  uint8_t lastGeoUsed = 0;
  uint8_t lastTrackedEntries = 0;
  uint8_t lastServiceFlags = 0;
  uint8_t lastIntegrityUsed = 0;
  bool everActive = false;
  bool everTrackedEntries = false;
  bool everGeoUsed = false;
  bool everCorrections = false;
  bool everIntegrity = false;
};

static SbasStats sbasStats;

struct SbasConfig {
  bool valid = false;
  bool enabled = false;
  bool testMode = false;
  bool ranging = false;
  bool diffCorrections = false;
  bool integrity = false;
  uint8_t maxSBAS = 0;
  uint8_t scanmode2 = 0;
  uint32_t scanmode1 = 0;
};

static SbasConfig sbasCfg;

struct GpsSnapshot {
  bool rmc_valid = false;
  bool have_position = false;
  bool have_fix_quality = false;
  bool have_sats = false;
  bool have_hdop = false;
  uint8_t fix_quality = 0;
  uint8_t sats = 0;
  float hdop = 99.9f;
  float lat = 0.0f;
  float lon = 0.0f;
};

static GpsSnapshot gpsSnap;

struct UtcClock {
  bool valid = false;
  uint32_t sync_local_ms = 0;
  uint64_t sync_unix_ms = 0;
};

static UtcClock utcClock;

struct PersistedRunSummary {
  bool valid = false;
  uint32_t sessionId = 0;
  uint8_t everActive = 0;
  uint8_t everEntries = 0;
  uint8_t everGeo = 0;
  uint8_t everCorr = 0;
  uint8_t everInt = 0;
  uint8_t maxEntries = 0;
  uint32_t activePolls = 0;
  uint32_t navPolls = 0;
  uint32_t lastUptimeMs = 0;
};

static PersistedRunSummary lastRunSummary;

struct StickySeenState {
  bool valid = false;
  uint32_t sessionId = 0;
  uint32_t uptimeMs = 0;
};

static StickySeenState stickySeenState;

static void ubx_ck_reset() {
  ubx_ck_a = 0;
  ubx_ck_b = 0;
}

static void ubx_ck_add(uint8_t b) {
  ubx_ck_a = (uint8_t)(ubx_ck_a + b);
  ubx_ck_b = (uint8_t)(ubx_ck_b + ubx_ck_a);
}

static void send_ubx(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  ubx_ck_reset();

  GPS_SERIAL.write((uint8_t)0xB5);
  GPS_SERIAL.write((uint8_t)0x62);
  GPS_SERIAL.write(cls); ubx_ck_add(cls);
  GPS_SERIAL.write(id);  ubx_ck_add(id);

  uint8_t len_l = (uint8_t)(len & 0xFF);
  uint8_t len_h = (uint8_t)(len >> 8);
  GPS_SERIAL.write(len_l); ubx_ck_add(len_l);
  GPS_SERIAL.write(len_h); ubx_ck_add(len_h);

  for (uint16_t i = 0; i < len; i++) {
    GPS_SERIAL.write(payload[i]);
    ubx_ck_add(payload[i]);
  }

  GPS_SERIAL.write(ubx_ck_a);
  GPS_SERIAL.write(ubx_ck_b);
}

static void send_cfg_rxm_continuous() {
  const uint8_t payload[2] = {0x00, 0x00};
  send_ubx(0x06, 0x11, payload, sizeof(payload));
}

static void send_cfg_sbas(uint8_t mode, uint8_t usage, uint8_t maxSBAS, uint8_t scanmode2, uint32_t scanmode1) {
  uint8_t payload[8];
  payload[0] = mode;
  payload[1] = usage;
  payload[2] = maxSBAS;
  payload[3] = scanmode2;
  payload[4] = (uint8_t)(scanmode1 & 0xFF);
  payload[5] = (uint8_t)((scanmode1 >> 8) & 0xFF);
  payload[6] = (uint8_t)((scanmode1 >> 16) & 0xFF);
  payload[7] = (uint8_t)((scanmode1 >> 24) & 0xFF);
  send_ubx(0x06, 0x16, payload, sizeof(payload));
}

static void poll_cfg_sbas() {
  send_ubx(0x06, 0x16, nullptr, 0);
}

static void poll_nav_sbas() {
  send_ubx(0x01, 0x32, nullptr, 0);
}

static bool southpan_cfg_matches_target() {
  return sbasCfg.valid &&
         sbasCfg.enabled &&
         sbasCfg.testMode &&
         !sbasCfg.ranging &&
         sbasCfg.diffCorrections &&
         !sbasCfg.integrity &&
         sbasCfg.maxSBAS == kSouthpanSbasChannels &&
         sbasCfg.scanmode2 == kSouthpanSbasScanmode2 &&
         sbasCfg.scanmode1 == kSouthpanSbasScanmode1;
}

static void apply_southpan_sbas_config() {
  Serial.println(F("Applying SouthPAN SBAS config"));
  send_cfg_sbas(kSouthpanSbasMode, kSouthpanSbasUsage, kSouthpanSbasChannels,
                kSouthpanSbasScanmode2, kSouthpanSbasScanmode1);
}

static void print_hex_byte(uint8_t b) {
  if (b < 16) Serial.print('0');
  Serial.print(b, HEX);
}

static void print_hex_block(const uint8_t* data, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    if (i) Serial.print(' ');
    print_hex_byte(data[i]);
  }
}

static const __FlashStringHelper* sbas_mode_name(uint8_t mode) {
  switch (mode) {
    case 0: return F("disabled");
    case 1: return F("enabled integrity");
    case 3: return F("enabled test mode");
    default: return F("other");
  }
}

static const __FlashStringHelper* sbas_system_name(int8_t sys) {
  switch (sys) {
    case -1: return F("Unknown");
    case 0:  return F("WAAS");
    case 1:  return F("EGNOS");
    case 2:  return F("MSAS");
    case 3:  return F("GAGAN");
    case 16: return F("GPS");
    default: return F("Other");
  }
}

static bool service_has_corrections(uint8_t flags) {
  return (flags & 0x02) != 0;
}

static bool service_has_integrity(uint8_t flags) {
  return (flags & 0x04) != 0;
}

static bool parse_2_digits(const char* s, uint8_t& out) {
  if (!s || !isDigit(s[0]) || !isDigit(s[1])) return false;
  out = (uint8_t)((s[0] - '0') * 10 + (s[1] - '0'));
  return true;
}

static bool is_leap_year(uint16_t year) {
  return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static bool gps_datetime_to_unix_ms(const char* utc_time, const char* utc_date, uint64_t& out_unix_ms) {
  if (!utc_time || !utc_date) return false;

  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t day = 0;
  uint8_t month = 0;
  uint8_t yy = 0;
  if (!parse_2_digits(utc_time + 0, hour)) return false;
  if (!parse_2_digits(utc_time + 2, minute)) return false;
  if (!parse_2_digits(utc_time + 4, second)) return false;
  if (!parse_2_digits(utc_date + 0, day)) return false;
  if (!parse_2_digits(utc_date + 2, month)) return false;
  if (!parse_2_digits(utc_date + 4, yy)) return false;

  uint16_t millis_part = 0;
  if (utc_time[6] == '.') {
    uint16_t scale = 100;
    for (const char* p = utc_time + 7; *p && scale > 0 && isDigit(*p); p++) {
      millis_part += (uint16_t)(*p - '0') * scale;
      scale /= 10;
    }
  }

  uint16_t year = (yy >= 80) ? (uint16_t)(1900 + yy) : (uint16_t)(2000 + yy);
  static const uint8_t days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (hour > 23 || minute > 59 || second > 59) return false;
  if (month < 1 || month > 12) return false;

  uint8_t max_day = days_in_month[month - 1];
  if (month == 2 && is_leap_year(year)) max_day = 29;
  if (day < 1 || day > max_day) return false;

  uint32_t days = 0;
  for (uint16_t y = 1970; y < year; y++) days += is_leap_year(y) ? 366UL : 365UL;
  for (uint8_t m = 1; m < month; m++) {
    days += days_in_month[m - 1];
    if (m == 2 && is_leap_year(year)) days++;
  }
  days += (uint32_t)(day - 1);

  uint64_t seconds = (uint64_t)days * 86400ULL + (uint32_t)hour * 3600UL + (uint32_t)minute * 60UL + second;
  out_unix_ms = seconds * 1000ULL + millis_part;
  return true;
}

static void utc_sync(uint32_t local_ms, uint64_t unix_ms) {
  utcClock.valid = true;
  utcClock.sync_local_ms = local_ms;
  utcClock.sync_unix_ms = unix_ms;
}

static uint64_t utc_ms_from_millis(uint32_t local_ms) {
  if (!utcClock.valid) return 0;

  int32_t delta = (int32_t)(local_ms - utcClock.sync_local_ms);
  if (delta < 0) {
    uint32_t back = (uint32_t)(-delta);
    return (utcClock.sync_unix_ms > back) ? (utcClock.sync_unix_ms - back) : 0;
  }
  return utcClock.sync_unix_ms + (uint32_t)delta;
}

static int nmea_hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool nmea_checksum_ok(const char* sentence) {
  if (!sentence || sentence[0] != '$') return false;
  const char* star = strchr(sentence, '*');
  if (!star || !star[1] || !star[2]) return false;

  int hi = nmea_hex_value(star[1]);
  int lo = nmea_hex_value(star[2]);
  if (hi < 0 || lo < 0) return false;

  uint8_t calc = 0;
  for (const char* p = sentence + 1; p < star; p++) calc ^= (uint8_t)(*p);
  return calc == (uint8_t)((hi << 4) | lo);
}

static bool nmea_degmin_to_dec(const char* s, bool is_lon, float& out) {
  if (!s || !*s) return false;
  const char* dot = strchr(s, '.');
  int len = dot ? (int)(dot - s) : (int)strlen(s);
  int deg_digits = is_lon ? 3 : 2;
  if (len < deg_digits + 2) return false;

  char deg_buf[4] = {0};
  for (int i = 0; i < deg_digits; i++) deg_buf[i] = s[i];
  int deg = atoi(deg_buf);
  float minutes = atof(s + deg_digits);
  out = (float)deg + (minutes / 60.0f);
  return true;
}

static int split_csv(char* line, char* fields[], int max_fields) {
  int n = 0;
  fields[n++] = line;
  for (char* p = line; *p && n < max_fields; p++) {
    if (*p == ',') {
      *p = '\0';
      fields[n++] = p + 1;
    }
  }
  return n;
}

static void print_service_flags(uint8_t flags) {
  bool first = true;
  if (flags & 0x01) {
    Serial.print(F("ranging"));
    first = false;
  }
  if (flags & 0x02) {
    if (!first) Serial.print(F(", "));
    Serial.print(F("corrections"));
    first = false;
  }
  if (flags & 0x04) {
    if (!first) Serial.print(F(", "));
    Serial.print(F("integrity"));
    first = false;
  }
  if (flags & 0x08) {
    if (!first) Serial.print(F(", "));
    Serial.print(F("test"));
    first = false;
  }
  if (flags & 0x10) {
    if (!first) Serial.print(F(", "));
    Serial.print(F("bad"));
    first = false;
  }
  if (first) Serial.print(F("none"));
}

static size_t sd_print_u64(SdFile& file, uint64_t v) {
  char buf[21];
  char* p = buf + sizeof(buf);
  *--p = '\0';
  do {
    *--p = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  return file.print(p);
}

static void make_session_log_filename(uint32_t sessionId, char out[13]) {
  snprintf(out, 13, "SB%06lu.CSV", (unsigned long)sessionId);
}

static bool read_u32_text_file(const char* path, uint32_t& out) {
  SdFile file;
  if (!file.open(path, O_RDONLY)) return false;

  char buf[24];
  uint16_t idx = 0;
  while (idx < sizeof(buf) - 1) {
    int c = file.read();
    if (c < 0 || c == '\n') break;
    if (c == '\r') continue;
    buf[idx++] = (char)c;
  }
  file.close();

  if (idx == 0) return false;
  buf[idx] = '\0';
  out = (uint32_t)strtoul(buf, nullptr, 10);
  return true;
}

static bool sticky_seen_read(StickySeenState& out) {
  SdFile file;
  if (!file.open(kSbasSeenFilename, O_RDONLY)) return false;

  char line[48];
  uint16_t idx = 0;
  while (idx < sizeof(line) - 1) {
    int c = file.read();
    if (c < 0 || c == '\n') break;
    if (c == '\r') continue;
    line[idx++] = (char)c;
  }
  file.close();

  if (idx == 0) return false;
  line[idx] = '\0';

  char* fields[2] = {0};
  int nf = split_csv(line, fields, 2);
  if (nf < 2) return false;

  out.valid = true;
  out.sessionId = (uint32_t)strtoul(fields[0], nullptr, 10);
  out.uptimeMs = (uint32_t)strtoul(fields[1], nullptr, 10);
  return true;
}

static bool sticky_seen_write(const StickySeenState& state) {
  SdFile file;
  if (!file.open(kSbasSeenFilename, O_RDWR | O_CREAT | O_TRUNC)) return false;

  uint32_t pos = file.curPosition();
  file.clearWriteError();
  file.print(state.sessionId);
  file.print(",");
  file.println(state.uptimeMs);
  bool ok = sd_commit_line(file, pos);
  file.close();
  return ok;
}

static bool persisted_summary_seen(const PersistedRunSummary& s) {
  return s.everActive || s.everEntries || s.everGeo || s.everCorr || s.everInt;
}

static bool persisted_summary_has_data(const PersistedRunSummary& s) {
  return s.navPolls > 0 || s.activePolls > 0 || s.maxEntries > 0 || persisted_summary_seen(s);
}

static PersistedRunSummary make_current_run_summary() {
  PersistedRunSummary s;
  s.valid = true;
  s.sessionId = currentSessionId;
  s.everActive = sbasStats.everActive ? 1 : 0;
  s.everEntries = sbasStats.everTrackedEntries ? 1 : 0;
  s.everGeo = sbasStats.everGeoUsed ? 1 : 0;
  s.everCorr = sbasStats.everCorrections ? 1 : 0;
  s.everInt = sbasStats.everIntegrity ? 1 : 0;
  s.maxEntries = sbasStats.maxTrackedEntries;
  s.activePolls = sbasStats.activePolls;
  s.navPolls = sbasStats.navPolls;
  s.lastUptimeMs = millis();
  return s;
}

static bool persist_read_summary(const char* path, PersistedRunSummary& out) {
  SdFile file;
  if (!file.open(path, O_RDONLY)) return false;

  char line[96];
  uint16_t idx = 0;
  while (idx < sizeof(line) - 1) {
    int c = file.read();
    if (c < 0 || c == '\n') break;
    if (c == '\r') continue;
    line[idx++] = (char)c;
  }
  file.close();

  if (idx == 0) return false;
  line[idx] = '\0';

  char* fields[10] = {0};
  int nf = split_csv(line, fields, 10);
  if (nf < 8) return false;

  out.valid = true;
  if (nf >= 10) {
    out.sessionId = (uint32_t)strtoul(fields[0], nullptr, 10);
    out.everActive = (uint8_t)atoi(fields[1]);
    out.everEntries = (uint8_t)atoi(fields[2]);
    out.everGeo = (uint8_t)atoi(fields[3]);
    out.everCorr = (uint8_t)atoi(fields[4]);
    out.everInt = (uint8_t)atoi(fields[5]);
    out.maxEntries = (uint8_t)atoi(fields[6]);
    out.activePolls = (uint32_t)strtoul(fields[7], nullptr, 10);
    out.navPolls = (uint32_t)strtoul(fields[8], nullptr, 10);
    out.lastUptimeMs = (uint32_t)strtoul(fields[9], nullptr, 10);
  } else {
    out.everActive = (uint8_t)atoi(fields[0]);
    out.everEntries = (uint8_t)atoi(fields[1]);
    out.everGeo = (uint8_t)atoi(fields[2]);
    out.everCorr = (uint8_t)atoi(fields[3]);
    out.everInt = (uint8_t)atoi(fields[4]);
    out.maxEntries = (uint8_t)atoi(fields[5]);
    out.activePolls = (uint32_t)strtoul(fields[6], nullptr, 10);
    out.navPolls = (uint32_t)strtoul(fields[7], nullptr, 10);
    out.lastUptimeMs = 0;
  }
  return true;
}

static bool persist_write_summary(const char* path, const PersistedRunSummary& s) {
  SdFile file;
  if (!file.open(path, O_RDWR | O_CREAT | O_TRUNC)) return false;

  uint32_t pos = file.curPosition();
  file.clearWriteError();
  file.print(s.sessionId); file.print(",");
  file.print((int)s.everActive); file.print(",");
  file.print((int)s.everEntries); file.print(",");
  file.print((int)s.everGeo); file.print(",");
  file.print((int)s.everCorr); file.print(",");
  file.print((int)s.everInt); file.print(",");
  file.print((int)s.maxEntries); file.print(",");
  file.print(s.activePolls); file.print(",");
  file.print(s.navPolls); file.print(",");
  file.println(s.lastUptimeMs);

  bool ok = sd_commit_line(file, pos);
  file.close();
  return ok;
}

static void persist_current_run_summary() {
  if (!sd_ok) return;
  PersistedRunSummary s = make_current_run_summary();
  if (!persist_write_summary(kSbasCurrentFilename, s)) {
    sd_ok = false;
    Serial.println(F("SD state write failed; disabling SBAS logging"));
  }
}

static void load_previous_run_summary() {
  lastRunSummary = PersistedRunSummary();

  PersistedRunSummary priorCurrent;
  if (persist_read_summary(kSbasCurrentFilename, priorCurrent) && persisted_summary_has_data(priorCurrent)) {
    lastRunSummary = priorCurrent;
    (void)persist_write_summary(kSbasLastFilename, priorCurrent);
  } else {
    PersistedRunSummary priorLast;
    if (persist_read_summary(kSbasLastFilename, priorLast)) lastRunSummary = priorLast;
  }
}

static void print_last_run_summary() {
  Serial.print(F("LAST_RUN_SBAS seen="));
  Serial.print((lastRunSummary.valid && persisted_summary_seen(lastRunSummary)) ? 1 : 0);
  Serial.print(F(" session="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.sessionId : 0);
  Serial.print(F(" entries="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.everEntries : 0);
  Serial.print(F(" geo="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.everGeo : 0);
  Serial.print(F(" corr="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.everCorr : 0);
  Serial.print(F(" int="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.everInt : 0);
  Serial.print(F(" max_entries="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.maxEntries : 0);
  Serial.print(F(" active_polls="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.activePolls : 0);
  Serial.print(F(" nav_polls="));
  Serial.print(lastRunSummary.valid ? lastRunSummary.navPolls : 0);
  Serial.print(F(" last_uptime_ms="));
  Serial.println(lastRunSummary.valid ? lastRunSummary.lastUptimeMs : 0);
}

static void print_sticky_seen_summary() {
  Serial.print(F("SBAS_STICKY seen="));
  Serial.print(stickySeenState.valid ? 1 : 0);
  Serial.print(F(" session="));
  Serial.print(stickySeenState.valid ? stickySeenState.sessionId : 0);
  Serial.print(F(" uptime_ms="));
  Serial.println(stickySeenState.valid ? stickySeenState.uptimeMs : 0);
}

static void sd_rollback_line(SdFile& file, uint32_t pos) {
  file.clearWriteError();
  (void)file.truncate(pos);
  (void)file.seekSet(pos);
  file.clearWriteError();
}

static bool sd_commit_line(SdFile& file, uint32_t pos) {
  if (file.getWriteError()) {
    sd_rollback_line(file, pos);
    return false;
  }
  if (!file.sync() || file.getWriteError()) {
    sd_rollback_line(file, pos);
    return false;
  }
  return true;
}

static bool sd_write_header_if_empty(SdFile& file, const char* header) {
  if (file.fileSize() != 0) return true;
  uint32_t pos = file.curPosition();
  file.clearWriteError();
  file.println(header);
  return sd_commit_line(file, pos);
}

static bool allocate_session_log_file() {
  uint32_t lastSessionId = 0;
  (void)read_u32_text_file(kSbasIndexFilename, lastSessionId);

  for (uint32_t attempt = 0; attempt < 1000; attempt++) {
    currentSessionId = lastSessionId + 1 + attempt;
    make_session_log_filename(currentSessionId, currentLogFilename);

    if (sd.exists(currentLogFilename)) continue;
    if (!fSbas.open(currentLogFilename, O_RDWR | O_CREAT | O_TRUNC)) continue;

    SdFile idxFile;
    if (!idxFile.open(kSbasIndexFilename, O_RDWR | O_CREAT | O_TRUNC)) {
      fSbas.close();
      return false;
    }

    uint32_t pos = idxFile.curPosition();
    idxFile.clearWriteError();
    idxFile.println(currentSessionId);
    bool ok = sd_commit_line(idxFile, pos);
    idxFile.close();
    if (!ok) {
      fSbas.close();
      return false;
    }
    return true;
  }

  return false;
}

static bool sd_init_log() {
  SdSpiConfig cfg(kSdCsPin, DEDICATED_SPI, kSdSpiHz);
  if (!sd.begin(cfg)) return false;
  if (!allocate_session_log_file()) return false;
  return sd_write_header_if_empty(
      fSbas,
      "session_id,uptime_ms,gps_unix_ms,rmc_valid,fix_quality,sats,hdop,lat_deg,lon_deg,"
      "cfg_seen,cfg_enabled,cfg_test_mode,cfg_ranging,cfg_diffcorr,cfg_integrity,cfg_max_sbas,cfg_scanmode1,cfg_scanmode2,"
      "nav_itow_ms,nav_mode,nav_system,nav_geo_used,nav_tracked_entries,nav_service_flags,nav_integrity_used,"
      "summary_ever_active,summary_ever_entries,summary_ever_geo,summary_ever_corr,summary_ever_int,summary_max_entries,summary_active_polls,summary_nav_polls");
}

static void print_sbas_summary() {
  Serial.print(F("SBAS_SUMMARY polls="));
  Serial.print(sbasStats.navPolls);
  Serial.print(F(" active="));
  Serial.print(sbasStats.activePolls);
  Serial.print(F(" ever_active="));
  Serial.print(sbasStats.everActive ? 1 : 0);
  Serial.print(F(" ever_entries="));
  Serial.print(sbasStats.everTrackedEntries ? 1 : 0);
  Serial.print(F(" ever_geo="));
  Serial.print(sbasStats.everGeoUsed ? 1 : 0);
  Serial.print(F(" ever_corr="));
  Serial.print(sbasStats.everCorrections ? 1 : 0);
  Serial.print(F(" ever_int="));
  Serial.print(sbasStats.everIntegrity ? 1 : 0);
  Serial.print(F(" max_entries="));
  Serial.print(sbasStats.maxTrackedEntries);
  Serial.print(F(" last_mode="));
  Serial.print(sbas_mode_name(sbasStats.lastMode));
  Serial.print(F(" last_sys="));
  Serial.print(sbas_system_name(sbasStats.lastSystem));
  Serial.print(F(" last_geo="));
  Serial.print(sbasStats.lastGeoUsed);
  Serial.print(F(" last_cnt="));
  Serial.print(sbasStats.lastTrackedEntries);
  Serial.print(F(" last_integrity="));
  Serial.print(sbasStats.lastIntegrityUsed);
  Serial.print(F(" last_services="));
  print_service_flags(sbasStats.lastServiceFlags);
  if (sbasStats.everActive) {
    Serial.print(F(" first_active_ms="));
    Serial.print(sbasStats.firstActiveMs);
    Serial.print(F(" last_active_ms="));
    Serial.print(sbasStats.lastActiveMs);
  }
  Serial.println();
}

static void log_nav_sbas(uint32_t uptime_ms, uint32_t iTow, uint8_t geo, uint8_t mode, int8_t sys, uint8_t service, uint8_t cnt, uint8_t statusFlags) {
  if (!sd_ok) return;

  uint32_t pos = fSbas.curPosition();
  fSbas.clearWriteError();

  fSbas.print(currentSessionId);
  fSbas.print(",");
  fSbas.print(uptime_ms);
  fSbas.print(",");
  if (utcClock.valid) sd_print_u64(fSbas, utc_ms_from_millis(uptime_ms));
  fSbas.print(",");
  fSbas.print(gpsSnap.rmc_valid ? 1 : 0);
  fSbas.print(",");
  if (gpsSnap.have_fix_quality) fSbas.print(gpsSnap.fix_quality);
  fSbas.print(",");
  if (gpsSnap.have_sats) fSbas.print(gpsSnap.sats);
  fSbas.print(",");
  if (gpsSnap.have_hdop) fSbas.print(gpsSnap.hdop, 2);
  fSbas.print(",");
  if (gpsSnap.have_position) fSbas.print(gpsSnap.lat, 6);
  fSbas.print(",");
  if (gpsSnap.have_position) fSbas.print(gpsSnap.lon, 6);
  fSbas.print(",");
  fSbas.print(sbasCfg.valid ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasCfg.enabled ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasCfg.testMode ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasCfg.ranging ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasCfg.diffCorrections ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasCfg.integrity ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasCfg.maxSBAS);
  fSbas.print(",");
  fSbas.print(sbasCfg.scanmode1);
  fSbas.print(",");
  fSbas.print(sbasCfg.scanmode2);
  fSbas.print(",");
  fSbas.print(iTow);
  fSbas.print(",");
  fSbas.print(mode);
  fSbas.print(",");
  fSbas.print((int)sys);
  fSbas.print(",");
  fSbas.print(geo);
  fSbas.print(",");
  fSbas.print(cnt);
  fSbas.print(",");
  fSbas.print(service);
  fSbas.print(",");
  fSbas.print((uint8_t)(statusFlags & 0x03));
  fSbas.print(",");
  fSbas.print(sbasStats.everActive ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasStats.everTrackedEntries ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasStats.everGeoUsed ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasStats.everCorrections ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasStats.everIntegrity ? 1 : 0);
  fSbas.print(",");
  fSbas.print(sbasStats.maxTrackedEntries);
  fSbas.print(",");
  fSbas.print(sbasStats.activePolls);
  fSbas.print(",");
  fSbas.println(sbasStats.navPolls);

  if (!sd_commit_line(fSbas, pos)) {
    sd_ok = false;
    Serial.println(F("SD write failed; disabling SBAS SD logging"));
  }
}

static void record_nav_sbas(uint32_t iTow, uint8_t geo, uint8_t mode, int8_t sys, uint8_t service, uint8_t cnt, uint8_t statusFlags) {
  bool hasEntries = cnt > 0;
  bool hasGeo = geo != 0;
  bool hasCorrections = service_has_corrections(service);
  bool hasIntegrity = service_has_integrity(service) || ((statusFlags & 0x03) >= 2);
  bool isActive = hasEntries || hasGeo || hasCorrections || hasIntegrity;

  sbasStats.navPolls++;
  sbasStats.lastMode = mode;
  sbasStats.lastSystem = sys;
  sbasStats.lastGeoUsed = geo;
  sbasStats.lastTrackedEntries = cnt;
  sbasStats.lastServiceFlags = service;
  sbasStats.lastIntegrityUsed = (uint8_t)(statusFlags & 0x03);

  if (cnt > sbasStats.maxTrackedEntries) sbasStats.maxTrackedEntries = cnt;
  if (hasEntries) {
    sbasStats.pollsWithTrackedEntries++;
    sbasStats.everTrackedEntries = true;
  }
  if (hasGeo) {
    sbasStats.pollsWithGeoUsed++;
    sbasStats.everGeoUsed = true;
  }
  if (hasCorrections) {
    sbasStats.pollsWithCorrections++;
    sbasStats.everCorrections = true;
  }
  if (hasIntegrity) {
    sbasStats.pollsWithIntegrity++;
    sbasStats.everIntegrity = true;
  }
  if (isActive) {
    sbasStats.activePolls++;
    sbasStats.lastActiveMs = millis();
    sbasStats.lastActiveItow = iTow;
    if (!sbasStats.everActive) {
      sbasStats.everActive = true;
      sbasStats.firstActiveMs = sbasStats.lastActiveMs;
      sbasStats.firstActiveItow = iTow;
      stickySeenState.valid = true;
      stickySeenState.sessionId = currentSessionId;
      stickySeenState.uptimeMs = sbasStats.firstActiveMs;
      if (sd_ok && !sticky_seen_write(stickySeenState)) {
        sd_ok = false;
        Serial.println(F("SD sticky write failed; disabling SBAS logging"));
      }
      Serial.print(F("SBAS EVENT first activity detected at uptime_ms="));
      Serial.print(sbasStats.firstActiveMs);
      Serial.print(F(" iTOW="));
      Serial.print(iTow);
      Serial.print(F(" geo="));
      Serial.print(geo);
      Serial.print(F(" cnt="));
      Serial.print(cnt);
      Serial.print(F(" services="));
      print_service_flags(service);
      Serial.println();
    }
  }

  persist_current_run_summary();
}

static void process_nmea_sentence(char* line, uint32_t sentence_ms) {
  if (!nmea_checksum_ok(line)) return;
  if (line[0] == '$') line++;
  char* star = strchr(line, '*');
  if (star) *star = '\0';
  if (strlen(line) < 5) return;

  char* fields[20] = {0};
  int nf = split_csv(line, fields, 20);
  if (nf < 1) return;

  const char* msg = fields[0];

  if (strcmp(msg, "GPGGA") == 0 || strcmp(msg, "GNGGA") == 0) {
    if (nf > 6 && fields[6] && *fields[6]) {
      gpsSnap.fix_quality = (uint8_t)atoi(fields[6]);
      gpsSnap.have_fix_quality = true;
    }
    if (nf > 7 && fields[7] && *fields[7]) {
      gpsSnap.sats = (uint8_t)atoi(fields[7]);
      gpsSnap.have_sats = true;
    }
    if (nf > 8 && fields[8] && *fields[8]) {
      gpsSnap.hdop = atof(fields[8]);
      gpsSnap.have_hdop = true;
    }
    if (nf > 5 && fields[2] && *fields[2] && fields[4] && *fields[4]) {
      float lat = 0.0f;
      float lon = 0.0f;
      if (nmea_degmin_to_dec(fields[2], false, lat) && nmea_degmin_to_dec(fields[4], true, lon)) {
        if (fields[3] && (*fields[3] == 'S' || *fields[3] == 's')) lat = -lat;
        if (fields[5] && (*fields[5] == 'W' || *fields[5] == 'w')) lon = -lon;
        gpsSnap.lat = lat;
        gpsSnap.lon = lon;
        gpsSnap.have_position = true;
      }
    }
    return;
  }

  if (strcmp(msg, "GPRMC") == 0 || strcmp(msg, "GNRMC") == 0) {
    if (nf > 2 && fields[2] && *fields[2]) gpsSnap.rmc_valid = (fields[2][0] == 'A');

    if (gpsSnap.rmc_valid && nf > 9 && fields[1] && *fields[1] && fields[9] && *fields[9]) {
      uint64_t unix_ms = 0;
      if (gps_datetime_to_unix_ms(fields[1], fields[9], unix_ms)) utc_sync(sentence_ms, unix_ms);
    }

    if (nf > 6 && fields[3] && *fields[3] && fields[5] && *fields[5]) {
      float lat = 0.0f;
      float lon = 0.0f;
      if (nmea_degmin_to_dec(fields[3], false, lat) && nmea_degmin_to_dec(fields[5], true, lon)) {
        if (fields[4] && (*fields[4] == 'S' || *fields[4] == 's')) lat = -lat;
        if (fields[6] && (*fields[6] == 'W' || *fields[6] == 'w')) lon = -lon;
        gpsSnap.lat = lat;
        gpsSnap.lon = lon;
        gpsSnap.have_position = true;
      }
    }
  }
}

static void handle_ubx_message(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  ubx_count++;

  Serial.print(F("UBX cls=0x"));
  print_hex_byte(cls);
  Serial.print(F(" id=0x"));
  print_hex_byte(id);
  Serial.print(F(" len="));
  Serial.println(len);

  if (cls == 0x05 && id == 0x01 && len >= 2) {
    Serial.print(F("ACK-ACK target cls=0x"));
    print_hex_byte(payload[0]);
    Serial.print(F(" id=0x"));
    print_hex_byte(payload[1]);
    Serial.println();
    return;
  }

  if (cls == 0x05 && id == 0x00 && len >= 2) {
    Serial.print(F("ACK-NAK target cls=0x"));
    print_hex_byte(payload[0]);
    Serial.print(F(" id=0x"));
    print_hex_byte(payload[1]);
    Serial.println();
    return;
  }

  if (cls == 0x06 && id == 0x16 && len == 8) {
    uint8_t mode = payload[0];
    uint8_t usage = payload[1];
    uint8_t maxSBAS = payload[2];
    uint8_t scanmode2 = payload[3];
    uint32_t scanmode1 = (uint32_t)payload[4]
                       | ((uint32_t)payload[5] << 8)
                       | ((uint32_t)payload[6] << 16)
                       | ((uint32_t)payload[7] << 24);

    sbasCfg.valid = true;
    sbasCfg.enabled = (mode & 0x01) != 0;
    sbasCfg.testMode = (mode & 0x02) != 0;
    sbasCfg.ranging = (usage & 0x01) != 0;
    sbasCfg.diffCorrections = (usage & 0x02) != 0;
    sbasCfg.integrity = (usage & 0x04) != 0;
    sbasCfg.maxSBAS = maxSBAS;
    sbasCfg.scanmode2 = scanmode2;
    sbasCfg.scanmode1 = scanmode1;

    Serial.println(F("CFG-SBAS response"));
    Serial.print(F("  enabled: "));
    Serial.println((mode & 0x01) ? F("YES") : F("NO"));
    Serial.print(F("  test mode: "));
    Serial.println((mode & 0x02) ? F("YES") : F("NO"));
    Serial.print(F("  ranging: "));
    Serial.println((usage & 0x01) ? F("YES") : F("NO"));
    Serial.print(F("  diff corrections: "));
    Serial.println((usage & 0x02) ? F("YES") : F("NO"));
    Serial.print(F("  integrity: "));
    Serial.println((usage & 0x04) ? F("YES") : F("NO"));
    Serial.print(F("  max SBAS: "));
    Serial.println(maxSBAS);
    Serial.print(F("  scanmode2: 0x"));
    print_hex_byte(scanmode2);
    Serial.println();
    Serial.print(F("  scanmode1: 0x"));
    Serial.println(scanmode1, HEX);
    return;
  }

  if (cls == 0x01 && id == 0x32 && len >= 12) {
    uint32_t iTow = (uint32_t)payload[0]
                  | ((uint32_t)payload[1] << 8)
                  | ((uint32_t)payload[2] << 16)
                  | ((uint32_t)payload[3] << 24);
    uint8_t geo = payload[4];
    uint8_t mode = payload[5];
    int8_t sys = (int8_t)payload[6];
    uint8_t service = payload[7];
    uint8_t cnt = payload[8];
    uint8_t statusFlags = payload[9];

    record_nav_sbas(iTow, geo, mode, sys, service, cnt, statusFlags);
    log_nav_sbas(millis(), iTow, geo, mode, sys, service, cnt, statusFlags);

    Serial.println(F("NAV-SBAS status"));
    Serial.print(F("  iTOW: "));
    Serial.println(iTow);
    Serial.print(F("  mode: "));
    if (mode == 0 || mode == 1 || mode == 3) Serial.println(sbas_mode_name(mode));
    else Serial.println(mode);

    Serial.print(F("  system: "));
    Serial.println(sbas_system_name(sys));
    Serial.print(F("  corrections GEO used: "));
    Serial.println(geo);
    Serial.print(F("  tracked SBAS entries: "));
    Serial.println(cnt);
    Serial.print(F("  services: "));
    print_service_flags(service);
    Serial.println();
    Serial.print(F("  integrityUsed: "));
    Serial.println(statusFlags & 0x03);

    if (len < (uint16_t)(12 + 12 * cnt)) {
      Serial.println(F("  payload shorter than expected for SBAS entries"));
      return;
    }

    for (uint8_t i = 0; i < cnt; i++) {
      const uint8_t* sv = payload + 12 + (12 * i);
      uint8_t svid = sv[0];
      uint8_t udre = sv[2];
      int8_t svSys = (int8_t)sv[3];
      uint8_t svService = sv[4];
      int16_t prc = (int16_t)((uint16_t)sv[6] | ((uint16_t)sv[7] << 8));
      int16_t ic = (int16_t)((uint16_t)sv[10] | ((uint16_t)sv[11] << 8));

      Serial.print(F("  SV "));
      Serial.print(svid);
      Serial.print(F(" sys="));
      Serial.print(sbas_system_name(svSys));
      Serial.print(F(" udre="));
      Serial.print(udre);
      Serial.print(F(" prc_cm="));
      Serial.print(prc);
      Serial.print(F(" ic_cm="));
      Serial.print(ic);
      Serial.print(F(" services="));
      print_service_flags(svService);
      Serial.println();
    }
    return;
  }

  Serial.print(F("  payload: "));
  print_hex_block(payload, len);
  Serial.println();
}

static void process_gps_stream() {
  enum ParseState : uint8_t {
    WAIT_SYNC1,
    WAIT_SYNC2,
    READ_CLASS,
    READ_ID,
    READ_LEN1,
    READ_LEN2,
    READ_PAYLOAD,
    READ_CK_A,
    READ_CK_B
  };

  static ParseState state = WAIT_SYNC1;
  static uint8_t cls = 0;
  static uint8_t id = 0;
  static uint16_t len = 0;
  static uint16_t index = 0;
  static uint8_t payload[128];
  static uint8_t ck_a = 0;
  static uint8_t ck_b = 0;
  static char nmea[128];
  static uint8_t nmea_idx = 0;
  static bool in_nmea = false;

  while (GPS_SERIAL.available() > 0) {
    uint8_t b = (uint8_t)GPS_SERIAL.read();

    if (b == '$') {
      in_nmea = true;
      nmea_idx = 0;
      nmea[nmea_idx++] = (char)b;
    } else if (in_nmea) {
      if (b == '\r') {
        // ignore
      } else if (b == '\n') {
        nmea[nmea_idx] = '\0';
        nmea_count++;
        process_nmea_sentence(nmea, millis());
        if (kPrintNmea) {
          Serial.print(F("NMEA "));
          Serial.println(nmea);
        }
        in_nmea = false;
        nmea_idx = 0;
      } else if (nmea_idx < sizeof(nmea) - 1) {
        nmea[nmea_idx++] = (char)b;
      } else {
        in_nmea = false;
        nmea_idx = 0;
      }
    }

    switch (state) {
      case WAIT_SYNC1:
        if (b == 0xB5) state = WAIT_SYNC2;
        break;
      case WAIT_SYNC2:
        if (b == 0x62) {
          state = READ_CLASS;
          ck_a = 0;
          ck_b = 0;
        } else {
          state = WAIT_SYNC1;
        }
        break;
      case READ_CLASS:
        cls = b;
        ck_a += b; ck_b += ck_a;
        state = READ_ID;
        break;
      case READ_ID:
        id = b;
        ck_a += b; ck_b += ck_a;
        state = READ_LEN1;
        break;
      case READ_LEN1:
        len = b;
        ck_a += b; ck_b += ck_a;
        state = READ_LEN2;
        break;
      case READ_LEN2:
        len |= ((uint16_t)b << 8);
        ck_a += b; ck_b += ck_a;
        index = 0;
        if (len > sizeof(payload)) {
          Serial.print(F("Skipping oversized UBX payload len="));
          Serial.println(len);
          state = WAIT_SYNC1;
        } else if (len == 0) {
          state = READ_CK_A;
        } else {
          state = READ_PAYLOAD;
        }
        break;
      case READ_PAYLOAD:
        payload[index++] = b;
        ck_a += b; ck_b += ck_a;
        if (index >= len) state = READ_CK_A;
        break;
      case READ_CK_A:
        if (b == ck_a) {
          state = READ_CK_B;
        } else {
          Serial.println(F("UBX checksum A mismatch"));
          state = WAIT_SYNC1;
        }
        break;
      case READ_CK_B:
        if (b == ck_b) {
          handle_ubx_message(cls, id, payload, len);
        } else {
          Serial.println(F("UBX checksum B mismatch"));
        }
        state = WAIT_SYNC1;
        break;
    }
  }
}

void setup() {
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#endif

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println(F("SBAS probe starting"));
  Serial.println(F("Using GPS UART at 9600 baud"));

  sd_ok = sd_init_log();
  if (sd_ok) {
    load_previous_run_summary();
    (void)sticky_seen_read(stickySeenState);
    persist_current_run_summary();
  }
  Serial.print(F("SD logging: "));
  if (sd_ok) {
    Serial.print(F("OK -> "));
    Serial.println(currentLogFilename);
  } else {
    Serial.println(F("FAILED"));
  }
  print_last_run_summary();
  print_sticky_seen_summary();

  GPS_SERIAL.begin(9600);
  delay(250);
  GPS_SERIAL.write('\r');
  GPS_SERIAL.write('\n');
  send_cfg_rxm_continuous();
  delay(50);
  apply_southpan_sbas_config();
  delay(50);
  poll_cfg_sbas();
}

void loop() {
  static uint32_t last_poll = 0;
  static uint32_t last_heartbeat = 0;
  static uint32_t last_led_toggle = 0;
  static bool led_on = false;

  process_gps_stream();

#ifdef LED_BUILTIN
  if ((uint32_t)(millis() - last_led_toggle) >= 500) {
    last_led_toggle = millis();
    led_on = !led_on;
    digitalWrite(LED_BUILTIN, led_on ? HIGH : LOW);
  }
#endif

  if ((uint32_t)(millis() - last_heartbeat) >= 2000) {
    last_heartbeat = millis();
    Serial.print(F("heartbeat ms="));
    Serial.print(millis());
    Serial.print(F(" session="));
    Serial.print(currentSessionId);
    Serial.print(F(" nmea="));
    Serial.print(nmea_count);
    Serial.print(F(" ubx="));
    Serial.print(ubx_count);
    Serial.print(F(" sd="));
    Serial.println(sd_ok ? 1 : 0);
    print_last_run_summary();
    print_sticky_seen_summary();
    print_sbas_summary();
  }

  if ((uint32_t)(millis() - last_poll) >= 5000) {
    last_poll = millis();
    if (!southpan_cfg_matches_target()) {
      apply_southpan_sbas_config();
      delay(50);
    }
    Serial.println(F("Polling UBX-CFG-SBAS"));
    send_cfg_rxm_continuous();
    delay(50);
    poll_cfg_sbas();
    delay(50);
    Serial.println(F("Polling UBX-NAV-SBAS"));
    poll_nav_sbas();
  }

  delay(20);
}
