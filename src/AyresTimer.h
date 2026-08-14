/*
 * AyresTimer v4.1.0 - Timers de Grado Profesional y Misión Crítica para ESP32
 * Desarrollado por AyresNet (https://ayresnet.com)
 *
 * Características principales:
 *   - Base de tiempo en microsegundos a 64 bits (sin overflow ni rollover).
 *   - Cero dependencias de polling y 100% libre de millis().
 *   - Timers por Hardware (Timer Groups TG0/TG1) y Software de Alta Resolución (esp_timer).
 *   - Despacho de callbacks C++ seguro en Tasks dedicadas FreeRTOS (nunca en ISR).
 *   - Soporte opcional de callbacks ISR puros con IRAM_ATTR.
 *   - Control total: start, stop, restart, pause, resume, retrigger, remaining, progress.
 *   - Telemetría integrada de disparos, latencia y jitter (desviación temporal).
 *   - Módulos auxiliares de ultra-bajo consumo: AyresTimeout y AyresStopwatch.
 *   - Seguridad multi-core / multi-hilo con mutex recursivo y spinlocks.
 *
 * MIT License
 */

#pragma once

#include <Arduino.h>
#include <functional>

#if !defined(ESP32) && !defined(ARDUINO_ARCH_ESP32)
#  error "AyresTimer requiere ESP32."
#endif

#include <driver/timer.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Afinidad automática: fija la Task al core desde el que se crea. Para dejarla
// sin afinidad, pasar tskNO_AFFINITY explícitamente a useTask().
constexpr BaseType_t AYRES_TIMER_AUTO_CORE = -2;

enum AyresTimerMode {
    AYRES_HARDWARE,
    AYRES_SOFTWARE
};

enum TimerMode {
    ONE_SHOT = 0,
    PERIODIC = 1,
    RETRIGGERABLE = 2
};

// Métricas y telemetría de rendimiento
struct TimerStats {
    uint64_t totalTriggers = 0;      // Cantidad total de disparos completados
    uint64_t droppedCallbacks = 0;   // Callbacks descartados o cancelados
    uint64_t lastTriggerUs = 0;      // Timestamp exacto (esp_timer_get_time) del último disparo
    int64_t  lastJitterUs = 0;       // Desviación en microsegundos respecto al intervalo teórico
    int64_t  maxJitterUs = 0;        // Máximo jitter absoluto registrado
};

template<AyresTimerMode MODE>
class AyresTimer;

// =============================================================================
// AYRES TIMER - HARDWARE BACKEND (ESP32 Timer Groups TG0 / TG1)
// =============================================================================
template<>
class AyresTimer<AYRES_HARDWARE> {
public:
    enum Group { TG0 = 0, TG1 = 1 };
    enum Index { T0 = 0, T1 = 1 };

    using Callback = std::function<void()>;
    using ISRCallback = void (*)(void*);

    explicit AyresTimer(Group group = TG0, Index idx = T0);
    ~AyresTimer();

    AyresTimer(const AyresTimer&) = delete;
    AyresTimer& operator=(const AyresTimer&) = delete;
    AyresTimer(AyresTimer&&) = delete;
    AyresTimer& operator=(AyresTimer&&) = delete;

    // Rango admitido por el timer ESP32: 2..65536. Solo se cambia detenido.
    bool setClockDivisor(uint32_t divisor);

    // Control principal en microsegundos
    bool start(uint64_t microseconds, TimerMode mode = PERIODIC);
    bool oneShot(uint64_t microseconds)       { return start(microseconds, ONE_SHOT); }
    bool periodic(uint64_t microseconds)      { return start(microseconds, PERIODIC); }
    bool retriggerable(uint64_t microseconds) { return start(microseconds, RETRIGGERABLE); }
    bool retrigger();

    // Métodos de conveniencia en milisegundos y segundos
    bool startMs(uint32_t ms, TimerMode mode = PERIODIC) { return start((uint64_t)ms * 1000ULL, mode); }
    bool oneShotMs(uint32_t ms)       { return oneShot((uint64_t)ms * 1000ULL); }
    bool periodicMs(uint32_t ms)      { return periodic((uint64_t)ms * 1000ULL); }
    bool retriggerableMs(uint32_t ms) { return retriggerable((uint64_t)ms * 1000ULL); }

    bool startSec(uint32_t sec, TimerMode mode = PERIODIC) { return start((uint64_t)sec * 1000000ULL, mode); }
    bool oneShotSec(uint32_t sec)       { return oneShot((uint64_t)sec * 1000000ULL); }
    bool periodicSec(uint32_t sec)      { return periodic((uint64_t)sec * 1000000ULL); }
    bool retriggerableSec(uint32_t sec) { return retriggerable((uint64_t)sec * 1000000ULL); }

    bool stop();
    bool restart();
    bool pause();
    bool resume();
    bool updatePeriod(uint64_t microseconds);

    // Consultas de estado
    uint64_t elapsed() const;
    uint32_t elapsedMs() const { return (uint32_t)(elapsed() / 1000ULL); }
    uint64_t remaining() const;
    uint32_t remainingMs() const { return (uint32_t)(remaining() / 1000ULL); }
    float    progress() const;
    bool     isRunning() const;
    bool     isPaused() const;

    // Callbacks
    bool setCallback(Callback callback);
    bool setISRCallback(ISRCallback callback, void* arg = nullptr);

    // Configuración de Task FreeRTOS para callbacks
    bool useTask(bool enable,
                 UBaseType_t priority = configMAX_PRIORITIES - 3,
                 uint32_t stackSize = 2048,
                 BaseType_t core = AYRES_TIMER_AUTO_CORE);

    // Diagnóstico y telemetría
    esp_err_t   lastError() const;
    const char* lastErrorStr() const;
    TimerStats  getStats() const;
    void        resetStats();

private:
    static bool IRAM_ATTR onTimerInterrupt(void* arg);
    static void callbackTask_(void* pv);

    bool validMode_(TimerMode mode) const;
    bool ensureTask_();
    void stopTask_();
    bool teardownHardware_();
    bool claimHardware_();
    void releaseHardware_();
    void setError_(esp_err_t error) const;

    timer_group_t _group;
    timer_idx_t   _idx;

    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    mutable SemaphoreHandle_t _apiMutex = nullptr;
    SemaphoreHandle_t _callbackMutex = nullptr;
    Callback      _callback = nullptr;
    bool          _hasCppCallback = false;
    bool          _callbackActive = false;
    ISRCallback   _isrCallback = nullptr;
    void*         _isrArg = nullptr;

    TimerMode     _mode = PERIODIC;
    uint64_t      _timeout_us = 0;
    bool          _running = false;
    bool          _paused = false;
    uint64_t      _pausedTicks = 0;

    bool          _useTask = true;
    TaskHandle_t  _taskHandle = nullptr;
    bool          _taskStopRequested = false;
    UBaseType_t   _taskPriority = configMAX_PRIORITIES - 3;
    uint32_t      _taskStackSize = 2048;
    BaseType_t    _taskCore = AYRES_TIMER_AUTO_CORE;
    uint64_t      _producedCallbacks = 0;
    uint64_t      _consumedCallbacks = 0;
    uint64_t      _cancelThrough = 0;

    uint32_t      _divisor = 80;
    uint32_t      _apbClockHz = 80000000;

    bool          _initialized = false;
    bool          _isrRegistered = false;
    bool          _ownsHardware = false;
    mutable esp_err_t _lastError = ESP_OK;

    TimerStats    _stats{};
    uint64_t      _expectedNextUs = 0;
};

// =============================================================================
// AYRES TIMER - SOFTWARE BACKEND (ESP-IDF esp_timer / High Resolution Timer)
// =============================================================================
template<>
class AyresTimer<AYRES_SOFTWARE> {
public:
    using Callback = std::function<void()>;
    using ISRCallback = void (*)(void*);

    explicit AyresTimer(const char* name = "AyresTimer");
    ~AyresTimer();

    AyresTimer(const AyresTimer&) = delete;
    AyresTimer& operator=(const AyresTimer&) = delete;
    AyresTimer(AyresTimer&&) = delete;
    AyresTimer& operator=(AyresTimer&&) = delete;

    // Devuelve false si el SDK fue compilado sin soporte ESP_TIMER_ISR.
    bool setDispatchInISR(bool enable);

    // Control principal en microsegundos
    bool start(uint64_t microseconds, TimerMode mode = PERIODIC);
    bool oneShot(uint64_t microseconds)       { return start(microseconds, ONE_SHOT); }
    bool periodic(uint64_t microseconds)      { return start(microseconds, PERIODIC); }
    bool retriggerable(uint64_t microseconds) { return start(microseconds, RETRIGGERABLE); }
    bool retrigger();

    // Métodos de conveniencia en milisegundos y segundos
    bool startMs(uint32_t ms, TimerMode mode = PERIODIC) { return start((uint64_t)ms * 1000ULL, mode); }
    bool oneShotMs(uint32_t ms)       { return oneShot((uint64_t)ms * 1000ULL); }
    bool periodicMs(uint32_t ms)      { return periodic((uint64_t)ms * 1000ULL); }
    bool retriggerableMs(uint32_t ms) { return retriggerable((uint64_t)ms * 1000ULL); }

    bool startSec(uint32_t sec, TimerMode mode = PERIODIC) { return start((uint64_t)sec * 1000000ULL, mode); }
    bool oneShotSec(uint32_t sec)       { return oneShot((uint64_t)sec * 1000000ULL); }
    bool periodicSec(uint32_t sec)      { return periodic((uint64_t)sec * 1000000ULL); }
    bool retriggerableSec(uint32_t sec) { return retriggerable((uint64_t)sec * 1000000ULL); }

    bool stop();
    bool restart();
    bool pause();
    bool resume();
    bool updatePeriod(uint64_t microseconds);

    // Consultas de estado
    uint64_t elapsed() const;
    uint32_t elapsedMs() const { return (uint32_t)(elapsed() / 1000ULL); }
    uint64_t remaining() const;
    uint32_t remainingMs() const { return (uint32_t)(remaining() / 1000ULL); }
    float    progress() const;
    bool     isRunning() const;
    bool     isPaused() const;

    // Callbacks
    bool setCallback(Callback callback);
    bool setISRCallback(ISRCallback callback, void* arg = nullptr);

    // Configuración de Task FreeRTOS para aislar callbacks pesados
    bool useTask(bool enable,
                 UBaseType_t priority = configMAX_PRIORITIES - 3,
                 uint32_t stackSize = 2048,
                 BaseType_t core = AYRES_TIMER_AUTO_CORE);

    // Diagnóstico y telemetría
    esp_err_t   lastError() const;
    const char* lastErrorStr() const;
    TimerStats  getStats() const;
    void        resetStats();

    // Utilidades estáticas de tiempo y CPU
    static uint32_t cpuFreqMHz();
    static uint32_t cycles();
    static void     busyWaitCycles(uint32_t cyc);
    static void     busyWaitUs(uint32_t us);
    static uint64_t micros64();

private:
    static void IRAM_ATTR trampoline(void* arg);
    static void callbackTask_(void* pv);

    bool validMode_(TimerMode mode) const;
    bool recreateTimer_();
    bool ensureTask_();
    void stopTask_();
    bool IRAM_ATTR dispatch_();
    void setError_(esp_err_t error) const;

    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
    mutable SemaphoreHandle_t _apiMutex = nullptr;
    SemaphoreHandle_t _callbackMutex = nullptr;

    char                    _name[24]{};
    esp_timer_handle_t      _handle = nullptr;
    esp_timer_create_args_t _args{};

    bool                    _running = false;
    bool                    _paused = false;
    bool                    _dispatchInISR = false;
    uint64_t                _timeout_us = 0;
    uint64_t                _start_us = 0;
    uint64_t                _paused_elapsed_us = 0;
    TimerMode               _mode = PERIODIC;

    Callback                _callback = nullptr;
    bool                    _hasCppCallback = false;
    bool                    _callbackActive = false;
    ISRCallback             _isrCallback = nullptr;
    void*                   _isrArg = nullptr;

    bool          _useTask = false;
    TaskHandle_t  _taskHandle = nullptr;
    bool          _taskStopRequested = false;
    UBaseType_t   _taskPriority = configMAX_PRIORITIES - 3;
    uint32_t      _taskStackSize = 2048;
    BaseType_t    _taskCore = AYRES_TIMER_AUTO_CORE;
    uint64_t      _producedCallbacks = 0;
    uint64_t      _consumedCallbacks = 0;
    uint64_t      _cancelThrough = 0;

    mutable esp_err_t _lastError = ESP_OK;

    TimerStats    _stats{};
    uint64_t      _expectedNextUs = 0;
};

using AyresTimerHW = AyresTimer<AYRES_HARDWARE>;
using AyresTimerSW = AyresTimer<AYRES_SOFTWARE>;

// =============================================================================
// AYRES TIMEOUT - Temporizador ultra-ligero sin Tasks ni asignación dinámica
// Ideal para máquinas de estado (FSM), retardos no bloqueantes y timeouts
// =============================================================================
class AyresTimeout {
public:
    AyresTimeout() = default;
    explicit AyresTimeout(uint64_t timeoutUs) { set(timeoutUs); }

    void set(uint64_t timeoutUs) {
        _timeoutUs = timeoutUs;
        restart();
    }
    void setMs(uint32_t ms)   { set((uint64_t)ms * 1000ULL); }
    void setSec(uint32_t sec) { set((uint64_t)sec * 1000000ULL); }

    void restart() {
        _startUs = (uint64_t)esp_timer_get_time();
        _running = true;
        _paused = false;
        _pausedElapsedUs = 0;
    }

    void stop() {
        _running = false;
        _paused = false;
    }

    void pause() {
        if (_running && !_paused) {
            _pausedElapsedUs = elapsed();
            _paused = true;
        }
    }

    void resume() {
        if (_running && _paused) {
            _startUs = (uint64_t)esp_timer_get_time() - _pausedElapsedUs;
            _paused = false;
        }
    }

    bool isExpired() const {
        if (!_running || _paused) return false;
        return elapsed() >= _timeoutUs;
    }

    bool isRunning() const { return _running && !_paused; }
    bool isPaused() const  { return _paused; }

    uint64_t elapsed() const {
        if (!_running) return 0;
        if (_paused) return _pausedElapsedUs;
        const uint64_t now = (uint64_t)esp_timer_get_time();
        return (now >= _startUs) ? (now - _startUs) : 0;
    }
    uint32_t elapsedMs() const  { return (uint32_t)(elapsed() / 1000ULL); }
    float    elapsedSec() const { return (float)elapsed() / 1000000.0f; }

    uint64_t remaining() const {
        const uint64_t el = elapsed();
        return (el >= _timeoutUs) ? 0 : (_timeoutUs - el);
    }
    uint32_t remainingMs() const  { return (uint32_t)(remaining() / 1000ULL); }
    float    remainingSec() const { return (float)remaining() / 1000000.0f; }

    float progress() const {
        if (_timeoutUs == 0) return 1.0f;
        const uint64_t el = elapsed();
        if (el >= _timeoutUs) return 1.0f;
        return (float)el / (float)_timeoutUs;
    }

private:
    uint64_t _timeoutUs = 0;
    uint64_t _startUs = 0;
    uint64_t _pausedElapsedUs = 0;
    bool     _running = false;
    bool     _paused = false;
};

// =============================================================================
// AYRES STOPWATCH - Cronómetro de precisión para benchmarking y medición
// =============================================================================
class AyresStopwatch {
public:
    explicit AyresStopwatch(bool startNow = true) {
        if (startNow) start();
    }

    void start() {
        _startUs = (uint64_t)esp_timer_get_time();
        _accumulatedUs = 0;
        _running = true;
    }

    void pause() {
        if (_running) {
            const uint64_t now = (uint64_t)esp_timer_get_time();
            _accumulatedUs += (now >= _startUs) ? (now - _startUs) : 0;
            _running = false;
        }
    }

    void resume() {
        if (!_running) {
            _startUs = (uint64_t)esp_timer_get_time();
            _running = true;
        }
    }

    void reset() {
        _accumulatedUs = 0;
        _running = false;
    }

    void restart() {
        reset();
        start();
    }

    uint64_t elapsedUs() const {
        if (_running) {
            const uint64_t now = (uint64_t)esp_timer_get_time();
            return _accumulatedUs + ((now >= _startUs) ? (now - _startUs) : 0);
        }
        return _accumulatedUs;
    }

    uint32_t elapsedMs() const  { return (uint32_t)(elapsedUs() / 1000ULL); }
    float    elapsedSec() const { return (float)elapsedUs() / 1000000.0f; }
    bool     isRunning() const  { return _running; }

private:
    uint64_t _startUs = 0;
    uint64_t _accumulatedUs = 0;
    bool     _running = false;
};
