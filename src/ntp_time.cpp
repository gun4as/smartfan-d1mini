#include "ntp_time.h"
#include "config.h"
#include <time.h>
#include "compat.h"

static bool synced = false;
static String currentTz;

void ntpInit() {
    // Nolasīt TZ no NVS vai lietot default
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    currentTz = prefs.getString(NVS_KEY_TZ, TZ_INFO_DEFAULT);
    prefs.end();

    configTzTime(currentTz.c_str(), NTP_SERVER);
    DBG("[NTP] TZ: %s, serveris: %s\n", currentTz.c_str(), NTP_SERVER);

    // Gaidām līdz 5s pirmo sync
    struct tm t;
    uint8_t tries = 0;
    while (!getLocalTime(&t, 100) && tries < 50) {
        tries++;
    }

    if (getLocalTime(&t, 0)) {
        synced = true;
        char buf[20];
        strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &t);
        DBG("[NTP] Laiks: %s\n", buf);
    } else {
        DBGLN("[NTP] Neizdevās sinhronizēt — mēģinās vēlāk");
    }
}

void ntpSetTimezone(const String& posixTz) {
    if (posixTz.isEmpty() || posixTz == currentTz) return;

    // Saglabāt NVS
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_TZ, posixTz);
    prefs.end();

    currentTz = posixTz;
    configTzTime(currentTz.c_str(), NTP_SERVER);
    DBG("[NTP] TZ mainīta: %s\n", currentTz.c_str());
}

String ntpGetTimezone() {
    return currentTz;
}

bool ntpIsSynced() {
    if (synced) return true;
    struct tm t;
    if (getLocalTime(&t, 0) && t.tm_year > 100) {
        synced = true;
    }
    return synced;
}

String ntpGetTime() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return "--:--:--";
    char buf[9];
    strftime(buf, sizeof(buf), "%H:%M:%S", &t);
    return String(buf);
}

String ntpGetDate() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return "--.---.----";
    char buf[11];
    strftime(buf, sizeof(buf), "%d.%m.%Y", &t);
    return String(buf);
}

time_t ntpGetEpoch() {
    time_t now;
    time(&now);
    return now;
}
