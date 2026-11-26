#!/bin/sh
set -eu
umask 077
mkdir -p /auth /audio-cache
chown 0:0 /auth
chmod 0711 /auth
chown 10001:10001 /audio-cache
chmod 0700 /audio-cache
device_password="$(cat /run/secrets/mqtt_password)"
if [ -z "$device_password" ]; then
    echo "MQTT device password must not be empty" >&2
    exit 1
fi
# Generate a separate random credential for the Python service account.
server_password="$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')"
[ "${#server_password}" -eq 64 ]
mosquitto_passwd -b -c /auth/passwd.new mqtt "$device_password"
mosquitto_passwd -b /auth/passwd.new gzeek-server "$server_password"
printf '%s\n' "$server_password" > /auth/server_password.new
chown 1883:1883 /auth/passwd.new
chmod 0400 /auth/passwd.new
chown 10001:10001 /auth/server_password.new
chmod 0400 /auth/server_password.new
mv -f /auth/passwd.new /auth/passwd
mv -f /auth/server_password.new /auth/server_password
unset device_password server_password
echo "MQTT authentication initialized (anonymous access disabled)."
