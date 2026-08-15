/*
 * =============================================================================
 * AyresTimer v4.1.0 - Backend de Hardware (Bare-Metal Silicon Register Direct Access)
 * Desarrollado y mantenido por Daniel Cristian Salgado - AyresNet (https://ayresnet.com)
 * =============================================================================
 *
 * ARQUITECTURA DE HARDWARE (TIMER GROUPS TG0 / TG1):
 * --------------------------------------------------
 * El microcontrolador ESP32 cuenta con 2 grupos de temporizadores de hardware
 * en el silicio (TG0 y TG1), cada uno compuesto por 2 temporizadores universales
 * de 64 bits (T0 y T1).
 *
 * Este archivo implementa el control directo sin capas intermedias genéricas:
 * 1. Acceso directo a registros MMIO mapeados en memoria mediante timg_dev_t.
 * 2. Prescaler de 16 bits derivado del bus de reloj de periféricos APB (80 MHz).
 * 3. Comparador de alarma de 64 bits con auto-recarga electrónica en silicio.
 * 4. Barreras de memoria en ensamblador inline Xtensa ('memw') para sincronización.
 * 5. Asignación de vector de interrupción de hardware directo en IRAM (esp_intr_alloc).
 * 6. Despacho seguro de callbacks C++ en tareas FreeRTOS dedicadas.
 *
 * Licencia: MIT
 * =============================================================================
 */

#include "AyresTimer.h"

#include <soc/timer_group_struct.h>
#include <soc/timer_group_reg.h>
#include <soc/soc.h>
#include <esp_intr_alloc.h>
#include <esp32/clk.h>
#include <esp_err.h>
#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

namespace {
// Intervalo de espera para apagar limpiamente la tarea FreeRTOS
constexpr TickType_t kTaskShutdownPollTicks = pdMS_TO_TICKS(1);

// Punteros a los mapas de registros físicos de los Timer Groups en memoria de silicio
timg_dev_t* const kTimerGroups[2] = { &TIMERG0, &TIMERG1 };

// Vector de fuentes de interrupción de hardware asignadas por grupo y timer
const int kTimerIntrSources[2][2] = {
    { ETS_TG0_T0_LEVEL_INTR_SOURCE, ETS_TG0_T1_LEVEL_INTR_SOURCE },
    { ETS_TG1_T0_LEVEL_INTR_SOURCE, ETS_TG1_T1_LEVEL_INTR_SOURCE }
};

/**
 * @brief Guardia RAII para semáforos/mutex recursivos.
 * Asegura la liberación automática del mutex al salir del ámbito (scope).
 */
class RecursiveGuard {
public:
    explicit RecursiveGuard(SemaphoreHandle_t mutex) : _mutex(mutex) {
        _locked = _mutex && xSemaphoreTakeRecursive(_mutex, portMAX_DELAY) == pdTRUE;
    }
    ~RecursiveGuard() {
        if (_locked) xSemaphoreGiveRecursive(_mutex);
    }
    explicit operator bool() const { return _locked; }
private:
    SemaphoreHandle_t _mutex = nullptr;
    bool _locked = false;
};

/**
 * @brief Valida que el identificador de núcleo de CPU sea válido en el ESP32.
 */
bool validCore(BaseType_t core) {
    return core == AYRES_TIMER_AUTO_CORE || core == tskNO_AFFINITY ||
           (core >= 0 && core < portNUM_PROCESSORS);
}

// Mutex de spinlock para proteger la tabla de propietarios de hardware entre cores
portMUX_TYPE hardwareOwnersMux = portMUX_INITIALIZER_UNLOCKED;
AyresTimerHW* hardwareOwners[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};

/**
 * @brief Multiplicación y división entera de 64 bits sin desbordamiento.
 * Calcula: floor(value * multiplier / divisor) evitando el overflow de uint64_t.
 */
bool mulDivFloor(uint64_t value, uint64_t multiplier, uint64_t divisor, uint64_t& result) {
    if (divisor == 0) return false;
    const uint64_t quotient = value / divisor;
    const uint64_t remainder = value % divisor;
    if (quotient != 0 && multiplier > UINT64_MAX / quotient) return false;
    const uint64_t whole = quotient * multiplier;
    const uint64_t fraction = (remainder * multiplier) / divisor;
    if (whole > UINT64_MAX - fraction) return false;
    result = whole + fraction;
    return true;
}

/**
 * @brief Barrera de memoria física Xtensa.
 * Fuerza a la CPU a vaciar el pipeline de escritura al bus de periféricos
 * antes de continuar ejecutando la siguiente instrucción.
 */
static inline void IRAM_ATTR memoryBarrier() {
    __asm__ __volatile__("memw" : : : "memory");
}
}

// Tabla global de handles de interrupción asignados
static intr_handle_t s_intrHandles[2][2] = {{nullptr, nullptr}, {nullptr, nullptr}};

// =============================================================================
// CONSTRUCTOR Y DESTRUCTOR
// =============================================================================

AyresTimer<AYRES_HARDWARE>::AyresTimer(Group group, Index idx)
    : _group(static_cast<timer_group_t>(group)),
      _idx(static_cast<timer_idx_t>(idx)) {
    // Crear mutexes recursivos para proteger operaciones multihilo
    _apiMutex = xSemaphoreCreateRecursiveMutex();
    _callbackMutex = xSemaphoreCreateRecursiveMutex();
    if (!_apiMutex || !_callbackMutex) setError_(ESP_ERR_NO_MEM);
}

AyresTimer<AYRES_HARDWARE>::~AyresTimer() {
    // Detener el hardware y liberar recursos
    (void)stop();
    const bool releasedCleanly = teardownHardware_();
    if (releasedCleanly) releaseHardware_();
    stopTask_();

    // Eliminar mutexes de sincronización
    if (_callbackMutex) {
        vSemaphoreDelete(_callbackMutex);
        _callbackMutex = nullptr;
    }
    if (_apiMutex) {
        vSemaphoreDelete(_apiMutex);
        _apiMutex = nullptr;
    }
}

// =============================================================================
// GESTION DE ERRORES Y ESTADO
// =============================================================================

bool AyresTimer<AYRES_HARDWARE>::validMode_(TimerMode mode) const {
    return mode == ONE_SHOT || mode == PERIODIC || mode == RETRIGGERABLE;
}

void AyresTimer<AYRES_HARDWARE>::setError_(esp_err_t error) const {
    portENTER_CRITICAL(&_mux);
    _lastError = error;
    portEXIT_CRITICAL(&_mux);
}

esp_err_t AyresTimer<AYRES_HARDWARE>::lastError() const {
    portENTER_CRITICAL(&_mux);
    const esp_err_t result = _lastError;
    portEXIT_CRITICAL(&_mux);
    return result;
}

const char* AyresTimer<AYRES_HARDWARE>::lastErrorStr() const {
    return esp_err_to_name(lastError());
}

TimerStats AyresTimer<AYRES_HARDWARE>::getStats() const {
    portENTER_CRITICAL(&_mux);
    const TimerStats s = _stats;
    portEXIT_CRITICAL(&_mux);
    return s;
}

void AyresTimer<AYRES_HARDWARE>::resetStats() {
    portENTER_CRITICAL(&_mux);
    _stats = TimerStats{};
    portEXIT_CRITICAL(&_mux);
}

// =============================================================================
// CONFIGURACION Y PROPIEDAD EXCLUSIVA DEL HARDWARE
// =============================================================================

bool AyresTimer<AYRES_HARDWARE>::setClockDivisor(uint32_t divisor) {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    if (divisor < 2 || divisor > 65536) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }
    if (isRunning()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    _divisor = divisor;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);
    return true;
}

bool AyresTimer<AYRES_HARDWARE>::claimHardware_() {
    const int group = static_cast<int>(_group);
    const int index = static_cast<int>(_idx);
    if (group < 0 || group >= 2 || index < 0 || index >= 2) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }

    portENTER_CRITICAL(&hardwareOwnersMux);
    AyresTimerHW*& owner = hardwareOwners[group][index];
    const bool available = owner == nullptr || owner == this;
    if (available) owner = this;
    portEXIT_CRITICAL(&hardwareOwnersMux);

    if (!available) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    _ownsHardware = true;
    return true;
}

void AyresTimer<AYRES_HARDWARE>::releaseHardware_() {
    if (!_ownsHardware || _initialized || _isrRegistered) return;
    const int group = static_cast<int>(_group);
    const int index = static_cast<int>(_idx);
    if (group >= 0 && group < 2 && index >= 0 && index < 2) {
        portENTER_CRITICAL(&hardwareOwnersMux);
        if (hardwareOwners[group][index] == this) hardwareOwners[group][index] = nullptr;
        portEXIT_CRITICAL(&hardwareOwnersMux);
    }
    _ownsHardware = false;
}

bool AyresTimer<AYRES_HARDWARE>::teardownHardware_() {
    const int group = static_cast<int>(_group);
    const int index = static_cast<int>(_idx);
    timg_dev_t* hw = kTimerGroups[group];

    // Detener conteo y deshabilitar interrupciones directamente en registros del silicio
    hw->hw_timer[index].config.enable = 0;
    hw->hw_timer[index].config.level_int_en = 0;
    hw->hw_timer[index].config.alarm_en = 0;
    memoryBarrier();

    if (_isrRegistered && s_intrHandles[group][index]) {
        esp_intr_free(s_intrHandles[group][index]);
        s_intrHandles[group][index] = nullptr;
        _isrRegistered = false;
    }

    _initialized = false;
    setError_(ESP_OK);
    return true;
}

// =============================================================================
// CONTROL PRINCIPAL: START, STOP, PAUSE, RESUME, RETRIGGER
// =============================================================================

bool AyresTimer<AYRES_HARDWARE>::start(uint64_t microseconds, TimerMode mode) {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    if (microseconds == 0 || !validMode_(mode)) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }
    if (!claimHardware_()) return false;

    portENTER_CRITICAL(&_mux);
    const bool hasCppCallback = _hasCppCallback;
    portEXIT_CRITICAL(&_mux);
    if (hasCppCallback) {
        if (!ensureTask_()) return false;
    }

    if (!stop()) return false;
    if (!teardownHardware_()) return false;

    portENTER_CRITICAL(&_mux);
    const uint32_t divisor = _divisor;
    portEXIT_CRITICAL(&_mux);

    // Leer la frecuencia real del bus de reloj de periféricos APB (80 MHz)
    const uint32_t apbHz = static_cast<uint32_t>(esp_clk_apb_freq());
    if (apbHz == 0) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    // Calcular ticks de hardware correspondientes al tiempo solicitado
    const uint64_t tickDenominator = static_cast<uint64_t>(divisor) * 1000000ULL;
    uint64_t ticks = 0;
    if (!mulDivFloor(microseconds, apbHz, tickDenominator, ticks)) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }
    if (ticks == 0) ticks = 1;

    const int g = static_cast<int>(_group);
    const int idx = static_cast<int>(_idx);
    timg_dev_t* hw = kTimerGroups[g];

    // -------------------------------------------------------------------------
    // ESCRITURA DIRECTA EN REGISTROS DEL SILICIO (BARE-METAL MMIO)
    // -------------------------------------------------------------------------
    
    // 1. Apagar temporalmente el contador mientras se aplica la configuración
    hw->hw_timer[idx].config.enable = 0;
    memoryBarrier();

    // 2. Cargar prescaler (divisor de reloj de 16 bits: 2 a 65536)
    hw->hw_timer[idx].config.divider = (divisor == 65536) ? 0 : static_cast<uint16_t>(divisor);

    // 3. Conteo ascendente (increase = 1)
    hw->hw_timer[idx].config.increase = 1;

    // 4. Auto-recarga en silicio para modo periódico
    hw->hw_timer[idx].config.autoreload = (mode == PERIODIC) ? 1 : 0;

    // 5. Cargar valor inicial 0 en registros de recarga del contador físico
    hw->hw_timer[idx].load_high = 0;
    hw->hw_timer[idx].load_low = 0;
    memoryBarrier();
    hw->hw_timer[idx].reload = 1; // Dispara la carga inmediata en silicio
    memoryBarrier();

    // 6. Configurar valor de alarma de hardware (64 bits dividido en High y Low)
    hw->hw_timer[idx].alarm_high = static_cast<uint32_t>(ticks >> 32);
    hw->hw_timer[idx].alarm_low = static_cast<uint32_t>(ticks & 0xFFFFFFFFULL);
    hw->hw_timer[idx].config.alarm_en = 1;
    hw->hw_timer[idx].config.level_int_en = 1;
    memoryBarrier();

    // 7. Asignar vector de interrupción de hardware directo en IRAM si no existe
    if (!s_intrHandles[g][idx]) {
        const int source = kTimerIntrSources[g][idx];
        const esp_err_t err = esp_intr_alloc(
            source, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
            reinterpret_cast<intr_handler_t>(&AyresTimer<AYRES_HARDWARE>::onTimerInterrupt),
            this, &s_intrHandles[g][idx]);
        if (err != ESP_OK) {
            setError_(err);
            return false;
        }
        _isrRegistered = true;
    }

    _initialized = true;
    const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());

    portENTER_CRITICAL(&_mux);
    _mode = mode;
    _timeout_us = microseconds;
    _apbClockHz = apbHz;
    _running = true;
    _paused = false;
    _pausedTicks = 0;
    _expectedNextUs = nowUs + microseconds;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);

    // 8. Arrancar físicamente el temporizador en silicio
    hw->hw_timer[idx].config.enable = 1;
    memoryBarrier();

    return true;
}

bool AyresTimer<AYRES_HARDWARE>::stop() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }

    const int g = static_cast<int>(_group);
    const int idx = static_cast<int>(_idx);
    timg_dev_t* hw = kTimerGroups[g];

    if (_initialized) {
        // Deshabilitar contador e interrupciones en silicio
        hw->hw_timer[idx].config.enable = 0;
        hw->hw_timer[idx].config.level_int_en = 0;
        hw->hw_timer[idx].config.alarm_en = 0;
        memoryBarrier();
    }

    portENTER_CRITICAL(&_mux);
    _running = false;
    _paused = false;
    _pausedTicks = 0;
    _cancelThrough = _producedCallbacks;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);
    return true;
}

bool AyresTimer<AYRES_HARDWARE>::pause() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool canPause = _initialized && _running && !_paused;
    portEXIT_CRITICAL(&_mux);
    if (!canPause) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    const int g = static_cast<int>(_group);
    const int idx = static_cast<int>(_idx);
    timg_dev_t* hw = kTimerGroups[g];

    // Detener el conteo en silicio y capturar los ticks transcurridos
    hw->hw_timer[idx].config.enable = 0;
    hw->hw_timer[idx].config.level_int_en = 0;
    memoryBarrier();

    // Latch del contador de hardware
    hw->hw_timer[idx].update = 1;
    memoryBarrier();
    const uint64_t ticks = (static_cast<uint64_t>(hw->hw_timer[idx].cnt_high) << 32) |
                           static_cast<uint64_t>(hw->hw_timer[idx].cnt_low);

    portENTER_CRITICAL(&_mux);
    _paused = true;
    _pausedTicks = ticks;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);
    return true;
}

bool AyresTimer<AYRES_HARDWARE>::resume() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool canResume = _initialized && _running && _paused;
    portEXIT_CRITICAL(&_mux);
    if (!canResume) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    const int g = static_cast<int>(_group);
    const int idx = static_cast<int>(_idx);
    timg_dev_t* hw = kTimerGroups[g];

    // Reanudar conteo e interrupciones en silicio
    hw->hw_timer[idx].config.alarm_en = 1;
    hw->hw_timer[idx].config.level_int_en = 1;
    hw->hw_timer[idx].config.enable = 1;
    memoryBarrier();

    portENTER_CRITICAL(&_mux);
    _paused = false;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);
    return true;
}

bool AyresTimer<AYRES_HARDWARE>::restart() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const uint64_t timeout = _timeout_us;
    const TimerMode mode = _mode;
    portEXIT_CRITICAL(&_mux);
    if (timeout == 0) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    return start(timeout, mode);
}

bool AyresTimer<AYRES_HARDWARE>::updatePeriod(uint64_t microseconds) {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    if (microseconds == 0) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }

    portENTER_CRITICAL(&_mux);
    const bool running = _running;
    const TimerMode mode = _mode;
    if (!running) {
        _timeout_us = microseconds;
        _lastError = ESP_OK;
    }
    portEXIT_CRITICAL(&_mux);
    return running ? start(microseconds, mode) : true;
}

bool AyresTimer<AYRES_HARDWARE>::retrigger() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool allowed = _initialized && _running && _mode == RETRIGGERABLE;
    portEXIT_CRITICAL(&_mux);
    if (!allowed) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    const int g = static_cast<int>(_group);
    const int idx = static_cast<int>(_idx);
    timg_dev_t* hw = kTimerGroups[g];

    // Reinicio directo de registros de precarga a cero en silicio
    hw->hw_timer[idx].load_high = 0;
    hw->hw_timer[idx].load_low = 0;
    memoryBarrier();
    hw->hw_timer[idx].reload = 1;
    memoryBarrier();

    return true;
}

// =============================================================================
// CONSULTAS EN TIEMPO REAL: ELAPSED, REMAINING, PROGRESS
// =============================================================================

uint64_t AyresTimer<AYRES_HARDWARE>::elapsed() const {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return 0;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return 0;
    }
    portENTER_CRITICAL(&_mux);
    const bool running = _running;
    const bool paused = _paused;
    const uint64_t pausedTicks = _pausedTicks;
    const bool initialized = _initialized;
    const uint32_t apbHz = _apbClockHz;
    const uint32_t divisor = _divisor;
    portEXIT_CRITICAL(&_mux);
    if (!running || !initialized || apbHz == 0) return 0;

    uint64_t ticks = pausedTicks;
    if (!paused) {
        const int g = static_cast<int>(_group);
        const int idx = static_cast<int>(_idx);
        timg_dev_t* hw = kTimerGroups[g];

        // Latch instantáneo del hardware para congelar y leer los 64 bits de forma atómica
        hw->hw_timer[idx].update = 1;
        memoryBarrier();
        ticks = (static_cast<uint64_t>(hw->hw_timer[idx].cnt_high) << 32) |
                static_cast<uint64_t>(hw->hw_timer[idx].cnt_low);
    }

    uint64_t microseconds = 0;
    const uint64_t tickScale = static_cast<uint64_t>(divisor) * 1000000ULL;
    if (!mulDivFloor(ticks, tickScale, apbHz, microseconds)) {
        setError_(ESP_ERR_INVALID_SIZE);
        return UINT64_MAX;
    }
    return microseconds;
}

uint64_t AyresTimer<AYRES_HARDWARE>::remaining() const {
    portENTER_CRITICAL(&_mux);
    const uint64_t timeout = _timeout_us;
    portEXIT_CRITICAL(&_mux);
    const uint64_t el = elapsed();
    return (el >= timeout) ? 0 : (timeout - el);
}

float AyresTimer<AYRES_HARDWARE>::progress() const {
    portENTER_CRITICAL(&_mux);
    const uint64_t timeout = _timeout_us;
    portEXIT_CRITICAL(&_mux);
    if (timeout == 0) return 1.0f;
    const uint64_t el = elapsed();
    if (el >= timeout) return 1.0f;
    return static_cast<float>(el) / static_cast<float>(timeout);
}

bool AyresTimer<AYRES_HARDWARE>::isRunning() const {
    portENTER_CRITICAL(&_mux);
    const bool running = _running && !_paused;
    portEXIT_CRITICAL(&_mux);
    return running;
}

bool AyresTimer<AYRES_HARDWARE>::isPaused() const {
    portENTER_CRITICAL(&_mux);
    const bool paused = _paused;
    portEXIT_CRITICAL(&_mux);
    return paused;
}

// =============================================================================
// REGISTRO DE CALLBACKS Y TAREAS FREERTOS
// =============================================================================

bool AyresTimer<AYRES_HARDWARE>::setCallback(Callback callback) {
    if (!_callbackMutex || xPortInIsrContext()) {
        setError_(_callbackMutex ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool pendingCallback = _producedCallbacks != _consumedCallbacks ||
                                 _callbackActive;
    portEXIT_CRITICAL(&_mux);
    if (isRunning() || pendingCallback) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    xSemaphoreTakeRecursive(_callbackMutex, portMAX_DELAY);
    portENTER_CRITICAL(&_mux);
    const bool becameBusy = _running || _callbackActive ||
                            _producedCallbacks != _consumedCallbacks;
    portEXIT_CRITICAL(&_mux);
    if (becameBusy) {
        xSemaphoreGiveRecursive(_callbackMutex);
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    _callback = std::move(callback);
    const bool hasCallback = static_cast<bool>(_callback);
    xSemaphoreGiveRecursive(_callbackMutex);
    portENTER_CRITICAL(&_mux);
    _hasCppCallback = hasCallback;
    _isrCallback = nullptr;
    _isrArg = nullptr;
    portEXIT_CRITICAL(&_mux);

    setError_(ESP_OK);
    return true;
}

bool AyresTimer<AYRES_HARDWARE>::setISRCallback(ISRCallback callback, void* arg) {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool pendingCallback = _producedCallbacks != _consumedCallbacks ||
                                 _callbackActive;
    portEXIT_CRITICAL(&_mux);
    if (isRunning() || pendingCallback || !_callbackMutex) {
        setError_(_callbackMutex ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM);
        return false;
    }

    xSemaphoreTakeRecursive(_callbackMutex, portMAX_DELAY);
    portENTER_CRITICAL(&_mux);
    const bool becameBusy = _running || _callbackActive ||
                            _producedCallbacks != _consumedCallbacks;
    portEXIT_CRITICAL(&_mux);
    if (becameBusy) {
        xSemaphoreGiveRecursive(_callbackMutex);
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    _callback = nullptr;
    xSemaphoreGiveRecursive(_callbackMutex);
    portENTER_CRITICAL(&_mux);
    _hasCppCallback = false;
    _isrCallback = callback;
    _isrArg = arg;
    portEXIT_CRITICAL(&_mux);
    stopTask_();
    setError_(ESP_OK);
    return true;
}

bool AyresTimer<AYRES_HARDWARE>::ensureTask_() {
    portENTER_CRITICAL(&_mux);
    if (_taskHandle) {
        _useTask = true;
        _lastError = ESP_OK;
        portEXIT_CRITICAL(&_mux);
        return true;
    }
    _taskStopRequested = false;
    _producedCallbacks = 0;
    _consumedCallbacks = 0;
    _cancelThrough = 0;
    portEXIT_CRITICAL(&_mux);

    char taskName[16];
    snprintf(taskName, sizeof(taskName), "AyTimerHW%d%d", static_cast<int>(_group), static_cast<int>(_idx));
    TaskHandle_t handle = nullptr;
    const BaseType_t core = _taskCore == AYRES_TIMER_AUTO_CORE
        ? xPortGetCoreID()
        : _taskCore;
    const BaseType_t created = xTaskCreatePinnedToCore(
        &AyresTimer<AYRES_HARDWARE>::callbackTask_, taskName,
        _taskStackSize, this, _taskPriority, &handle, core);
    if (created != pdPASS || !handle) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }

    portENTER_CRITICAL(&_mux);
    _taskHandle = handle;
    _useTask = true;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);
    return true;
}

void AyresTimer<AYRES_HARDWARE>::stopTask_() {
    portENTER_CRITICAL(&_mux);
    TaskHandle_t handle = _taskHandle;
    _taskStopRequested = true;
    _useTask = false;
    portEXIT_CRITICAL(&_mux);
    if (!handle) return;

    xTaskNotifyGive(handle);
    if (xTaskGetCurrentTaskHandle() == handle) return;
    for (;;) {
        portENTER_CRITICAL(&_mux);
        const bool stopped = (_taskHandle == nullptr);
        portEXIT_CRITICAL(&_mux);
        if (stopped) break;
        vTaskDelay(kTaskShutdownPollTicks);
    }
}

bool AyresTimer<AYRES_HARDWARE>::useTask(bool enable, UBaseType_t priority,
                                         uint32_t stackSize, BaseType_t core) {
    if (xPortInIsrContext() || stackSize < 1024 ||
        priority >= configMAX_PRIORITIES || !validCore(core)) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool busy = _running || _callbackActive ||
                      _producedCallbacks != _consumedCallbacks;
    portEXIT_CRITICAL(&_mux);
    if (busy) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    if (enable) {
        portENTER_CRITICAL(&_mux);
        const bool hasISRCallback = _isrCallback != nullptr;
        const bool taskExists = _taskHandle != nullptr;
        const bool changed = _taskPriority != priority ||
                             _taskStackSize != stackSize || _taskCore != core;
        portEXIT_CRITICAL(&_mux);
        if (hasISRCallback) {
            setError_(ESP_ERR_INVALID_STATE);
            return false;
        }
        if (taskExists && changed) stopTask_();
        portENTER_CRITICAL(&_mux);
        _taskPriority = priority;
        _taskStackSize = stackSize;
        _taskCore = core;
        portEXIT_CRITICAL(&_mux);
        return ensureTask_();
    }

    portENTER_CRITICAL(&_mux);
    const bool hasCppCallback = _hasCppCallback;
    portEXIT_CRITICAL(&_mux);
    if (hasCppCallback) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    stopTask_();
    setError_(ESP_OK);
    return true;
}

// =============================================================================
// MANEJADOR DE INTERRUPCION DE SILICIO (ISR) Y DESPACHO DE TAREA
// =============================================================================

bool IRAM_ATTR AyresTimer<AYRES_HARDWARE>::onTimerInterrupt(void* arg) {
    auto* self = static_cast<AyresTimer<AYRES_HARDWARE>*>(arg);
    const int g = static_cast<int>(self->_group);
    const int idx = static_cast<int>(self->_idx);
    timg_dev_t* hw = kTimerGroups[g];

    // Limpieza física inmediata de la bandera de interrupción en el registro de silicio
    if (idx == 0) {
        hw->int_clr_timers.t0 = 1;
    } else {
        hw->int_clr_timers.t1 = 1;
    }
    memoryBarrier();

    const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());

    portENTER_CRITICAL_ISR(&self->_mux);
    if (self->_mode != PERIODIC) {
        self->_running = false;
        hw->hw_timer[idx].config.enable = 0; // Detener conteo si es disparo único
    } else {
        hw->hw_timer[idx].config.alarm_en = 1; // Re-armar alarma periódica en hardware
    }
    memoryBarrier();

    // Telemetría en tiempo real
    ++self->_stats.totalTriggers;
    self->_stats.lastTriggerUs = nowUs;
    if (self->_expectedNextUs > 0) {
        const int64_t jitter = static_cast<int64_t>(nowUs) - static_cast<int64_t>(self->_expectedNextUs);
        self->_stats.lastJitterUs = jitter;
        const int64_t absJitter = jitter < 0 ? -jitter : jitter;
        if (absJitter > self->_stats.maxJitterUs) {
            self->_stats.maxJitterUs = absJitter;
        }
    }
    if (self->_mode == PERIODIC) {
        self->_expectedNextUs = nowUs + self->_timeout_us;
    } else {
        self->_expectedNextUs = 0;
    }

    TaskHandle_t task = self->_useTask ? self->_taskHandle : nullptr;
    ISRCallback callback = self->_isrCallback;
    void* callbackArg = self->_isrArg;
    if (task) ++self->_producedCallbacks;
    portEXIT_CRITICAL_ISR(&self->_mux);

    // Despertar la tarea FreeRTOS de despacho seguro si está configurada
    if (task) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &woken);
        return woken == pdTRUE;
    }
    // Si no hay tarea, invocar callback directo de ISR
    if (callback) callback(callbackArg);
    return false;
}

void AyresTimer<AYRES_HARDWARE>::callbackTask_(void* pv) {
    auto* self = static_cast<AyresTimer<AYRES_HARDWARE>*>(pv);
    for (;;) {
        // Espera bloqueante sin consumo de CPU hasta que la ISR de hardware le avise
        (void)ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

        portENTER_CRITICAL(&self->_mux);
        const bool stopRequested = self->_taskStopRequested;
        const uint64_t sequence = ++self->_consumedCallbacks;
        const bool cancelled = sequence <= self->_cancelThrough;
        if (cancelled) ++self->_stats.droppedCallbacks;
        portEXIT_CRITICAL(&self->_mux);
        if (stopRequested) break;
        if (cancelled) continue;

        // Ejecución segura del callback C++ dentro del contexto de tarea FreeRTOS
        if (self->_callbackMutex) {
            xSemaphoreTakeRecursive(self->_callbackMutex, portMAX_DELAY);
            portENTER_CRITICAL(&self->_mux);
            const bool callCpp = self->_hasCppCallback;
            self->_callbackActive = callCpp;
            portEXIT_CRITICAL(&self->_mux);
            xSemaphoreGiveRecursive(self->_callbackMutex);
            if (callCpp) self->_callback();
            portENTER_CRITICAL(&self->_mux);
            self->_callbackActive = false;
            portEXIT_CRITICAL(&self->_mux);
        }
    }

    portENTER_CRITICAL(&self->_mux);
    self->_taskHandle = nullptr;
    portEXIT_CRITICAL(&self->_mux);
    vTaskDelete(nullptr);
}
