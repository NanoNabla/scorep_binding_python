#pragma once

#include <cstdint>

#include <scorep/SCOREP_User_Types.h>

namespace scorepy
{

/// Function pointer table for the Score-P API used by the binding.
///
/// All Score-P symbols are resolved at runtime via dlsym on the handle of the
/// loaded subsystem instead of being linked directly. This is required for the
/// fork-reinit support: after the Score-P measurement environment has been
/// unloaded and reloaded inside a forked child, the function pointers are
/// resolved again (see scorep/fork.hpp). Direct references from this binding
/// would dangle after an unload.
struct ScorepApi
{
    bool loaded = false;

    void (*user_region_init)(SCOREP_User_RegionHandle* handle, const char** last_file_name,
                             SCOREP_SourceFileHandle* last_file, const char* name,
                             SCOREP_User_RegionType region_type, const char* file_name,
                             std::uint32_t line_number) = nullptr;
    void (*user_region_enter)(SCOREP_User_RegionHandle handle) = nullptr;
    void (*user_region_end)(SCOREP_User_RegionHandle handle) = nullptr;
    void (*user_region_set_group)(SCOREP_User_RegionHandle handle,
                                  const char* group_name) = nullptr;
    void (*user_rewind_region_enter)(SCOREP_User_RegionHandle handle) = nullptr;
    void (*user_rewind_region_end)(SCOREP_User_RegionHandle handle, bool value) = nullptr;
    void (*user_parameter_int64)(SCOREP_User_ParameterHandle* handle, const char* name,
                                 std::int64_t value) = nullptr;
    void (*user_parameter_uint64)(SCOREP_User_ParameterHandle* handle, const char* name,
                                  std::uint64_t value) = nullptr;
    void (*user_parameter_string)(SCOREP_User_ParameterHandle* handle, const char* name,
                                  const char* value) = nullptr;
    void (*user_enable_recording)(void) = nullptr;
    void (*user_disable_recording)(void) = nullptr;

    const char* (*get_experiment_dir_name)(void) = nullptr;
    void (*finalize_measurement)(void) = nullptr;
    void (*register_exit_handler)(void) = nullptr;
};

/** Resolve the Score-P API using `handle`.
 *
 * `handle` must be a dlopen handle that provides the Score-P runtime (e.g. the
 * handle of the compiled subsystem library, whose dependency closure contains
 * libscorep_measurement). If `handle` is null, the symbols are looked up in
 * the global scope (RTLD_DEFAULT), which covers the case where Score-P was
 * already loaded via LD_PRELOAD.
 *
 * Note: a RTLD_DEFAULT lookup goes through the gotcha-interposed dlsym, which
 * pins the Score-P libraries into memory. That is harmless when Score-P was
 * preloaded (it cannot be unloaded anyway), but it must be avoided in
 * dlopen-mode, where an explicit handle is used instead.
 *
 * @return true if all symbols were resolved.
 */
bool load_scorep_api(void* handle);

/** Access the resolved Score-P API.
 *
 * Only valid after load_scorep_api() returned true.
 */
const ScorepApi& scorep_api();

} // namespace scorepy
