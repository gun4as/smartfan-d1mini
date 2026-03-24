#pragma once

// ═══════════════════════════════════════════════════════════
//  ESP8266 / ESP32 saderības slānis
// ═══════════════════════════════════════════════════════════

#ifdef ESP8266

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ── Preferences emulācija caur LittleFS JSON ────────────────
// ESP32 Preferences API emulācija — saglabā NVS datus kā JSON failus
class Preferences {
public:
    bool begin(const char* ns, bool readOnly = false) {
        _ns = String("/prefs/") + ns + ".json";
        _readOnly = readOnly;
        if (!LittleFS.exists("/prefs")) {
            LittleFS.mkdir("/prefs");
        }
        // Ielādēt esošo JSON
        if (LittleFS.exists(_ns.c_str())) {
            File f = LittleFS.open(_ns.c_str(), "r");
            if (f) {
                deserializeJson(_doc, f);
                f.close();
            }
        }
        return true;
    }

    void end() {
        if (!_readOnly) {
            File f = LittleFS.open(_ns.c_str(), "w");
            if (f) {
                serializeJson(_doc, f);
                f.close();
            }
        }
        _doc.clear();
    }

    // String
    String getString(const char* key, const String& def = "") {
        return _doc.containsKey(key) ? _doc[key].as<String>() : def;
    }
    void putString(const char* key, const String& val) {
        _doc[key] = val;
    }

    // UChar (uint8_t)
    uint8_t getUChar(const char* key, uint8_t def = 0) {
        return _doc.containsKey(key) ? _doc[key].as<uint8_t>() : def;
    }
    void putUChar(const char* key, uint8_t val) {
        _doc[key] = val;
    }

    // UShort (uint16_t)
    uint16_t getUShort(const char* key, uint16_t def = 0) {
        return _doc.containsKey(key) ? _doc[key].as<uint16_t>() : def;
    }
    void putUShort(const char* key, uint16_t val) {
        _doc[key] = val;
    }

    // Bool
    bool getBool(const char* key, bool def = false) {
        return _doc.containsKey(key) ? _doc[key].as<bool>() : def;
    }
    void putBool(const char* key, bool val) {
        _doc[key] = val;
    }

    // Bytes
    size_t getBytes(const char* key, void* buf, size_t maxLen) {
        if (!_doc.containsKey(key)) return 0;
        const char* hex = _doc[key].as<const char*>();
        if (!hex) return 0;
        size_t len = strlen(hex) / 2;
        if (len > maxLen) len = maxLen;
        uint8_t* out = (uint8_t*)buf;
        for (size_t i = 0; i < len; i++) {
            char h[3] = { hex[i*2], hex[i*2+1], 0 };
            out[i] = strtoul(h, NULL, 16);
        }
        return len;
    }
    void putBytes(const char* key, const void* buf, size_t len) {
        String hex;
        hex.reserve(len * 2);
        const uint8_t* in = (const uint8_t*)buf;
        for (size_t i = 0; i < len; i++) {
            char h[3];
            sprintf(h, "%02x", in[i]);
            hex += h;
        }
        _doc[key] = hex;
    }

    // getBytesLength
    size_t getBytesLength(const char* key) {
        if (!_doc.containsKey(key)) return 0;
        const char* hex = _doc[key].as<const char*>();
        if (!hex) return 0;
        return strlen(hex) / 2;
    }

    // isKey
    bool isKey(const char* key) {
        return _doc.containsKey(key);
    }

    // Clear
    bool clear() {
        _doc.clear();
        return true;
    }

    bool remove(const char* key) {
        _doc.remove(key);
        return true;
    }

private:
    String _ns;
    bool _readOnly = false;
    StaticJsonDocument<4096> _doc;
};

// ESP8266 MAC helper
inline uint64_t espGetMac() { return (uint64_t)ESP.getChipId(); }

#else
// ESP32 — viss standarts
#include <WiFi.h>
#include <AsyncTCP.h>
#include <Preferences.h>

// ESP32 MAC helper
inline uint64_t espGetMac() { return ESP.getEfuseMac(); }

#endif
