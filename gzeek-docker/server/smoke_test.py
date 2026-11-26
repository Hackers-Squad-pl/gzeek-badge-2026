#!/usr/bin/env python3
"""Run inside the schedule image. Read the device password from stdin."""
import json
import queue
import sys
import threading
import time
import uuid
from pathlib import Path

import paho.mqtt.client as mqtt

HOST = 'mqtt'
PASSWORD = sys.stdin.readline().rstrip('\r\n')
if not PASSWORD:
    raise SystemExit('Pass the device password on stdin.')


def session(username=None, password=None, accepted=True, client_id=None):
    event = threading.Event()
    status = []
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                    client_id=client_id or 'smoke-' + uuid.uuid4().hex[:12],
                    protocol=mqtt.MQTTv5)
    if username is not None:
        c.username_pw_set(username, password)
    def connected(client, userdata, flags, reason_code, properties):
        status.append(not reason_code.is_failure)
        event.set()
    c.on_connect = connected
    c.connect(HOST, 1883, 10)
    c.loop_start()
    if not event.wait(5) or status != [accepted]:
        c.disconnect()
        c.loop_stop()
        raise AssertionError('Unexpected authentication result')
    return c


def close(c):
    c.disconnect()
    c.loop_stop()


for username, password in [(None, None), ('mqtt', PASSWORD + '-wrong')]:
    c = session(username, password, accepted=False)
    close(c)
print('PASS: anonymous and incorrect-password connections rejected')

cid = 'gzeek-smoke-' + uuid.uuid4().hex[:10]
c = session('mqtt', PASSWORD, client_id=cid)
messages = queue.Queue()
acks = queue.Queue()
subscribed = threading.Event()
c.on_message = lambda client, userdata, message: messages.put((message.topic, message.payload))
c.on_publish = lambda client, userdata, mid, reason, props: acks.put((mid, reason.value))
c.on_subscribe = lambda *args: subscribed.set()
c.subscribe(f'gzeek/odpowiedz/{cid}/#', qos=1)
assert subscribed.wait(5), 'Subscription timeout'

# MQTT 5 PUBACK must deny a device pretending to be the schedule server.
info = c.publish(f'gzeek/odpowiedz/{cid}/harmonogram', b'{}', qos=1)
deadline = time.monotonic() + 5
while True:
    mid, reason = acks.get(timeout=max(0.1, deadline-time.monotonic()))
    if mid == info.mid:
        assert reason == 135, f'Expected Not authorized, got {reason}'
        break
print('PASS: device cannot forge schedule responses')

# Real server roundtrip; allow time for the service to connect after first startup.
response = None
for request_id in range(1, 6):
    c.publish('/gzeek/prelekcje', json.dumps({'client': cid, 'id': request_id}), qos=1)
    try:
        topic, payload = messages.get(timeout=3)
        response = json.loads(payload)
        if 'schedule' in response:
            break
    except queue.Empty:
        continue
assert response and set(response['schedule']) == {'A', 'B', 'warsztat'}, 'No complete schedule response'
assert 0 <= response['seconds_of_day'] < 86400
print('PASS: authenticated device receives all rooms and server time')
close(c)

server_password = Path('/auth/server_password').read_text().strip()
c = session('gzeek-server', server_password)
close(c)
print('PASS: separate service credential authenticates')
print('All Docker authentication/protocol checks passed.')
