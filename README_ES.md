# AyresTimer ⏱️🚀
> **Suite Profesional de Temporizadores de 64 bits por Hardware y Software para ESP32**
> *Desarrollado por [AyresNet](https://ayresnet.com) — [Perfil de GitHub](https://github.com/ayresnet)*

[![Idioma: Español](https://img.shields.io/badge/Idioma-Espa%C3%B1ol-yellow.svg)](#)
[![Read in English](https://img.shields.io/badge/Documentation-English-blue.svg)](README.md)
[![Repositorio GitHub](https://img.shields.io/badge/GitHub-ayresnet%2FAyresTimer-181717.svg?logo=github)](https://github.com/ayresnet/AyresTimer)
[![PlatformIO Registry](https://img.shields.io/badge/PlatformIO-Registry-orange.svg?logo=platformio)](https://registry.platformio.org/)
[![Arduino Library](https://img.shields.io/badge/Arduino-Library%20Manager-00979D.svg?logo=arduino)](https://www.arduino.cc/reference/en/libraries/)
[![Sitio Web](https://img.shields.io/badge/Sitio%20Web-AyresNet.com-0052CC.svg)](https://ayresnet.com)
[![Framework](https://img.shields.io/badge/Framework-Arduino%20%7C%20ESP--IDF-blue.svg)](https://espressif.github.io/arduino-esp32/)
[![Licencia: MIT](https://img.shields.io/badge/Licencia-MIT-yellow.svg)](LICENSE)

---

> 📖 **Selector de idioma:** [🇪🇸 **Español**](README_ES.md) | [🇬🇧 **English**](README.md)

---

**AyresTimer** es una suite de temporización de nivel misión crítica para ESP32 desarrollada por **[AyresNet](https://ayresnet.com)**. Diseñada para sistemas industriales, centrales de alarma (como *iAlarma*), electrónica de potencia y robótica. Elimina por completo las limitaciones y fallas de `millis()` al proporcionar resolución real de microsegundos en 64 bits, timers por hardware (Timer Groups TG0/TG1), timers por software de alta resolución (`esp_timer`), despacho seguro de callbacks en tareas FreeRTOS, telemetría de fluctuación temporal (jitter) en tiempo real, pausas/reanudaciones sin pérdida de cuenta y módulos de timeout ultra-ligeros.

---

## 🌟 Características Principales

* ⏱️ **Base de Tiempo de 64 bits (`uint64_t`)**: Inmune al desbordamiento (*rollover* libre por más de 500.000 años de ejecución continua).
* 🔒 **Aislamiento Seguro de Callbacks**: Las funciones lambda y callbacks C++ se ejecutan en Tasks dedicadas FreeRTOS, evitando bloqueos de interrupción (ISR), saturación de stack y *Kernel Panics*.
* ⚡ **Soporte ISR de Latencia Cero (`IRAM_ATTR`)**: Registro directo de interrupciones de hardware para tareas críticas que requieran respuesta inmediata a nivel nanosegundos.
* 🛠️ **Timers por Hardware (`AyresTimerHW`)**: Control directo de los temporizadores periféricos del silicio del ESP32 (`TG0`/`TG1`, `T0`/`T1`) con divisor de frecuencia APB configurable.
* 💻 **Timers por Software de Alta Resolución (`AyresTimerSW`)**: Basados en el subsistema de alta resolución `esp_timer` del kernel ESP-IDF.
* 🔄 **Modos de Operación Versátiles**: `ONE_SHOT` (un solo disparo), `PERIODIC` (cíclico) y `RETRIGGERABLE` (watchdog dinámico para sensores y presencia).
* 📊 **Telemetría de Vuelo (`TimerStats`)**: Monitoreo en tiempo real de disparos totales, timestamps exactos, jitter instantáneo (desviación en microsegundos) y jitter máximo registrado.
* ⏸️ **Control en Tiempo Real**: Métodos `pause()`, `resume()`, `isPaused()`, `remaining()`, `remainingMs()` y `progress()` (0.0% a 100.0%).
* 🪶 **Módulos Ligeros de Cero Sobrecarga (Zero-Overhead)**:
  - `AyresTimeout`: Temporizador para máquinas de estados y retardos no bloqueantes sin consumir temporizadores ni crear tareas FreeRTOS.
  - `AyresStopwatch`: Cronómetro de microsegundos para benchmarking y medición del tiempo exacto de ejecución de código.

---

## 📊 Gran Comparativa Técnica: `delay()` vs `millis()` vs `AyresTimerSW` vs `AyresTimerHW`

| Característica / Criterio | `delay()` Clásico | `millis()` Tradicional | 💻 `AyresTimerSW` (Software) | ⚙️ `AyresTimerHW` (Bare-Metal HW) |
| :--- | :--- | :--- | :--- | :--- |
| **¿Bloquea la CPU / Código?** | ❌ **100% Bloqueante.** Congela por completo la ejecución del ESP32. | ⚠️ No bloquea, pero satura el `loop()` con variables y comprobaciones `if`. | ✅ **0% Bloqueo.** Totalmente asíncrono en segundo plano (Kernel FreeRTOS). | ✅ **0% Bloqueo.** Totalmente autónomo en el silicio (Circuitos de Hardware). |
| **Comportamiento ante Carga (WiFi / HTTP / Pantallas)** | ❌ Se congela todo el microcontrolador. | ❌ **Se desincroniza.** Si el `loop()` tarda 50 ms en atender WiFi, el timer se retrasa 50 ms. | ✅ **Inmune.** Se despacha en tarea dedicada; salta al microsegundo exacto. | ✅ **Inmune.** Interrupción directa por hardware en silicio (Cero latencia). |
| **Resolución Temporal** | Milisegundos imprecisos. | 1 Milisegundo (1.000 µs). | ✅ **1 Microsegundo (`uint64_t`)** (1.000 veces más fino). | ✅ **12.5 Nanosegundos** (Pulsos de reloj APB a 80 MHz). |
| **Límite de Desbordamiento (*Rollover*)** | N/A | ❌ **Se rompe a los 49.7 días** (Desborde de variable de 32 bits). | ✅ **Inmune:** Más de **500.000 años** de ejecución continua sin fallar (64 bits). | ✅ **Inmune:** Contador de silicio de 64 bits (> 500.000 años). |
| **Cambios Dinámicos de Frecuencia de CPU (80/160/240 MHz)** | ❌ Se descalibra y varía tiempos. | ❌ Se descalibra con cambios de reloj. | ✅ **Inmune:** Sincronizado con el temporizador de alta resolución del sistema. | ✅ **Inmune:** Autocalibración directa con el bus APB y cristal oscilador. |
| **Control en Caliente (`pause`, `resume`, `retrigger`)** | ❌ Imposible. | ❌ Requiere lógica manual compleja y propensa a errores. | ✅ **Nativo:** Métodos directos de pausa, reanudación y rearme inmediato. | ✅ **Nativo en Silicio:** Congelamiento de ticks y reinicio instantáneo de registros. |
| **Monitoreo de Jitter y Telemetría en Vivo** | ❌ Ninguno. | ❌ Ninguno. | ✅ **Sí:** Mide desviación en microsegundos (`lastJitterUs`, `maxJitterUs`). | ✅ **Sí:** Mide fluctuación exacta de hardware en tiempo real. |
| **Consultas de Progreso (`progress%`, `remainingMs`)** | ❌ Imposible. | ❌ Cálculo manual. | ✅ **Nativo:** `remainingMs()` y `progress()` (0.0% a 100.0%). | ✅ **Nativo:** Lectura instantánea de contador por *latch* en silicio. |
| **Seguridad Multihilo / Multicore (ESP32 Dual Core)** | ❌ No aplicable. | ❌ Inseguro (Condiciones de carrera en variables compartidas). | ✅ **Thread-Safe:** Mutex recursivos + Spinlocks (`portMUX_TYPE`). | ✅ **Thread-Safe:** Spinlocks de hardware + Asignación segura de núcleos. |
| **Capacidad / Cantidad Simultánea** | 1 (y congela todo). | Requiere decenas de variables globales. | ✅ **Ilimitados:** Decenas o cientos de temporizadores virtuales independientes. | ✅ **4 Temporizadores Físicos** independientes (TG0-T0/T1, TG1-T0/T1). |
| **Casos de Uso Recomendados** | Pruebas rápidas de laboratorio. | Temporizaciones simples no críticas. | **Sistemas de Alarma, Heartbeats, Sirenas, Pantallas, IoT y FSMs.** | **Electrónica de Potencia (220V Triac), Audio, RF 433 MHz, DSP.** |

---

## 🚀 Instalación

### PlatformIO
Añade la librería a tu archivo `platformio.ini`:
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
1. Abre el **Arduino IDE**.
2. Ve al menú **Programa -> Incluir Librería -> Administrar Bibliotecas...**
3. Busca **`AyresTimer`** y haz clic en **Instalar**.

---

## 💡 Ejemplos Rápidos de Código

### 1. Temporizador por Software Periódico y One-Shot
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerSW heartbeat("Heartbeat");
AyresTimerSW oneShotTimer("OneShot");

void setup() {
    Serial.begin(115200);

    // Pulso periódico cada 500 ms
    heartbeat.setCallback([]() {
        Serial.printf("[Heartbeat] Pulso en el Core %d\n", xPortGetCoreID());
    });
    heartbeat.periodicMs(500);

    // Disparo único a los 5 segundos
    oneShotTimer.setCallback([]() {
        Serial.println(">>> ¡Han pasado 5 segundos! <<<");
    });
    oneShotTimer.oneShotSec(5);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

### 2. Temporizador por Hardware con Telemetría de Jitter
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerHW hwTimer(AyresTimerHW::TG0, AyresTimerHW::T0);

void setup() {
    Serial.begin(115200);

    hwTimer.setCallback([]() {
        // Ejecución segura en Task FreeRTOS dedicada
    });
    hwTimer.periodicMs(100);
}

void loop() {
    TimerStats stats = hwTimer.getStats();
    Serial.printf("Disparos: %llu | Jitter: %+lld us | MaxJitter: %lld us | Restante: %u ms\n",
                  stats.totalTriggers, stats.lastJitterUs, stats.maxJitterUs, hwTimer.remainingMs());
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

### 3. Watchdog Rearmable Dinámico (Alarmas y Sensores)
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerSW watchdog("Watchdog");

void setup() {
    Serial.begin(115200);

    watchdog.setCallback([]() {
        Serial.println(">>> [ALERTA] ¡Sensor inactivo por más de 3 segundos! <<<");
    });
    watchdog.retriggerableSec(3); // Ventana de vigilancia de 3 segundos
}

void onSensorMovimiento() {
    // Rearma el temporizador ante cada evento de presencia
    watchdog.retrigger();
}
```

### 4. Temporizador Ligero de Estado No Bloqueante (`AyresTimeout`)
```cpp
#include <Arduino.h>
#include <AyresTimer.h>

AyresTimeout retardoEntrada;

void onAperturaPuerta() {
    retardoEntrada.setSec(30); // 30 segundos para desarmar la alarma
}

void loop() {
    if (retardoEntrada.isRunning()) {
        Serial.printf("Restan %u segundos (Progreso: %.0f%%)\n",
                      retardoEntrada.remainingSec(),
                      retardoEntrada.progress() * 100.0f);

        if (retardoEntrada.isExpired()) {
            Serial.println("¡Tiempo agotado! Activando sirena de alarma.");
            retardoEntrada.stop();
        }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
}
```

---

## 📁 Ejemplos Incluidos en el Paquete
Todos los ejemplos incluyen implementación dual para **PlatformIO (`main.cpp`)** y **Arduino IDE (`.ino`)**:
- `01_BasicSoftwareTimer`: Parpadeo de LED y temporizadores One-Shot.
- `02_HardwarePrecisionTimer`: Grupos de timers en silicio, pausas y reanudaciones.
- `03_RetriggerableWatchdog`: Detección de presencia y rearme dinámico de zonas.
- `04_LightweightTimeout`: Máquina de estados no bloqueante y medición de tiempos.
- `05_FullTelemetryBenchmark`: Consola de telemetría completa de jitter y rendimiento.

---

## 👨‍💻 Autor y Mantenimiento
* **AyresNet**
* Sitio Web: [https://ayresnet.com](https://ayresnet.com)
* Perfil de GitHub: [@ayresnet](https://github.com/ayresnet)

---

## 📜 Licencia
Este proyecto es software de código abierto bajo la [Licencia MIT](LICENSE).
