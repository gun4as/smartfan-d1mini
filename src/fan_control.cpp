#include "fan_control.h"
#include "config.h"
#include "device_manager.h"
#include "compat.h"

// ── Struktūra katram fanam ─────────────────────────────────
struct FanData {
    FanType  type;
    uint8_t  pwmPin;
    uint8_t  tachPin;
    volatile uint32_t tachPulses;
    volatile uint32_t lastPulseUs;
    uint32_t rpm;
    uint8_t  speedPct;
    uint8_t  pwmDuty;
    bool     active;
    bool     manualOff;   // manuāli izslēgts (relejs OFF, bet speed > 0)
    uint8_t  minPwm;      // minimālais PWM % (zem tā = 0)
    uint8_t  maxPwm;      // maksimālais PWM %
};

static FanData fans[FAN_COUNT];
static uint32_t lastRpmCalc = 0;
static uint32_t lastTachDebug = 0;
static uint32_t lastNvsSave[FAN_COUNT] = {0, 0};  // NVS throttle katram fanam

// ── Pin tabula ─────────────────────────────────────────────
static const uint8_t pwmPins[FAN_COUNT]  = { PIN_FAN1_PWM,  PIN_FAN2_PWM };
static const uint8_t tachPins[FAN_COUNT] = { PIN_FAN1_TACH, PIN_FAN2_TACH };

// ── TACH interrupt handleri ────────────────────────────────
static void IRAM_ATTR tachISR0() {
    uint32_t now = micros();
    if (now - fans[0].lastPulseUs > TACH_DEBOUNCE_US) {
        fans[0].tachPulses = fans[0].tachPulses + 1;
        fans[0].lastPulseUs = now;
    }
}

static void IRAM_ATTR tachISR1() {
    uint32_t now = micros();
    if (now - fans[1].lastPulseUs > TACH_DEBOUNCE_US) {
        fans[1].tachPulses = fans[1].tachPulses + 1;
        fans[1].lastPulseUs = now;
    }
}

static void (*tachISRs[FAN_COUNT])() = { tachISR0, tachISR1 };

// ── Publiskās funkcijas ────────────────────────────────────

void fanInit(uint8_t id, FanType type) {
    if (id >= FAN_COUNT) return;
    FanData& f = fans[id];

    f.type       = type;
    f.pwmPin     = pwmPins[id];
    f.tachPin    = tachPins[id];
    f.tachPulses = 0;
    f.lastPulseUs = 0;
    f.rpm        = 0;
    f.active     = true;

    // Ielādēt pēdējo ātrumu un manualOff stāvokli no NVS
    Preferences p;
    p.begin(NVS_NAMESPACE, true);
    char key[12];
    snprintf(key, sizeof(key), "fan_spd%d", id);
    f.speedPct = p.getUChar(key, 50);
    char mkey[12];
    snprintf(mkey, sizeof(mkey), "fan_off%d", id);
    f.manualOff = p.getBool(mkey, false);
    char minKey[12], maxKey[12];
    snprintf(minKey, sizeof(minKey), "fan_min%d", id);
    snprintf(maxKey, sizeof(maxKey), "fan_max%d", id);
    f.minPwm = p.getUChar(minKey, 0);
    f.maxPwm = p.getUChar(maxKey, 100);
    p.end();
    DBG("[FAN%d] NVS: speed=%d%%, manualOff=%d, minPwm=%d%%, maxPwm=%d%%\n",
        id + 1, f.speedPct, f.manualOff, f.minPwm, f.maxPwm);
    uint8_t initPct;
    if (f.speedPct == 0) {
        initPct = 0;
    } else if (f.minPwm > 0) {
        initPct = map(f.speedPct, 1, 100, f.minPwm, 100);
    } else {
        initPct = f.speedPct;
    }
    f.pwmDuty = map(initPct, 0, 100, 0, 255);

    uint8_t val = 255 - f.pwmDuty;
#ifdef ESP8266
    analogWriteFreq(PWM_FREQ);
    analogWriteRange(255);
    analogWrite(f.pwmPin, val);
    DBG("[FAN%d] PWM: GPIO%d, analogWrite=%d (duty=%d%%)\n",
        id + 1, f.pwmPin, val, f.speedPct);
#else
    bool ok = ledcAttach(f.pwmPin, PWM_FREQ, PWM_RES);
    delay(10);
    ledcWrite(f.pwmPin, val);
    DBG("[FAN%d] PWM: GPIO%d, ledcAttach=%s, ledcWrite=%d (duty=%d%%)\n",
        id + 1, f.pwmPin, ok ? "OK" : "FAIL", val, f.speedPct);
#endif

    // TACH
    pinMode(f.tachPin, INPUT_PULLUP);
    int intNum = digitalPinToInterrupt(f.tachPin);
    attachInterrupt(intNum, tachISRs[id], FALLING);
    DBG("[FAN%d] TACH: GPIO%d, interrupt=%d\n", id + 1, f.tachPin, intNum);

    // Releju ieslēgt tikai ja ātrums > 0% UN nav manuāli izslēgts
    if (f.speedPct == 0 || f.manualOff) {
        relaySetForFan(id, false);
        DBG("[FAN%d] relejs OFF (%s)\n", id + 1,
            f.manualOff ? "manualOff" : "speed=0%");
    } else {
        relaySetForFan(id, true);
    }
}

void fanStop(uint8_t id) {
    if (id >= FAN_COUNT) return;
    FanData& f = fans[id];
    if (!f.active) return;

    detachInterrupt(digitalPinToInterrupt(f.tachPin));
#ifndef ESP8266
    ledcDetach(f.pwmPin);
#endif
    relaySetForFan(id, false);
    f.active = false;
    f.type   = FAN_NONE;
    f.rpm    = 0;
    DBG("[FAN%d] Atslēgts\n", id + 1);
}

void fanLoop() {
    uint32_t now = millis();
    uint32_t dt  = now - lastRpmCalc;
    if (dt < 1000) return;

    bool doDebug = (now - lastTachDebug > 5000);

    for (uint8_t i = 0; i < FAN_COUNT; i++) {
        FanData& f = fans[i];
        if (!f.active) continue;

        noInterrupts();
        uint32_t pulses = f.tachPulses;
        f.tachPulses = 0;
        interrupts();

        if (doDebug) {
            DBG("[TACH] F%d: %lu pulses, dt=%lu ms\n", i + 1, pulses, dt);
        }

        uint32_t calcRpm = (pulses * 60000UL) / (2 * dt);
        f.rpm = (calcRpm <= RPM_MAX) ? calcRpm : f.rpm;
    }

    if (doDebug) lastTachDebug = now;
    lastRpmCalc = now;
}

void fanSetSpeed(uint8_t id, uint8_t percent) {
    if (id >= FAN_COUNT || !fans[id].active) return;
    FanData& f = fans[id];

    f.speedPct = constrain(percent, 0, 100);
    // Pārmapēt: 0% = OFF, 1-100% → minPwm-100%
    uint8_t effectivePct;
    if (f.speedPct == 0) {
        effectivePct = 0;
    } else if (f.minPwm > 0) {
        effectivePct = map(f.speedPct, 1, 100, f.minPwm, 100);
    } else {
        effectivePct = f.speedPct;
    }
    f.pwmDuty  = map(effectivePct, 0, 100, 0, 255);

    // Automātiskā releja vadība: 0% = OFF, >0% = ON
    // Bet ja manuāli izslēgts — neiesl relej automātiski
    if (f.speedPct == 0) {
        relaySetForFan(id, false);
        if (f.manualOff) {
            f.manualOff = false;
            Preferences p; p.begin(NVS_NAMESPACE, false);
            char mk[12]; snprintf(mk, sizeof(mk), "fan_off%d", id);
            p.putBool(mk, false); p.end();
        }
    } else if (!f.manualOff) {
        relaySetForFan(id, true);
    }

    // 4-pin MOSFET invertēts: 255 - duty
    uint8_t val = 255 - f.pwmDuty;
#ifdef ESP8266
    analogWrite(f.pwmPin, val);
#else
    ledcWrite(f.pwmPin, val);
#endif
    DBG("[FAN%d] speed=%d%%, pwm=%d (GPIO%d)\n",
        id + 1, f.speedPct, val, f.pwmPin);

    // Saglabāt NVS — ne biežāk kā reizi 5s (katram fanam atsevišķi)
    uint32_t now = millis();
    if (now - lastNvsSave[id] > 5000) {
        Preferences p;
        p.begin(NVS_NAMESPACE, false);
        char key[12];
        snprintf(key, sizeof(key), "fan_spd%d", id);
        p.putUChar(key, f.speedPct);
        p.end();
        lastNvsSave[id] = now;
        DBG("[FAN%d] NVS saglabats: %d%%\n", id + 1, f.speedPct);
    }
}

void fanSetSpeedAuto(uint8_t id, uint8_t percent) {
    // Automatizācijas versija — bez NVS saglabāšanas, bez manualOff pārbaudes
    if (id >= FAN_COUNT || !fans[id].active) return;
    FanData& f = fans[id];

    f.speedPct = constrain(percent, 0, 100);
    // Pārmapēt: 0% = OFF, 1-100% → minPwm-100%
    uint8_t effectivePct;
    if (f.speedPct == 0) {
        effectivePct = 0;
    } else if (f.minPwm > 0) {
        effectivePct = map(f.speedPct, 1, 100, f.minPwm, 100);
    } else {
        effectivePct = f.speedPct;
    }
    f.pwmDuty  = map(effectivePct, 0, 100, 0, 255);

    // Releja vadība: 0% = OFF, >0% = ON
    if (f.speedPct == 0) {
        relaySetForFan(id, false);
    } else {
        relaySetForFan(id, true);
    }

    // 4-pin MOSFET invertēts: 255 - duty
    uint8_t val = 255 - f.pwmDuty;
#ifdef ESP8266
    analogWrite(f.pwmPin, val);
#else
    ledcWrite(f.pwmPin, val);
#endif
}

uint8_t fanGetSpeed(uint8_t id) {
    if (id >= FAN_COUNT) return 0;
    return fans[id].speedPct;
}

uint32_t fanGetRPM(uint8_t id) {
    if (id >= FAN_COUNT) return 0;
    return fans[id].rpm;
}

bool fanIsActive(uint8_t id) {
    if (id >= FAN_COUNT) return false;
    return fans[id].active;
}

void fanSetManualOff(uint8_t id, bool off) {
    if (id >= FAN_COUNT) return;
    fans[id].manualOff = off;
    // Saglabāt NVS
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    char key[12];
    snprintf(key, sizeof(key), "fan_off%d", id);
    p.putBool(key, off);
    p.end();
    DBG("[FAN%d] manualOff=%d (NVS saved)\n", id + 1, off);
}

bool fanIsManualOff(uint8_t id) {
    if (id >= FAN_COUNT) return false;
    return fans[id].manualOff;
}

uint8_t fanGetMinPwm(uint8_t id) { return id < FAN_COUNT ? fans[id].minPwm : 0; }
uint8_t fanGetMaxPwm(uint8_t id) { return id < FAN_COUNT ? fans[id].maxPwm : 100; }

void fanSetMinPwm(uint8_t id, uint8_t percent) {
    if (id >= FAN_COUNT) return;
    fans[id].minPwm = constrain(percent, 0, 100);
    Preferences p; p.begin(NVS_NAMESPACE, false);
    char key[12]; snprintf(key, sizeof(key), "fan_min%d", id);
    p.putUChar(key, fans[id].minPwm); p.end();
    DBG("[FAN%d] minPwm=%d%% (NVS saved)\n", id + 1, fans[id].minPwm);
    fanSetSpeed(id, fans[id].speedPct);  // Pārrēķināt ar jauno min
}

void fanSetMaxPwm(uint8_t id, uint8_t percent) {
    if (id >= FAN_COUNT) return;
    fans[id].maxPwm = constrain(percent, 1, 100);
    Preferences p; p.begin(NVS_NAMESPACE, false);
    char key[12]; snprintf(key, sizeof(key), "fan_max%d", id);
    p.putUChar(key, fans[id].maxPwm); p.end();
    DBG("[FAN%d] maxPwm=%d%% (NVS saved)\n", id + 1, fans[id].maxPwm);
    fanSetSpeed(id, fans[id].speedPct);  // Pārrēķināt ar jauno max
}

void fanSaveNow(uint8_t id) {
    if (id >= FAN_COUNT) return;
    Preferences p;
    p.begin(NVS_NAMESPACE, false);
    char key[12];
    snprintf(key, sizeof(key), "fan_spd%d", id);
    p.putUChar(key, fans[id].speedPct);
    p.end();
    lastNvsSave[id] = millis();
    DBG("[FAN%d] NVS forced save: %d%%\n", id + 1, fans[id].speedPct);
}
