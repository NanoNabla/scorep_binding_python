#include "scorep_api.hpp"

#include <dlfcn.h>

namespace scorepy
{

namespace
{
ScorepApi api;

template <typename T>
bool resolve(T& target, void* handle, const char* name)
{
    void* symbol = handle ? dlsym(handle, name) : dlsym(RTLD_DEFAULT, name);
    if (symbol == nullptr)
    {
        return false;
    }
    target = reinterpret_cast<T>(symbol);
    return true;
}
} // namespace

bool load_scorep_api(void* handle)
{
    // dlsym on a specific handle searches the object and its dependency
    // closure, which provides all symbols of the Score-P runtime.
    api.loaded = resolve(api.user_region_init, handle, "SCOREP_User_RegionInit") &&
                 resolve(api.user_region_enter, handle, "SCOREP_User_RegionEnter") &&
                 resolve(api.user_region_end, handle, "SCOREP_User_RegionEnd") &&
                 resolve(api.user_region_set_group, handle, "SCOREP_User_RegionSetGroup") &&
                 resolve(api.user_rewind_region_enter, handle, "SCOREP_User_RewindRegionEnter") &&
                 resolve(api.user_rewind_region_end, handle, "SCOREP_User_RewindRegionEnd") &&
                 resolve(api.user_parameter_int64, handle, "SCOREP_User_ParameterInt64") &&
                 resolve(api.user_parameter_uint64, handle, "SCOREP_User_ParameterUint64") &&
                 resolve(api.user_parameter_string, handle, "SCOREP_User_ParameterString") &&
                 resolve(api.user_enable_recording, handle, "SCOREP_User_EnableRecording") &&
                 resolve(api.user_disable_recording, handle, "SCOREP_User_DisableRecording") &&
                 resolve(api.get_experiment_dir_name, handle, "SCOREP_GetExperimentDirName") &&
                 resolve(api.finalize_measurement, handle, "SCOREP_FinalizeMeasurement") &&
                 resolve(api.register_exit_handler, handle, "SCOREP_RegisterExitHandler");
    if (!api.loaded)
    {
        api = ScorepApi();
    }
    return api.loaded;
}

const ScorepApi& scorep_api()
{
    return api;
}

} // namespace scorepy
