#include <algorithm>
#include <array>

#include <Python.h>

#include "compat.hpp"
#include "events.hpp"
#include "pythonHelpers.hpp"
#include "scorep_api.hpp"

namespace scorepy
{

std::unordered_map<compat::PyCodeObject*, region_handle> regions;
static std::unordered_map<std::string, region_handle> user_regions;
static std::unordered_map<std::string, region_handle> rewind_regions;

// File-scope instead of function-local, so reset_event_state() can reset them
// after the measurement environment was re-initialized in a forked child.
static region_handle error_region;
static SCOREP_User_ParameterHandle error_region_param = SCOREP_USER_INVALID_PARAMETER;
static bool error_printed = false;
static SCOREP_User_ParameterHandle parameter_handle_int = SCOREP_USER_INVALID_PARAMETER;
static SCOREP_User_ParameterHandle parameter_handle_uint = SCOREP_USER_INVALID_PARAMETER;
static SCOREP_User_ParameterHandle parameter_handle_string = SCOREP_USER_INVALID_PARAMETER;

/**
 * @brief when Python PyCodeObject is deallocated, remove it from our regions list.
 *
 * @param co code object to remove
 */
void on_dealloc(PyCodeObject* co)
{
    regions.erase(co);
}

static compat::RegisterCodeDealloc register_dealloc(on_dealloc);

void reset_event_state(void)
{
    regions.clear();
    user_regions.clear();
    rewind_regions.clear();
    error_region = region_handle();
    error_region_param = SCOREP_USER_INVALID_PARAMETER;
    error_printed = false;
    parameter_handle_int = SCOREP_USER_INVALID_PARAMETER;
    parameter_handle_uint = SCOREP_USER_INVALID_PARAMETER;
    parameter_handle_string = SCOREP_USER_INVALID_PARAMETER;
}

// Used for regions, that have an identifier, aka a code object id. (instrumenter regions and
// some decorated regions)
void region_begin(std::string_view function_name, std::string module, std::string file_name,
                  const std::uint64_t line_number, compat::PyCodeObject* identifier)
{
    const ScorepApi& scorep = scorep_api();
    region_handle& region = regions[identifier];

    if (region == uninitialised_region_handle)
    {
        auto region_name = make_region_name(std::move(module), function_name);
        scorep.user_region_init(&region.value, NULL, NULL, region_name.c_str(),
                                SCOREP_USER_REGION_TYPE_FUNCTION, file_name.c_str(), line_number);

        if (const auto pos = region_name.find(':'); pos != std::string::npos)
        {
            region_name.resize(pos);
            scorep.user_region_set_group(region.value, region_name.c_str());
        }
    }
    scorep.user_region_enter(region.value);
}

// Used for regions, that only have a function name, a module, a file and a line number (user
// regions)
void region_begin(std::string_view function_name, std::string module, std::string file_name,
                  const std::uint64_t line_number)
{
    const ScorepApi& scorep = scorep_api();
    auto region_name = make_region_name(std::move(module), function_name);
    region_handle& region = user_regions[region_name];

    if (region == uninitialised_region_handle)
    {
        scorep.user_region_init(&region.value, NULL, NULL, region_name.c_str(),
                                SCOREP_USER_REGION_TYPE_FUNCTION, file_name.c_str(), line_number);

        if (const auto pos = region_name.find(':'); pos != std::string::npos)
        {
            region_name.resize(pos);
            scorep.user_region_set_group(region.value, region_name.c_str());
        }
    }
    scorep.user_region_enter(region.value);
}

// Used for regions, that have an identifier, aka a code object id. (instrumenter regions and
// some decorated regions)
void region_end(std::string_view function_name, std::string module,
                compat::PyCodeObject* identifier)
{
    const auto it_region = regions.find(identifier);
    if (it_region != regions.end())
    {
        scorep_api().user_region_end(it_region->second.value);
    }
    else
    {
        const auto region_name = make_region_name(std::move(module), function_name);
        region_end_error_handling(std::move(region_name));
    }
}

// Used for regions, that only have a function name, a module (user regions)
void region_end(std::string_view function_name, std::string module)
{
    const auto region_name = make_region_name(std::move(module), function_name);
    auto it_region = user_regions.find(region_name);
    if (it_region != user_regions.end())
    {
        scorep_api().user_region_end(it_region->second.value);
    }
    else
    {
        region_end_error_handling(std::move(region_name));
    }
}

void region_end_error_handling(std::string region_name)
{
    const ScorepApi& scorep = scorep_api();

    if (std::find(compat::exit_region_whitelist.begin(), compat::exit_region_whitelist.end(),
                  region_name) != compat::exit_region_whitelist.end())
    {
        return;
    }

    if (error_region.value == SCOREP_USER_INVALID_REGION)
    {
        scorep.user_region_init(&error_region.value, NULL, NULL, "error_region",
                                SCOREP_USER_REGION_TYPE_FUNCTION, "scorep.cpp", 0);
        scorep.user_region_set_group(error_region.value, "error");
    }
    scorep.user_region_enter(error_region.value);
    scorep.user_parameter_string(&error_region_param, "leave-region", region_name.c_str());
    scorep.user_region_end(error_region.value);

    if (!error_printed)
    {
        std::cerr << "SCOREP_BINDING_PYTHON ERROR: There was a region exit without an enter!\n"
                  << "SCOREP_BINDING_PYTHON ERROR: For details look for \"error_region\" in "
                     "the trace or profile."
                  << std::endl;
        error_printed = true;
    }
}

void rewind_begin(std::string region_name, std::string file_name, std::uint64_t line_number)
{
    const ScorepApi& scorep = scorep_api();
    auto pair = rewind_regions.emplace(make_pair(region_name, region_handle()));
    bool inserted_new = pair.second;
    auto& handle = pair.first->second;
    if (inserted_new)
    {
        scorep.user_region_init(&handle.value, NULL, NULL, region_name.c_str(),
                                SCOREP_USER_REGION_TYPE_FUNCTION, file_name.c_str(), line_number);
    }
    scorep.user_rewind_region_enter(handle.value);
}

void rewind_end(std::string region_name, bool value)
{
    auto& handle = rewind_regions.at(region_name);
    /* don't call SCOREP_ExitRewindRegion, as
     * SCOREP_User_RewindRegionEnd does some additional magic
     * */
    scorep_api().user_rewind_region_end(handle.value, value);
}

void parameter_int(std::string name, int64_t value)
{
    scorep_api().user_parameter_int64(&parameter_handle_int, name.c_str(), value);
}

void parameter_uint(std::string name, uint64_t value)
{
    scorep_api().user_parameter_uint64(&parameter_handle_uint, name.c_str(), value);
}

void parameter_string(std::string name, std::string value)
{
    scorep_api().user_parameter_string(&parameter_handle_string, name.c_str(), value.c_str());
}

} // namespace scorepy
