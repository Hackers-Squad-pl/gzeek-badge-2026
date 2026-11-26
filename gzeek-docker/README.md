# Gzeek 2026 — Docker Compose

Broker Mosquitto i serwer harmonogramu/audio działają w kontenerach.
Paczka zawiera pełny firmware ESP32 i rzeczywisty program konferencji.

## Uruchomienie

Wymagane: Docker Engine i Docker Compose v2. Na hoście nie instalujesz
Pythona, Mosquitto, espeak-ng ani FFmpeg.

```bash
unzip Gzeek2026-Docker.zip
cd gzeek-docker
```

W `.env` jest domyślnie:

```dotenv
MQTT_BIND_IP=10.42.0.1
MQTT_PORT=1883
GZ_VOICE=pl
```

`MQTT_BIND_IP` musi być istniejącym adresem hosta dostępnym dla ESP32.
Sprawdź `ip -br addr`; jeżeli używasz innej sieci, zmień IP także
w `firmware/GzeekMqtt/config.h`. Nie ustawiaj tu adresu kontenera.

Jeżeli port 1883 zajmuje wcześniejszy broker, zatrzymaj go. Dla usługi
systemowej jest to `sudo systemctl stop mosquitto`; dla instancji
uruchomionej w terminalu — Ctrl+C.

```bash
docker compose config --quiet
docker compose up -d --build
docker compose ps -a
docker compose logs -f mqtt schedule
```

`auth-init` ma zakończyć się kodem 0 — to jednorazowa inicjalizacja,
nie stale działająca usługa. Serwisy `mqtt` i `schedule` pozostają uruchomione.
Jeżeli Docker wymaga uprawnień administratora, poprzedź komendy `sudo`.

## Login i hasło ESP32

- Login: `mqtt`
- Hasło: `gzeek{you_can_see_me}`

Są już wpisane w dołączonym `firmware/GzeekMqtt/config.h`:

```cpp
constexpr char kBrokerHost[] = "10.42.0.1";
constexpr unsigned kBrokerPort = 1883;
constexpr char kBrokerUser[] = "mqtt";
constexpr char kBrokerPassword[] = "gzeek{you_can_see_me}";
```

Zachowaj własne dane Wi-Fi, piny i obrót ekranu. Otwórz GzeekMqtt.ino,
ustaw ESP32C3 Dev Module / USB CDC enabled / DIO / 40 MHz i działający
schemat partycji z obszarem SPIFFS. Nie kasuj całej flash, jeśli chcesz
zachować wcześniejszą kopię harmonogramu offline. Skompiluj i wgraj.
Interfejs firmware i komunikaty błędów pozostają po angielsku.

## Harmonogram i głos

Edytuj `data/harmonogram.json`. Serwer ponownie czyta ten plik przy każdym
żądaniu; firmware odświeża go co 15 s. Plik jest montowany do kontenera
jako katalog tylko do odczytu — edycja i atomowa podmiana z hosta działają.
Aktualizacja JSON nie wymaga przebudowy ani restartu kontenerów.

Program pochodzi z https://gzeek.pl/posts/program/ — Aula A, Aula B,
warsztaty z prefiksem sali. Dla zamknięcia rozpoczynającego się o 17:30
roboczo przyjęto koniec o 17:45, którego strona nie podaje.

Domyślnie eSpeak NG mówi po polsku. Aby użyć angielskiego głosu, zmień
`GZ_VOICE=en-us` w `.env` i wykonaj `docker compose up -d`.
Kontener używa zegara hosta i strefy `Europe/Warsaw`, niezależnie od strefy
lokalnego terminala. Audio uruchamia wyłącznie przycisk na ESP32.

## Izolacja i uwierzytelnianie

- Anonimowe logowanie jest wyłączone. Port 1883 publikowany tylko na
  adresie wskazanym przez MQTT_BIND_IP. Serwer Python nie publikuje portów.
- Mosquitto pracuje jako UID 1883, Python jako UID 10001; oba serwisy
  mają główny system plików tylko do odczytu, usunięte capabilities,
  no-new-privileges oraz limity pamięci, CPU, procesów i logów.
- Python jest wyłącznie w wewnętrznej sieci Docker. Broker ma dodatkową
  sieć do obsługi połączeń ESP32. Brak host network, privileged,
  montowania gniazda Dockera i dostępu do urządzeń hosta.
- `auth-init` działa jednorazowo jako root, bez sieci; ma tylko uprawnienia
  potrzebne do przygotowania plików haseł i praw katalogów w wolumenach.
- Hasło urządzeń trafia do init z pliku `secrets/mqtt_password.txt`.
  Nie jest wpisane w Dockerfile ani w zmienne środowiskowe serwera.
  Init generuje hash Mosquitto i odrębne losowe hasło konta `gzeek-server`.
- Konto urządzeń może publikować tylko żądania harmonogramu/audio i
  odczytywać odpowiedzi pod własnym MQTT client id. Nie może publikować
  odpowiedzi. Konto serwera ma uprawnienia odwrotne. Wspólne konto urządzeń
  nie daje kryptograficznej identyfikacji pojedynczej płytki.
- Root systemu hosta oraz administrator Dockera nadal kontrolują kontenery
  i ich dane. MQTT na 1883 nie używa TLS; hasło logowania nie szyfruje ruchu.
  Ta paczka nie zmienia firmware na TLS i nie wystawia 8883.

Plik z hasłem jest celowo dostarczony zgodnie z żądaniem. Zachowaj go
prywatnie; `.gitignore` wyklucza `secrets/` i `.env` z przypadkowego commitu.

## Test po uruchomieniu

Po `docker compose up -d --build` wykonaj:

```bash
docker compose run --rm --no-deps -T --entrypoint python schedule /app/smoke_test.py < secrets/mqtt_password.txt
```

Test sprawdza odrzucanie logowania anonimowego i błędnego hasła,
zakaz publikowania fałszywych odpowiedzi przez konto ESP32, odbiór pełnego
harmonogramu oraz logowanie konta serwera. Hasło jest podawane przez stdin,
nie jako argument testu. Test używa MQTT 5 do sprawdzenia kodów odmowy;
firmware nadal używa MQTT 3.1.1 i nie wymaga zmiany protokołu.

## Zmiana hasła i administracja

Zmień `secrets/mqtt_password.txt` i hasło w firmware, następnie:

```bash
docker compose run --rm auth-init
docker compose restart mqtt schedule
```

Inicjalizacja losuje również nowe hasło konta serwera. Zrestartuj oba
serwisy, aby broker i Python używały nowych danych.

```bash
docker compose logs --tail=100 mqtt schedule
docker compose down
```

`down` zachowuje wolumeny z hasłami i cache audio; nie dodawaj `-v`,
jeśli chcesz je zachować. JSON pozostaje w katalogu data na hoście.

## Weryfikacja tej paczki

Sprawdzono parsowanie YAML, zgodność haseł firmware/secret, izolację
zadeklarowaną w Compose, składnię shell/Python i 5 testów logiki serwera
oraz odczytu sekretów. W środowisku przygotowania nie było Docker Engine:
nie zbudowano obrazów i nie wykonano testu uruchomieniowego kontenerów.
Dołączony smoke_test.py służy tej weryfikacji na hoście docelowym.
Nie wykonywano także pełnej kompilacji firmware ESP32.

## Poprawka inicjalizacji haseł

Usługa auth-init ma zapisywalny /tmp jako tmpfs. mosquitto_passwd używa
pliku tymczasowego przy dodawaniu drugiego użytkownika. Główny system
plików nadal jest tylko do odczytu. Login urządzeń w tej wersji to mqtt,
a hasło pozostaje gzeek{you_can_see_me}. Konto serwera pozostaje gzeek-server.

Jeżeli wdrożenie jest już skonfigurowane, wystarczy dopisać tmpfs do
auth-init w swoim compose.yaml. Zachowaj własny .env i lokalne dane Wi-Fi.
Następnie docker compose up -d --force-recreate uruchomi init i oba serwisy
z nowo przygotowanymi danymi. Nie trzeba usuwać wolumenów.
