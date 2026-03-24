#include "automation.h"
#include "config.h"
#include "device_manager.h"
#include "fan_control.h"
#include "ntp_time.h"
#include "compat.h"
#include <time.h>

// ── Iekšējie dati ───────────────────────────────────────────
static AutoMode  modes[FAN_COUNT];
static AutoMode  prevModes[FAN_COUNT];         // režīms pirms SERVER (fallback)
static uint8_t   schedule[FAN_COUNT][SCHEDULE_SIZE];
static TempCurve tempCurves[FAN_COUNT];
static uint32_t  lastAutoRun = 0;
static uint8_t   lastAppliedSpeed[FAN_COUNT] = {255, 255};

// Server mode
static uint8_t   serverSpeed[FAN_COUNT] = {0, 0};
static uint32_t  lastServerHeartbeat = 0;
#define SERVER_TIMEOUT_MS  120000   // 2 minūtes bez heartbeat → fallback

// ── NVS atslēgas ────────────────────────────────────────────
static const char* modeKeys[FAN_COUNT]  = { "auto_mode0", "auto_mode1" };
static const char* schKeys[FAN_COUNT]   = { "auto_sch0",  "auto_sch1" };
static const char* tcKeys[FAN_COUNT]    = { "auto_tc0",   "auto_tc1" };

// ── Palīgfunkcija: nolasīt temperatūru no sensora ──────────
static float readSensorTemp(const TempCurve& tc) {
    switch (tc.sensorType) {
        case 0: {  // DS18B20
            if (!devIsEnabled(DEV_DS18B20)) return -127.0;
            const DS18B20Sensor* s = devDS18B20Get(tc.sensorIdx);
            if (!s || !s->valid) return -127.0;
            return s->temp;
        }
        case 1:  // DHT22
            if (!devIsEnabled(DEV_DHT22)) return -127.0;
            return devGetDHTTemp();
        default:
            return -127.0;
    }
}

// ── Tekošais nedēļas dienas + slota indekss ─────────────────
static bool getCurrentSlot(uint8_t& day, uint8_t& slot) {
    if (!ntpIsSynced()) return false;

    struct tm t;
    if (!getLocalTime(&t, 0)) return false;

    // tm_wday: 0=svētdiena, 1=pirmdiena ... 6=sestdiena
    // Mums vajag: 0=pirmdiena ... 6=svētdiena
    day = (t.tm_wday == 0) ? 6 : (t.tm_wday - 1);
    slot = t.tm_hour * 2 + (t.tm_min >= 30 ? 1 : 0);

    return true;
}

// ═══════════════════════════════════════════════════════════
//  PUBLISKĀS FUNKCIJAS
// ═══════════════════════════════════════════════════════════

void autoInit() {
    Preferences p;
    p.begin(NVS_NAMESPACE, true);

    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        // Mode
        modes[i] = (AutoMode)p.getUChar(modeKeys[i], AUTO_MANUAL);
        if (modes[i] > AUTO_SERVER) modes[i] = AUTO_MANUAL;
        // Ja pēc restarta bija SERVER režīmā — atgriezties uz MANUAL
        if (modes[i] == AUTO_SERVER) modes[i] = AUTO_MANUAL;
        prevModes[i] = modes[i];

        // Schedule — 336 baiti blob
        size_t len = p.getBytesLength(schKeys[i]);
        if (len == SCHEDULE_SIZE) {
            p.getBytes(schKeys[i], schedule[i], SCHEDULE_SIZE);
        } else {
            memset(schedule[i], 0, SCHEDULE_SIZE);  // visi sloti = 0%
        }

        // Temp curve
        len = p.getBytesLength(tcKeys[i]);
        if (len == sizeof(TempCurve)) {
            p.getBytes(tcKeys[i], &tempCurves[i], sizeof(TempCurve));
        } else {
            tempCurves[i] = { 0, 0, 20.0, 35.0, 0, 100 };  // default
        }

        DBG("[AUTO] Fan%d: mode=%s\n", i + 1, autoModeStr(modes[i]));
    }

    p.end();
}

void autoLoop() {
    uint32_t now = millis();
    if (now - lastAutoRun < 1000) return;  // reizi sekundē
    lastAutoRun = now;

    // ── Server heartbeat timeout pārbaude ───────────────────
    if (lastServerHeartbeat > 0 && (now - lastServerHeartbeat > SERVER_TIMEOUT_MS)) {
        // Serveris nav atsaucies — fallback uz iepriekšējo režīmu
        for (uint8_t i = 0; i < FAN_COUNT; i++) {
            if (modes[i] == AUTO_SERVER) {
                DBG("[AUTO] Fan%d: server timeout, fallback -> %s\n", i + 1, autoModeStr(prevModes[i]));
                modes[i] = prevModes[i];
                lastAppliedSpeed[i] = 255;  // piespiest recalc
            }
        }
        lastServerHeartbeat = 0;
    }

    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (!fanIsActive(i)) continue;
        if (modes[i] == AUTO_MANUAL) continue;

        // Ja automatizācija aktīva — noņemt manualOff
        if (fanIsManualOff(i)) {
            fanSetManualOff(i, false);
            DBG("[AUTO] Fan%d: manualOff cleared by automation\n", i + 1);
        }

        uint8_t targetSpeed = 255;

        // SERVER režīms — izmanto servera iestatīto ātrumu
        if (modes[i] == AUTO_SERVER) {
            targetSpeed = serverSpeed[i];
        }
        else if (modes[i] == AUTO_SCHEDULE) {
            uint8_t day, slot;
            if (getCurrentSlot(day, slot)) {
                targetSpeed = schedule[i][day * SLOTS_PER_DAY + slot];
            } else {
                // Debug: kāpēc nav slota
                static uint32_t lastNtpWarn = 0;
                if (now - lastNtpWarn > 10000) {
                    DBG("[AUTO] Fan%d: SCHEDULE - nav NTP sync!\n", i + 1);
                    lastNtpWarn = now;
                }
            }
        }
        else if (modes[i] == AUTO_TEMP) {
            float temp = readSensorTemp(tempCurves[i]);
            if (temp > -100) {  // valid reading
                TempCurve& tc = tempCurves[i];
                if (temp <= tc.tempMin) {
                    targetSpeed = tc.speedMin;
                } else if (temp >= tc.tempMax) {
                    targetSpeed = tc.speedMax;
                } else {
                    // Lineāra interpolācija
                    float ratio = (temp - tc.tempMin) / (tc.tempMax - tc.tempMin);
                    targetSpeed = tc.speedMin + (uint8_t)(ratio * (tc.speedMax - tc.speedMin));
                }
            }
        }

        // Ja aprēķināts un atšķiras no pēdējā — piemērot
        if (targetSpeed != 255 && targetSpeed != lastAppliedSpeed[i]) {
            fanSetSpeedAuto(i, targetSpeed);
            lastAppliedSpeed[i] = targetSpeed;
            DBG("[AUTO] Fan%d -> %d%% (%s)\n",
                i + 1, targetSpeed, autoModeStr(modes[i]));
        }
    }
}

AutoMode autoGetMode(uint8_t fanId) {
    if (fanId >= FAN_COUNT) return AUTO_MANUAL;
    return modes[fanId];
}

void autoSetMode(uint8_t fanId, AutoMode mode) {
    if (fanId >= FAN_COUNT) return;

    // Saglabāt iepriekšējo režīmu (ja pāriet uz SERVER)
    if (mode == AUTO_SERVER && modes[fanId] != AUTO_SERVER) {
        prevModes[fanId] = modes[fanId];
    }
    // Ja pāriet no SERVER uz citu — nav jāglabā
    if (mode != AUTO_SERVER && modes[fanId] != AUTO_SERVER) {
        prevModes[fanId] = mode;
    }

    modes[fanId] = mode;
    lastAppliedSpeed[fanId] = 255;

    if (mode != AUTO_MANUAL && fanIsManualOff(fanId)) {
        fanSetManualOff(fanId, false);
        DBG("[AUTO] Fan%d: manualOff cleared (mode change)\n", fanId + 1);
    }

    // SERVER režīmu neglabāj NVS (pēc restarta nav jēgas)
    if (mode != AUTO_SERVER) {
        autoSaveMode(fanId);
    }
    DBG("[AUTO] Fan%d mode -> %s\n", fanId + 1, autoModeStr(mode));
}

void autoSaveMode(uint8_t fanId) {
    if (fanId >= FAN_COUNT) return;
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putUChar(modeKeys[fanId], (uint8_t)modes[fanId]);
    p.end();
}

// ── Grafiks ─────────────────────────────────────────────────

uint8_t autoGetSlot(uint8_t fanId, uint8_t day, uint8_t slot) {
    if (fanId >= FAN_COUNT || day >= DAYS_PER_WEEK || slot >= SLOTS_PER_DAY) return 0;
    return schedule[fanId][day * SLOTS_PER_DAY + slot];
}

void autoSetSlot(uint8_t fanId, uint8_t day, uint8_t slot, uint8_t speed) {
    if (fanId >= FAN_COUNT || day >= DAYS_PER_WEEK || slot >= SLOTS_PER_DAY) return;
    schedule[fanId][day * SLOTS_PER_DAY + slot] = constrain(speed, 0, 100);
}

void autoSaveSchedule(uint8_t fanId) {
    if (fanId >= FAN_COUNT) return;
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putBytes(schKeys[fanId], schedule[fanId], SCHEDULE_SIZE);
    p.end();
    DBG("[AUTO] Fan%d schedule saved (%d bytes)\n", fanId + 1, SCHEDULE_SIZE);
}

String autoGetScheduleJSON(uint8_t fanId) {
    if (fanId >= FAN_COUNT) return "[]";

    // JSON: 7 dienu masīvs, katrā 48 skaitļi
    String json = "[";
    for (uint8_t d = 0; d < DAYS_PER_WEEK; d++) {
        if (d > 0) json += ",";
        json += "[";
        for (uint8_t s = 0; s < SLOTS_PER_DAY; s++) {
            if (s > 0) json += ",";
            json += String(schedule[fanId][d * SLOTS_PER_DAY + s]);
        }
        json += "]";
    }
    json += "]";
    return json;
}

void autoSetScheduleFromJSON(uint8_t fanId, const String& json) {
    if (fanId >= FAN_COUNT) return;

    // JSON: [[n,n,...48...],[n,n,...48...], ...×7]
    // Parsējam ar depth tracking: depth==2 → skaitļi
    memset(schedule[fanId], 0, SCHEDULE_SIZE);

    int len = json.length();
    uint8_t day = 0, slot = 0;
    int depth = 0;

    for (int i = 0; i < len; i++) {
        char c = json.charAt(i);
        if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
            if (depth == 1) {
                day++;
                slot = 0;
            }
        } else if (c >= '0' && c <= '9' && depth == 2) {
            int num = 0;
            while (i < len && json.charAt(i) >= '0' && json.charAt(i) <= '9') {
                num = num * 10 + (json.charAt(i) - '0');
                i++;
            }
            i--;
            if (day < DAYS_PER_WEEK && slot < SLOTS_PER_DAY) {
                schedule[fanId][day * SLOTS_PER_DAY + slot] = constrain(num, 0, 100);
                slot++;
            }
        }
    }

    autoSaveSchedule(fanId);
}

// ── Temperatūras līkne ──────────────────────────────────────

TempCurve& autoGetTempCurve(uint8_t fanId) {
    static TempCurve dummy = { 0, 0, 20.0, 35.0, 0, 100 };
    if (fanId >= FAN_COUNT) return dummy;
    return tempCurves[fanId];
}

void autoSetTempCurve(uint8_t fanId, const TempCurve& curve) {
    if (fanId >= FAN_COUNT) return;
    tempCurves[fanId] = curve;
    lastAppliedSpeed[fanId] = 255;  // piespiest recalc
    autoSaveTempCurve(fanId);
}

void autoSaveTempCurve(uint8_t fanId) {
    if (fanId >= FAN_COUNT) return;
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    p.putBytes(tcKeys[fanId], &tempCurves[fanId], sizeof(TempCurve));
    p.end();
    DBG("[AUTO] Fan%d temp curve saved\n", fanId + 1);
}

// ── Server mode funkcijas ──────────────────────────────────

void autoServerHeartbeat() {
    lastServerHeartbeat = millis();
    // Tikai atjaunina taimeri — nepārslēdz nevienu fanu.
    // Fani pārslēdzas uz SERVER tikai saņemot /server/fanX/set komandu.
    DBG("[AUTO] Server heartbeat (timer reset)\n");
}

void autoServerSetSpeed(uint8_t fanId, uint8_t speed) {
    if (fanId >= FAN_COUNT) return;
    serverSpeed[fanId] = constrain(speed, 0, 100);

    // Ja nav SERVER režīmā — pārslēgt
    if (modes[fanId] != AUTO_SERVER) {
        prevModes[fanId] = modes[fanId];
        modes[fanId] = AUTO_SERVER;
        lastAppliedSpeed[fanId] = 255;
    }

    lastServerHeartbeat = millis();
    DBG("[AUTO] Fan%d: server set %d%%\n", fanId + 1, speed);
}

AutoMode autoGetPreviousMode(uint8_t fanId) {
    if (fanId >= FAN_COUNT) return AUTO_MANUAL;
    return prevModes[fanId];
}

const char* autoModeStr(AutoMode mode) {
    switch (mode) {
        case AUTO_MANUAL:   return "MANUAL";
        case AUTO_SCHEDULE: return "SCHEDULE";
        case AUTO_TEMP:     return "TEMP";
        case AUTO_SERVER:   return "SERVER";
        default:            return "unknown";
    }
}
