import ast
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import yaml
ROOT = Path(__file__).resolve().parents[1]

class ContainerConfiguration(unittest.TestCase):
    def test_isolation_and_credentials(self):
        compose = yaml.safe_load((ROOT/'compose.yaml').read_text())
        services = compose['services']
        for name, uid in [('mqtt','1883:1883'), ('schedule','10001:10001')]:
            self.assertEqual(services[name]['user'], uid)
            self.assertTrue(services[name]['read_only'])
            self.assertEqual(services[name]['cap_drop'], ['ALL'])
            self.assertIn('no-new-privileges:true', services[name]['security_opt'])
        self.assertNotIn('ports', services['schedule'])
        self.assertEqual(services['schedule']['networks'], ['backend'])
        self.assertTrue(compose['networks']['backend']['internal'])
        self.assertEqual(services['auth-init']['network_mode'], 'none')
        self.assertTrue(services['auth-init']['read_only'])
        self.assertTrue(any(str(m).startswith('/tmp:rw,') for m in services['auth-init']['tmpfs']))
        self.assertIn('user mqtt\n', (ROOT/'docker/acl').read_text())
        self.assertIn('/auth/passwd.new mqtt ', (ROOT/'docker/init-auth.sh').read_text())
        self.assertIn('kBrokerUser[] = "mqtt"', (ROOT/'firmware/GzeekMqtt/config.h').read_text())
        self.assertNotIn('GZ_MQTT_PASSWORD', services['schedule']['environment'])
        self.assertIn('service_completed_successfully',
                      services['mqtt']['depends_on']['auth-init']['condition'])
        self.assertIn('allow_anonymous false', (ROOT/'docker/mosquitto.conf').read_text())
        secret = (ROOT/'secrets/mqtt_password.txt').read_text().strip()
        self.assertEqual(secret, 'gzeek{you_can_see_me}')
        self.assertIn('kBrokerPassword[] = "'+secret+'"',
                      (ROOT/'firmware/GzeekMqtt/config.h').read_text())

    def test_password_file_and_environment_fallback(self):
        tree = ast.parse((ROOT/'server/server.py').read_text())
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                     and node.name == 'read_mqtt_password']
        ns = {'os':os, 'Path':Path}
        exec(compile(ast.Module(body=functions,type_ignores=[]),'server.py','exec'),ns)
        with tempfile.TemporaryDirectory() as tmp:
            secret = Path(tmp)/'secret'
            secret.write_text('secret{with_braces}\n')
            with patch.dict(os.environ, {'GZ_MQTT_PASSWORD_FILE':str(secret)}, clear=True):
                self.assertEqual(ns['read_mqtt_password'](), 'secret{with_braces}')
            with patch.dict(os.environ, {'GZ_MQTT_PASSWORD':'fallback'}, clear=True):
                self.assertEqual(ns['read_mqtt_password'](), 'fallback')
            with patch.dict(os.environ, {}, clear=True):
                with self.assertRaises(RuntimeError): ns['read_mqtt_password']()
            secret.write_text('')
            with patch.dict(os.environ, {'GZ_MQTT_PASSWORD_FILE':str(secret)}, clear=True):
                with self.assertRaises(RuntimeError): ns['read_mqtt_password']()

if __name__ == '__main__':
    unittest.main()
