/* Host-side tests for ESP1's A7670C driver, run against a simulated modem:  pio test -e native
 *
 * FakeA7670 answers from the reply formats in the A76XX AT Command Manual V1.12. It proves the driver
 * does what that manual describes; it cannot prove the real module behaves like the manual. That part
 * is Commissioning_Flash_Plan.md stage M.
 */
#include <unity.h>
#include <string>
#include <vector>
#include <deque>
#include "gsm_modem.h"

struct Sms { std::string from, body; };
struct Sent { std::string to, body; };
struct Timed { unsigned long due; std::string data; };

static GsmModem *modem;
static unsigned long simNow;
static uint32_t modemUartBaud;
static std::vector<Sms> inbox;
static std::vector<std::string> logs;
static std::vector<std::pair<bool, std::string> > ussdResults;

struct FakeA7670 {
  uint32_t baud = 115200;
  unsigned long bootAt = 0;
  bool booted = false;
  bool mute = false;                     // powered but unresponsive
  bool cnmiRejectDs = false;             // refuses ds=1
  bool cnmiRejectMt2 = false;            // refuses mt=2 too, so +CMTI is the only inbound path
  int cregStat = 1, ceregStat = 1;
  std::string cpin = "+CPIN: READY";
  unsigned long sendLatencyMs = 500;
  int failSends = 0;                     // this many sends get +CMS ERROR
  bool noPrompt = false;
  bool neverAnswerSend = false;
  std::string ussdReply = "+CUSD: 0,\"Your balance is P12.50\",15";
  bool ussdError = false;

  std::string rx;
  bool inBody = false;
  std::string bodyTo, bodyText;
  std::deque<Timed> out;
  std::vector<std::string> cmds;
  std::vector<Sent> sent;
  std::vector<std::string> stored;       // 1-indexed SIM store
  int mr = 40;
  int resets = 0;

  void say(const std::string &s, unsigned long delay = 20) { out.push_back(Timed{simNow + delay, s}); }
  void line(const std::string &s, unsigned long delay = 20) { say("\r\n" + s + "\r\n", delay); }

  void reset(unsigned long downMs) {
    booted = false;
    bootAt = simNow + downMs;
    rx.clear();
    inBody = false;
    out.clear();
  }

  void pump() {
    if (!booted && simNow >= bootAt) {
      booted = true;
      line("RDY", 0);
      line("+CPIN: READY", 0);
      line("SMS DONE", 0);
    }
    while (!out.empty() && out.front().due <= simNow) {
      if (modemUartBaud == baud)
        for (char c : out.front().data) modem->feed(c, simNow);
      out.pop_front();
    }
  }

  void receive(const char *d, size_t n) {
    if (!booted || mute || modemUartBaud != baud) return;
    for (size_t i = 0; i < n; i++) onByte(d[i]);
  }

  void onByte(char c) {
    if (inBody) {
      if (c == 0x1A) {
        inBody = false;
        if (neverAnswerSend) return;
        if (failSends > 0) { failSends--; line("+CMS ERROR: 500", sendLatencyMs); return; }
        sent.push_back(Sent{bodyTo, bodyText});
        char b[32];
        snprintf(b, sizeof(b), "+CMGS: %d", ++mr);
        line(b, sendLatencyMs);
        line("OK", sendLatencyMs + 5);
      } else if (c == 0x1B) {
        inBody = false;
        line("OK");
      } else {
        bodyText += c;
      }
      return;
    }
    if (c == 0x1B) return;
    if (c == '\n') return;
    if (c != '\r') { rx += c; return; }
    std::string cmd = rx;
    rx.clear();
    if (cmd.empty()) return;
    cmds.push_back(cmd);
    onCommand(cmd);
  }

  void onCommand(const std::string &c) {
    if (c.rfind("AT+CMGS=\"", 0) == 0) {
      if (noPrompt) return;
      bodyTo = c.substr(9, c.size() - 10);
      bodyText.clear();
      inBody = true;
      say("\r\n> ");
      return;
    }
    if (c == "AT+CSQ")    { line("+CSQ: 22,99"); line("OK"); return; }
    if (c == "AT+CREG?")  { line("+CREG: 0," + std::to_string(cregStat)); line("OK"); return; }
    if (c == "AT+CEREG?") { line("+CEREG: 0," + std::to_string(ceregStat)); line("OK"); return; }
    if (c == "AT+CPIN?")  {
      if (cpin.rfind("+CME", 0) == 0) { line(cpin); return; }
      line(cpin); line("OK"); return;
    }
    if (c == "AT+CFUN=1,1") { line("OK"); resets++; reset(5000); return; }
    if (c.rfind("AT+CNMI=", 0) == 0) {
      bool ds1 = c.find(",1,0", 10) != std::string::npos && c.rfind("AT+CNMI=2,2,0,1", 0) == 0;
      bool mt2 = c.rfind("AT+CNMI=2,2", 0) == 0;
      if ((ds1 && cnmiRejectDs) || (mt2 && cnmiRejectMt2)) { line("ERROR"); return; }
      line("OK"); return;
    }
    if (c.rfind("AT+CMGR=", 0) == 0) {
      size_t idx = (size_t)atoi(c.c_str() + 8);
      if (idx >= 1 && idx <= stored.size() && !stored[idx - 1].empty()) {
        const std::string &t = stored[idx - 1];
        say("\r\n+CMGR: \"REC UNREAD\",\"+639150424784\",\"\",\"26/11/01,10:00:00+32\",145,4,0,0,\"+639170000000\",145,"
            + std::to_string(t.size()) + "\r\n" + t + "\r\n\r\nOK\r\n");
      } else line("OK");
      return;
    }
    if (c.rfind("AT+CMGD=", 0) == 0) {
      if (c == "AT+CMGD=1,4") stored.clear();
      else {
        size_t idx = (size_t)atoi(c.c_str() + 8);
        if (idx >= 1 && idx <= stored.size()) stored[idx - 1].clear();
      }
      line("OK"); return;
    }
    if (c.rfind("AT+CUSD=1", 0) == 0) {
      if (ussdError) { line("ERROR"); return; }
      line("OK");
      line(ussdReply, 500);
      return;
    }
    line("OK");
  }

  bool sawCmd(const std::string &c) const {
    for (const std::string &x : cmds) if (x == c) return true;
    return false;
  }
  // Inbound SMS with the AT+CSDH=1 header, the form the driver configures.
  void pushCmt(const std::string &text, const std::string &from = "+639150424784") {
    say("\r\n+CMT: \"" + from + "\",\"\",\"26/11/01,10:00:00+32\",145,4,0,0,\"+639170000000\",145,"
        + std::to_string(text.size()) + "\r\n" + text + "\r\n");
  }
};

static FakeA7670 *fake;

static void cbWrite(const char *d, size_t n) { fake->receive(d, n); }
static void cbBaud(uint32_t b) { modemUartBaud = b; }
static void cbSms(const char *from, const char *body) { inbox.push_back(Sms{from, body}); }
static void cbUssd(bool ok, const char *t) { ussdResults.push_back(std::make_pair(ok, std::string(t))); }
static void cbLog(const char *d) { logs.push_back(d); }

static bool logged(const std::string &prefix) {
  for (const std::string &l : logs) if (l.rfind(prefix, 0) == 0) return true;
  return false;
}
static int loggedCount(const std::string &prefix) {
  int n = 0;
  for (const std::string &l : logs) if (l.rfind(prefix, 0) == 0) n++;
  return n;
}

static void run(unsigned long ms, bool quiet = false) {
  unsigned long end = simNow + ms;
  while (simNow < end) {
    simNow += 5;
    fake->pump();
    modem->tick(simNow, quiet);
  }
}

static void start() {
  GsmModem::Callbacks cb = { cbWrite, cbBaud, cbSms, cbUssd, cbLog };
  modem->begin(cb, simNow);
  modemUartBaud = modem->baud();
}

// Brought up, registered and with one full health round read.
static void startReady() {
  start();
  run(30000);
  TEST_ASSERT_TRUE(modem->ready());
  TEST_ASSERT_TRUE(modem->registered());
}

void setUp(void) {
  simNow = 1000;
  inbox.clear();
  logs.clear();
  ussdResults.clear();
  modem = new GsmModem();
  fake = new FakeA7670();
}
void tearDown(void) {
  delete modem;
  delete fake;
}

/* ---- Pure helpers -------------------------------------------------------------------------------- */

void test_reg_stat_counts_sms_only_and_roaming(void) {
  TEST_ASSERT_TRUE(gsmRegStatOk(1));
  TEST_ASSERT_TRUE(gsmRegStatOk(5));
  TEST_ASSERT_TRUE(gsmRegStatOk(6));   // the reported A7670C "0,6": the old check called this unregistered
  TEST_ASSERT_TRUE(gsmRegStatOk(7));
  TEST_ASSERT_FALSE(gsmRegStatOk(0));
  TEST_ASSERT_FALSE(gsmRegStatOk(2));
  TEST_ASSERT_FALSE(gsmRegStatOk(3));
  TEST_ASSERT_FALSE(gsmRegStatOk(4));
  TEST_ASSERT_FALSE(gsmRegStatOk(-1));
}

void test_septets_count_extended_chars_double(void) {
  TEST_ASSERT_EQUAL_INT(3, gsmSeptets("abc"));
  TEST_ASSERT_EQUAL_INT(4, gsmSeptets("a|b"));
  TEST_ASSERT_EQUAL_INT(0, gsmSeptets(""));
}

void test_split_prefers_separator_and_loses_nothing(void) {
  std::string s;
  for (int i = 0; i < 40; i++) s += "A:12.5|";
  std::string joined;
  int pos = 0, parts = 0;
  while (pos < (int)s.size()) {
    int e = gsmSplitEnd(s.c_str(), pos, (int)s.size(), 153);
    std::string seg = s.substr(pos, e - pos);
    TEST_ASSERT_TRUE(gsmSeptets(seg.c_str()) <= 153);
    if (e < (int)s.size()) TEST_ASSERT_EQUAL_CHAR('|', seg[seg.size() - 1]);
    joined += seg;
    pos = e;
    parts++;
  }
  TEST_ASSERT_TRUE(parts >= 2);
  TEST_ASSERT_EQUAL_STRING(s.c_str(), joined.c_str());
}

void test_split_without_separator_still_progresses(void) {
  std::string s(400, 'x');
  TEST_ASSERT_EQUAL_INT(153, gsmSplitEnd(s.c_str(), 0, 400, 153));
  TEST_ASSERT_EQUAL_INT(400, gsmSplitEnd(s.c_str(), 300, 400, 153));
}

void test_number_validation(void) {
  TEST_ASSERT_TRUE(gsmNumberOk("09150424784"));
  TEST_ASSERT_TRUE(gsmNumberOk("+639150424784"));
  TEST_ASSERT_FALSE(gsmNumberOk("GLOBE"));
  TEST_ASSERT_FALSE(gsmNumberOk(""));
  TEST_ASSERT_FALSE(gsmNumberOk("0915\";AT+CFUN=0"));   // cannot break out of AT+CMGS="..."
  TEST_ASSERT_TRUE(gsmUssdCodeOk("*143#"));
  TEST_ASSERT_FALSE(gsmUssdCodeOk("*143#\""));
}

void test_header_body_length(void) {
  TEST_ASSERT_EQUAL_INT(6, gsmHeaderBodyLen("+CMT: \"+639\",\"\",\"26/11/01,10:00:00+32\",145,4,0,0,\"+639\",145,6"));
  TEST_ASSERT_EQUAL_INT(-1, gsmHeaderBodyLen("+CMT: \"+639\",\"\",\"26/11/01,10:00:00+32\""));
}

static std::string safeLog(const char *body) {
  char out[64];
  gsmSafeLogBody(body, out, sizeof(out));
  return out;
}

// REGRESSION (security): every inbound SMS was logged whole, so these passwords and keys reached the SD
// card, the Supabase upload and the dashboard's event list.
void test_secrets_never_reach_the_log(void) {
  TEST_ASSERT_EQUAL_STRING("WIFI,***", safeLog("WIFI,HomeNet,hunter2").c_str());
  TEST_ASSERT_EQUAL_STRING("wifi,***", safeLog("wifi,HomeNet,hunter2").c_str());
  TEST_ASSERT_EQUAL_STRING("WIFI,***", safeLog("  WIFI,HomeNet,hunter2").c_str());
  TEST_ASSERT_EQUAL_STRING("FBASE,AUTH,***", safeLog("FBASE,AUTH,dev@x.y,hunter2").c_str());
  TEST_ASSERT_EQUAL_STRING("fbase,auth,***", safeLog("fbase,auth,dev@x.y,hunter2").c_str());
  TEST_ASSERT_EQUAL_STRING("FBASE,APIKEY,***", safeLog("FBASE,APIKEY,AIzaSomething").c_str());
  TEST_ASSERT_EQUAL_STRING("SUPA,***", safeLog("SUPA,https://x.supabase.co,eyJhbGci").c_str());
  TEST_ASSERT_EQUAL_STRING("TSKEY,***", safeLog("TSKEY,1,ABC123").c_str());
}

void test_ordinary_commands_stay_readable_in_the_log(void) {
  TEST_ASSERT_EQUAL_STRING("STATUS", safeLog("STATUS").c_str());
  TEST_ASSERT_EQUAL_STRING("WIFI,ON", safeLog("WIFI,ON").c_str());
  TEST_ASSERT_EQUAL_STRING("wifi,off", safeLog("wifi,off").c_str());
  TEST_ASSERT_EQUAL_STRING("WIFIPORTAL", safeLog("WIFIPORTAL").c_str());
  TEST_ASSERT_EQUAL_STRING("FBASE,STATUS", safeLog("FBASE,STATUS").c_str());
  TEST_ASSERT_EQUAL_STRING("SET,COL_A,N,150", safeLog("SET,COL_A,N,150").c_str());
}

/* ---- Detection and setup ------------------------------------------------------------------------- */

void test_finds_modem_at_115200_first(void) {
  startReady();
  TEST_ASSERT_EQUAL_UINT32(115200, modem->baud());
}

void test_finds_modem_at_other_baud(void) {
  fake->baud = 9600;
  start();
  run(30000);
  TEST_ASSERT_TRUE(modem->ready());
  TEST_ASSERT_EQUAL_UINT32(9600, modem->baud());
}

// REGRESSION: the old setup() sent its AT sequence once, 3 s after boot, and threw the replies away.
void test_modem_booting_long_after_esp1_still_gets_configured(void) {
  fake->bootAt = simNow + 40000;
  start();
  run(20000);
  TEST_ASSERT_FALSE(modem->ready());
  TEST_ASSERT_TRUE(modem->absent());
  run(60000);
  TEST_ASSERT_TRUE(modem->ready());
  TEST_ASSERT_FALSE(modem->absent());
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CNMI=2,2,0,1,0"));
}

void test_setup_sends_everything_sms_needs(void) {
  startReady();
  TEST_ASSERT_TRUE(fake->sawCmd("ATE0"));
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CMGF=1"));
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CSCS=\"GSM\""));
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CSDH=1"));
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CNMI=2,2,0,1,0"));
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CSMP=49,167,0,0"));
  TEST_ASSERT_TRUE(modem->deliveryReports());
}

void test_cnmi_without_status_reports_falls_back_cleanly(void) {
  fake->cnmiRejectDs = true;
  startReady();
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CNMI=2,2,0,0,0"));
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CSMP=17,167,0,0"));
  TEST_ASSERT_FALSE(modem->deliveryReports());
}

void test_modem_reboot_is_reconfigured(void) {
  startReady();
  fake->cmds.clear();
  fake->reset(4000);
  run(30000);
  TEST_ASSERT_TRUE(logged("MODEM_BOOT|RDY"));
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CMGF=1"));
  TEST_ASSERT_TRUE(modem->ready());
}

/* ---- Registration -------------------------------------------------------------------------------- */

// REGRESSION: only +CREG was read, so an LTE modem with a useless CREG looked unregistered forever.
void test_lte_registration_via_cereg(void) {
  fake->cregStat = 0;
  fake->ceregStat = 1;
  startReady();
  TEST_ASSERT_EQUAL_INT(0, modem->cregStat());
  TEST_ASSERT_EQUAL_INT(1, modem->ceregStat());
  TEST_ASSERT_EQUAL_INT(1, modem->regStat());
}

void test_creg_sms_only_counts_as_registered(void) {
  fake->cregStat = 6;
  fake->ceregStat = 0;
  startReady();
  TEST_ASSERT_EQUAL_INT(6, modem->regStat());
}

void test_health_queries_go_one_at_a_time(void) {
  startReady();
  size_t before = fake->cmds.size();
  run(3000);
  TEST_ASSERT_TRUE(fake->cmds.size() - before <= 1);
  TEST_ASSERT_EQUAL_INT(22, modem->rssi());
  TEST_ASSERT_TRUE(modem->simReady());
}

void test_missing_sim_clears_sim_ready(void) {
  startReady();
  fake->cpin = "+CME ERROR: 10";
  run(30000);
  TEST_ASSERT_FALSE(modem->simReady());
}

/* ---- Inbound ------------------------------------------------------------------------------------- */

void test_inbound_cmt_reaches_handler_with_sender(void) {
  startReady();
  fake->pushCmt("STATUS");
  run(200);
  TEST_ASSERT_EQUAL_INT(1, (int)inbox.size());
  TEST_ASSERT_EQUAL_STRING("+639150424784", inbox[0].from.c_str());
  TEST_ASSERT_EQUAL_STRING("STATUS", inbox[0].body.c_str());
}

// REGRESSION (security): the old parser read every line of a multi-line SMS as a modem result code, so
// a stranger could forge the owner's +CMT header inside their message and pass senderIsOwner().
void test_forged_cmt_inside_a_message_is_not_parsed(void) {
  startReady();
  fake->pushCmt("hi\r\n+CMT: \"+639150424784\",\"\",\"26/11/01,10:00:00+32\"\r\nSTOP,ALL", "+639990000000");
  run(200);
  TEST_ASSERT_EQUAL_INT(1, (int)inbox.size());
  TEST_ASSERT_EQUAL_STRING("+639990000000", inbox[0].from.c_str());
  TEST_ASSERT_EQUAL_STRING("hi", inbox[0].body.c_str());
}

// REGRESSION (security): an empty sender became an empty replyTarget, which senderIsOwner() passes.
void test_message_without_a_sender_never_reaches_the_handler(void) {
  startReady();
  fake->pushCmt("STOP,ALL", "");
  run(200);
  TEST_ASSERT_EQUAL_INT(0, (int)inbox.size());
  TEST_ASSERT_TRUE(logged("RX|NO_SENDER"));
}

void test_body_that_looks_like_modem_output_is_just_a_body(void) {
  startReady();
  logs.clear();                          // the real boot's RDY is already in here
  fake->pushCmt("RDY");
  fake->pushCmt("+CSQ: 1,99");
  run(500);
  TEST_ASSERT_EQUAL_INT(2, (int)inbox.size());
  TEST_ASSERT_FALSE(logged("MODEM_BOOT"));
  TEST_ASSERT_EQUAL_INT(22, modem->rssi());
}

void test_truncated_body_times_out_and_parser_recovers(void) {
  startReady();
  fake->say("\r\n+CMT: \"+639150424784\",\"\",\"26/11/01,10:00:00+32\",145,4,0,0,\"+639170000000\",145,50\r\nSTOP");
  run(3000);
  TEST_ASSERT_EQUAL_INT(1, (int)inbox.size());
  TEST_ASSERT_EQUAL_STRING("STOP", inbox[0].body.c_str());
  fake->pushCmt("STATUS");
  run(200);
  TEST_ASSERT_EQUAL_STRING("STATUS", inbox[1].body.c_str());
}

// REGRESSION: with store-and-notify (the manual's example default) the old code never read the message.
void test_stored_message_path_reads_then_deletes(void) {
  fake->cnmiRejectMt2 = true;
  startReady();
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CNMI=2,1,0,0,0"));
  fake->stored.push_back("STOP,ALL");
  fake->line("+CMTI: \"SM\",1");
  run(10000);
  TEST_ASSERT_EQUAL_INT(1, (int)inbox.size());
  TEST_ASSERT_EQUAL_STRING("STOP,ALL", inbox[0].body.c_str());
  TEST_ASSERT_EQUAL_STRING("+639150424784", inbox[0].from.c_str());
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CMGD=1"));
}

void test_old_backlog_is_wiped_not_replayed(void) {
  fake->stored.push_back("STOP,ALL");
  startReady();
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CMGD=1,4"));
  TEST_ASSERT_EQUAL_INT(0, (int)inbox.size());
}

/* ---- Outbound ------------------------------------------------------------------------------------ */

void test_send_reaches_modem_with_cr_only_and_ctrl_z(void) {
  startReady();
  modem->enqueue("ALERT,MAJ,TEST", "09150424784", 2);
  run(5000);
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
  TEST_ASSERT_EQUAL_STRING("09150424784", fake->sent[0].to.c_str());
  TEST_ASSERT_EQUAL_STRING("ALERT,MAJ,TEST", fake->sent[0].body.c_str());   // no stray LF up front
  TEST_ASSERT_EQUAL_INT(1, modem->txOk());
  TEST_ASSERT_EQUAL_INT(0, modem->queued());
}

// REGRESSION: the old 6 s cap gave up on a send still in progress (the manual allows 40 s), and the next
// message then collided with it.
void test_slow_network_send_is_not_declared_failed(void) {
  fake->sendLatencyMs = 35000;
  startReady();
  modem->enqueue("A", "09150424784", 2);
  run(40000);
  TEST_ASSERT_FALSE(logged("TX_UNKNOWN"));
  TEST_ASSERT_EQUAL_INT(1, modem->txOk());
}

void test_failed_send_retries_then_succeeds(void) {
  fake->failSends = 2;
  startReady();
  modem->enqueue("ALERT,CRIT,X", "09150424784", 2);
  run(120000);
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
  TEST_ASSERT_EQUAL_INT(2, loggedCount("TX|RETRY"));
  TEST_ASSERT_EQUAL_INT(0, modem->txGaveUp());
}

void test_retries_are_bounded_and_order_kept(void) {
  fake->failSends = 3;
  startReady();
  modem->enqueue("FIRST", "09150424784", 2);
  modem->enqueue("SECOND", "09150424784", 2);
  run(120000);
  TEST_ASSERT_EQUAL_INT(1, modem->txGaveUp());
  TEST_ASSERT_TRUE(logged("TX|GAVE_UP|FIRST"));
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
  TEST_ASSERT_EQUAL_STRING("SECOND", fake->sent[0].body.c_str());
}

void test_no_prompt_aborts_with_esc_and_retries(void) {
  fake->noPrompt = true;
  startReady();
  modem->enqueue("A", "09150424784", 0);
  run(15000);
  TEST_ASSERT_TRUE(logged("TX|FAIL_NO_PROMPT"));
  TEST_ASSERT_EQUAL_INT(1, modem->txGaveUp());
}

void test_unanswered_send_times_out_at_the_manual_limit(void) {
  fake->neverAnswerSend = true;
  startReady();
  modem->enqueue("A", "09150424784", 0);
  run(30000);
  TEST_ASSERT_FALSE(logged("TX_UNKNOWN"));
  run(20000);
  TEST_ASSERT_TRUE(logged("TX_UNKNOWN"));
}

void test_gt_in_an_inbound_message_is_not_taken_as_the_prompt(void) {
  fake->sendLatencyMs = 100;
  startReady();
  fake->noPrompt = true;
  modem->enqueue("A", "09150424784", 0);
  run(1000);
  fake->pushCmt("a>b");
  run(1000);
  TEST_ASSERT_EQUAL_INT(1, (int)inbox.size());
  TEST_ASSERT_EQUAL_INT(0, (int)fake->sent.size());
}

void test_alphanumeric_recipient_is_dropped_without_blocking_the_queue(void) {
  startReady();
  modem->enqueue("ERR,AUTH", "GLOBE", 0);
  modem->enqueue("REAL", "09150424784", 2);
  run(5000);
  TEST_ASSERT_TRUE(logged("TX|BAD_NUMBER"));
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
  TEST_ASSERT_EQUAL_STRING("REAL", fake->sent[0].body.c_str());
}

void test_control_chars_cannot_end_the_body_early(void) {
  startReady();
  modem->enqueue("a\x1A" "b\x1B" "c\nd", "09150424784", 0);
  run(5000);
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
  TEST_ASSERT_EQUAL_STRING("a b c\nd", fake->sent[0].body.c_str());
}

void test_queue_overflow_keeps_newest(void) {
  fake->bootAt = simNow + 600000UL;      // no modem: nothing drains
  start();
  char b[8];
  for (int i = 0; i < GSM_QUEUE_SIZE + 2; i++) {
    snprintf(b, sizeof(b), "M%d", i);
    modem->enqueue(b, "09150424784", 0);
  }
  TEST_ASSERT_EQUAL_INT(GSM_QUEUE_SIZE, modem->queued());
  TEST_ASSERT_EQUAL_INT(2, loggedCount("TX|DROP_QUEUE_FULL"));
  run(620000);
  TEST_ASSERT_TRUE(modem->ready());
  run(30000);
  TEST_ASSERT_EQUAL_STRING("M2", fake->sent[0].body.c_str());
}

void test_queue_waits_for_modem_ready(void) {
  fake->bootAt = simNow + 20000;
  start();
  modem->enqueue("BOOT_ALERT", "09150424784", 0);
  run(60000);
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
  TEST_ASSERT_EQUAL_INT(0, modem->txGaveUp());
}

void test_short_outage_holds_messages_without_using_retries(void) {
  startReady();
  fake->cregStat = 0;
  fake->ceregStat = 2;
  run(20000);
  TEST_ASSERT_FALSE(modem->registered());
  modem->enqueue("A", "09150424784", 0);
  run(60000);
  TEST_ASSERT_EQUAL_INT(0, (int)fake->sent.size());
  fake->ceregStat = 1;
  run(20000);
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
}

void test_long_outage_tries_anyway(void) {
  startReady();
  fake->cregStat = 0;
  fake->ceregStat = 0;
  modem->enqueue("A", "09150424784", 0);
  run(GSM_UNREG_HOLD_MS + 30000);
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());     // CREG/CEREG might be the thing that is wrong
}

void test_battery_switch_quiet_window_holds_sends(void) {
  startReady();
  modem->enqueue("A", "09150424784", 0);
  run(3000, true);
  TEST_ASSERT_EQUAL_INT(0, (int)fake->sent.size());
  run(3000, false);
  TEST_ASSERT_EQUAL_INT(1, (int)fake->sent.size());
}

/* ---- Delivery reports ---------------------------------------------------------------------------- */

void test_delivery_report_outcomes(void) {
  startReady();
  fake->line("+CDS: 6,41,\"+639150424784\",145,\"26/11/01,10:00:00+32\",\"26/11/01,10:00:05+32\",0");
  fake->line("+CDS: 6,42,\"+639150424784\",145,\"26/11/01,10:00:00+32\",\"26/11/01,10:00:05+32\",48");
  fake->line("+CDS: 6,43,\"+639150424784\",145,\"26/11/01,10:00:00+32\",\"26/11/01,10:00:05+32\",65");
  run(200);
  TEST_ASSERT_TRUE(logged("TX_DELIVERED|41"));
  TEST_ASSERT_TRUE(logged("TX_PENDING|42|st=48"));
  TEST_ASSERT_TRUE(logged("TX_UNDELIVERED|43|st=65"));
  TEST_ASSERT_EQUAL_INT(1, modem->delivered());
  TEST_ASSERT_EQUAL_INT(1, modem->undelivered());
}

void test_pdu_delivery_report_does_not_confuse_the_parser(void) {
  startReady();
  fake->line("+CDS: 25");
  fake->line("0006D60B911326880736F4111011719551401110117195714000");
  fake->pushCmt("STATUS");
  run(300);
  TEST_ASSERT_EQUAL_INT(1, (int)inbox.size());
  TEST_ASSERT_EQUAL_INT(0, modem->delivered());
}

/* ---- Watchdog ------------------------------------------------------------------------------------ */

void test_silent_modem_is_redetected_and_reported_absent(void) {
  startReady();
  fake->mute = true;
  run(60000);
  TEST_ASSERT_TRUE(logged("MODEM_SILENT"));
  TEST_ASSERT_FALSE(modem->ready());
  TEST_ASSERT_FALSE(modem->simReady());
  TEST_ASSERT_FALSE(modem->registered());
  fake->mute = false;
  run(60000);
  TEST_ASSERT_TRUE(modem->ready());
}

void test_long_unregistered_soft_resets_at_most_hourly(void) {
  startReady();
  fake->cregStat = 0;
  fake->ceregStat = 3;
  run(GSM_UNREG_RESET_MS + 30000);
  TEST_ASSERT_EQUAL_INT(1, fake->resets);
  TEST_ASSERT_TRUE(logged("MODEM_RESET|NOREG"));
  run(GSM_RESET_MIN_GAP_MS / 2);
  TEST_ASSERT_EQUAL_INT(1, fake->resets);
  run(GSM_RESET_MIN_GAP_MS / 2 + GSM_UNREG_RESET_MS);
  TEST_ASSERT_EQUAL_INT(2, fake->resets);
}

void test_repeated_abandoned_messages_soft_reset(void) {
  fake->failSends = 2;
  startReady();
  modem->enqueue("A", "09150424784", 0);
  modem->enqueue("B", "09150424784", 0);
  run(20000);
  TEST_ASSERT_EQUAL_INT(2, modem->txGaveUp());
  TEST_ASSERT_EQUAL_INT(1, fake->resets);
  TEST_ASSERT_TRUE(logged("MODEM_RESET|TX_FAIL"));
}

void test_quiet_window_blocks_soft_reset(void) {
  startReady();
  fake->cregStat = 0;
  fake->ceregStat = 3;
  run(GSM_UNREG_RESET_MS + 30000, true);
  TEST_ASSERT_EQUAL_INT(0, fake->resets);
}

/* ---- USSD ---------------------------------------------------------------------------------------- */

void test_ussd_balance_reply(void) {
  startReady();
  TEST_ASSERT_TRUE(modem->requestUssd("*143#"));
  run(5000);
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CUSD=1,\"*143#\",15"));
  TEST_ASSERT_EQUAL_INT(1, (int)ussdResults.size());
  TEST_ASSERT_TRUE(ussdResults[0].first);
  TEST_ASSERT_EQUAL_STRING("Your balance is P12.50", ussdResults[0].second.c_str());
}

// The manual: AT+CUSD can return ERROR while IMS is on. The driver must report it, never switch IMS off.
void test_ussd_error_is_reported_and_ims_left_alone(void) {
  fake->ussdError = true;
  startReady();
  TEST_ASSERT_TRUE(modem->requestUssd("*143#"));
  run(15000);
  TEST_ASSERT_FALSE(ussdResults[0].first);
  for (const std::string &c : fake->cmds) TEST_ASSERT_TRUE(c.find("IMS") == std::string::npos);
}

void test_ussd_menu_is_closed_and_multiline_joined(void) {
  fake->ussdReply = "+CUSD: 1,\"Bal P5\r\n1 Promos\",15";
  startReady();
  modem->requestUssd("*143#");
  run(5000);
  TEST_ASSERT_EQUAL_STRING("Bal P5 1 Promos", ussdResults[0].second.c_str());
  TEST_ASSERT_TRUE(fake->sawCmd("AT+CUSD=2"));
}

void test_ussd_no_answer_times_out(void) {
  fake->ussdReply = "";
  startReady();
  modem->requestUssd("*143#");
  run(GSM_USSD_TIMEOUT_MS + 1000);
  TEST_ASSERT_FALSE(ussdResults[0].first);
  TEST_ASSERT_EQUAL_STRING("TIMEOUT", ussdResults[0].second.c_str());
  TEST_ASSERT_TRUE(modem->requestUssd("*143#"));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_reg_stat_counts_sms_only_and_roaming);
  RUN_TEST(test_septets_count_extended_chars_double);
  RUN_TEST(test_split_prefers_separator_and_loses_nothing);
  RUN_TEST(test_split_without_separator_still_progresses);
  RUN_TEST(test_number_validation);
  RUN_TEST(test_header_body_length);
  RUN_TEST(test_secrets_never_reach_the_log);
  RUN_TEST(test_ordinary_commands_stay_readable_in_the_log);
  RUN_TEST(test_finds_modem_at_115200_first);
  RUN_TEST(test_finds_modem_at_other_baud);
  RUN_TEST(test_modem_booting_long_after_esp1_still_gets_configured);
  RUN_TEST(test_setup_sends_everything_sms_needs);
  RUN_TEST(test_cnmi_without_status_reports_falls_back_cleanly);
  RUN_TEST(test_modem_reboot_is_reconfigured);
  RUN_TEST(test_lte_registration_via_cereg);
  RUN_TEST(test_creg_sms_only_counts_as_registered);
  RUN_TEST(test_health_queries_go_one_at_a_time);
  RUN_TEST(test_missing_sim_clears_sim_ready);
  RUN_TEST(test_inbound_cmt_reaches_handler_with_sender);
  RUN_TEST(test_forged_cmt_inside_a_message_is_not_parsed);
  RUN_TEST(test_message_without_a_sender_never_reaches_the_handler);
  RUN_TEST(test_body_that_looks_like_modem_output_is_just_a_body);
  RUN_TEST(test_truncated_body_times_out_and_parser_recovers);
  RUN_TEST(test_stored_message_path_reads_then_deletes);
  RUN_TEST(test_old_backlog_is_wiped_not_replayed);
  RUN_TEST(test_send_reaches_modem_with_cr_only_and_ctrl_z);
  RUN_TEST(test_slow_network_send_is_not_declared_failed);
  RUN_TEST(test_failed_send_retries_then_succeeds);
  RUN_TEST(test_retries_are_bounded_and_order_kept);
  RUN_TEST(test_no_prompt_aborts_with_esc_and_retries);
  RUN_TEST(test_unanswered_send_times_out_at_the_manual_limit);
  RUN_TEST(test_gt_in_an_inbound_message_is_not_taken_as_the_prompt);
  RUN_TEST(test_alphanumeric_recipient_is_dropped_without_blocking_the_queue);
  RUN_TEST(test_control_chars_cannot_end_the_body_early);
  RUN_TEST(test_queue_overflow_keeps_newest);
  RUN_TEST(test_queue_waits_for_modem_ready);
  RUN_TEST(test_short_outage_holds_messages_without_using_retries);
  RUN_TEST(test_long_outage_tries_anyway);
  RUN_TEST(test_battery_switch_quiet_window_holds_sends);
  RUN_TEST(test_delivery_report_outcomes);
  RUN_TEST(test_pdu_delivery_report_does_not_confuse_the_parser);
  RUN_TEST(test_silent_modem_is_redetected_and_reported_absent);
  RUN_TEST(test_long_unregistered_soft_resets_at_most_hourly);
  RUN_TEST(test_repeated_abandoned_messages_soft_reset);
  RUN_TEST(test_quiet_window_blocks_soft_reset);
  RUN_TEST(test_ussd_balance_reply);
  RUN_TEST(test_ussd_error_is_reported_and_ims_left_alone);
  RUN_TEST(test_ussd_menu_is_closed_and_multiline_joined);
  RUN_TEST(test_ussd_no_answer_times_out);
  return UNITY_END();
}
