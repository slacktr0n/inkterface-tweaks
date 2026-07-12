// vim: foldmethod=marker:foldmarker={{{,}}}
#include <iomanip>
#include <limits>
#include <sstream>

#include <GxEPD2_BW.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>

#if defined(GABEN_STARTUP)
#include "gaben.h"
#endif

#define SERVICE_UUID                                                                               \
    NimBLEUUID { "95c7b479-8e84-4ce7-a121-faf74bf48c84" }
#define TOPLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871900" }
#define MIDLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871901" }
#define BOTLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871902" }
#define KEYVAL_UUID                                                                                \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871903" }
#define VECTOR_UUID                                                                                \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871904" }
#define FLUSH_UUID                                                                                 \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a8719FF" }

#define SPARKBOX_HEIGHT 100
#define SPARKBOX_WIDTH 209

#define INTERFACE_VERSION "IFv01"

// minimum voltage battery can reach before we go into deep sleep
#define BATT_MINV 2.9

NimBLEServer *BLE_SERVER = nullptr;
std::string BLE_NAME = "INKTF";

bool INVERTED = false;
#define FG_COLOR (INVERTED ? GxEPD_WHITE : GxEPD_BLACK)
#define BG_COLOR (INVERTED ? GxEPD_BLACK : GxEPD_WHITE)

class DualPrint : public Print
{ // {{{
  public:
    DualPrint(Print &a, Print &b)
        : _a(a)
        , _b(b)
    {
    }
    size_t write(uint8_t c) override
    {
        _a.write(c);
        return _b.write(c);
    }
    size_t write(const uint8_t *buffer, size_t size) override
    {
        _a.write(buffer, size);
        return _b.write(buffer, size);
    }

  private:
    Print &_a;
    Print &_b;
}; // }}}
DualPrint Debug(Serial, Serial1);

// GxEPD2 driver for the 5.83" 648x480 UC8179 panel (GDEW0583T8 class).
// This replaces the ThinkInk driver + hand-rolled partial refresh: GxEPD2
// ships tested fast-partial-update waveforms (LUTs) for this panel, so
// partial refreshes are differential and flash-free.
// Full-frame buffer: 648*480/8 = 38,880 bytes, held in ESP32 RAM.
GxEPD2_BW<GxEPD2_583_T8, GxEPD2_583_T8::HEIGHT>
    MF_DISPLAY(GxEPD2_583_T8(/*CS=*/EPD_CS, /*DC=*/EPD_DC,
                             /*RST=*/EPD_RESET, /*BUSY=*/EPD_BUSY));

static unsigned long DISP_DEBOUNCE = 0;

struct Point { // {{{
    float x;
    float y;

    Point()
        : x(0)
        , y(0)
    {
    }
    Point(float _x, float _y)
        : x(_x)
        , y(_y)
    {
    }
}; // }}}

struct Points { // {{{
    float yMin;
    float yMax;
    std::vector<Point> points;

    Points()
        : yMin(0)
        , yMax(0)
        , points()
    {
    }

    void clear()
    {
        yMin = 0;
        yMax = 0;
        points.clear();
    }
}; // }}}

struct KeyVal { // {{{
    std::string key;
    std::string val;

    KeyVal()
        : key{""}
        , val{""}
    {
    }
}; // }}}
typedef std::vector<KeyVal> KeyVals;

struct State { // {{{
    bool connected = false;
    std::string topLine{"Starting up..."};
    std::string midLine{"No User"};
    std::string botLine{"No Activity"};
    std::string hostMsg{""};
    std::string battLine{""};

    KeyVals keyvals{9};
    std::vector<Points> sparks{6};

    void reset()
    {
        keyvals.clear();
        keyvals.resize(9);
        sparks.clear();
        sparks.resize(6);

        uint32_t addr = (uint64_t)NimBLEDevice::getAddress() & 0xFFFFFF;
        std::stringstream name;
        name << "INKTF-";
        name << std::uppercase << std::hex << std::setfill('0') << std::setw(6) << addr;
        BLE_NAME = name.str();

        connected = false;
        topLine = "Waiting on connection...";
        midLine = BLE_NAME;
        botLine = "";
        hostMsg = "";
        keyvals[0].key = "OS";
        keyvals[0].val = "--";
        keyvals[1].key = "BIOS";
        keyvals[1].val = "--";
        keyvals[2].key = "STEAM";
        keyvals[2].val = "--";
        keyvals[3].key = "CPU";
        keyvals[3].val = "-- dC";
        keyvals[4].key = "GPU";
        keyvals[4].val = "-- dC";
        keyvals[5].key = "FAN";
        keyvals[5].val = "-- RPM";
        keyvals[6].key = "CPU";
        keyvals[6].val = "--%";
        keyvals[7].key = "GPU";
        keyvals[7].val = "--%";
        keyvals[8].key = "MEM";
        keyvals[8].val = "--%";
    }
} STATE; // }}}

void drawStatic();

class ServerCallbacks : public NimBLEServerCallbacks
{ // {{{
    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn) override
    {
        Debug.println("got connection");
        // we don't want any other devices to see us once we are connected
        // to a host
        NimBLEDevice::stopAdvertising();
        STATE.connected = true;
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &conn, int reason) override
    {
        Debug.print("got disconnect event, connected count: ");
        Debug.println(server->getConnectedCount());
        // connected count appears to be updated after this callback is
        // triggered, so the count will be at least 1 higher than reality
        if (server->getConnectedCount() <= 1) {
            if (STATE.connected) {
                DISP_DEBOUNCE = 100;
            }
            STATE.reset();
        }
        NimBLEDevice::startAdvertising();
    }
} SERVER_CALLBACKS; // }}}

class StatusLineCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        auto uuid = characteristic->getUUID();
        if (uuid == TOPLINE_UUID && STATE.topLine != value) {
            STATE.topLine = value;
        } else if (uuid == MIDLINE_UUID && STATE.midLine != value) {
            STATE.midLine = value;
        } else if (uuid == BOTLINE_UUID && STATE.botLine != value) {
            STATE.botLine = value;
        } else if (uuid != TOPLINE_UUID && uuid != MIDLINE_UUID && uuid != BOTLINE_UUID) {
            Debug.print("Got value (");
            Debug.print(value.c_str());
            Debug.print(") for unknown UUID (");
            Debug.print(uuid.toString().c_str());
            Debug.println("), ignoring.");
            return;
        }
    }
} STATUS_CALLBACKS; // }}}

class KeyValCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    typedef struct __attribute__((packed)) {
        uint8_t index;
        char key[32];
        char val[32];
    } Msg;

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        Msg msg;
        if (value.length() == sizeof(Msg)) {
            memcpy(&msg, value.data(), sizeof(Msg));
            STATE.keyvals[msg.index].key = msg.key;
            STATE.keyvals[msg.index].val = msg.val;
        } else {
            Debug.print("got bad keyval write, size: ");
            Debug.println(value.length());
        }
    }
} KEYVAL_CALLBACKS; // }}}

class VectorCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    typedef struct __attribute__((packed)) {
        uint8_t index;
        uint8_t count;
        float minVal;
        float maxVal;
        uint8_t values[32 * 2]; // 32 (x, y) pairs, 64 bytes
        // total 74 bytes
    } Msg;

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        Msg msg;
        if (value.length() >= 2) {
            memcpy(&msg, value.data(), sizeof(Msg));
            Debug.print("got vector for index (");
            Debug.print(msg.index);
            Debug.print(") with ");
            Debug.print(msg.count);
            Debug.print(" values, min ");
            Debug.print(msg.minVal);
            Debug.print(", max ");
            Debug.println(msg.maxVal);
            STATE.sparks[msg.index].clear();
            STATE.sparks[msg.index].yMin = msg.minVal;
            STATE.sparks[msg.index].yMax = msg.maxVal;
            for (int i = 0; i < msg.count; i += 2) {
                STATE.sparks[msg.index].points.emplace_back(msg.values[i] / 255.0,
                                                            msg.values[i + 1] / 255.0);
            }
        } else {
            Debug.print("got bad vectors write, size: ");
            Debug.println(value.length());
        }
    }
} VECTOR_CALLBACKS; // }}}

class FlushCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        STATE.hostMsg = characteristic->getValue();
        DISP_DEBOUNCE = 100;
    }
} FLUSH_CALLBACKS; // }}}

void setup()
{ // {{{
    Serial.begin(115200);
    Serial1.begin(115200);
    // these usb serial and rx/tx interfaces are combined in the Debug instance

#if defined(STARTUP_DELAY_MS)
    delay(STARTUP_DELAY_MS);
#endif

    // Feather V2: no MAX17048 fuel gauge; battery is read via the
    // onboard voltage divider on A13 (GPIO 35) in loop() instead.

    Debug.println("setting up ble device and service");
    NimBLEDevice::init("");
    NimBLEDevice::setPower(2); // we don't need much power
    NimBLEDevice::setMTU(256); // bump the mtu to fit a decent number of points
    BLE_SERVER = NimBLEDevice::createServer();
    BLE_SERVER->setCallbacks(&SERVER_CALLBACKS);
    BLEService *service = BLE_SERVER->createService(SERVICE_UUID);
    BLECharacteristic *characteristic = nullptr;

    // status line characteristics, they share callbacks
    characteristic =
        service->createCharacteristic(TOPLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.topLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);
    characteristic =
        service->createCharacteristic(MIDLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.midLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);
    characteristic =
        service->createCharacteristic(BOTLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.botLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);

    characteristic = service->createCharacteristic(KEYVAL_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&KEYVAL_CALLBACKS);
    characteristic = service->createCharacteristic(VECTOR_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&VECTOR_CALLBACKS);

    characteristic = service->createCharacteristic(FLUSH_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&FLUSH_CALLBACKS);

    BLE_SERVER->start();

    Debug.println("initializing display");
    pinMode(EPD_EN, OUTPUT);
    digitalWrite(EPD_EN, HIGH);
    delay(10); // let the breakout's power rail settle before init
    STATE.reset();
    MF_DISPLAY.init(0); // pass 115200 instead of 0 for driver diagnostics
    MF_DISPLAY.setRotation(0);
    MF_DISPLAY.setFullWindow();
    MF_DISPLAY.fillScreen(BG_COLOR);
#if defined(GABEN_STARTUP)
    MF_DISPLAY.drawXBitmap(0, 0, GABEN_BITS, GABEN_WIDTH, GABEN_HEIGHT, FG_COLOR);
#else
    drawStatic();
#endif
    MF_DISPLAY.display(false); // full refresh on boot
    DISP_DEBOUNCE = 10;

    Debug.println("starting ble advert");
    uint32_t addr = (uint64_t)NimBLEDevice::getAddress() & 0xFFFFFF;
    std::stringstream name;
    name << "INKTF-";
    name << std::uppercase << std::hex << std::setfill('0') << std::setw(6) << addr;
    BLE_NAME = name.str();
    BLEAdvertising *advert = NimBLEDevice::getAdvertising();
    BLEAdvertisementData ad_data{};
    ad_data.setName(BLE_NAME);
    ad_data.setManufacturerData("\x5d\x05" INTERFACE_VERSION);
    advert->setAdvertisementData(ad_data);
    advert->addServiceUUID(SERVICE_UUID);
    advert->enableScanResponse(false);
    NimBLEDevice::startAdvertising();

#if defined(GABEN_STARTUP)
    // just delaying here so folks can look at gabe for a bit
    Debug.println("observing gabe");
    delay(2000);
#endif
} // }}}

void loop()
{ // {{{
    static unsigned long LAST_MS = 0;
    static unsigned long CONN_DEBOUNCE = 5000;
    static unsigned long BATT_DEBOUNCE = 1000;
    static float last_battp = 0;
    static float last_battv = 0;

    auto now = millis();
    auto delta = now - LAST_MS;
    if (now < LAST_MS) {
        // handling rollover
        Debug.println("handling time rollover");
        delta = (std::numeric_limits<unsigned long>::max() - LAST_MS) + now;
    }

    if (CONN_DEBOUNCE > 0 && CONN_DEBOUNCE > delta) {
        CONN_DEBOUNCE -= delta;
    } else if (CONN_DEBOUNCE > 0) {
        // NOTE: this should be handled in our SERVER_CALLBACKS but we don't
        //       always seem to get the onDisconnect() callback and get stuck
        //       with advertising stopped, so might as well just check here...
        bool advertising = NimBLEDevice::getAdvertising()->isAdvertising();
        uint8_t connections = BLE_SERVER->getConnectedCount();
        if (!advertising && connections == 0) {
            Debug.print("starting advertisement, we have no connections");
            NimBLEDevice::startAdvertising();
        } else if (advertising && connections > 0) {
            Debug.print("stopping advertisement, we have connections");
            NimBLEDevice::stopAdvertising();
        }
        CONN_DEBOUNCE = 5000;
    }

    if (BATT_DEBOUNCE > 0 && BATT_DEBOUNCE > delta) {
        BATT_DEBOUNCE -= delta;
    } else if (BATT_DEBOUNCE > 0) {
        // Feather V2: read battery through the onboard divider (halves VBAT)
        float battv = analogReadMilliVolts(A13) * 2.0 / 1000.0;
        float battp = constrain((battv - 3.3) / (4.2 - 3.3) * 100.0, 0.0, 100.0); // rough estimate
        // 0.05 V threshold (was 0.01 for the fuel gauge) so ADC noise
        // doesn't trigger constant e-ink refreshes
        if (abs(battp - last_battp) > 1 || abs(battv - last_battv) > 0.05) {
            last_battp = battp;
            last_battv = battv;
            Debug.print("batt: ");
            Debug.print(battp, 1);
            Debug.print("% total, ");
            Debug.print(battv, 2);
            Debug.println(" V");
            std::stringstream line;
            line << std::fixed << std::setprecision(2) << battv << " V";
            STATE.battLine = line.str();
            DISP_DEBOUNCE = 10;
        }
        BATT_DEBOUNCE = 1000;
    }

    if (last_battv < BATT_MINV) {
        Debug.println("drawing low battery message");
        DISP_DEBOUNCE = 0;
        MF_DISPLAY.setFullWindow();
        MF_DISPLAY.fillScreen(BG_COLOR);
        drawLowBatt();
        MF_DISPLAY.display(false); // full refresh
        MF_DISPLAY.hibernate();
        Debug.println("going into deep sleep");
        // esp_sleep_enable_timer_wakeup(10 * 1000000ULL);
        esp_deep_sleep_start();
        goto loop_end;
    }

    if (DISP_DEBOUNCE > 0 && DISP_DEBOUNCE > delta) {
        DISP_DEBOUNCE -= delta;
    } else if (DISP_DEBOUNCE > 0) {
        Debug.println("drawing to display");
        DISP_DEBOUNCE = 0;
        MF_DISPLAY.setFullWindow();
        MF_DISPLAY.fillScreen(BG_COLOR);
        drawStatic();
#if defined(PARTIAL_REFRESH)
// minutes between cleansing full (flashing) refreshes; all updates in
// between are flash-free partials (override via build_flags, e.g.
// '-D FULL_REFRESH_INTERVAL_MIN=10')
#ifndef FULL_REFRESH_INTERVAL_MIN
#define FULL_REFRESH_INTERVAL_MIN 30
#endif
        static unsigned long last_full_ms = 0; // boot full refresh in setup()
        const unsigned long full_interval_ms =
            FULL_REFRESH_INTERVAL_MIN * 60UL * 1000UL;
        if (STATE.connected && (millis() - last_full_ms) < full_interval_ms) {
            Debug.println("fast partial refresh");
            MF_DISPLAY.display(true);
#if defined(INVERT_PULSE_CLEANUP)
        } else if (STATE.connected) {
            // interval lapsed: instead of the flashing full refresh, scrub
            // ghosting by driving every pixel to its opposite state and
            // back using two partial refreshes. Looks like a brief
            // negative-image blink; quieter than the inversion flash and
            // nearly as effective on ghosting. True full refreshes still
            // happen on boot and on host disconnect, which keeps the
            // panel's DC-balanced waveform hygiene in the rotation.
            last_full_ms = millis();
            Debug.println("invert-pulse cleanup");
            INVERTED = !INVERTED;
            MF_DISPLAY.fillScreen(BG_COLOR);
            drawStatic();
            MF_DISPLAY.display(true);
            INVERTED = !INVERTED;
            MF_DISPLAY.fillScreen(BG_COLOR);
            drawStatic();
            MF_DISPLAY.display(true);
#endif
        } else {
            last_full_ms = millis();
            Debug.println("full refresh (cleanup)");
            MF_DISPLAY.display(false);
        }
#else
        MF_DISPLAY.display(false);
#endif
        MF_DISPLAY.powerOff();
        Debug.println("drew to display");
    }

    // Debug.println("entering light sleep");
    // esp_sleep_enable_timer_wakeup(10 * 1000ULL);
    // esp_err_t err = esp_light_sleep_start();
    // Debug.println("woke from light sleep");
    // Debug.print("After sleep, err=%s\n", esp_err_to_name(err));
    // Debug.print("Wake reason=%d\n", esp_sleep_get_wakeup_cause());
    // delay(1);

loop_end:
    LAST_MS = now;
    delay(10);
} // }}}

void drawText(const char *text, const int16_t &x = -1, const int16_t &y = -1,
              const uint8_t &size = 1, const bool &wrap = false)
{ // {{{
    if (x >= 0 && y >= 0) {
        MF_DISPLAY.setCursor(x, y);
    }
    MF_DISPLAY.setTextSize(size);
    MF_DISPLAY.setTextColor(FG_COLOR);
    MF_DISPLAY.setTextWrap(wrap);
    MF_DISPLAY.print(text);
} // }}}

void drawLogo(int16_t &x, const int16_t &y = 0)
{ // {{{
    MF_DISPLAY.fillRoundRect(x, y, 101, 101, 3, FG_COLOR);
    MF_DISPLAY.fillCircle(x + 50, y + 50, 31, BG_COLOR);
    MF_DISPLAY.fillCircle(x + 50, y + 50, 23, FG_COLOR);
    x += 101;
} // }}}

void drawSparkbox(int16_t &x, const int16_t &y, std::string &title, const std::string &value,
                  const Points &points)
{ // {{{
    const int16_t w = SPARKBOX_WIDTH;
    const int16_t h = SPARKBOX_HEIGHT;
    const int16_t hpad = 8;
    const int16_t vpad = 6;
    const int16_t title_h = 26;
    const int16_t graph_h = (h - title_h) - 32;
    const int16_t graph_w = w - 20;
    const int16_t graph_x = x + 10;
    const int16_t graph_y = (y + h) - 16;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        MF_DISPLAY.fillRect(x, y + title_h, w, 1, FG_COLOR);
        drawText(title.c_str(), x + hpad, y + vpad, 2);
        drawText(value.c_str(), (x + (w - hpad)) - (12 * strlen(value.c_str())), y + vpad, 2);

        std::stringstream maxstrm;
        maxstrm << std::fixed << std::setprecision(0) << points.yMax;
        auto maxstr = maxstrm.str();
        drawText(maxstr.c_str(), x + hpad, y + title_h + vpad);

        std::stringstream minstrm;
        minstrm << std::fixed << std::setprecision(0) << points.yMin;
        auto minstr = minstrm.str();
        drawText(minstr.c_str(), x + hpad, y + h - (vpad + 7));

        if (points.points.size() >= 2) {
            int16_t s_x = 0.0, s_y = 0.0, e_x = 0.0, e_y = 0.0;
            for (auto p = points.points.cbegin(); p != points.points.cend() - 1; ++p) {
                s_x = graph_x + (p->x * graph_w);
                e_x = graph_x + ((p + 1)->x * graph_w);
                s_y = graph_y + (p->y * graph_h * -1.0);
                e_y = graph_y + ((p + 1)->y * graph_h * -1.0);
                MF_DISPLAY.drawLine(s_x, s_y, e_x, e_y, FG_COLOR);
                MF_DISPLAY.drawLine(s_x, s_y - 1, e_x, e_y - 1, FG_COLOR);
                MF_DISPLAY.drawLine(s_x, s_y + 1, e_x, e_y + 1, FG_COLOR);
                MF_DISPLAY.drawLine(s_x - 1, s_y, e_x - 1, e_y, FG_COLOR);
                MF_DISPLAY.drawLine(s_x + 1, s_y, e_x + 1, e_y, FG_COLOR);
            }
        }
    }

    x += w;
} // }}}

void drawDiscreteBox(int16_t &x, const int16_t &y, const std::string &title,
                     const std::string &value)
{ // {{{
    const int16_t w = 209;
    const int16_t h = 26;
    const int16_t hpad = 8;
    const int16_t vpad = 6;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        drawText(title.c_str(), x + hpad, y + vpad, 2);
        drawText(value.c_str(), (x + (w - hpad)) - (12 * strlen(value.c_str())), y + vpad, 2);
    }

    x += w;
} // }}}

void drawStatic()
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    // fremont logo in top left corner
    x = 5;
    y = 5;
    drawLogo(x, y);

    // show connected fremont hostname/serial or connecting status
    x = 120;
    y = 15;
    drawText(STATE.topLine.c_str(), x, y, 3);
    y += 35;
    drawText(STATE.midLine.c_str(), x, y, 2);
    y += 30;
    drawText(STATE.botLine.c_str(), x, y, 2);

    // first row of boxes with no sparklines
    x = 5;
    y = 115;
    drawDiscreteBox(x, y, STATE.keyvals[0].key, STATE.keyvals[0].val);
    x += 5;
    drawDiscreteBox(x, y, STATE.keyvals[1].key, STATE.keyvals[1].val);
    x += 5;
    drawDiscreteBox(x, y, STATE.keyvals[2].key, STATE.keyvals[2].val);

    // second row
    x = 5;
    y += 26 + 5;
    drawSparkbox(x, y, STATE.keyvals[3].key, STATE.keyvals[3].val, STATE.sparks[0]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[4].key, STATE.keyvals[4].val, STATE.sparks[1]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[5].key, STATE.keyvals[5].val, STATE.sparks[2]);

    // third row
    x = 5;
    y += SPARKBOX_HEIGHT + 5;
    drawSparkbox(x, y, STATE.keyvals[6].key, STATE.keyvals[6].val, STATE.sparks[3]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[7].key, STATE.keyvals[7].val, STATE.sparks[4]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[8].key, STATE.keyvals[8].val, STATE.sparks[5]);

    // battery state
    x = 5;
    y = MF_DISPLAY.height() - 24;
    drawText(STATE.battLine.c_str(), x, y);

    // version tag
    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    x = 5;
    y = MF_DISPLAY.height() - 12;
    drawText(tag.str().c_str(), x, y);

    // host message if provided (usually a timestamp)
    x = MF_DISPLAY.width() - (6 * strlen(STATE.hostMsg.c_str())) - 5;
    drawText(STATE.hostMsg.c_str(), x, y);
} // }}}

void drawLowBatt()
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    x = 5;
    y = 15;
    drawText("Please charge the battery!", x, y, 3);
    y += 35;
    drawText("Once charged, press reset on the back!", x, y, 2);

    // battery state
    x = 5;
    y = MF_DISPLAY.height() - 24;
    drawText(STATE.battLine.c_str(), x, y);

    // version tag
    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    x = 5;
    y = MF_DISPLAY.height() - 12;
    drawText(tag.str().c_str(), x, y);

    // host message if provided (usually a timestamp)
    x = MF_DISPLAY.width() - (6 * strlen(STATE.hostMsg.c_str())) - 5;
    drawText(STATE.hostMsg.c_str(), x, y);
} // }}}
