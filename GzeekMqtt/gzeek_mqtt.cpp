#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <utility>
#include <LittleFS.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_random.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

#include "config.h"
#include "navigation_policy.h"
#include "offline_policy.h"
#include "src/codec/es8311.h"

#if !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "Select ESP32C3 Dev Module."
#endif

namespace {
constexpr char kScheduleRequest[] = "/gzeek/prelekcje";
constexpr uint32_t kRate = 24000;
constexpr unsigned kChunkBytes = 960;
constexpr unsigned kMaxAudioBytes = kRate * 2 * 20;
constexpr uint32_t kRefreshMs = 15000;
constexpr uint32_t kScheduleFreshMs = 90000;
constexpr size_t kMaxScheduleBytes = 15000;
constexpr size_t kMaxCacheBytes = 16500;
constexpr unsigned kTitlePageChars = 42;
constexpr uint32_t kTitlePageMs = 2500;

struct BoardPins {
    const char* name;
    int sda, scl, mclk, bclk, lrck, dout, pa;
};

constexpr BoardPins kProfiles[] = {
    {"Xmini-C3", 3, 4, 10, 8, 6, 5, 11},
    {"Xmini-C3 V3", 0, 1, 8, 7, 5, 4, 10},
};

struct Room {
    const char* jsonKey;
    const char* label;
    const char* audioTopic;
};

constexpr Room kRooms[] = {
    {"A", "ROOM A", "gzeek/prelekcja_audio/sala_a"},
    {"B", "ROOM B", "gzeek/prelekcja_audio/sala_b"},
    {"warsztat", "WORKSHOPS", "gzeek/prelekcja_audio/sala_warsztat"},
};

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
I2SClass i2s;
WiFiClient socket;
PubSubClient mqtt(socket);
JsonDocument schedule;
const BoardPins* board = nullptr;
uint8_t codecAddress = 0;
bool oledReady = false;
uint32_t serverSeconds = 0;
uint32_t scheduleReceivedAt = 0;
int64_t clockAnchorUs = 0;
bool clockKnown = false;
bool cacheReady = false;
int cacheSlot = -1;
uint32_t cacheGeneration = 0;
String lastSavedSchedule;
unsigned offlineEventIndex = 0;
uint32_t scheduleRequestId = 0;
uint32_t sequenceId = 0;
uint32_t nextScheduleAt = 0;
bool scheduleReady = false;
bool refreshSchedule = false;
String clientId, replyPrefix;
std::atomic<bool> mqttOnline{false};
struct OutgoingMessage { char topic[96]; char payload[2048]; };
struct IncomingMessage { char topic[128]; unsigned length; uint8_t* payload; };
QueueHandle_t outgoingQueue = nullptr;
QueueHandle_t incomingQueue = nullptr;
unsigned roomIndex = 0;
uint32_t roomShownAt = 0;
uint32_t lastButtonAt = 0;
bool buttonWasPressed = false;
unsigned audioViewMinute = 0;
String roomContentKey;
uint32_t lastDrawAt = 0;
String notice;
uint32_t noticeUntil = 0;

enum class AudioState { idle, requested, playing, draining };
AudioState audioState = AudioState::idle;
AudioState lastDrawAudioState = AudioState::idle;
uint32_t audioId = 0;
uint32_t audioAt = 0;
uint32_t audioRequestedAt = 0;
uint32_t expectedBytes = 0;
uint32_t receivedBytes = 0;
uint32_t nextChunk = 0;
int ampPin = -1;
es8311_handle_t codec = nullptr;

bool due(uint32_t now, uint32_t target) {
    return static_cast<int32_t>(now - target) >= 0;
}

uint32_t newId() {
    if (++sequenceId == 0) {
        ++sequenceId;
    }
    return sequenceId;
}

bool acknowledges(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

uint8_t findCodec() {
    for (uint8_t address : {uint8_t{0x18}, uint8_t{0x19}}) {
        uint8_t high = 0, low = 0;
        if (gzeek_codec_read_reg(address, 0xfd, &high) == ESP_OK &&
            gzeek_codec_read_reg(address, 0xfe, &low) == ESP_OK &&
            high == 0x83 && low == 0x11) {
            return address;
        }
    }
    return 0;
}

void initializeBoard() {
    for (const auto& pins : kProfiles) {
        if (!Wire.begin(pins.sda, pins.scl, 100000)) {
            continue;
        }
        Wire.setTimeOut(25);
        const uint8_t address = acknowledges(0x3c) ? 0x3c : (acknowledges(0x3d) ? 0x3d : 0);
        codecAddress = findCodec();
        Serial.printf("%s SDA=%d SCL=%d OLED=0x%02X ES8311=0x%02X\n",
                      pins.name, pins.sda, pins.scl, unsigned(address), unsigned(codecAddress));
        if (address && oled.begin(SSD1306_SWITCHCAPVCC, address, true, false)) {
            board = &pins;
            oledReady = true;
            oled.setRotation(kOledRotation);
            oled.setTextSize(1);
            oled.setTextWrap(false);
            oled.setTextColor(SSD1306_WHITE);
            return;
        }
        Wire.end();
    }
    codecAddress = 0;
}

bool hasFreshSchedule(uint32_t now) {
    return scheduleReady && now - scheduleReceivedAt < kScheduleFreshMs;
}

unsigned minuteAt(uint32_t) {
    if (!clockKnown) return 0;
    return gzeek::offlineMinute(serverSeconds, uint64_t(esp_timer_get_time() - clockAnchorUs));
}

int timeInMinutes(const char* clock) {
    if (!clock || strlen(clock) != 5 || clock[2] != ':') {
        return -1;
    }
    for (int i : {0, 1, 3, 4}) {
        if (clock[i] < '0' || clock[i] > '9') {
            return -1;
        }
    }
    const int hours = (clock[0] - '0') * 10 + clock[1] - '0';
    const int mins = (clock[3] - '0') * 10 + clock[4] - '0';
    return hours < 24 && mins < 60 ? hours * 60 + mins : -1;
}

bool validSchedule(JsonObjectConst root) {
    if (root.isNull()) return false;
    for (const auto& room : kRooms) {
        if (!root[room.jsonKey].is<JsonArrayConst>()) return false;
        int previousEnd = -1;
        for (JsonObjectConst event : root[room.jsonKey].as<JsonArrayConst>()) {
            const int start = timeInMinutes(event["start"] | "");
            const int end = timeInMinutes(event["end"] | "");
            if (start < 0 || end <= start || start < previousEnd ||
                !event["Prelekcja"].is<const char*>() ||
                !strlen(event["Prelekcja"].as<const char*>())) return false;
            previousEnd = end;
        }
    }
    return measureJson(root) <= kMaxScheduleBytes;
}

bool validCache(JsonDocument& record, String& serialized) {
    if ((record["version"] | 0) != 1 || !(record["seq"] | uint32_t{0}) ||
        !validSchedule(record["schedule"].as<JsonObjectConst>())) return false;
    serializeJson(record["schedule"], serialized);
    return (record["crc"] | uint32_t{0}) == gzeek::checksum(serialized.c_str(), serialized.length());
}

void loadCachedSchedule() {
    // The dedicated data partition is initialized on first use.
    cacheReady = LittleFS.begin(true);
    if (!cacheReady) { Serial.println("Schedule cache unavailable"); return; }
    for (int slot = 0; slot < 2; ++slot) {
        const char* path = slot ? "/schedule1.json" : "/schedule0.json";
        File file = LittleFS.open(path, "r");
        if (!file || file.size() > kMaxCacheBytes) continue;
        JsonDocument record;
        const bool parsed = !deserializeJson(record, file);
        file.close();
        String serialized;
        if (!parsed || !validCache(record, serialized)) continue;
        const uint32_t generation = record["seq"] | uint32_t{0};
        if (cacheSlot < 0 || gzeek::newerGeneration(generation, cacheGeneration)) {
            JsonDocument candidate;
            if (!candidate.set(record["schedule"].as<JsonObjectConst>())) continue;
            schedule = std::move(candidate);
            cacheSlot = slot;
            cacheGeneration = generation;
            lastSavedSchedule = serialized;
            scheduleReady = true;
        }
    }
    if (scheduleReady) Serial.println("Loaded complete offline schedule; time sync required");
}

bool saveCachedSchedule() {
    if (!cacheReady) return false;
    String serialized;
    serializeJson(schedule, serialized);
    if (serialized == lastSavedSchedule) return true; // Do not write every 15 seconds.
    JsonDocument record;
    record["version"] = 1;
    uint32_t generation = cacheGeneration + 1;
    if (!generation) generation = 1;
    record["seq"] = generation;
    record["crc"] = gzeek::checksum(serialized.c_str(), serialized.length());
    if (!record["schedule"].set(schedule.as<JsonObjectConst>())) return false;
    const int slot = cacheSlot == 0 ? 1 : 0; // Keep the last good slot untouched.
    const char* path = slot ? "/schedule1.json" : "/schedule0.json";
    File file = LittleFS.open(path, "w");
    if (!file) return false;
    const size_t expected = measureJson(record);
    const size_t written = serializeJson(record, file);
    file.flush();
    file.close();
    if (written != expected) return false;
    File check = LittleFS.open(path, "r");
    JsonDocument verified;
    const bool parsed = check && !deserializeJson(verified, check);
    check.close();
    String verifiedText;
    if (!parsed || !validCache(verified, verifiedText) || verifiedText != serialized ||
        (verified["seq"] | uint32_t{0}) != generation) return false;
    cacheSlot = slot;
    cacheGeneration = generation;
    lastSavedSchedule = serialized;
    return true;
}

struct RoomSchedule {
    JsonObjectConst current;
    JsonObjectConst next;
};

RoomSchedule eventsAt(unsigned room, unsigned nowMinute) {
    RoomSchedule selected{};
    for (JsonObjectConst event : schedule[kRooms[room].jsonKey].as<JsonArrayConst>()) {
        const int start = timeInMinutes(event["start"]);
        const int end = timeInMinutes(event["end"]);
        if (start < 0 || end <= start) {
            continue;
        }
        if (start <= int(nowMinute) && int(nowMinute) < end) {
            selected.current = event;
        } else if (start > int(nowMinute) &&
                   (selected.next.isNull() || start < timeInMinutes(selected.next["start"]))) {
            selected.next = event;
        }
    }
    return selected;
}

RoomSchedule viewEvents(uint32_t now) {
    const auto mode = gzeek::scheduleView(scheduleReady, clockKnown);
    if (mode == gzeek::ScheduleView::unavailable) return {};
    if (mode == gzeek::ScheduleView::timed) {
        return eventsAt(roomIndex, audioState == AudioState::idle ? minuteAt(now) : audioViewMinute);
    }
    const auto events = schedule[kRooms[roomIndex].jsonKey].as<JsonArrayConst>();
    RoomSchedule selected{};
    selected.current = events[offlineEventIndex].as<JsonObjectConst>();
    selected.next = events[offlineEventIndex + 1].as<JsonObjectConst>();
    return selected;
}

void advanceView() {
    const unsigned count = sizeof(kRooms) / sizeof(kRooms[0]);
    const auto events = schedule[kRooms[roomIndex].jsonKey].as<JsonArrayConst>();
    const auto position = gzeek::nextOfflineView(roomIndex, offlineEventIndex,
                                               clockKnown ? 0 : events.size(), count);
    offlineEventIndex = position.event;
    roomIndex = position.room;
}

String oledText(const char* text) {
    String output;
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    while (*bytes) {
        const uint8_t first = *bytes++;
        if (first < 128) {
            output += first < 32 ? ' ' : static_cast<char>(first);
            continue;
        }
        uint32_t cp = first;
        if ((first & 0xE0) == 0xC0 && (*bytes & 0xC0) == 0x80) {
            cp = ((first & 0x1F) << 6) | (*bytes++ & 0x3F);
        } else {
            while ((*bytes & 0xC0) == 0x80) {
                ++bytes;
            }
        }
        switch (cp) {
            case 0x104: output += 'A'; break;
            case 0x105: output += 'a'; break;
            case 0x106: output += 'C'; break;
            case 0x107: output += 'c'; break;
            case 0x118: output += 'E'; break;
            case 0x119: output += 'e'; break;
            case 0x141: output += 'L'; break;
            case 0x142: output += 'l'; break;
            case 0x143: output += 'N'; break;
            case 0x144: output += 'n'; break;
            case 0xD3: output += 'O'; break;
            case 0xF3: output += 'o'; break;
            case 0x15A: output += 'S'; break;
            case 0x15B: output += 's'; break;
            case 0x179: case 0x17B: output += 'Z'; break;
            case 0x17A: case 0x17C: output += 'z'; break;
            default: output += '?';
        }
    }
    return output;
}

void line(int y, const String& text) {
    oled.setCursor(0, y);
    oled.print(text.substring(0, 21));
}

void eventLines(int y, const char* prefix, JsonObjectConst event, uint32_t now) {
    if (event.isNull()) {
        line(y, String(prefix) + " -");
        return;
    }
    line(y, String(prefix) + " " + (event["start"] | "--:--") + " " + (event["end"] | "--:--"));
    const String title = oledText(event["Prelekcja"] | "");
    const unsigned lastPage = title.isEmpty() ? 0 : (title.length() - 1) / kTitlePageChars;
    const unsigned offset = std::min<unsigned>((now - roomShownAt) / kTitlePageMs, lastPage)
                            * kTitlePageChars;
    line(y + 8, title.substring(offset));
    line(y + 16, title.substring(offset + 21));
}

uint32_t roomInterval(uint32_t now) {
    if (!scheduleReady) return kRoomIntervalMs;
    const auto events = viewEvents(now);
    const unsigned longest = std::max(oledText(events.current["Prelekcja"] | "").length(),
                                      oledText(events.next["Prelekcja"] | "").length());
    const unsigned pages = (longest + kTitlePageChars - 1) / kTitlePageChars;
    return gzeek::roomDuration(kRoomIntervalMs, pages, kTitlePageMs);
}

void draw(uint32_t now) {
    if (!oledReady) {
        return;
    }
    oled.clearDisplay();
    const unsigned minute = minuteAt(now);
    char header[22];
    const auto mode = gzeek::scheduleView(scheduleReady, clockKnown);
    if (mode != gzeek::ScheduleView::unavailable) {
        if (mode == gzeek::ScheduleView::timed) {
            snprintf(header, sizeof(header), "%-12s %02u:%02u", kRooms[roomIndex].label, minute / 60, minute % 60);
        } else {
            snprintf(header, sizeof(header), "%s  FULL PLAN", kRooms[roomIndex].label);
        }
        line(0, header);
        const auto events = viewEvents(now);
        eventLines(8, clockKnown ? "NOW" : "PLAN", events.current, now);
        eventLines(32, clockKnown ? "NEXT" : "ALSO", events.next, now);
    } else {
        line(0, "GZEEK 2026");
        line(21, "Waiting for schedule");
    }

    if (!notice.isEmpty() && !due(now, noticeUntil)) {
        line(56, notice);
    } else if (audioState == AudioState::requested) {
        line(56, "Loading speech...");
    } else if (audioState != AudioState::idle) {
        line(56, "Speaking...");
    } else if (!clockKnown && scheduleReady) {
        line(56, "Time sync required");
    } else if (WiFi.status() != WL_CONNECTED || !mqttOnline.load()) {
        line(56, scheduleReady ? "Offline schedule" : "Connecting...");
    } else if (scheduleReady && !hasFreshSchedule(now)) {
        line(56, "Cached / server lost");
    } else if (scheduleReady) {
        line(56, "Press button to read");
    }
    oled.display();
}

void setNotice(const char* text) {
    notice = text;
    noticeUntil = millis() + 3500;
    lastDrawAt = millis() - 500;
}

void stopAudio(const char* reason = nullptr) {
    if (ampPin >= 0) {
        digitalWrite(ampPin, LOW);
        ampPin = -1;
    }
    if (codec) {
        es8311_delete(codec);
        codec = nullptr;
        i2s.end();
    }
    audioState = AudioState::idle;
    audioId = 0; // Discard late packets for a cancelled/timed-out request.
    lastDrawAt = millis() - 500;
    // Preserve the original room dwell timer; the button's 30 s guard is separate.
    refreshSchedule = true;
    if (reason) {
        setNotice(reason);
        Serial.printf("Audio: %s\n", reason);
    }
}

bool startAudio() {
    if (!board || !codecAddress) {
        return false;
    }
    ampPin = board->pa;
    if (ampPin == 11 && !esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO)) {
        ampPin = -1;
        Serial.println("GPIO11 remains VDD_SPI; amp control skipped.");
    }
    if (ampPin >= 0) {
        pinMode(ampPin, OUTPUT);
        digitalWrite(ampPin, LOW);
    }

    i2s.setPins(board->bclk, board->lrck, board->dout, -1, board->mclk);
    if (!i2s.begin(I2S_MODE_STD, kRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
        return false;
    }
    codec = es8311_create(codecAddress);
    if (!codec) {
        i2s.end();
        return false;
    }
    es8311_clock_config_t clock{};
    clock.mclk_from_mclk_pin = true;
    clock.mclk_frequency = kRate * 256;
    clock.sample_frequency = kRate;
    if (es8311_init(codec, &clock, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK ||
        es8311_voice_volume_set(codec, 75, nullptr) != ESP_OK) {
        return false;
    }
    if (ampPin >= 0) {
        digitalWrite(ampPin, HIGH);
    }
    return true;
}

uint32_t littleEndian32(const uint8_t* input) {
    return uint32_t(input[0]) | (uint32_t(input[1]) << 8) |
           (uint32_t(input[2]) << 16) | (uint32_t(input[3]) << 24);
}

void acceptChunk(const uint8_t* data, unsigned length) {
    constexpr unsigned headerBytes = 8;
    if (length < headerBytes || littleEndian32(data) != audioId || audioState != AudioState::playing) {
        return;
    }
    const unsigned bytes = length - headerBytes;
    if (littleEndian32(data + 4) != nextChunk ||
        bytes != std::min<unsigned>(kChunkBytes, expectedBytes - receivedBytes)) {
        stopAudio("Audio packet missing");
        return;
    }
    int16_t stereo[kChunkBytes];
    for (unsigned sample = 0; sample < bytes / 2; ++sample) {
        const auto* pcm = data + headerBytes + sample * 2;
        const int16_t value = static_cast<int16_t>(uint16_t(pcm[0]) | (uint16_t(pcm[1]) << 8));
        stereo[2 * sample] = value;
        stereo[2 * sample + 1] = value;
    }
    if (i2s.write(reinterpret_cast<const uint8_t*>(stereo), bytes * 2) != bytes * 2) {
        stopAudio("Audio output error");
        return;
    }
    ++nextChunk;
    receivedBytes += bytes;
    audioAt = millis();
}

void handleMessage(char* topic, uint8_t* payload, unsigned length) {
    const size_t prefixLength = replyPrefix.length();
    if (strncmp(topic, replyPrefix.c_str(), prefixLength) != 0 || topic[prefixLength] != '/') {
        return;
    }
    const char* subtype = topic + prefixLength + 1;
    if (strcmp(subtype, "audio/chunk") == 0) {
        acceptChunk(payload, length);
        return;
    }

    JsonDocument response;
    if (deserializeJson(response, static_cast<const uint8_t*>(payload), length)) {
        return;
    }
    const uint32_t id = response["id"] | uint32_t{0};
    if (strcmp(subtype, "harmonogram") == 0) {
        if (!id || id != scheduleRequestId) {
            return;
        }
        if (audioState != AudioState::idle) {
            // Ignore an in-flight refresh so it cannot replace the spoken view.
            refreshSchedule = true;
            return;
        }
        const int seconds = response["seconds_of_day"] | -1;
        if (seconds < 0 || seconds >= 86400 || !validSchedule(response["schedule"].as<JsonObjectConst>())) {
            setNotice("Invalid schedule");
            return;
        }
        JsonDocument candidate;
        if (!candidate.set(response["schedule"].as<JsonObjectConst>())) {
            setNotice("Schedule memory error");
            return;
        }
        schedule = std::move(candidate);
        serverSeconds = seconds;
        scheduleReceivedAt = millis();
        scheduleReady = true;
        clockKnown = true;
        clockAnchorUs = esp_timer_get_time();
        offlineEventIndex = 0;
        if (!saveCachedSchedule()) setNotice("Cache write failed");
        Serial.printf("Schedule: %s\n", response["server_time"] | "?");
        return;
    }
    if (!id || id != audioId || audioState == AudioState::idle) {
        return;
    }
    if (strcmp(subtype, "audio/error") == 0) {
        Serial.printf("Audio server: %s\n", response["error"] | "?");
        stopAudio("Speech unavailable");
        refreshSchedule = true;
    } else if (strcmp(subtype, "audio/start") == 0 && audioState == AudioState::requested) {
        expectedBytes = response["bytes"] | unsigned{0};
        if ((response["rate"] | 0) != kRate ||
            strcmp(response["format"] | "", "s16le_mono") != 0 ||
            !expectedBytes || expectedBytes > kMaxAudioBytes || expectedBytes % 2) {
            stopAudio("Invalid audio format");
            return;
        }
        if (!startAudio()) {
            stopAudio("Audio codec error");
            return;
        }
        receivedBytes = 0;
        nextChunk = 0;
        audioAt = millis();
        audioState = AudioState::playing;
    } else if (strcmp(subtype, "audio/end") == 0 && audioState == AudioState::playing) {
        if (receivedBytes != expectedBytes) {
            stopAudio("Incomplete audio");
            return;
        }
        audioAt = millis();
        audioState = AudioState::draining;
    }
}

bool sendRequest(const char* topic, uint32_t id, const char* start = nullptr,
                 const char* title = nullptr) {
    JsonDocument request;
    request["client"] = clientId;
    request["id"] = id;
    if (start) {
        request["start"] = start;
    }
    if (title) request["title"] = title;
    String payload;
    serializeJson(request, payload);
    if (!outgoingQueue || !mqttOnline.load() || WiFi.status() != WL_CONNECTED) return false;
    OutgoingMessage message{};
    if (strlen(topic) >= sizeof(message.topic) || payload.length() >= sizeof(message.payload)) return false;
    strlcpy(message.topic, topic, sizeof(message.topic));
    strlcpy(message.payload, payload.c_str(), sizeof(message.payload));
    return xQueueSend(outgoingQueue, &message, 0) == pdTRUE;
}

void readCurrentTitle(uint32_t now) {
    if (audioState != AudioState::idle) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED) { setNotice("No Wi-Fi connection"); return; }
    if (!mqttOnline.load()) { setNotice("MQTT unavailable"); return; }
    if (!scheduleReady) { setNotice("No saved schedule"); return; }
    if (!clockKnown) { setNotice("Time sync required"); return; }
    const auto current = eventsAt(roomIndex, minuteAt(now)).current;
    if (current.isNull()) {
        setNotice("No current talk");
        return;
    }
    if (!codecAddress) {
        setNotice("Audio codec missing");
        return;
    }
    audioId = newId();
    if (sendRequest(kRooms[roomIndex].audioTopic, audioId, current["start"] | "",
                    current["Prelekcja"] | "")) {
        audioViewMinute = minuteAt(now);
        audioState = AudioState::requested;
        audioAt = now;
        audioRequestedAt = now;
        Serial.printf("Loading speech: %s\n", kRooms[roomIndex].label);
    } else {
        setNotice("MQTT request failed");
    }
}

void pollButton(uint32_t now) {
    static bool stable = HIGH;
    static bool previous = HIGH;
    static uint32_t changedAt = 0;
    const bool reading = digitalRead(kButtonPin);
    if (reading != previous) {
        previous = reading;
        changedAt = now;
    }
    if (reading != stable && now - changedAt >= 30) {
        stable = reading;
        if (stable == LOW) {
            lastButtonAt = now;
            buttonWasPressed = true;
            // Busy clicks extend the pause without interrupting or queuing audio.
            readCurrentTitle(now);
        }
    }
}

// MQTT is owned by this worker. Reconnects and DNS never block OLED/button handling.
void enqueueMessage(char* topic, uint8_t* payload, unsigned length) {
    if (length > 16384 || strlen(topic) >= 128) return;
    IncomingMessage message{};
    message.payload = static_cast<uint8_t*>(malloc(length + 1));
    if (!message.payload) return;
    memcpy(message.payload, payload, length);
    message.payload[length] = 0;
    message.length = length;
    strlcpy(message.topic, topic, sizeof(message.topic));
    if (xQueueSend(incomingQueue, &message, pdMS_TO_TICKS(100)) != pdTRUE) free(message.payload);
}

void networkTask(void*) {
    mqtt.setServer(kBrokerHost, kBrokerPort);
    mqtt.setCallback(enqueueMessage);
    mqtt.setSocketTimeout(1);
    socket.setConnectionTimeout(750);
    if (!mqtt.setBufferSize(16384)) {
        Serial.println("MQTT buffer allocation failed; offline mode remains available");
        vTaskDelete(nullptr);
        return;
    }
    uint32_t reconnectAt = 0;
    for (;;) {
        const auto now = millis();
        if (WiFi.status() != WL_CONNECTED) {
            mqttOnline.store(false);
            socket.stop();
        } else {
            if (!mqtt.connected() && due(now, reconnectAt)) {
                reconnectAt = now + 3000;
                const bool connected = *kBrokerUser ?
                    mqtt.connect(clientId.c_str(), kBrokerUser, kBrokerPassword) : mqtt.connect(clientId.c_str());
                if (connected && !mqtt.subscribe((replyPrefix + "/#").c_str())) mqtt.disconnect();
            }
            mqttOnline.store(mqtt.connected());
            if (mqtt.connected()) mqtt.loop();
        }
        OutgoingMessage message;
        // Discard stale requests while disconnected; main code will retry schedules.
        for (unsigned i = 0; i < 4 && xQueueReceive(outgoingQueue, &message, 0) == pdTRUE; ++i) {
            if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
                if (!mqtt.publish(message.topic, message.payload)) {
                    mqttOnline.store(false);
                    socket.stop();
                }
            }
        }
        if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) mqttOnline.store(false);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void serviceMqtt(uint32_t now) {
    static bool wasOnline = false;
    const bool online = mqttOnline.load() && WiFi.status() == WL_CONNECTED;
    if (online && !wasOnline) refreshSchedule = true;
    wasOnline = online;
    IncomingMessage message;
    // Bound the work per UI iteration, including synchronous I2S writes.
    for (unsigned i = 0; incomingQueue && i < 4 &&
         xQueueReceive(incomingQueue, &message, 0) == pdTRUE; ++i) {
        handleMessage(message.topic, message.payload, message.length);
        free(message.payload);
    }
    if (online && audioState == AudioState::idle &&
        (refreshSchedule || due(now, nextScheduleAt))) {
        scheduleRequestId = newId();
        const bool sent = sendRequest(kScheduleRequest, scheduleRequestId);
        nextScheduleAt = now + (sent ? kRefreshMs : 3000);
        refreshSchedule = false;
    }
}
} // namespace

void startGzeek() {
    Serial.begin(115200);
    delay(900);
    initializeBoard();
    pinMode(kButtonPin, INPUT_PULLUP);

    char id[24];
    snprintf(id, sizeof(id), "gzeek-%012llx", static_cast<unsigned long long>(ESP.getEfuseMac()));
    clientId = id;
    replyPrefix = "gzeek/odpowiedz/" + clientId;
    sequenceId = esp_random();
    loadCachedSchedule();
    // Show cache before starting Wi-Fi; cold boot without a clock browses FULL PLAN.
    draw(millis());
    outgoingQueue = xQueueCreate(4, sizeof(OutgoingMessage));
    incomingQueue = xQueueCreate(16, sizeof(IncomingMessage));
    if (!outgoingQueue || !incomingQueue) {
        setNotice("Network memory error");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(kWifiSsid, *kWifiPassword ? kWifiPassword : nullptr);
    if (xTaskCreate(networkTask, "gzeek-mqtt", 8192, nullptr, 1, nullptr) != pdPASS) {
        setNotice("Network task failed");
    }
}

void runGzeek() {
    auto now = millis();
    serviceMqtt(now);
    now = millis();
    if (audioState != AudioState::idle) {
        const auto failure = gzeek::audioFailure(
            WiFi.status() == WL_CONNECTED, mqttOnline.load(),
            audioState == AudioState::requested, now, audioRequestedAt, audioAt,
            kAudioRequestTimeoutMs, kAudioChunkTimeoutMs, kAudioTotalTimeoutMs);
        if (failure) stopAudio(failure);
        else if (audioState == AudioState::draining && now - audioAt >= 100) stopAudio();
    }

    pollButton(now);
    if (audioState == AudioState::idle && scheduleReady) {
        const auto events = viewEvents(now);
        String key;
        serializeJson(events.current, key);
        key += '|';
        serializeJson(events.next, key);
        if (key != roomContentKey) {
            roomContentKey = key;
            roomShownAt = now; // New content starts at its first title page.
        }
    }
    if ((audioState == AudioState::idle || audioState == AudioState::requested ||
         audioState != lastDrawAudioState) && now - lastDrawAt >= 350) {
        if (gzeek::mayAdvance(now, roomShownAt, roomInterval(now),
                              audioState != AudioState::idle,
                              buttonWasPressed, lastButtonAt, kButtonPauseMs)) {
            advanceView();
            roomShownAt = now;
            roomContentKey = "";
        }
        lastDrawAt = now;
        draw(now);
        lastDrawAudioState = audioState;
    }
    delay(1);
}
