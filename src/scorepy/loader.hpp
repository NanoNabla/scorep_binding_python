#pragma once

namespace scorepy
{

/** Make the Score-P runtime usable for this binding.
 *
 * Two modes are supported:
 *
 * - preloaded-mode (default): Score-P was loaded via LD_PRELOAD before the
 *   interpreter started and its measurement environment is already
 *   initialized. The API symbols are resolved via RTLD_DEFAULT.
 *
 * - dlopen-mode: requested via SCOREP_PYTHON_BINDINGS_DLOPEN=1 (required for
 *   the fork-reinit support). The binding itself loads the Score-P runtime by
 *   dlopening the interposition libraries, the compiled subsystem library and
 *   the Score-P constructor library (whose initialization routine runs
 *   SCOREP_InitMeasurementWithArgs). Because the runtime is loaded with
 *   dlopen, it can be unloaded again, which the fork-reinit relies on.
 *
 * Idempotent. Must be called before any Score-P function is used.
 *
 * @return true if the Score-P API is usable, false otherwise.
 */
bool init_binding_measurement(void);

/** @return true if the Score-P runtime was loaded by this binding (dlopen-mode)
 * and can therefore be unloaded/reloaded. */
bool measurement_relocatable(void);

/** Handle of the loaded subsystem library (dlopen-mode), else null. */
void* subsystem_lib_handle(void);

/** Handle of the loaded Score-P constructor library (dlopen-mode), else null. */
void* constructor_lib_handle(void);

/** Original (non-interposed) dlopen/dlsym/dlclose captured before Score-P
 * initialized its libwrap interposition, or null if unavailable. */
void* orig_dlopen(void);
void* orig_dlsym(void);
void* orig_dlclose(void);

/** Unload the Score-P runtime (dlopen-mode only).
 *
 * Note: the caller must prevent the destructor-driven finalization of the
 * environment first (see fork.cpp), otherwise the measurement data is
 * flushed into the experiment directory of the parent process.
 *
 * @return true on success.
 */
bool unload_measurement_env(void);

/** Re-load the Score-P runtime and resolve the API again (dlopen-mode only).
 * Loading the constructor library triggers a fresh measurement
 * initialization, which picks up the current environment (e.g. a child
 * specific SCOREP_EXPERIMENT_DIRECTORY).
 *
 * @return true on success.
 */
bool reload_measurement_env(void);

/** Snapshot all GOT entries (JUMP_SLOT / GLOB_DAT) of the already loaded
 * objects. Called after the Score-P runtime libraries are mapped but before
 * the measurement initialization installs the gotcha interposition, so the
 * snapshot represents the pre-interposition state.
 *
 * @return true on success.
 */
bool snapshot_gots(void);

/** Restore all GOT entries captured by snapshot_gots() that were since
 * re-pointed (into the gotcha interposition). This removes every dangling
 * reference into the Score-P runtime before it is unloaded.
 *
 * @return true on success.
 */
bool restore_gots_from_snapshot(void);

} // namespace scorepy
