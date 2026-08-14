/*
 * =============================================================================
 * AyresTimer v4.1.0 - Suite Profesional de Temporización para ESP32
 * Desarrollado por AyresNet (https://ayresnet.com)
 * =============================================================================
 *
 * Esta librería proporciona un sistema integral de temporizadores de 64 bits
 * en microsegundos, diseñado para aplicaciones industriales, de seguridad
 * (como sistemas de alarma) y de misión crítica en ESP32.
 *
 * ARQUITECTURA GENERAL:
 * ---------------------
 * 1. AyresTimerHW:
 *    - Temporizador por HARDWARE directo en el silicio (Timer Groups TG0 y TG1).
 *    - Acceso directo a registros MMIO del SoC sin intermediarios genéricos.
 *    - Resolución a nivel de pulsos de reloj (APB 80 MHz, 12.5 ns por tick).
 *    - Ideal para tareas de latencia cero, control de potencia y pulsos de RF.
 *
 * 2. AyresTimerSW:
 *    - Temporizador por SOFTWARE de alta resolución basado en esp_timer.
 *    - Permite crear cientos de temporizadores virtuales simultáneos de 64 bits.
 *    - Ideal para heartbeats, sirenas, watchdogs y máquinas de estado.
 *
 * 3. AyresTimeout:
 *    - Objeto ultra-ligero para temporización no bloqueante en bucle (polling seguro).
 *    - Cero asignación dinámica, cero tareas FreeRTOS y cero consumo de hardware.
 *
 * 4. AyresStopwatch:
 *    - Cronómetro de alta precisión en microsegundos para benchmarking y medición
 *      del tiempo exacto de ejecución de bloques de código.
 *
 * SEGURIDAD Y CONCURRENCIA (THREAD-SAFETY):
 * -----------------------------------------
 * - Inmune a desbordamientos (rollover de 64 bits > 500.000 años de ejecución continua).
 * - Despacho seguro de callbacks C++ (std::function) en tareas FreeRTOS dedicadas
 *   para evitar Kernel Panics y no saturar el contexto de interrupción (ISR).
 * - Protección multihilo y multicore mediante Mutex Recursivos y Spinlocks (portMUX_TYPE).
 *
 * Licencia: MIT
 * =============================================================================
 */

#pragma once

#include <Arduino.h>
#include <functional>

#if !defined(ESP32) && !defined(ARDUINO_ARCH_ESP32)
#  error "AyresTimer requiere microcontrolador ESP32."
#endif

#include <driver/timer.h>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/**
 * @brief Constante para afinidad automática de tareas FreeRTOS.
 * Si se usa, fija la tarea al núcleo (Core 0 o Core 1) desde el que se invoca useTask().
 * Para permitir que la tarea migre libremente entre núcleos, pasar 'tskNO_AFFINITY'.
 */
constexpr BaseType_t AYRES_TIMER_AUTO_CORE = -2;

/**
 * @brief Selector del backend de temporización (Hardware o Software).
 */
enum AyresTimerMode {
    AYRES_HARDWARE, ///< Temporizador directo en el silicio (Timer Groups)
    AYRES_SOFTWARE  ///< Temporizador de alta resolución por software (esp_timer)
};

/**
 * @brief Modos de operación del temporizador.
 */
enum TimerMode {
    ONE_SHOT = 0,      ///< Disparo único: ejecuta el callback una sola vez y se detiene.
    PERIODIC = 1,      ///< Cíclico / Periódico: se repite indefinidamente a intervalos regulares.
    RETRIGGERABLE = 2  ///< Rearmable: watchdog que renueva su cuenta atrás con retrigger().
};

/**
 * @brief Estructura de telemetría y diagnóstico en tiempo real.
 * Permite monitorear la salud y precisión del temporizador en vuelo.
 */
struct TimerStats {
    uint64_t totalTriggers = 0;      ///< Cantidad total acumulada de disparos completados.
    uint64_t droppedCallbacks = 0;   ///< Callbacks que fueron cancelados o descartados por sobrecarga.
    uint64_t lastTriggerUs = 0;      ///< Marca de tiempo exacta (esp_timer_get_time) del último disparo.
    int64_t  lastJitterUs = 0;       ///< Desviación temporal en microsegundos del último disparo vs el teórico.
    int64_t  maxJitterUs = 0;        ///< Máxima desviación temporal (jitter pico) registrada.
};

template<AyresTimerMode MODE>
class AyresTimer;

// =============================================================================
// AYRES TIMER - BACKEND DE HARDWARE (ESP32 Timer Groups TG0 / TG1)
// =============================================================================

/**
 * @brief Temporizador de Hardware Bare-Metal para ESP32.
 * 
 * Controla directamente los circuitos integrados de temporización del chip.
 * El ESP32 cuenta con 2 grupos de temporizadores (TG0 y TG1), cada uno con 2
 * temporizadores independientes (T0 y T1), dando un total de 4 timers físicos.
 */
template<>
class AyresTimer<AYRES_HARDWARE> {
public:
    enum Group { TG0 = 0, TG1 = 1 }; ///< Grupos de hardware disponibles
    enum Index { T0 = 0, T1 = 1 };   ///< Índice del temporizador dentro del grupo

    using Callback = std::function<void()>;  ///< Firma de callback seguro en Task
    using ISRCallback = void (*)(void*);     ///< Firma de callback directo en interrupción (IRAM)

    /**
     * @brief Constructor del temporizador por hardware.
     * @param group Grupo de temporizadores (TG0 por defecto).
     * @param idx Índice del temporizador en el grupo (T0 por defecto).
     */
    explicit AyresTimer(Group group = TG0, Index idx = T0);

    /**
     * @brief Destructor. Detiene el timer y libera los recursos de hardware y memoria.
     */
    ~AyresTimer();

    // Prohibir copia y movimiento para evitar duplicación de control de hardware
    AyresTimer(const AyresTimer&) = delete;
    AyresTimer& operator=(const AyresTimer&) = delete;
    AyresTimer(AyresTimer&&) = delete;
    AyresTimer& operator=(AyresTimer&&) = delete;

    /**
     * @brief Configura el divisor de reloj del prescaler de hardware (2 a 65536).
     * El reloj base del bus APB es de 80 MHz. Un divisor de 80 equivale a 1 tick = 1 microsegundo.
     * Solo puede modificarse cuando el temporizador está detenido.
     * @param divisor Valor del divisor (ej: 80 para 1 MHz).
     * @return true si se aplicó con éxito, false si el timer está corriendo o el argumento es inválido.
     */
    bool setClockDivisor(uint32_t divisor);

    // --- Control Principal en Microsegundos ---
    bool start(uint64_t microseconds, TimerMode mode = PERIODIC);
    bool oneShot(uint64_t microseconds)       { return start(microseconds, ONE_SHOT); }
    bool periodic(uint64_t microseconds)      { return start(microseconds, PERIODIC); }
    bool retriggerable(uint64_t microseconds) { return start(microseconds, RETRIGGERABLE); }
    bool retrigger(); ///< Rearma el temporizador en modo RETRIGGERABLE (Watchdog de sensores).

    // --- Métodos de Conveniencia en Milisegundos y Segundos ---
    bool startMs(uint32_t ms, TimerMode mode = PERIODIC) { return start((uint64_t)ms * 1000ULL, mode); }
    bool oneShotMs(uint32_t ms)       { return oneShot((uint64_t)ms * 1000ULL); }
    bool periodicMs(uint32_t ms)      { return periodic((uint64_t)ms * 1000ULL); }
    bool retriggerableMs(uint32_t ms) { return retriggerable((uint64_t)ms * 1000ULL); }

    bool startSec(uint32_t sec, TimerMode mode = PERIODIC) { return start((uint64_t)sec * 1000000ULL, mode); }
    bool oneShotSec(uint32_t sec)       { return oneShot((uint64_t)sec * 1000000ULL); }
    bool periodicSec(uint32_t sec)      { return periodic((uint64_t)sec * 1000000ULL); }
    bool retriggerableSec(uint32_t sec) { return retriggerable((uint64_t)sec * 1000000ULL); }

    // --- Control de Estado y Modificación en Caliente ---
    bool stop();                             ///< Detiene y deshabilita el temporizador de hardware.
    bool restart();                          ///< Reinicia el temporizador con la última configuración.
    bool pause();                            ///< Pausa el contador de hardware sin perder los ticks acumulados.
    bool resume();                           ///< Reanuda el conteo desde el punto exacto donde se pausó.
    bool updatePeriod(uint64_t microseconds);///< Modifica el periodo en caliente sin reinicializar el hardware.

    // --- Consultas en Tiempo Real ---
    uint64_t elapsed() const;                ///< Retorna el tiempo transcurrido en microsegundos.
    uint32_t elapsedMs() const { return (uint32_t)(elapsed() / 1000ULL); }
    uint64_t remaining() const;              ///< Retorna el tiempo restante para el próximo disparo en microsegundos.
    uint32_t remainingMs() const { return (uint32_t)(remaining() / 1000ULL); }
    float    progress() const;               ///< Retorna el progreso del ciclo actual (0.0f a 1.0f / 0% a 100%).
    bool     isRunning() const;              ///< Retorna true si el temporizador está activo y contando.
    bool     isPaused() const;               ///< Retorna true si el temporizador está pausado.

    // --- Registro de Callbacks ---
    /**
     * @brief Registra un callback C++ seguro (std::function, lambdas con captura).
     * Se ejecuta en una tarea FreeRTOS dedicada para total seguridad de memoria.
     */
    bool setCallback(Callback callback);

    /**
     * @brief Registra un callback directo de interrupción de hardware (ISR).
     * Debe ser una función ligera marcada con IRAM_ATTR y solo usar APIs ISR-safe.
     */
    bool setISRCallback(ISRCallback callback, void* arg = nullptr);

    /**
     * @brief Configura los parámetros de la tarea FreeRTOS que atiende el callback.
     * @param enable Habilita el despacho en tarea.
     * @param priority Prioridad de la tarea FreeRTOS (por defecto: alta).
     * @param stackSize Tamaño de la pila en bytes (por defecto: 2048 bytes).
     * @param core Afinidad de núcleo de CPU (Core 0, Core 1, o AUTO_CORE).
     */
    bool useTask(bool enable,
                 UBaseType_t priority = configMAX_PRIORITIES - 3,
                 uint32_t stackSize = 2048,
                 BaseType_t core = AYRES_TIMER_AUTO_CORE);

    // --- Diagnóstico y Telemetría ---
    esp_err_t   lastError() const;     ///< Retorna el último código de error del sistema (esp_err_t).
    const char* lastErrorStr() const;  ///< Retorna la descripción en texto del último error.
    TimerStats  getStats() const;      ///< Retorna la estructura con las estadísticas y jitter medido.
    void        resetStats();          ///< Reinicia los contadores de telemetría a cero.

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
// AYRES TIMER - BACKEND DE SOFTWARE (ESP-IDF esp_timer / High Resolution)
// =============================================================================

/**
 * @brief Temporizador por Software de Alta Resolución para ESP32.
 * 
 * Basado en el subsistema esp_timer del kernel de ESP-IDF. Permite crear
 * múltiples instancias sin límite de hardware físico, con precisión de microsegundos.
 */
template<>
class AyresTimer<AYRES_SOFTWARE> {
public:
    using Callback = std::function<void()>;
    using ISRCallback = void (*)(void*);

    /**
     * @brief Constructor del temporizador por software.
     * @param name Nombre identificativo para depuración en el sistema operativo.
     */
    explicit AyresTimer(const char* name = "AyresTimer");
    ~AyresTimer();

    AyresTimer(const AyresTimer&) = delete;
    AyresTimer& operator=(const AyresTimer&) = delete;
    AyresTimer(AyresTimer&&) = delete;
    AyresTimer& operator=(AyresTimer&&) = delete;

    bool setDispatchInISR(bool enable);

    // --- Control Principal en Microsegundos ---
    bool start(uint64_t microseconds, TimerMode mode = PERIODIC);
    bool oneShot(uint64_t microseconds)       { return start(microseconds, ONE_SHOT); }
    bool periodic(uint64_t microseconds)      { return start(microseconds, PERIODIC); }
    bool retriggerable(uint64_t microseconds) { return start(microseconds, RETRIGGERABLE); }
    bool retrigger();

    // --- Métodos de Conveniencia en Milisegundos y Segundos ---
    bool startMs(uint32_t ms, TimerMode mode = PERIODIC) { return start((uint64_t)ms * 1000ULL, mode); }
    bool oneShotMs(uint32_t ms)       { return oneShot((uint64_t)ms * 1000ULL); }
    bool periodicMs(uint32_t ms)      { return periodic((uint64_t)ms * 1000ULL); }
    bool retriggerableMs(uint32_t ms) { return retriggerable((uint64_t)ms * 1000ULL); }

    bool startSec(uint32_t sec, TimerMode mode = PERIODIC) { return start((uint64_t)sec * 1000000ULL, mode); }
    bool oneShotSec(uint32_t sec)       { return oneShot((uint64_t)sec * 1000000ULL); }
    bool periodicSec(uint32_t sec)      { return periodic((uint64_t)sec * 1000000ULL); }
    bool retriggerableSec(uint32_t sec) { return retriggerable((uint64_t)sec * 1000000ULL); }

    // --- Control de Estado y Modificación en Caliente ---
    bool stop();
    bool restart();
    bool pause();
    bool resume();
    bool updatePeriod(uint64_t microseconds);

    // --- Consultas en Tiempo Real ---
    uint64_t elapsed() const;
    uint32_t elapsedMs() const { return (uint32_t)(elapsed() / 1000ULL); }
    uint64_t remaining() const;
    uint32_t remainingMs() const { return (uint32_t)(remaining() / 1000ULL); }
    float    progress() const;
    bool     isRunning() const;
    bool     isPaused() const;

    // --- Callbacks ---
    bool setCallback(Callback callback);
    bool setISRCallback(ISRCallback callback, void* arg = nullptr);

    bool useTask(bool enable,
                 UBaseType_t priority = configMAX_PRIORITIES - 3,
                 uint32_t stackSize = 2048,
                 BaseType_t core = AYRES_TIMER_AUTO_CORE);

    // --- Diagnóstico y Telemetría ---
    esp_err_t   lastError() const;
    const char* lastErrorStr() const;
    TimerStats  getStats() const;
    void        resetStats();

    // --- Utilidades Estáticas de CPU y Tiempo ---
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
// AYRES TIMEOUT - Temporizador Ultra-Ligero para Máquinas de Estados (FSM)
// =============================================================================

/**
 * @brief Temporizador ligero sin tareas ni asignaciones de memoria.
 * 
 * Diseñado para reemplazar los patrones tradicionales de 'millis()' en máquinas
 * de estado y verificaciones de timeout mediante polling no bloqueante:
 * 
 *   if (timeout.isExpired()) { ... }
 */
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
// AYRES STOPWATCH - Cronómetro de Precisión en Microsegundos
// =============================================================================

/**
 * @brief Cronómetro de benchmarking para medir tiempos de ejecución de código.
 */
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
