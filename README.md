#  SecurityCam

**IoT security camera system with edge AI inference, cloud backend, and mobile app.**

An end-to-end smart surveillance system built as a university IoT + Mobile Programming exam project. The system detects movement via PIR sensor, classifies the scene using on-device TFLite Micro inference to filter false positives, uploads verified events to Firebase, and notifies the user through an Android app with real-time ML Kit detection and live streaming with bounding box overlay.

---

##  Architecture

```
┌─────────────────────┐      HTTPS/REST       ┌──────────────────┐
│    ESP32-S3 Board    │ ──────────────────────▶│     Firebase     │
│                      │     JWT auth           │                  │
│  • OV2640 Camera     │     frame upload       │  • Firestore DB  │
│  • PIR HC-SR501      │     status updates     │  • Auth          │
│  • SD Card backup    │◀──────────────────────│  • Security Rules│
│  • TFLite Micro      │     remote config      │                  │
│    (MobileNetV2)     │     ownership check    └────────┬─────────┘
│  • HTTPS Server      │                                 │
│  • FreeRTOS          │      HTTPS (LAN)                │ Firestore SDK
└─────────┬────────────┘ ◀─────────────────┐             │ (realtime listener)
          │              ─────────────────▶│             │
          │               stream / config  │             ▼
          │               Basic Auth + TLS  ┌──────────────────┐
          │                                 │   Android App    │
          └─────────────────────────────────│                  │
                    MJPEG stream            │  • Jetpack Compose│
                                            │  • ML Kit         │
                                            │  • Material 3     │
                                            │  • MVVM + StateFlow│
                                            └──────────────────┘
```

### Split Inference

The system implements ML inference at two levels:

- **Edge (ESP32):** MobileNetV2 classifier filters PIR false positives *before* uploading — saves bandwidth and Firebase costs
- **On-device (Android):** ML Kit Object Detection + Image Labeling for accurate bounding boxes and push notifications

---

##  Project Structure

```
SecurityCam/
├── firmware/              # ESP-IDF project (C/C++)
│   ├── main/
│   │   ├── main.c                    # Boot sequence, init all modules
│   │   ├── event_manager.c           # Core pipeline: PIR → TFLite → burst → upload
│   │   ├── tflite_classifier.cc      # TFLite Micro inference (MobileNetV2)
│   │   ├── firebase_uploader.c       # REST client for Firebase (no SDK)
│   │   ├── http_server.c             # HTTPS server + MJPEG streaming
│   │   ├── camera_handler.c          # OV2640 driver, burst capture, health check
│   │   ├── pir_handler.c             # PIR interrupt, debounce, health check
│   │   ├── error_monitor.c           # Periodic health check, fallback logic
│   │   ├── wifi_manager.c            # WiFi STA with auto-retry
│   │   ├── softap_provisioning.c     # First-boot setup (SoftAP + web page)
│   │   ├── sd_card_manager.c         # SD card backup (FAT32)
│   │   ├── cert_manager.c            # Self-signed TLS certificate generation
│   │   ├── led_controller.c          # Status LED patterns
│   │   └── include/                  # Headers
│   ├── partitions.csv                # Custom partition table (3.5MB app)
│   ├── sdkconfig.defaults            # ESP-IDF configuration
│   ├── CMakeLists.txt
│   └── idf_component.yml            # Dependencies (esp-tflite-micro, esp32-camera)
│
├── app/                   # Android Studio project (Kotlin)
│   └── app/src/main/
│       ├── java/com/securitycam/
│       │   ├── MainActivity.kt              # Single Activity, Navigation host
│       │   ├── ui/screens/
│       │   │   ├── LoginScreen.kt            # Email/password auth
│       │   │   ├── DevicesScreen.kt          # Device list, add/share/remove
│       │   │   ├── DashboardScreen.kt        # Device status, peripherals, actions
│       │   │   ├── EventsScreen.kt           # Event list with pagination
│       │   │   ├── ConfigScreen.kt           # Remote configuration
│       │   │   ├── StreamScreen.kt           # MJPEG stream + bounding box overlay
│       │   │   └── ProfileScreen.kt          # Account management
│       │   ├── viewmodel/
│       │   │   ├── AuthViewModel.kt          # Login, register, password reset
│       │   │   ├── DashboardViewModel.kt     # Polling, status, event analysis
│       │   │   ├── EventsViewModel.kt        # CRUD events, ML Kit analysis
│       │   │   ├── DevicesViewModel.kt       # Device management, sharing
│       │   │   └── ConfigViewModel.kt        # Remote config, reboot, reset
│       │   ├── repository/
│       │   │   ├── DeviceRepository.kt       # REST client for ESP (Retrofit)
│       │   │   ├── FirestoreDeviceRepository.kt  # Device CRUD on Firestore
│       │   │   ├── EventRepository.kt        # Event CRUD on Firestore
│       │   │   └── MlKitRepository.kt        # Object Detection + Image Labeling
│       │   ├── model/
│       │   │   └── Models.kt                 # Data classes (Device, Event, Status)
│       │   ├── network/
│       │   │   └── Esp32ApiService.kt        # Retrofit interface + OkHttp config
│       │   └── util/
│       │       ├── NotificationHelper.kt     # Push notifications
│       │       ├── EventCheckWorker.kt       # Background analysis (WorkManager)
│       │       └── PreferencesManager.kt     # SharedPreferences helper
│       └── res/
│           ├── values/strings.xml            # English strings
│           └── values-it/strings.xml         # Italian strings
│
├── colab/                 # Training notebook
│   └── ESP32_MicroClassifier_Training.ipynb              # MobileNetV2 transfer learning on COCO
│
├── docs/                  # PlantUML diagrams
│ 
│
├── LICENSE
└── README.md
```

---

##  Hardware

| Component | Model | Purpose |
|-----------|-------|---------|
| Microcontroller | ESP32-S3-WROOM (8MB PSRAM, 8MB Flash) | Main controller |
| Camera | OV2640 (VGA 640×480, JPEG) | Image capture |
| Motion sensor | PIR HC-SR501 (GPIO1) | Movement detection |
| Storage | SD Card (FAT32, SDMMC 1-bit) | Local backup |
| Indicator | LED (GPIO2) | Status feedback |

---

##  Firmware Features

### Event Pipeline
```
PIR trigger → warm-up frame (discarded) → test frame → TFLite classify (~3s)
    → threshold ≥ 0.5 → burst 3 frames → upload Firebase → backup SD → cooldown
    → threshold < 0.5 → SKIP (false positive) → cooldown
```

### TFLite Micro Edge Classifier
- **Model:** MobileNetV2 (alpha=0.35, input 96×96×3, 3 outputs with sigmoid)
- **Training:** Transfer learning on Google Colab, backbone frozen (ImageNet), head retrained on COCO subset (person/cat/dog)
- **Format:** Float32 (int8 crashed due to esp-nn + PSRAM bug, hybrid not supported by TFLite Micro)
- **Performance:** ~2.9s inference, 604/700 KB arena (86%), 1.7 MB model in flash

### Graceful Degradation
| Component Missing | Fallback |
|-------------------|----------|
| WiFi | Save to SD card, retry later |
| SD Card | RAM circular buffer (last 3 events) |
| WiFi + SD | RAM buffer (lost on reboot) |
| Camera | Event with metadata only (no photos) |
| PIR | Polling mode (capture every 30s) |
| TFLite | No filter, upload everything (fail-safe) |

### Connectivity
- **Firebase:** REST API with anonymous JWT auth (no SDK available for ESP32)
- **App (LAN):** HTTPS server with self-signed certificate + Basic Auth
- **Streaming:** MJPEG multipart over HTTPS
- **Provisioning:** SoftAP + web page for first-time setup

### FreeRTOS Tasks
| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| event_task | 1 | 6 | Event processing pipeline |
| monitor_task | 0 | 3 | Health check + Firebase status |
| WiFi stack | 0 | 23 | Network management |
| HTTP server | 0 | 5 | REST API + MJPEG streaming |

---

## Android App Features

### Architecture
- **Pattern:** Single Activity + MVVM + Jetpack Compose
- **UI:** Material 3 with dynamic dark/light theme
- **Navigation:** Compose Navigation with parameterized routes
- **State:** StateFlow + collectAsState for reactive UI
- **Networking:** Retrofit + OkHttp (ESP), Firestore SDK (cloud)

### Screens
| Screen | Purpose |
|--------|---------|
| Login | Email/password auth, registration, password reset dialog |
| Devices | Device list, add/remove, share with 6-digit invite code |
| Dashboard | Live status with polling, peripheral chips, statistics |
| Events | Paginated event list, multi-select delete, auto ML Kit analysis |
| Stream | MJPEG live view with Canvas bounding box overlay (ML Kit) |
| Config | Remote parameters (cooldown, JPEG quality, frame count), categories, reboot, factory reset |
| Profile | Change password, logout, delete account with re-authentication |

### ML Kit Integration
- **Object Detection:** Bounding boxes for detected objects
- **Image Labeling:** Classification per crop (Person, Cat, Dog)
- **Streaming overlay:** Canvas Compose with real-time detection at 1 fps
- **Auto-analysis:** Events analyzed on arrival via Firestore listener + push notification

### Offline Support
- Firestore SDK local cache (automatic)
- WorkManager with network constraint for background event check
- Try-then-fallback: HTTP direct → Firestore cloud

---

##  Firebase Structure

```
users/{uid}/
├── devices/{deviceId}/
│   ├── name, ip, monitorCategories
│   ├── events/{eventId}/
│   │   ├── event_id, timestamp, frame_count, validated
│   │   └── frames/{frameId}/
│   │       └── image_base64, frame_index, jpeg_size
│   ├── status/current
│   │   └── uptime_s, camera_ok, pir_ok, sd_card_ok, state, free_heap
│   └── pending_config/current
│       └── cooldown_ms, frame_count, jpeg_quality
└── ...

invites/{code}/
└── ownerUid, deviceId, deviceName, createdAt, expiresAt
```

---

##  Setup

### Prerequisites
- [ESP-IDF v5.5.1](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
- [Android Studio](https://developer.android.com/studio) (Hedgehog or later)
- [Firebase project](https://console.firebase.google.com/) with Firestore + Auth enabled
- Google Colab account (for model training)

### Firmware Setup
```bash
cd firmware
# Generate the TFLite model (see colab/train_model.ipynb)
# Place micro_model_data.h in main/include/

idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

### App Setup
1. Create a Firebase project and enable Firestore + Email/Password Auth
2. Download `google-services.json` and place it in `app/app/`
3. Open the project in Android Studio
4. Build and run on a device or emulator

### Model Training
1. Open `colab/train_model.ipynb` in Google Colab
2. Run all cells (trains MobileNetV2 on COCO person/cat/dog subset)
3. Download the generated `micro_model_data.h`
4. Place it in `firmware/main/include/`

---

##  Security

| Layer | Mechanism |
|-------|-----------|
| ESP → Firebase | HTTPS + JWT token (anonymous auth) + CA certificate bundle |
| ESP ← App (LAN) | HTTPS + self-signed certificate + HTTP Basic Auth |
| App → Firebase | HTTPS + Firebase SDK (automatic JWT management) |
| Firestore | Security Rules (auth != null) |
| Data at rest | NVS encryption for credentials |

> **Note:** Current Security Rules are permissive for the prototype. Production deployment would require Custom Tokens or Cloud Functions for proper device authentication, and Firebase App Check for request verification.

---

##  Performance

| Metric | Value |
|--------|-------|
| TFLite inference (ESP32) | ~2.9s (float32, 160 MHz CPU) |
| ML Kit inference (Android) | ~100ms per frame |
| JPEG decode (ESP32) | ~300ms |
| Resize 640×480 → 96×96 | ~17ms |
| Model size | 1.7 MB (float32) |
| Tensor arena | 604/700 KB (86%) |
| Free heap (runtime) | ~7.4 MB / 8 MB PSRAM |
| Firebase writes (status every 15s) | ~5,760/day (11.5% of free tier) |

---

##  Tech Stack

### Firmware
C/C++ · ESP-IDF v5.5.1 · FreeRTOS · TFLite Micro · mbedTLS · cJSON · esp32-camera

### Android
Kotlin · Jetpack Compose · Material 3 · Navigation Compose · Retrofit + OkHttp · Firebase SDK · ML Kit · WorkManager · Coroutines + StateFlow

### Cloud
Firebase Firestore · Firebase Authentication · Firebase Security Rules

---

##  License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

##  Author

**Simone Candiani** — University IoT + Mobile Programming Exam Project (2026)
