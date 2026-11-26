"""Offline tests of schedule reload and audio snapshot validation; no broker needed."""
import ast
import json
import logging
import re
import struct
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock
from zoneinfo import ZoneInfo

# Load the real server definitions without importing the optional MQTT client.
source = Path(__file__).resolve().parents[1] / 'server/server.py'
tree = ast.parse(source.read_text())
keep = [node for node in tree.body if isinstance(node, (ast.FunctionDef, ast.ClassDef))
        and getattr(node, 'name', '') != 'main']
ns = dict(json=json, re=re, logging=logging, struct=struct, datetime=datetime,
          CLOCK=re.compile(r'(?:[01][0-9]|2[0-3]):[0-5][0-9]'),
          AUDIO_ROOMS={'sala_a':'A','sala_b':'B','sala_warsztat':'warsztat'},
          SAMPLE_RATE=24000, CHUNK_BYTES=960, time=SimpleNamespace(sleep=lambda _:None))
exec(compile(ast.Module(body=keep, type_ignores=[]), str(source), 'exec'), ns)

class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)/'schedule.json'
        self.data = {'A':[{'start':'00:00','end':'23:59','Prelekcja':'First title','prelegent':'Speaker'}],
                     'B':[], 'warsztat':[]}
        self.write()

    def write(self):
        self.path.write_text(json.dumps(self.data), encoding='utf-8')

    def test_reload_reads_changed_file(self):
        self.assertEqual(ns['load_schedule'](self.path)['A'][0]['Prelekcja'], 'First title')
        self.data['A'][0]['Prelekcja'] = 'Updated title'
        self.write()
        self.assertEqual(ns['load_schedule'](self.path)['A'][0]['Prelekcja'], 'Updated title')

    def server(self):
        obj = ns['GzeekServer'].__new__(ns['GzeekServer'])
        obj.options = SimpleNamespace(schedule=self.path)
        obj.tz = ZoneInfo('Europe/Warsaw')
        obj.slots = Mock()
        obj.publish = Mock()
        obj.mqtt = Mock()
        obj.generate_pcm = Mock(return_value=b'\x00\x00'*20)
        return obj

    def test_changed_title_does_not_speak_different_event(self):
        obj = self.server()
        obj.serve_audio('reply', 42, 'A', '00:00', 'Outdated title')
        obj.generate_pcm.assert_not_called()
        topic, payload = obj.publish.call_args.args
        self.assertEqual(topic, 'reply/error')
        self.assertEqual(json.loads(payload)['error'], 'schedule_changed')
        obj.slots.release.assert_called_once()

    def test_matching_title_is_streamed_to_completion(self):
        # Fix the wall clock so the test also works in the 23:59 gap.
        old = ns['datetime']
        ns['datetime'] = SimpleNamespace(now=lambda tz: datetime(2026,11,19,12,0,tzinfo=tz))
        self.addCleanup(lambda: ns.update(datetime=old))
        obj = self.server()
        obj.serve_audio('reply',42,'A','00:00','First title')
        obj.generate_pcm.assert_called_once_with('First title')
        topics = [c.args[0] for c in obj.publish.call_args_list]
        self.assertEqual(topics, ['reply/start','reply/chunk','reply/end'])
        obj.slots.release.assert_called_once()

if __name__ == '__main__':
    unittest.main()
