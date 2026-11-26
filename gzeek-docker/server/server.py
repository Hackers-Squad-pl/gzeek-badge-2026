#!/usr/bin/env python3
import argparse
import hashlib
import json
import logging
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path
from threading import BoundedSemaphore
from zoneinfo import ZoneInfo

import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parent
SCHEDULE_TOPIC = "/gzeek/prelekcje"
AUDIO_PREFIX = "gzeek/prelekcja_audio/"
AUDIO_ROOMS = {"sala_a": "A", "sala_b": "B", "sala_warsztat": "warsztat"}
REPLY_PREFIX = "gzeek/odpowiedz"
CLIENT_ID = re.compile(r"[A-Za-z0-9_-]{1,32}")
CLOCK = re.compile(r"(?:[01][0-9]|2[0-3]):[0-5][0-9]")
SAMPLE_RATE = 24000
CHUNK_BYTES = 960
MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * 20
MAX_SCHEDULE_BYTES = 15000


def read_mqtt_password():
    secret_path = os.getenv("GZ_MQTT_PASSWORD_FILE")
    password = (Path(secret_path).read_text(encoding="utf-8").strip()
                if secret_path else os.getenv("GZ_MQTT_PASSWORD"))
    if not password:
        raise RuntimeError("MQTT password is missing")
    return password


def compact_json(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def minutes(value):
    if not isinstance(value, str) or not CLOCK.fullmatch(value):
        raise ValueError(f"Invalid time: {value!r}")
    hours, mins = value.split(":")
    return int(hours) * 60 + int(mins)


def load_schedule(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or set(data) != set(AUDIO_ROOMS.values()):
        raise ValueError("Schedule must contain A, B and warsztat")
    for room, events in data.items():
        if not isinstance(events, list):
            raise ValueError(f"Room {room} requires a list")
        for event in events:
            if not isinstance(event, dict):
                raise ValueError(f"Invalid entry in room {room}")
            start = minutes(event.get("start"))
            end = minutes(event.get("end"))
            if start >= end:
                raise ValueError(f"Talk in room {room} requires start < end")
            for key in ("Prelekcja", "prelegent"):
                value = event.get(key)
                if not isinstance(value, str) or not value.strip() or len(value) > 200 or "\x00" in value:
                    raise ValueError(f"Invalid field {key} in room {room}")
        events.sort(key=lambda item: item["start"])
        if any(a["end"] > b["start"] for a, b in zip(events, events[1:])):
            raise ValueError(f"Overlapping talks in room {room}")
    return data


def current_event(events, now):
    minute = now.hour * 60 + now.minute
    return next((event for event in events
                 if minutes(event["start"]) <= minute < minutes(event["end"])), None)


class GzeekServer:
    def __init__(self, options):
        self.options = options
        self.tz = ZoneInfo(options.timezone)
        self.options.cache.mkdir(parents=True, exist_ok=True)
        self.voice = options.voice
        self.espeak = shutil.which("espeak-ng")
        self.ffmpeg = shutil.which("ffmpeg")
        if not self.espeak or not self.ffmpeg:
            raise RuntimeError("Install espeak-ng and ffmpeg")
        self.jobs = ThreadPoolExecutor(max_workers=2)
        self.slots = BoundedSemaphore(4)
        self.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=options.server_id)
        if options.username is not None:
            self.mqtt.username_pw_set(options.username, read_mqtt_password())
        self.mqtt.on_connect = self.on_connect
        self.mqtt.on_message = self.on_message

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            logging.error("MQTT: %s", reason_code)
            return
        client.subscribe([(SCHEDULE_TOPIC, 0), (AUDIO_PREFIX + "+", 0)])
        logging.info("MQTT connected, timezone %s", self.tz)

    def publish(self, topic, payload):
        result = self.mqtt.publish(topic, payload, qos=0, retain=False)
        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            raise RuntimeError(f"MQTT publish {topic}: {result.rc}")

    def on_message(self, client, userdata, message):
        if message.retain or len(message.payload) > 2048:
            return
        try:
            request = json.loads(message.payload)
            peer = request.get("client") if isinstance(request, dict) else None
            if not isinstance(peer, str) or not CLIENT_ID.fullmatch(peer):
                return
            request_id = request.get("id")
            if type(request_id) is not int or not 0 < request_id <= 0xFFFFFFFF:
                return
        except (ValueError, UnicodeError):
            return
        reply = f"{REPLY_PREFIX}/{peer}"
        if message.topic == SCHEDULE_TOPIC:
            try:
                now = datetime.now(self.tz)
                response = {
                    "id": request_id, "server_time": now.isoformat(timespec="seconds"),
                    "seconds_of_day": now.hour * 3600 + now.minute * 60 + now.second,
                    "schedule": load_schedule(self.options.schedule),
                }
                payload = compact_json(response)
                if len(payload) > MAX_SCHEDULE_BYTES:
                    raise ValueError(f"Schedule exceeds {MAX_SCHEDULE_BYTES} B")
                self.publish(reply + "/harmonogram", payload)
            except (OSError, ValueError, RuntimeError) as error:
                logging.error("Schedule: %s", error)
                self.mqtt.publish(reply + "/harmonogram",
                                  compact_json({"id": request_id, "error": "schedule_invalid"}))
            return

        room = AUDIO_ROOMS.get(message.topic.removeprefix(AUDIO_PREFIX))
        expected_start = request.get("start")
        if not room or not isinstance(expected_start, str) or not CLOCK.fullmatch(expected_start):
            return
        expected_title = request.get("title")
        if expected_title is not None and not isinstance(expected_title, str):
            return
        audio_reply = reply + "/audio"
        if not self.slots.acquire(blocking=False):
            self.mqtt.publish(audio_reply + "/error",
                              compact_json({"id": request_id, "error": "busy"}))
            return
        self.jobs.submit(self.serve_audio, audio_reply, request_id, room, expected_start, expected_title)

    def generate_pcm(self, title):
        key = hashlib.sha256(f"espeak-ng|{self.voice}|135|38|{SAMPLE_RATE}|{title}".encode()).hexdigest()
        cached = self.options.cache / f"{key}.pcm"
        if cached.is_file():
            data = cached.read_bytes()
            if 0 < len(data) <= MAX_AUDIO_BYTES and len(data) % 2 == 0:
                return data
        with tempfile.TemporaryDirectory(dir=self.options.cache) as temp:
            wav = Path(temp) / "speech.wav"
            pcm = Path(temp) / "speech.pcm"
            subprocess.run([self.espeak, "-v", self.voice, "-s", "135", "-p", "38",
                            "-w", str(wav), "--stdin"], input=title.encode("utf-8"),
                           check=True, timeout=12, capture_output=True)
            subprocess.run([self.ffmpeg, "-nostdin", "-v", "error", "-i", str(wav),
                            "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "s16le", str(pcm)],
                           check=True, timeout=12, capture_output=True)
            data = pcm.read_bytes()
            if not 0 < len(data) <= MAX_AUDIO_BYTES or len(data) % 2:
                raise ValueError("Audio must contain between 1 sample and 20 seconds")
            pcm.replace(cached)
        return data

    def serve_audio(self, reply, request_id, room, expected_start, expected_title=None):
        try:
            event = current_event(load_schedule(self.options.schedule)[room], datetime.now(self.tz))
            if (event is None or event["start"] != expected_start or
                    (expected_title is not None and event["Prelekcja"] != expected_title)):
                self.publish(reply + "/error", compact_json({"id": request_id, "error": "schedule_changed"}))
                return
            pcm = self.generate_pcm(event["Prelekcja"])
            self.publish(reply + "/start", compact_json({
                "id": request_id, "bytes": len(pcm), "rate": SAMPLE_RATE, "format": "s16le_mono",
            }))
            for sequence, offset in enumerate(range(0, len(pcm), CHUNK_BYTES)):
                self.publish(reply + "/chunk",
                             struct.pack("<II", request_id, sequence) + pcm[offset:offset + CHUNK_BYTES])
                # Slightly faster than playback so normal network jitter does not starve I2S.
                time.sleep(0.017)
            self.publish(reply + "/end", compact_json({"id": request_id}))
            logging.info("Audio %s, %d B", room, len(pcm))
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            logging.error("Audio %s: %s", room, error)
            self.mqtt.publish(reply + "/error", compact_json({"id": request_id, "error": "audio_failed"}))
        finally:
            self.slots.release()

    def run(self):
        load_schedule(self.options.schedule)
        self.mqtt.connect_async(self.options.host, self.options.port, keepalive=30)
        try:
            self.mqtt.loop_forever(retry_first_connection=True)
        finally:
            self.mqtt.disconnect()
            self.jobs.shutdown(wait=True)


def main():
    parser = argparse.ArgumentParser(description="Gzeek schedule and speech over MQTT")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--timezone", default="Europe/Warsaw")
    parser.add_argument("--schedule", type=Path, default=ROOT / "harmonogram.json")
    parser.add_argument("--cache", type=Path, default=ROOT / "audio_cache")
    parser.add_argument("--voice", default="pl")
    parser.add_argument("--username", help="Password read from GZ_MQTT_PASSWORD_FILE or GZ_MQTT_PASSWORD")
    parser.add_argument("--server-id", default="gzeek-kali-server")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        GzeekServer(parser.parse_args()).run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
