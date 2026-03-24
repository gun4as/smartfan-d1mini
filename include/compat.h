#pragma once

// ═══════════════════════════════════════════════════════════
//  ESP8266 saderības slānis
// ═══════════════════════════════════════════════════════════

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <LittleFS.h>
#include <EEPROM.h>

// ── Preferences emulācija caur EEPROM (bez JSON) ──────────
// Vienkāršs key-value store EEPROM sektorā.
// Formāts: [magic 4B] [entries...] [0x00 terminator]
// Entry: [keyLen:1] [key:N] [valLen:2 LE] [value:N]
//
#define EEPROM_SIZE     4096
#define EEPROM_MAGIC    0x53464E56  // "SFNV"
#define EEPROM_DATA_START 4

static bool _eepromInited = false;

class Preferences {
public:
    bool begin(const char* ns, bool readOnly = false) {
        (void)ns;
        _readOnly = readOnly;
        if (!_eepromInited) {
            EEPROM.begin(EEPROM_SIZE);
            _eepromInited = true;
        }
        // Pārbaudīt magic
        uint32_t magic = 0;
        EEPROM.get(0, magic);
        _valid = (magic == EEPROM_MAGIC);
        return true;
    }

    void end() {
        // Neko nedara — dati tiek rakstīti uzreiz put* metodēs
        _valid = false;
    }

    // ── String ──
    String getString(const char* key, const String& def = "") {
        if (!_valid) return def;
        int pos = _findKey(key);
        if (pos < 0) return def;
        uint16_t vLen = _readU16(pos);
        pos += 2;
        String result;
        result.reserve(vLen);
        for (uint16_t i = 0; i < vLen; i++) {
            result += (char)EEPROM.read(pos + i);
        }
        return result;
    }

    void putString(const char* key, const String& val) {
        if (_readOnly) return;
        _ensureMagic();
        _deleteKey(key);
        _appendEntry(key, (const uint8_t*)val.c_str(), val.length());
    }

    // ── UChar ──
    uint8_t getUChar(const char* key, uint8_t def = 0) {
        String s = getString(key, "");
        return s.length() > 0 ? (uint8_t)s.toInt() : def;
    }
    void putUChar(const char* key, uint8_t val) {
        putString(key, String(val));
    }

    // ── UShort ──
    uint16_t getUShort(const char* key, uint16_t def = 0) {
        String s = getString(key, "");
        return s.length() > 0 ? (uint16_t)s.toInt() : def;
    }
    void putUShort(const char* key, uint16_t val) {
        putString(key, String(val));
    }

    // ── Bool ──
    bool getBool(const char* key, bool def = false) {
        String s = getString(key, "");
        if (s.length() == 0) return def;
        return s == "1" || s == "true";
    }
    void putBool(const char* key, bool val) {
        putString(key, val ? "1" : "0");
    }

    // ── Bytes ──
    size_t getBytes(const char* key, void* buf, size_t maxLen) {
        if (!_valid) return 0;
        int pos = _findKey(key);
        if (pos < 0) return 0;
        uint16_t vLen = _readU16(pos);
        pos += 2;
        size_t copyLen = vLen < maxLen ? vLen : maxLen;
        uint8_t* out = (uint8_t*)buf;
        for (size_t i = 0; i < copyLen; i++) {
            out[i] = EEPROM.read(pos + i);
        }
        return copyLen;
    }
    void putBytes(const char* key, const void* buf, size_t len) {
        if (_readOnly) return;
        _ensureMagic();
        _deleteKey(key);
        _appendEntry(key, (const uint8_t*)buf, len);
    }

    size_t getBytesLength(const char* key) {
        if (!_valid) return 0;
        int pos = _findKey(key);
        if (pos < 0) return 0;
        return _readU16(pos);
    }

    bool isKey(const char* key) {
        return _findKey(key) >= 0;
    }

    bool clear() {
        // Nodzēst magic → pēc restarta viss būs tukšs
        for (int i = 0; i < EEPROM_DATA_START + 1; i++) EEPROM.write(i, 0xFF);
        EEPROM.commit();
        _valid = false;
        Serial.println("[PREFS] EEPROM cleared");
        return true;
    }

    bool remove(const char* key) {
        if (_readOnly) return false;
        _deleteKey(key);
        return true;
    }

private:
    bool _readOnly = false;
    bool _valid = false;

    void _ensureMagic() {
        uint32_t magic = 0;
        EEPROM.get(0, magic);
        if (magic != EEPROM_MAGIC) {
            magic = EEPROM_MAGIC;
            EEPROM.put(0, magic);
            EEPROM.write(EEPROM_DATA_START, 0x00); // empty terminator
            EEPROM.commit();
            _valid = true;
        }
    }

    uint16_t _readU16(int pos) {
        return EEPROM.read(pos) | (EEPROM.read(pos + 1) << 8);
    }
    void _writeU16(int pos, uint16_t val) {
        EEPROM.write(pos, val & 0xFF);
        EEPROM.write(pos + 1, (val >> 8) & 0xFF);
    }

    // Atrast key pozīciju EEPROM. Atgriež pozīciju uz value length, vai -1.
    int _findKey(const char* key) {
        uint8_t targetLen = strlen(key);
        int pos = EEPROM_DATA_START;
        while (pos < EEPROM_SIZE - 4) {
            uint8_t kLen = EEPROM.read(pos);
            if (kLen == 0 || kLen == 0xFF) return -1; // end
            pos++; // skip kLen
            // Salīdzināt key
            bool match = (kLen == targetLen);
            if (match) {
                for (uint8_t i = 0; i < kLen; i++) {
                    if (EEPROM.read(pos + i) != (uint8_t)key[i]) {
                        match = false;
                        break;
                    }
                }
            }
            pos += kLen; // skip key bytes
            uint16_t vLen = _readU16(pos);
            pos += 2; // skip vLen
            if (match) return pos - 2; // atgriež pozīciju uz vLen
            pos += vLen; // skip value
        }
        return -1;
    }

    // Atrast brīvo pozīciju (terminator 0x00)
    int _findEnd() {
        int pos = EEPROM_DATA_START;
        while (pos < EEPROM_SIZE - 4) {
            uint8_t kLen = EEPROM.read(pos);
            if (kLen == 0 || kLen == 0xFF) return pos;
            pos++; // kLen
            pos += kLen; // key
            uint16_t vLen = _readU16(pos);
            pos += 2 + vLen; // vLen + value
        }
        return -1; // EEPROM pilns
    }

    // Dzēst key — atrast entry, pārkopēt pārējos entries uz priekšu
    void _deleteKey(const char* key) {
        if (!_valid) return;
        uint8_t targetLen = strlen(key);
        int pos = EEPROM_DATA_START;
        while (pos < EEPROM_SIZE - 4) {
            int entryStart = pos;
            uint8_t kLen = EEPROM.read(pos);
            if (kLen == 0 || kLen == 0xFF) return; // end, nav atrasts
            pos++;
            bool match = (kLen == targetLen);
            if (match) {
                for (uint8_t i = 0; i < kLen; i++) {
                    if (EEPROM.read(pos + i) != (uint8_t)key[i]) {
                        match = false;
                        break;
                    }
                }
            }
            pos += kLen;
            uint16_t vLen = _readU16(pos);
            pos += 2 + vLen;
            if (match) {
                // Atrast kopēšanas beigas (nākamie entries līdz terminatoram)
                int endPos = pos;
                while (endPos < EEPROM_SIZE - 3) {
                    uint8_t eKLen = EEPROM.read(endPos);
                    if (eKLen == 0 || eKLen == 0xFF) break;
                    endPos++; // kLen
                    endPos += eKLen; // key
                    uint16_t eVLen = _readU16(endPos);
                    endPos += 2 + eVLen; // vLen + value
                }
                // Pārkopēt entries no pos..endPos uz entryStart
                int copyLen = endPos - pos;
                for (int i = 0; i < copyLen; i++) {
                    EEPROM.write(entryStart + i, EEPROM.read(pos + i));
                }
                EEPROM.write(entryStart + copyLen, 0x00); // terminator
                EEPROM.commit();
                return;
            }
        }
    }

    // Pievienot jaunu entry beigās
    void _appendEntry(const char* key, const uint8_t* data, uint16_t dataLen) {
        int pos = _findEnd();
        if (pos < 0 || pos + 1 + strlen(key) + 2 + dataLen + 1 >= EEPROM_SIZE) {
            Serial.printf("[PREFS] EEPROM full! Cannot store %s\n", key);
            return;
        }
        uint8_t kLen = strlen(key);
        EEPROM.write(pos++, kLen);
        for (uint8_t i = 0; i < kLen; i++) {
            EEPROM.write(pos++, (uint8_t)key[i]);
        }
        _writeU16(pos, dataLen);
        pos += 2;
        for (uint16_t i = 0; i < dataLen; i++) {
            EEPROM.write(pos++, data[i]);
        }
        EEPROM.write(pos, 0x00); // terminator
        EEPROM.commit();
        Serial.printf("[PREFS] Saved %s (%d bytes)\n", key, dataLen);
    }
};

// ESP8266 MAC helper
inline uint64_t espGetMac() { return (uint64_t)ESP.getChipId(); }
