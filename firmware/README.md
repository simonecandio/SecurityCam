# Security Camera - ESP32-S3 Freenove WROOM
## Progetto esame IoT e Programmazione Mobile

### Struttura progetto
```
esp32_security_cam/
├── CMakeLists.txt
├── sdkconfig.defaults          <- config per PSRAM, flash, wifi ecc
├── partitions.csv              <- partizioni custom (3MB app)
└── main/
    ├── CMakeLists.txt
    ├── main.c                  <- entry point, boot sequence
    ├── camera_handler.c        <- init camera OV2640, cattura frame
    ├── pir_handler.c           <- interrupt PIR + debounce + cooldown
    ├── wifi_manager.c          <- connessione STA, auto-reconnect
    ├── softap_provisioning.c   <- hotspot + pagina web per config iniziale
    ├── http_server.c           <- stream MJPEG, /capture, /status
    ├── sd_card_manager.c       <- salvataggio locale su SD
    ├── led_controller.c        <- pattern LED per indicare lo stato
    ├── firebase_uploader.c     <- upload REST su Firebase Storage + Firestore
    ├── event_manager.c         <- pipeline: PIR -> cattura -> upload -> cooldown
    ├── error_monitor.c         <- health check periodico di tutto
    └── include/                <- header files
        └── *.h
```

### Come buildare
1. Aprire la cartella in VSCode con estensione ESP-IDF
2. Selezionare target `esp32s3` dalla barra in basso
3. Build (icona martello o Ctrl+Shift+P -> ESP-IDF: Build)
4. Flash (icona fulmine)
5. Monitor (icona schermo) per vedere i log

**Importante**: se il build non trova `esp_camera`, aggiungere la dipendenza:
```bash
idf.py add-dependency "espressif/esp32-camera"
```

### Cablaggio
```
PIR HC-SR501:
  VCC  -> 3.3V
  OUT  -> GPIO 1
  GND  -> GND

LED (opzionale, c'è già quello sulla board):
  GPIO 2 -> LED sulla board

SD Card: slot integrato sul retro
Camera OV2640: connettore sulla board
```

### Prima accensione (provisioning)
Al primo avvio il dispositivo non ha le credenziali WiFi, quindi:
1. Crea un hotspot: `SecurityCam-Setup` password `setup1234`
2. Collegarsi con il telefono/PC
3. Aprire http://192.168.4.1 nel browser
4. Compilare il form con SSID WiFi, password, e dati Firebase
5. Cliccare "Salva e Riavvia"
6. Il dispositivo si riavvia e si connette al WiFi configurato

### Firebase (gratis)
1. Andare su console.firebase.google.com
2. Creare un nuovo progetto
3. Attivare Firestore Database in modalità test
4. Attivare Storage in modalità test
5. Da Impostazioni progetto prendere: Project ID, Web API Key, Storage Bucket
6. Inserire nella pagina di provisioning

I limiti del piano gratuito (Spark) sono più che sufficienti per il progetto.

### Gestione errori (importante per l'esame!!)

| Cosa succede | Come reagisce il sistema |
|---|---|
| Camera scollegata | LED errore, log, niente foto ma eventi registrati, tenta re-init |
| PIR scollegato | Sistema in idle, nessun trigger, monitor lo segnala |
| WiFi perso | Upload sospesi, auto-reconnect, frame salvati su SD |
| SD card tolta | Warning, se manca anche WiFi = dati persi (LED errore) |
| Tutto scollegato | LED errore, log critico, sistema degradato |

Il prof può verificare tutto da:
- **Monitor seriale**: log dettagliati per ogni cambio di stato
- **Endpoint /status**: JSON con lo stato di ogni periferica
- **LED**: pattern diverso per ogni condizione (idle, motion, error, ecc)

### Endpoint HTTP
- `/` - pagina con stream live
- `/stream` - stream MJPEG diretto
- `/capture` - singola foto JPEG
- `/status` - JSON con stato completo

### Prossimi step
- [ ] App Android (Kotlin + Jetpack Compose)
- [ ] Human detection con ML Kit
- [ ] Notifiche push con FCM
- [ ] Test su emulatore Android Studio
