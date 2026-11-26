#pragma once

constexpr char kWifiSsid[] = "B1337";
constexpr char kWifiPassword[] = "test1234TEST1234!"; // Open network. For WPA2 use >=8 characters, e.g. test2026.
constexpr char kBrokerHost[] = "192.168.88.2"; // Address of Kali on the test Wi-Fi.
constexpr unsigned kBrokerPort = 1883;
constexpr char kBrokerUser[] = "mqtt";
constexpr char kBrokerPassword[] = "gzeek{you_can_see_me}";

constexpr int kButtonPin = 9; // BOOT/user button, active LOW.
constexpr int kOledRotation = 0; // Correct orientation on the tested YYLX-C3.
constexpr unsigned long kRoomIntervalMs = 20000;

constexpr unsigned long kButtonPauseMs = 30000; // No automatic room change after last click.

// Failed speech requests return to the schedule without a permanent loading screen.
constexpr unsigned long kAudioRequestTimeoutMs = 8000;
constexpr unsigned long kAudioChunkTimeoutMs = 2500;
constexpr unsigned long kAudioTotalTimeoutMs = 30000;
