#pragma once

// ═══════════════════════════════════════════════════════════
//  ESP8266 saderības slānis
// ═══════════════════════════════════════════════════════════

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// ── Preferences emulācija caur EEPROM ─────────────────────
// ESP32 Preferences API emulācija — saglabā datus EEPROM sektorā
// (atsevišķs flash sektors, netiek ietekmēts no LittleFS uploadfs/OTA)
//
// EEPROM izkārtojums:
//   [0..3]   — magic 0x53464E56 ("SFNV")
//   [4..5]   — JSON garums (uint16_t, little-endian)
//   [6..N]   — JSON string
//
#define EEPROM_SIZE     4096
#define EEPROM_MAGIC    0x53464E56  // "SFNV"

static bool _eepromInited = false;

class Preferences {
public:
    bool begin(const char* ns, bool readOnly = false) {
        (void)ns;  // ESP8266 — viens namespace
        _readOnly = readOnly;
        _doc.clear();  // Vienmēr sākt ar tīru doc

        if (!_eepromInited) {
            EEPROM.begin(EEPROM_SIZE);
            _eepromInited = true;
        }

        // Nolasīt magic
        uint32_t magic = 0;
        EEPROM.get(0, magic);
        if (magic == EEPROM_MAGIC) {
            uint16_t len = 0;
            EEPROM.get(4, len);
            if (len > 0 && len < EEPROM_SIZE - 6) {
                char* buf = (char*)malloc(len + 1);
                if (buf) {
                    for (uint16_t i = 0; i < len; i++) {
                        buf[i] = (char)EEPROM.read(6 + i);
                    }
                    buf[len] = '\0';
                    DeserializationError err = deserializeJson(_doc, buf);
                    free(buf);
                    if (err) {
                        Serial.printf("[PREFS] JSON parse error: %s — clearing EEPROM\n", err.c_str());
                        _doc.clear();
                        // Nodzēst korumpēto EEPROM
                        for (int i = 0; i < 6; i++) EEPROM.write(i, 0xFF);
                        EEPROM.commit();
                    }
                }
            }
        }
        return true;
    }

    void end() {
        if (!_readOnly) {
            String json;
            serializeJson(_doc, json);
            uint16_t len = json.length();

            if (len > 500) {
                Serial.printf("[PREFS] WARN: JSON suspiciously large (%d), skipping write\n", len);
                _doc.clear();
                return;
            }
            if (len < EEPROM_SIZE - 6) {
                uint32_t magic = EEPROM_MAGIC;
                EEPROM.put(0, magic);
                EEPROM.put(4, len);
                for (uint16_t i = 0; i < len; i++) {
                    EEPROM.write(6 + i, (uint8_t)json[i]);
                }
                EEPROM.commit();
                Serial.printf("[PREFS] EEPROM saved (%d bytes)\n", len);
            } else {
                Serial.printf("[PREFS] ERROR: JSON too large (%d > %d)\n", len, EEPROM_SIZE - 6);
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

    size_t getBytesLength(const char* key) {
        if (!_doc.containsKey(key)) return 0;
        const char* hex = _doc[key].as<const char*>();
        if (!hex) return 0;
        return strlen(hex) / 2;
    }

    bool isKey(const char* key) {
        return _doc.containsKey(key);
    }

    bool clear() {
        _doc.clear();
        return true;
    }

    bool remove(const char* key) {
        _doc.remove(key);
        return true;
    }

private:
    bool _readOnly = false;
    StaticJsonDocument<512> _doc;
};

// ESP8266 MAC helper
inline uint64_t espGetMac() { return (uint64_t)ESP.getChipId(); }
