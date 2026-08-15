# AyresTimer ⏱️🚀
> **Professional, Mission-Critical 64-bit Hardware & Software Timer Suite for ESP32**
> *Developed by [AyresNet](https://ayresnet.com) — [GitHub Profile](https://github.com/ayresnet)*

[![Language: English](https://img.shields.io/badge/Language-English-blue.svg)](#)
[![Leer en Español](https://img.shields.io/badge/Documentaci%C3%B3n-Espa%C3%B1ol-yellow.svg)](README_ES.md)
[![GitHub Repo](https://img.shields.io/badge/GitHub-ayresnet%2FAyresTimer-181717.svg?logo=github)](https://github.com/ayresnet/AyresTimer)
[![PlatformIO Registry](https://img.shields.io/badge/PlatformIO-Registry-orange.svg?logo=platformio)](https://registry.platformio.org/)
[![Arduino Library](https://img.shields.io/badge/Arduino-Library%20Manager-00979D.svg?logo=arduino)](https://www.arduino.cc/reference/en/libraries/)
[![Website](https://img.shields.io/badge/Website-AyresNet.com-0052CC.svg)](https://ayresnet.com)
[![Framework](https://img.shields.io/badge/Framework-Arduino%20%7C%20ESP--IDF-blue.svg)](https://espressif.github.io/arduino-esp32/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

> 📖 **Language selector:** [🇬🇧 **English**](README.md) | [🇪🇸 **Español**](README_ES.md)

---

**AyresTimer** is a mission-critical timing suite for ESP32 developed by **[AyresNet](https://ayresnet.com)**. Designed for aerospace, industrial security systems (e.g. alarm panels), power electronics, and robotics. It eliminates the pitfalls of `millis()` by providing true 64-bit microsecond precision, hardware timers (Timer Groups TG0/TG1), software high-resolution timers (`esp_timer`), thread-safe FreeRTOS task callback dispatching, real-time jitter telemetry, pause/resume controls, and ultra-lightweight zero-overhead timeout modules.

---

## 🌟 Key Features

* ⏱️ **64-bit Microsecond Base (`uint64_t`)**: Immune to timer rollover/overflow (> 500,000 years of continuous runtime).
* 🔒 **Safe Callback Execution**: C++ lambdas and `std::function` run inside dedicated FreeRTOS tasks to prevent ISR stack starvation, WiFi/FreeRTOS blocking, and Kernel Panics.
* ⚡ **Direct Zero-Latency ISR Support (`IRAM_ATTR`)**: Dedicated hardware ISR registration for nanosecond-critical tasks.
* 🛠️ **Hardware Timers (`AyresTimerHW`)**: Direct control of ESP32 silicon Timer Groups (`TG0`/`TG1`, `T0`/`T1`) with configurable APB clock dividers.
* 💻 **Software High-Resolution Timers (`AyresTimerSW`)**: Built on ESP-IDF's high-resolution `esp_timer` engine.
* 🔄 **Versatile Operating Modes**: `ONE_SHOT`, `PERIODIC`, and `RETRIGGERABLE` (dynamic software watchdog / sensor presence detection).
* 📊 **Flight-Grade Telemetry (`TimerStats`)**: Real-time tracking of total trigger count, exact timestamping, instantaneous jitter (drift in microseconds), and maximum peak jitter.
* ⏸️ **Real-Time State Control**: `pause()`, `resume()`, `isPaused()`, `remaining()`, `remainingMs()`, and `progress()` (0.0% to 100.0%).
* 🪶 **Zero-Overhead Lightweight Modules**:
  - `AyresTimeout`: State-machine & non-blocking delay helper without creating tasks or hardware allocations.
  - `AyresStopwatch`: Microsecond benchmarking stopwatch to measure exact code execution time.

---

## 📊 Comprehensive Comparison: `delay()` vs `millis()` vs `AyresTimerSW` vs `AyresTimerHW`

| Feature / Criteria | Classic `delay()` | Traditional `millis()` | 💻 `AyresTimerSW` (Software) | ⚙️ `AyresTimerHW` (Bare-Metal HW) |
| :--- | :--- | :--- | :--- | :--- |
| **CPU / Code Blocking** | ❌ **100% Blocking.** Freezes entire ESP32 execution. | ⚠️ Non-blocking, but pollutes `loop()` with `if` checks and globals. | ✅ **0% Blocking.** Fully asynchronous in background (FreeRTOS Kernel). | ✅ **0% Blocking.** Fully autonomous in silicon (Hardware Circuits). |
| **Behavior Under Heavy Load (WiFi / HTTP / Displays)** | ❌ Complete microcontroller freeze. | ❌ **Drifts & Loses Sync.** If `loop()` takes 50 ms on WiFi, timer delays 50 ms. | ✅ **Immune.** Dispatched in dedicated task; triggers on the exact microsecond. | ✅ **Immune.** Direct silicon hardware interrupt (Zero latency). |
| **Time Resolution** | Inaccurate milliseconds. | 1 Millisecond (1,000 µs). | ✅ **1 Microsecond (`uint64_t`)** (1,000x finer). | ✅ **12.5 Nanoseconds** (APB clock ticks at 80 MHz). |
| **Overflow / Rollover Limit** | N/A | ❌ **Breaks at 49.7 days** (32-bit unsigned rollover). | ✅ **Immune:** Over **500,000 years** of continuous uptime (64-bit). | ✅ **Immune:** 64-bit silicon counter (> 500,000 years). |
| **Dynamic CPU Frequency Changes (80/160/240 MHz)** | ❌ De-calibrates and alters timing. | ❌ Shifts with clock speed changes. | ✅ **Immune:** Synced with OS high-resolution timer. | ✅ **Immune:** Auto-calibrated with APB bus & crystal oscillator. |
| **Hot Control (`pause`, `resume`, `retrigger`)** | ❌ Impossible. | ❌ Requires complex error-prone manual logic. | ✅ **Native:** Direct pause, resume, and immediate retrigger methods. | ✅ **Native in Silicon:** Tick freezing and instant register reload. |
| **Live Jitter Monitoring & Telemetry** | ❌ None. | ❌ None. | ✅ **Yes:** Measures microsecond deviation (`lastJitterUs`, `maxJitterUs`). | ✅ **Yes:** Real-time exact hardware fluctuation telemetry. |
| **Progress Queries (`progress%`, `remainingMs`)** | ❌ Impossible. | ❌ Manual math. | ✅ **Native:** `remainingMs()` and `progress()` (0.0% to 100.0%). | ✅ **Native:** Instant hardware counter latch readout. |
| **Multicore / Thread-Safety (ESP32 Dual Core)** | ❌ Not applicable. | ❌ Unsafe (Race conditions on shared globals). | ✅ **Thread-Safe:** Recursive Mutex + Spinlocks (`portMUX_TYPE`). | ✅ **Thread-Safe:** Hardware spinlocks + Safe core affinity. |
| **Simultaneous Capacity** | 1 (and freezes everything). | Requires dozens of manual global state variables. | ✅ **Unlimited:** Hundreds of independent virtual timers. | ✅ **4 Independent Physical Timers** (TG0-T0/T1, TG1-T0/T1). |
| **Recommended Use Cases** | Quick lab benchmarks only. | Simple non-critical delays. | **Alarm Systems, Heartbeats, Sirens, Displays, IoT & FSMs.** | **Power Electronics (220V Triac), Audio, 433 MHz RF, DSP.** |

---

## ⏱️ Supported Time Units

The library provides symmetrical and native convenience methods across both Hardware (`AyresTimerHW`) and Software (`AyresTimerSW`) backends, spanning from nanoseconds to days of uptime:

| Unit | Available Methods | Resolution / Precision | Usage Example |
| :--- | :--- | :--- | :--- |
| **Seconds (`s`)** | `startSec()`, `oneShotSec()`, `periodicSec()`, `retriggerableSec()` | 1 second | `timer.periodicSec(5);` (every 5s) |
| **Milliseconds (`ms`)** | `startMs()`, `oneShotMs()`, `periodicMs()`, `retriggerableMs()`, `remainingMs()`, `elapsedMs()` | 1 millisecond | `timer.oneShotMs(1500);` (1.5s delay) |
| **Microseconds (`µs`)** | `start()`, `oneShot()`, `periodic()`, `retriggerable()`, `updatePeriod()`, `remaining()`, `elapsed()` | **1 microsecond (`uint64_t`)** | `timer.periodic(250000);` (250 ms) |
| **Nanoseconds (`ns`)** | **HW:** Silicon APB bus at 80 MHz with prescaler / **SW:** `AyresTimerSW::busyWaitCycles()` | **4.16 ns to 12.5 ns** | `AyresTimerSW::busyWaitCycles(100);` |
| **CPU Cycles** | `AyresTimerSW::cycles()`, `AyresTimerSW::cpuFreqMHz()` | 1 Xtensa clock cycle | `uint32_t c = AyresTimerSW::cycles();` |

---

## 🚀 Installation

### PlatformIO
Add to your `platformio.ini`:
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    ayresnet/AyresTimer
```

### Arduino IDE
1. Open **Arduino IDE**.
2. Navigate to **Sketch -> Include Library -> Manage Libraries...**
3. Search for **`AyresTimer`** and click **Install**.

---

## 💡 Quick Code Examples

### 1. Software Timer (Periodic & One-Shot)
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerSW heartbeat("Heartbeat");
AyresTimerSW oneShotTimer("OneShot");

void setup() {
    Serial.begin(115200);

    // Periodic heartbeat every 500 ms
    heartbeat.setCallback([]() {
        Serial.printf("[Heartbeat] Pulse on Core %d\n", xPortGetCoreID());
    });
    heartbeat.periodicMs(500);

    // Single trigger after 5 seconds
    oneShotTimer.setCallback([]() {
        Serial.println(">>> 5 seconds elapsed! <<<");
    });
    oneShotTimer.oneShotSec(5);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

### 2. Hardware Timer with Sub-Microsecond Jitter Telemetry
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerHW hwTimer(AyresTimerHW::TG0, AyresTimerHW::T0);

void setup() {
    Serial.begin(115200);

    hwTimer.setCallback([]() {
        // Safe FreeRTOS task execution
    });
    hwTimer.periodicMs(100);
}

void loop() {
    TimerStats stats = hwTimer.getStats();
    Serial.printf("Triggers: %llu | Jitter: %+lld us | MaxJitter: %lld us | Remaining: %u ms\n",
                  stats.totalTriggers, stats.lastJitterUs, stats.maxJitterUs, hwTimer.remainingMs());
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

### 3. Dynamic Retriggerable Watchdog (Alarms & Security)
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerSW watchdog("Watchdog");

void setup() {
    Serial.begin(115200);

    watchdog.setCallback([]() {
        Serial.println(">>> [ALARM] Sensor inactivity detected! <<<");
    });
    watchdog.retriggerableSec(3); // 3-second watchdog window
}

void onSensorMotionDetected() {
    // Rearm the watchdog on sensor activity
    watchdog.retrigger();
}
```

### 4. Lightweight Non-Blocking Timeout (`AyresTimeout`)
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimeout entryDelay;

void onDoorOpened() {
    entryDelay.setSec(30); // 30 seconds to disarm
}

void loop() {
    if (entryDelay.isRunning()) {
        Serial.printf("Remaining: %u s (Progress: %.0f%%)\n",
                      entryDelay.remainingSec(),
                      entryDelay.progress() * 100.0f);

        if (entryDelay.isExpired()) {
            Serial.println("Time expired! Sounding siren.");
            entryDelay.stop();
        }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

---

## 📁 Included Examples
All examples include dual implementation for **PlatformIO (`main.cpp`)** and **Arduino IDE (`.ino`)**:
- `01_BasicSoftwareTimer`: LED blinking and One-Shot delayed executions.
- `02_HardwarePrecisionTimer`: Silicon timer groups, real-time pause and resume.
- `03_RetriggerableWatchdog`: Sensor activity watchdog and dynamic retriggering.
- `04_LightweightTimeout`: Non-blocking state-machine timeout and execution benchmarking.
- `05_FullTelemetryBenchmark`: Complete multi-timer jitter and performance telemetry console.

---

## 👨‍💻 Author & Maintainer
* **AyresNet**
* Website: [https://ayresnet.com](https://ayresnet.com)
* GitHub: [@ayresnet](https://github.com/ayresnet)

---

## 📜 License
This project is open-source software licensed under the [MIT License](LICENSE).
