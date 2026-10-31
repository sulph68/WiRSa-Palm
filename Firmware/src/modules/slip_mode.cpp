// SLIP Gateway mode handler

#include "slip_mode.h"
#include "mode_exit.h"
#include "globals.h"
#include "settings.h"   // slipTimeoutMin
#include "serial_io.h"
#include "display_menu.h"
#include "network.h"
#include <EEPROM.h>

extern bool usbDebug;

// ---- Global contexts ----

SlipContext slipCtx;
NatContext natCtx;
SlipModeContext slipModeCtx;

// showMenu()/showHeader() are declared in display_menu.h (included above); a local extern here would mismatch its signature (both gained a showWiRSaPrefix parameter).
extern void showMessage(String message);
extern void readSwitches();
extern void waitSwitches();
extern void mainMenu(bool arrow);

extern WirsaDisplay display;
extern int menuMode;
extern int menuIdx;
extern int menuCnt;
extern int lastMenu;
extern bool msgFlag;
extern bool BTNUP;
extern bool BTNDN;
extern bool BTNEN;
extern bool BTNBK;

// ---- link health, for the stats line ----
// What the gateway saw on the Palm line: UART events (FIFO overflow and buffer-full lose bytes silently; the Arduino core's own warning is compiled out), how far behind the loop fell (bytes waiting
// in the serial buffer) and the longest gap between two passes of the loop.
static volatile uint32_t gUartOvf, gUartFull, gUartFrame, gUartBreak, gUartParity;
static uint32_t gMaxBacklog, gMaxGapMs, gSlowPasses, gStatsPasses;
static unsigned long gLastPassMs, gLastStatsMs, gLastTrafficMs;
static uint32_t gStatFramesIn, gStatBytesIn, gStatFramesOut, gStatBytesOut, gStatTxMicros, gStatTxBytes;

static void slipUartError(hardwareSerial_error_t e) {
    switch (e) {
        case UART_FIFO_OVF_ERROR:    gUartOvf++; break;
        case UART_BUFFER_FULL_ERROR: gUartFull++; break;
        case UART_FRAME_ERROR:       gUartFrame++; break;
        case UART_BREAK_ERROR:       gUartBreak++; break;
        case UART_PARITY_ERROR:      gUartParity++; break;
        default: break;
    }
}

static void slipStatsReset() {
    gUartOvf = gUartFull = gUartFrame = gUartBreak = gUartParity = 0;
    gMaxBacklog = gMaxGapMs = gSlowPasses = gStatsPasses = 0;
    gLastPassMs = gLastStatsMs = millis();
    gLastTrafficMs = 0;
    gStatFramesIn = gStatBytesIn = gStatFramesOut = gStatBytesOut = gStatTxMicros = gStatTxBytes = 0;
    slipTxMicros = slipTxBytes = 0;
}

// One line every 10 s while there is traffic and when the gateway stops. Compare with the line speed: "tx x%" is the share of the line rate the gateway's own sending reached (100 = back to back at full speed).
static void slipLogStats(const char *why) {
    unsigned long nowMs = millis();
    uint32_t baud = PhysicalSerial.baudRate();
    uint32_t txPct = 0;
    if (slipTxMicros && baud) txPct = (uint32_t) ((uint64_t) slipTxBytes * 10ULL * 1000000ULL * 100ULL / ((uint64_t) slipTxMicros * baud));
    LOGI("SLIP stats (%s) line %lu baud, pace %lu (slowed %lu x): in %lu fr/%lu B (slip err %lu, ip cksum %lu, tcp cksum %lu, out of seq %lu), "
         "out %lu fr/%lu B (tx %lu%% of line)\r\n",
         why, (unsigned long) baud, (unsigned long) slipPaceRate(), (unsigned long) slipPaceBackoffs(),
         (unsigned long) slipCtx.framesReceived, (unsigned long) slipCtx.bytesReceived, (unsigned long) slipCtx.rxErrors,
         (unsigned long) natCtx.statIpBadCksum, (unsigned long) natCtx.statTcpBadCksum, (unsigned long) natCtx.statOutOfSeq,
         (unsigned long) slipCtx.framesSent, (unsigned long) slipCtx.bytesSent, (unsigned long) txPct);
    LOGI("SLIP stats (%s) retransmit rto %lu / fast %lu, dup ack %lu, short write %lu, queue to server max %lu B full %lu, "
         "uart ovf %lu full %lu frame %lu brk %lu par %lu, backlog max %lu B, loop gap max %lu ms (>50 ms: %lu of %lu)\r\n",
         why, (unsigned long) natCtx.statRtoRewinds, (unsigned long) natCtx.statFastRetx, (unsigned long) natCtx.statDupAcks,
         (unsigned long) natCtx.statPartialWrites, (unsigned long) natCtx.statQueueMax, (unsigned long) natCtx.statQueueFull,
         (unsigned long) gUartOvf, (unsigned long) gUartFull, (unsigned long) gUartFrame, (unsigned long) gUartBreak,
         (unsigned long) gUartParity, (unsigned long) gMaxBacklog, (unsigned long) gMaxGapMs,
         (unsigned long) gSlowPasses, (unsigned long) gStatsPasses);
    (void) nowMs;
}

// forward declarations
static void slipActiveLoop();
static void slipUpdateActiveDisplay();
static bool parseIPAddress(const String& str, IPAddress& ip);

// ---- SLIP loop (the mode handler) ----

void slipLoop() {
    if (slipModeCtx.state == SLIP_MODE_ACTIVE) {
        slipActiveLoop();
    } else {
        // There is no SLIP menu: MODE_SLIP exists only while the gateway runs. If this is reached without it, go back to the main menu rather than sit in a mode with no screen.
        msgFlag = true;
        mainMenu(false);
    }
}

// ---- SLIP active loop (gateway running) ----

static void slipActiveLoop() {
    {   // how late this pass is, and how much the serial buffer holds
        unsigned long nowMs = millis();
        unsigned long gap = nowMs - gLastPassMs;
        gLastPassMs = nowMs;
        gStatsPasses++;
        if (gap > gMaxGapMs) gMaxGapMs = gap;
        if (gap > 50) gSlowPasses++;
        int backlog = PhysicalSerial.available();
        if (backlog > 0 && (uint32_t) backlog > gMaxBacklog) gMaxBacklog = (uint32_t) backlog;
        if (nowMs - gLastStatsMs >= 10000) {
            gLastStatsMs = nowMs;
            if (slipCtx.framesReceived != gStatFramesIn || slipCtx.framesSent != gStatFramesOut) {
                slipLogStats("10 s");
                gStatFramesIn = slipCtx.framesReceived; gStatFramesOut = slipCtx.framesSent;
            }
        }
    }

    // BACK stops the gateway and returns to the main menu: the gateway starts from there and has no menu of its own.
    if (ModeExit::backPressed()) {
        LOGI("SLIP: stopped by the BACK button");
        exitSlipMode();
        ModeExit::toMenu();
        return;
    }

    if (BTNUP || BTNDN || BTNEN) {
        waitSwitches();
        // nothing to select: repaint the status screen
        slipShowStatus();
    }

    // "+++" on the USB serial only: PhysicalSerial carries binary SLIP data that must not be read as commands
    if (ModeExit::escapeTyped()) {
        LOGI("SLIP: stopped by +++");
        exitSlipMode();
        ModeExit::toMenu();
        return;
    }

    // SLIP timeout (Config > Exit Timers): no traffic either way for that long (the Palm disconnected or never connected): stop the gateway and return to the main menu, as BACK. Frames sent to the Palm
    // (WiFi-side replies) count too (slip.h).
    if (!PhysicalSerial.available() &&
        millis() - slipLastActivityMs > (unsigned long) slipTimeoutMin * 60000UL) {
        LOGI("SLIP: no traffic for %u min - back to the main menu\r\n", (unsigned) slipTimeoutMin);
        exitSlipMode();
        msgFlag = true;
        mainMenu(false);
        return;
    }

    // incoming SLIP frames from PhysicalSerial only
    if (PhysicalSerial.available()) { slipLastActivityMs = millis(); gLastTrafficMs = slipLastActivityMs; }
    while (PhysicalSerial.available()) {
        uint8_t byte = PhysicalSerial.read();
        int frameLen = slipReceiveByte(&slipCtx, byte);

        if (frameLen > 0) {
            // slipCtx.framesReceived is reset by slipInit() on every enterSlipMode(), so == 1 is the first complete frame THIS session: a one-time "the vintage computer is talking to us" signal at the link
            // layer, before anything IP-level is parsed and apart from any TCP/UDP connection succeeding or failing.
            if (logOn(LOG_VERBOSE) && slipCtx.framesReceived == 1) {
                logPrintf(LOG_VERBOSE, "SLIP: *** Client link UP - first frame received from vintage computer ***\r\n");
            }
            if (logOn(LOG_VERBOSE)) {
                logPrintf(LOG_VERBOSE, "SLIP: Received frame, %d bytes\r\n", frameLen);
            }
            natProcessPacket(&natCtx, slipCtx.rxBuffer, frameLen);
        }
    }

    // poll NAT connections for data from the internet
    natPollConnections(&natCtx);

    // clean up expired connections periodically
    static unsigned long lastCleanup = 0;
    if (millis() - lastCleanup > 10000) {
        natCleanupExpired(&natCtx);
        lastCleanup = millis();
    }

    // WiFi was checked only once, at enterSlipMode(): a later drop (AP reboot, DHCP lease loss, weak signal) went unnoticed and every NAT forward just failed (now logging per packet, with no obvious
    // common cause). Poll it on a slow cadence and log only a STATE CHANGE, plus a periodic reminder while it stays down, so "can't resolve anything" traces to "the gateway's WiFi uplink is down", not a SLIP/NAT bug.
    static unsigned long lastWifiCheck = 0;
    static bool wifiWasConnected = true;  // assume true; enterSlipMode already verified it once
    static unsigned long wifiDownSince = 0;
    if (millis() - lastWifiCheck > 3000) {
        bool wifiConnected = (WiFi.status() == WL_CONNECTED);
        if (wifiConnected != wifiWasConnected) {
            if (logOn(LOG_WARN)) {
                if (!wifiConnected) {
                    logPrintf(LOG_WARN, "NAT: *** WiFi uplink LOST (status=%d) - client requests will fail until it reconnects ***\r\n",
                                  (int) WiFi.status());
                } else {
                    logPrintf(LOG_WARN, "NAT: WiFi uplink RESTORED, IP=%s\r\n", ipToString(WiFi.localIP()).c_str());
                }
            }
            if (!wifiConnected) wifiDownSince = millis();
            wifiWasConnected = wifiConnected;
        } else if (!wifiConnected && (millis() - wifiDownSince > 15000)) {
            if (logOn(LOG_VERBOSE)) {
                logPrintf(LOG_VERBOSE, "NAT: WiFi uplink still down (%lus)\r\n", (millis() - wifiDownSince) / 1000);
            }
        }
        lastWifiCheck = millis();
    }

    static unsigned long lastDisplayUpdate = 0;
    // The OLED takes about 100 ms over I2C and would hold up the line while frames flow, so it refreshes only after the Palm side has been quiet for a second.
    if (millis() - lastDisplayUpdate > 1000 && millis() - gLastTrafficMs > 1000) {
        slipUpdateActiveDisplay();
        lastDisplayUpdate = millis();
    }
}

// ---- Enter SLIP Gateway mode ----

void enterSlipMode() {
    // remember the previous mode to return to
    slipModeCtx.previousMenuMode = menuMode;

    menuMode = MODE_SLIP;

    // binary mode: SLIP needs a clean RS232 channel, no text output on PhysicalSerial
    setBinaryMode(true);

    SerialPrintLn("\r\nEntering SLIP Gateway Mode...");

    loadSlipSettings();

    slipInit(&slipCtx);

    natInit(&natCtx);

    natCtx.slipIP = slipModeCtx.config.slipIP;
    natCtx.clientIP = slipModeCtx.config.clientIP;
    natCtx.subnetMask = slipModeCtx.config.subnetMask;
    natCtx.dnsServer = slipModeCtx.config.dnsServer;

    // Connect to WiFi if not connected. waitForWiFiConnection() shows its own "Waiting for WiFi..." / "WiFi unavailable. Retry?" UI and retries for up to a minute; false means the user pressed BACK, so
    // undo the menuMode and binary-mode switch above and return to the main menu (SLIP's only entry) rather than leave menuMode on MODE_SLIP with nothing running.
    if (WiFi.status() != WL_CONNECTED) {
        if (!waitForWiFiConnection()) {
            SerialPrintLn("SLIP Gateway: WiFi not available - cancelled.");
            setBinaryMode(false);
            menuMode = slipModeCtx.previousMenuMode;
            mainMenu(false);
            return;
        }
    }

    slipModeCtx.state = SLIP_MODE_ACTIVE;
    slipLastActivityMs = millis();   // the timeout counts from here
    slipStatsReset();
    slipPaceInit(PhysicalSerial.baudRate());
    PhysicalSerial.onReceiveError(slipUartError);
    // At 115200 the Arduino core lets the UART's 128-byte FIFO fill to 120 bytes before it asks for a read (about 1 ms of slack); wake the reader at 32 so a busy moment of WiFi or flash work cannot overflow it.
    if (PhysicalSerial.baudRate() > 57600) PhysicalSerial.setRxFIFOFull(32);
    LOGI("SLIP: mode entered - gateway %s, client %s, WiFi %s\r\n", ipToString(natCtx.slipIP).c_str(),
         ipToString(natCtx.clientIP).c_str(), ipToString(WiFi.localIP()).c_str());

    SerialPrintLn("SLIP Gateway Active");
    SerialPrint("  Gateway IP: ");
    SerialPrintLn(ipToString(natCtx.slipIP));
    SerialPrint("  Client IP:  ");
    SerialPrintLn(ipToString(natCtx.clientIP));
    SerialPrint("  WiFi IP:    ");
    SerialPrintLn(ipToString(WiFi.localIP()));
    SerialPrintLn("\r\nPress +++ or BACK button to exit");

    slipShowStatus();
}

// ---- Exit SLIP Gateway mode ----

void exitSlipMode() {
    slipLogStats("exit");
    PhysicalSerial.onReceiveError(NULL);
    // binary mode off: text output to PhysicalSerial again
    setBinaryMode(false);

    SerialPrintLn("\r\nExiting SLIP Gateway Mode...");
    LOGI("SLIP: mode exit - %lu frame(s) in / %lu out, %lu byte(s) in / %lu out, %lu TCP, %lu UDP, %lu ping(s), %lu dropped\r\n",
         (unsigned long) slipCtx.framesReceived, (unsigned long) slipCtx.framesSent,
         (unsigned long) slipCtx.bytesReceived, (unsigned long) slipCtx.bytesSent,
         (unsigned long) natCtx.tcpConnections, (unsigned long) natCtx.udpSessions,
         (unsigned long) natCtx.icmpPackets, (unsigned long) natCtx.droppedPackets);

    // shut down NAT (closes all connections)
    natShutdown(&natCtx);

    slipReset(&slipCtx);

    slipModeCtx.state = SLIP_MODE_IDLE;

    SerialPrintLn("SLIP Gateway Stopped");
}

// ---- SLIP status on the display ----

void slipShowStatus() {
    display.clearDisplay();

    display.fillRect(0, 0, 128, 16, SSD1306_WHITE);
    display.drawLine(1, 1, 126, 1, SSD1306_BLACK);
    display.drawLine(1, 14, 126, 14, SSD1306_BLACK);
    display.drawLine(1, 1, 1, 14, SSD1306_BLACK);
    display.drawLine(126, 1, 126, 14, SSD1306_BLACK);

    display.setCursor(4, 4);
    display.setTextColor(SSD1306_BLACK);
    // "SLIP", not "SLIP Gateway", to leave room for the WiFi icon. The live baud (not the configured showHeader() value) is top right, as hotsync.cpp's hsDrawHeader() and sasd.cpp's sasdDrawStatusLines():
    // every title bar shows the active rate.
    {
      String line = "SLIP";
      display.print(line);
      display.print(padLeft(liveBaudStr(PhysicalSerial.baudRate()), 20 - line.length()));
    }
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(3, 19);
    display.print("GW: ");
    display.print(ipToString(natCtx.slipIP));

    display.setCursor(3, 29);
    display.print("CL: ");
    display.print(ipToString(natCtx.clientIP));

    display.setCursor(3, 39);
    display.print("WiFi: ");
    display.print(ipToString(WiFi.localIP()));

    display.setCursor(3, 53);
    display.print("TCP:");
    display.print(natGetActiveTcpCount(&natCtx));
    display.print(" UDP:");
    display.print(natGetActiveUdpCount(&natCtx));

    display.display();
    drawWifiStatusIcon();  // WiFi status on every title bar
    lastMenu = -1;  // Force menu redraw on exit
}

// ---- Update the active display (called periodically) ----

static void slipUpdateActiveDisplay() {
    // only the connection counts
    display.fillRect(3, 53, 124, 10, SSD1306_BLACK);
    display.setCursor(3, 53);
    display.print("TCP:");
    display.print(natGetActiveTcpCount(&natCtx));
    display.print(" UDP:");
    display.print(natGetActiveUdpCount(&natCtx));
    display.print(" ");
    display.print(natCtx.packetsToInternet + natCtx.packetsFromInternet);
    display.print("pkt");
    display.display();
}

// ---- Load SLIP settings from EEPROM ----

void loadSlipSettings() {
    uint8_t ip[4];

    for (int i = 0; i < 4; i++) {
        ip[i] = EEPROM.read(SLIP_IP_ADDRESS + i);
    }
    if (ip[0] == 255 || ip[0] == 0) {
        // invalid: use the default
        slipModeCtx.config.slipIP = IPAddress(192, 168, 7, 1);
    } else {
        slipModeCtx.config.slipIP = IPAddress(ip[0], ip[1], ip[2], ip[3]);
    }

    for (int i = 0; i < 4; i++) {
        ip[i] = EEPROM.read(SLIP_CLIENT_IP_ADDRESS + i);
    }
    if (ip[0] == 255 || ip[0] == 0) {
        slipModeCtx.config.clientIP = IPAddress(192, 168, 7, 2);
    } else {
        slipModeCtx.config.clientIP = IPAddress(ip[0], ip[1], ip[2], ip[3]);
    }

    for (int i = 0; i < 4; i++) {
        ip[i] = EEPROM.read(SLIP_SUBNET_ADDRESS + i);
    }
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) {
        slipModeCtx.config.subnetMask = IPAddress(255, 255, 255, 0);
    } else if (ip[0] == 0) {
        slipModeCtx.config.subnetMask = IPAddress(255, 255, 255, 0);
    } else {
        slipModeCtx.config.subnetMask = IPAddress(ip[0], ip[1], ip[2], ip[3]);
    }

    for (int i = 0; i < 4; i++) {
        ip[i] = EEPROM.read(SLIP_DNS_ADDRESS + i);
    }
    if (ip[0] == 255 || ip[0] == 0) {
        slipModeCtx.config.dnsServer = IPAddress(8, 8, 8, 8);
    } else {
        slipModeCtx.config.dnsServer = IPAddress(ip[0], ip[1], ip[2], ip[3]);
    }

    slipModeCtx.configChanged = false;
}

// ---- Save SLIP settings to EEPROM ----

void saveSlipSettings() {
    for (int i = 0; i < 4; i++) {
        EEPROM.write(SLIP_IP_ADDRESS + i, slipModeCtx.config.slipIP[i]);
    }

    for (int i = 0; i < 4; i++) {
        EEPROM.write(SLIP_CLIENT_IP_ADDRESS + i, slipModeCtx.config.clientIP[i]);
    }

    for (int i = 0; i < 4; i++) {
        EEPROM.write(SLIP_SUBNET_ADDRESS + i, slipModeCtx.config.subnetMask[i]);
    }

    for (int i = 0; i < 4; i++) {
        EEPROM.write(SLIP_DNS_ADDRESS + i, slipModeCtx.config.dnsServer[i]);
    }

    EEPROM.commit();
    slipModeCtx.configChanged = false;
}

// ---- SLIP setup (Config menu): Gateway IP, Client IP, DNS Server ----

// One address prompt, pre-filled with the current value; digits and '.' only. False if the user cancelled (the caller saves nothing). An entry that is not a usable address is refused and asked again.
static bool slipPromptIP(const char *title, IPAddress& ip) {
    char buf[16];
    while (true) {
        String cur = ipToString(ip);
        strncpy(buf, cur.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (!charEntryPrompt(title, buf, 15, "0123456789."))
            return false;

        IPAddress entered;
        // A first octet of 0 or 255 is what loadSlipSettings() reads as "not set" and silently replaces with the default, so refuse it here.
        if (parseIPAddress(String(buf), entered) && entered[0] != 0 && entered[0] != 255) {
            ip = entered;
            return true;
        }
        SerialPrintLn("\r\nInvalid IP address");
        showMessage("\n  Invalid IP");
        delay(1500);
    }
}

// Called from the Config menu (settingsLoop(), display_menu.cpp), which redraws itself afterwards. The gateway cannot be running (it is left only via BACK to the main menu) and reads these values each time it starts (enterSlipMode()).
void slipSetup() {
    loadSlipSettings();
    IPAddress gateway = slipModeCtx.config.slipIP;
    IPAddress client = slipModeCtx.config.clientIP;
    IPAddress dns = slipModeCtx.config.dnsServer;

    if (!slipPromptIP("Gateway IP", gateway)) return;
    while (true) {
        IPAddress entered = client;
        if (!slipPromptIP("Client IP", entered)) return;
        if (!(entered == gateway)) {
            client = entered;
            break;
        }
        SerialPrintLn("\r\nClient IP must differ from Gateway IP");
        showMessage("\nClient must\ndiffer from GW");
        delay(1500);
    }
    if (!slipPromptIP("DNS Server", dns)) return;

    if (gateway == slipModeCtx.config.slipIP && client == slipModeCtx.config.clientIP &&
        dns == slipModeCtx.config.dnsServer) {
        SerialPrintLn("\r\nSLIP settings unchanged");
        return;
    }

    slipModeCtx.config.slipIP = gateway;
    slipModeCtx.config.clientIP = client;
    slipModeCtx.config.dnsServer = dns;
    saveSlipSettings();

    SerialPrintLn("\r\nSLIP settings saved:");
    SerialPrint("  Gateway IP: ");
    SerialPrintLn(ipToString(gateway));
    SerialPrint("  Client IP:  ");
    SerialPrintLn(ipToString(client));
    SerialPrint("  DNS Server: ");
    SerialPrintLn(ipToString(dns));

    showMessage("\n SLIP config\n    saved");
    delay(1500);
}

// ---- Parse an IP address from a string ----

static bool parseIPAddress(const String& str, IPAddress& ip) {
    int parts[4];
    int partIdx = 0;
    int num = 0;

    for (int i = 0; i <= str.length() && partIdx < 4; i++) {
        char c = (i < str.length()) ? str[i] : '.';
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
        } else if (c == '.') {
            if (num > 255) return false;
            parts[partIdx++] = num;
            num = 0;
        } else {
            return false;
        }
    }

    if (partIdx != 4) return false;

    ip = IPAddress(parts[0], parts[1], parts[2], parts[3]);
    return true;
}
