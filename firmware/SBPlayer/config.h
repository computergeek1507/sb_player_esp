#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#define CONFIG_FILE  "/config.json"
#define OUTPUTS_FILE "/outputs.json"

#define MAX_E131_OUTPUTS 8
#define MAX_DDP_OUTPUTS  8

// ── Per-output configs ────────────────────────────────────────────────────────

struct E131OutCfg {
    bool     enabled;
    uint16_t universeStart;
    uint16_t universeSize;
    uint32_t channelStart;  // first FSEQ channel for this output
    uint32_t channelCount;  // 0 = all remaining channels
    bool     multicast;
    char     unicastIp[16]; // used when multicast=false
};

struct DDPOutCfg {
    bool     enabled;
    char     ip[16];
    uint32_t channelStart;
    uint32_t channelCount;  // 0 = all remaining channels
};

struct OutputsCfg {
    E131OutCfg e131[MAX_E131_OUTPUTS];
    uint8_t    e131Count;
    DDPOutCfg  ddp[MAX_DDP_OUTPUTS];
    uint8_t    ddpCount;
};

// ── Main device config ────────────────────────────────────────────────────────

struct Config {
    char    hostname[32];
    char    apSsid[64];
    char    apPassword[64];
    char    ntpServer[64];
    int16_t tzOffsetMins;  // UTC offset in minutes (e.g. -240 for UTC-4)

    // Audio
    uint8_t audioVolume;
    uint8_t i2sBclk;
    uint8_t i2sLrclk;
    uint8_t i2sData;

    // MultiSync
    bool     multisyncEnabled;
    uint16_t multisyncPort;

    // Ethernet
    bool    ethEnabled;
    uint8_t ethPhyAddr;
    int8_t  ethPowerPin;
    uint8_t ethMdcPin;
    uint8_t ethMdioPin;
    bool    ethStatic;
    char    ethIp[16];
    char    ethGateway[16];
    char    ethSubnet[16];
    char    ethDns[16];

    // OLED
    bool    oledEnabled;
    uint8_t oledSda;
    uint8_t oledScl;

    // RTC (shares I2C bus with OLED)
    bool rtcEnabled;
    char rtcType[16];   // "DS3231", "DS1307", "PCF8523"
};

// ── Defaults ──────────────────────────────────────────────────────────────────

inline void configDefaults(Config &c) {
    strlcpy(c.hostname,   "sb-player",    sizeof(c.hostname));
    strlcpy(c.apSsid,     "SB-Player",    sizeof(c.apSsid));
    strlcpy(c.apPassword, "sbplayer",     sizeof(c.apPassword));
    strlcpy(c.ntpServer,  "pool.ntp.org", sizeof(c.ntpServer));
    c.tzOffsetMins = -240;
    c.audioVolume      = 80;
    c.i2sBclk          = 26;
    c.i2sLrclk         = 25;
    c.i2sData          = 22;
    c.multisyncEnabled = true;
    c.multisyncPort    = 32320;
    c.ethEnabled       = true;
    c.ethPhyAddr       = 1;
    c.ethPowerPin      = 51;
    c.ethMdcPin        = 31;
    c.ethMdioPin       = 52;
    c.ethStatic        = false;
    strlcpy(c.ethIp,      "192.168.1.100", sizeof(c.ethIp));
    strlcpy(c.ethGateway, "192.168.1.1",   sizeof(c.ethGateway));
    strlcpy(c.ethSubnet,  "255.255.255.0", sizeof(c.ethSubnet));
    strlcpy(c.ethDns,     "8.8.8.8",       sizeof(c.ethDns));
    c.oledEnabled      = true;
    c.oledSda          = 7;
    c.oledScl          = 8;
    c.rtcEnabled = false;
    strlcpy(c.rtcType, "DS3231", sizeof(c.rtcType));
}

inline void outputsDefaults(OutputsCfg &o) {
    // One E1.31 output: multicast, universe 1, all channels
    o.e131Count = 1;
    o.e131[0] = { true, 1, 512, 0, 0, true, "" };
    // No DDP outputs by default
    o.ddpCount = 0;
}

// ── Config JSON ───────────────────────────────────────────────────────────────

inline void configFromJson(Config &c, const JsonDocument &doc) {
    if (doc["hostname"].is<const char*>())    strlcpy(c.hostname,   doc["hostname"],    sizeof(c.hostname));
    if (doc["ap_ssid"].is<const char*>())     strlcpy(c.apSsid,     doc["ap_ssid"],     sizeof(c.apSsid));
    if (doc["ap_password"].is<const char*>()) strlcpy(c.apPassword, doc["ap_password"], sizeof(c.apPassword));
    if (doc["ntp_server"].is<const char*>())  strlcpy(c.ntpServer,  doc["ntp_server"],  sizeof(c.ntpServer));
    if (doc["tz_offset_mins"].is<int>())      c.tzOffsetMins     = doc["tz_offset_mins"];
    if (doc["audio_volume"].is<int>())        c.audioVolume      = doc["audio_volume"];
    if (doc["i2s_bclk"].is<int>())            c.i2sBclk          = doc["i2s_bclk"];
    if (doc["i2s_lrclk"].is<int>())           c.i2sLrclk         = doc["i2s_lrclk"];
    if (doc["i2s_data"].is<int>())            c.i2sData          = doc["i2s_data"];
    if (doc["multisync_enabled"].is<bool>())  c.multisyncEnabled = doc["multisync_enabled"];
    if (doc["multisync_port"].is<int>())      c.multisyncPort    = doc["multisync_port"];
    if (doc["eth_enabled"].is<bool>())        c.ethEnabled       = doc["eth_enabled"];
    if (doc["eth_phy_addr"].is<int>())        c.ethPhyAddr       = doc["eth_phy_addr"];
    if (doc["eth_power_pin"].is<int>())       c.ethPowerPin      = doc["eth_power_pin"];
    if (doc["eth_mdc_pin"].is<int>())         c.ethMdcPin        = doc["eth_mdc_pin"];
    if (doc["eth_mdio_pin"].is<int>())        c.ethMdioPin       = doc["eth_mdio_pin"];
    if (doc["eth_static"].is<bool>())         c.ethStatic        = doc["eth_static"];
    if (doc["eth_ip"].is<const char*>())      strlcpy(c.ethIp,      doc["eth_ip"],      sizeof(c.ethIp));
    if (doc["eth_gateway"].is<const char*>()) strlcpy(c.ethGateway, doc["eth_gateway"], sizeof(c.ethGateway));
    if (doc["eth_subnet"].is<const char*>())  strlcpy(c.ethSubnet,  doc["eth_subnet"],  sizeof(c.ethSubnet));
    if (doc["eth_dns"].is<const char*>())     strlcpy(c.ethDns,     doc["eth_dns"],     sizeof(c.ethDns));
    if (doc["oled_enabled"].is<bool>())       c.oledEnabled      = doc["oled_enabled"];
    if (doc["oled_sda"].is<int>())            c.oledSda          = doc["oled_sda"];
    if (doc["oled_scl"].is<int>())            c.oledScl          = doc["oled_scl"];
    if (doc["rtc_enabled"].is<bool>())     c.rtcEnabled = doc["rtc_enabled"];
    if (doc["rtc_type"].is<const char*>()) strlcpy(c.rtcType, doc["rtc_type"], sizeof(c.rtcType));
}

inline void configToJson(const Config &c, JsonDocument &doc) {
    doc["hostname"]          = c.hostname;
    doc["ap_ssid"]           = c.apSsid;
    doc["ap_password"]       = c.apPassword;
    doc["ntp_server"]        = c.ntpServer;
    doc["tz_offset_mins"]    = c.tzOffsetMins;
    doc["audio_volume"]      = c.audioVolume;
    doc["i2s_bclk"]          = c.i2sBclk;
    doc["i2s_lrclk"]         = c.i2sLrclk;
    doc["i2s_data"]          = c.i2sData;
    doc["multisync_enabled"] = c.multisyncEnabled;
    doc["multisync_port"]    = c.multisyncPort;
    doc["eth_enabled"]       = c.ethEnabled;
    doc["eth_phy_addr"]      = c.ethPhyAddr;
    doc["eth_power_pin"]     = c.ethPowerPin;
    doc["eth_mdc_pin"]       = c.ethMdcPin;
    doc["eth_mdio_pin"]      = c.ethMdioPin;
    doc["eth_static"]        = c.ethStatic;
    doc["eth_ip"]            = c.ethIp;
    doc["eth_gateway"]       = c.ethGateway;
    doc["eth_subnet"]        = c.ethSubnet;
    doc["eth_dns"]           = c.ethDns;
    doc["oled_enabled"]      = c.oledEnabled;
    doc["oled_sda"]          = c.oledSda;
    doc["oled_scl"]          = c.oledScl;
    doc["rtc_enabled"] = c.rtcEnabled;
    doc["rtc_type"]    = c.rtcType;
}

// ── Outputs JSON ──────────────────────────────────────────────────────────────

inline void outputsFromJson(OutputsCfg &o, const JsonDocument &doc) {
    o.e131Count = 0;
    for (JsonVariantConst e : doc["e131"].as<JsonArrayConst>()) {
        if (o.e131Count >= MAX_E131_OUTPUTS) break;
        E131OutCfg &out = o.e131[o.e131Count++];
        out.enabled      = e["enabled"]       | true;
        out.universeStart= e["universe_start"] | 1;
        out.universeSize = e["universe_size"]  | 512;
        out.channelStart = e["channel_start"]  | 0;
        out.channelCount = e["channel_count"]  | 0;
        out.multicast    = e["multicast"]      | true;
        strlcpy(out.unicastIp, e["unicast_ip"] | "", sizeof(out.unicastIp));
    }
    o.ddpCount = 0;
    for (JsonVariantConst e : doc["ddp"].as<JsonArrayConst>()) {
        if (o.ddpCount >= MAX_DDP_OUTPUTS) break;
        DDPOutCfg &out = o.ddp[o.ddpCount++];
        out.enabled      = e["enabled"]       | true;
        strlcpy(out.ip,  e["ip"] | "",              sizeof(out.ip));
        out.channelStart = e["channel_start"]  | 0;
        out.channelCount = e["channel_count"]  | 0;
    }
}

inline void outputsToJson(const OutputsCfg &o, JsonDocument &doc) {
    JsonArray e131Arr = doc["e131"].to<JsonArray>();
    for (int i = 0; i < o.e131Count; i++) {
        const E131OutCfg &out = o.e131[i];
        JsonObject obj = e131Arr.add<JsonObject>();
        obj["enabled"]       = out.enabled;
        obj["universe_start"]= out.universeStart;
        obj["universe_size"] = out.universeSize;
        obj["channel_start"] = out.channelStart;
        obj["channel_count"] = out.channelCount;
        obj["multicast"]     = out.multicast;
        obj["unicast_ip"]    = out.unicastIp;
    }
    JsonArray ddpArr = doc["ddp"].to<JsonArray>();
    for (int i = 0; i < o.ddpCount; i++) {
        const DDPOutCfg &out = o.ddp[i];
        JsonObject obj = ddpArr.add<JsonObject>();
        obj["enabled"]       = out.enabled;
        obj["ip"]            = out.ip;
        obj["channel_start"] = out.channelStart;
        obj["channel_count"] = out.channelCount;
    }
}

// ── Load / Save ───────────────────────────────────────────────────────────────

inline bool loadConfig(Config &c) {
    configDefaults(c);
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();
    configFromJson(c, doc);
    return true;
}

inline bool saveConfig(const Config &c) {
    JsonDocument doc;
    configToJson(c, doc);
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

inline bool loadOutputs(OutputsCfg &o) {
    outputsDefaults(o);
    File f = LittleFS.open(OUTPUTS_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();
    outputsFromJson(o, doc);
    return true;
}

inline bool saveOutputs(const OutputsCfg &o) {
    JsonDocument doc;
    outputsToJson(o, doc);
    File f = LittleFS.open(OUTPUTS_FILE, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}
