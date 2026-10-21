#include "fork.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#include "events.hpp"
#include "loader.hpp"
#include "scorep_api.hpp"

namespace scorepy
{

namespace
{

using restore_fn_t = void (*)(void* orig_dlopen, void* orig_dlsym);

restore_fn_t restore_fn = nullptr;
void* exit_hook_handle = nullptr;
bool registered = false;

/// Point the _exit-hook library (preloaded, interposing _exit) at the
/// finalize function of the current measurement environment.
void set_exit_hook_finalize(void)
{
    if (exit_hook_handle == nullptr)
    {
        return;
    }
    using setter_t = void (*)(void (*)(void));
    auto* setter = reinterpret_cast<setter_t>(dlsym(exit_hook_handle, "scorep_py_set_finalize"));
    if (setter != nullptr)
    {
        setter(scorep_api().finalize_measurement);
    }
}

/// Locate `libwrap_subsystem_restore_original_bindings`, which restores the
/// gotcha-interposed symbols (libwrap wrappers, dlopen/dlsym) to their
/// original targets. Required before unloading the Score-P runtime, as the
/// interposed GOT entries would otherwise dangle.
restore_fn_t find_restore_api(void)
{
    if (const char* shim = std::getenv("SCOREP_PYTHON_BINDINGS_RESTORE_LIB"))
    {
        if (shim[0] != '\0')
        {
            if (void* handle = dlopen(shim, RTLD_NOW | RTLD_LOCAL))
            {
                if (void* symbol = dlsym(handle, "libwrap_subsystem_restore_original_bindings"))
                {
                    return reinterpret_cast<restore_fn_t>(symbol);
                }
            }
            std::fprintf(stderr, "SCOREP_BINDING_PYTHON WARNING: cannot load the restore "
                                 "library from SCOREP_PYTHON_BINDINGS_RESTORE_LIB=%s: %s\n",
                         shim, dlerror());
            return nullptr;
        }
    }
    for (void* handle : {constructor_lib_handle(), subsystem_lib_handle()})
    {
        if (handle == nullptr)
        {
            continue;
        }
        if (void* symbol = dlsym(handle, "libwrap_subsystem_restore_original_bindings"))
        {
            return reinterpret_cast<restore_fn_t>(symbol);
        }
    }
    return nullptr;
}

void nop_fini(void)
{
}

/// Neutralize the fini_array destructors of the inherited measurement
/// environment. Score-P finalizes via __cxa_atexit handlers that are run
/// through __cxa_finalize inside the fini_array of its libraries. Running
/// them while unloading in a forked child would flush the environment of the
/// parent into the parent's experiment directory and break its finalization.
int fini_patcher(struct dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    (void)data;
    const char* name = info->dlpi_name ? info->dlpi_name : "";
    if (std::strstr(name, "libscorep_constructor") == nullptr &&
        std::strstr(name, "libscorep_measurement") == nullptr)
    {
        return 0;
    }
    // See loader.cpp: detect whether the DT_ pointer is absolute or a
    // relative virtual address.
    uintptr_t bias = info->dlpi_addr;
    uintptr_t map_size = 0;
    for (int i = 0; i < info->dlpi_phnum; i++)
    {
        if (info->dlpi_phdr[i].p_type == PT_LOAD)
        {
            uintptr_t end = info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz;
            if (end > map_size)
            {
                map_size = end;
            }
        }
    }
    const auto dyn_ptr = [&](ElfW(Addr) value) -> void* {
        if (value >= bias && value < bias + map_size)
        {
            return reinterpret_cast<void*>(value);
        }
        return reinterpret_cast<void*>(bias + value);
    };
    for (int i = 0; i < info->dlpi_phnum; i++)
    {
        if (info->dlpi_phdr[i].p_type != PT_DYNAMIC)
        {
            continue;
        }
        auto* dyn = static_cast<ElfW(Dyn)*>(dyn_ptr(info->dlpi_phdr[i].p_vaddr));
        void** arr = nullptr;
        size_t bytes = 0;
        for (ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++)
        {
            if (d->d_tag == DT_FINI_ARRAY)
            {
                arr = static_cast<void**>(dyn_ptr(d->d_un.d_ptr));
            }
            if (d->d_tag == DT_FINI_ARRAYSZ)
            {
                bytes = d->d_un.d_val;
            }
        }
        if (arr == nullptr || bytes == 0)
        {
            continue;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(arr) & ~uintptr_t(0xfff);
        const uintptr_t end =
            (reinterpret_cast<uintptr_t>(arr) + bytes + 0xfff) & ~uintptr_t(0xfff);
        if (mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE) != 0)
        {
            continue;
        }
        for (size_t j = 0; j < bytes / sizeof(void*); j++)
        {
            arr[j] = reinterpret_cast<void*>(&nop_fini);
        }
        mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ);
    }
    return 0;
}

void atfork_child(void)
{
    // Remove all references from other objects into the Score-P runtime:
    // first via the Score-P provided restore function (if available), then
    // via our own pre-interposition GOT snapshot, which covers the remaining
    // gotcha-interposed entries. Without this, the GOT entries would dangle
    // once the runtime is unloaded.
    if (restore_fn != nullptr)
    {
        restore_fn(orig_dlopen(), orig_dlsym());
    }
    restore_gots_from_snapshot();
    dl_iterate_phdr(fini_patcher, nullptr);

    // Give the child its own experiment directory. If the user pinned one via
    // SCOREP_EXPERIMENT_DIRECTORY, suffix it with the child's pid; otherwise
    // leave it unset so Score-P generates a fresh default directory.
    if (const char* user_dir = std::getenv("SCOREP_EXPERIMENT_DIRECTORY"))
    {
        if (user_dir[0] != '\0')
        {
            std::string dir = std::string(user_dir) + "_child_" + std::to_string(getpid());
            setenv("SCOREP_EXPERIMENT_DIRECTORY", dir.c_str(), 1);
        }
    }

    reset_event_state();

    if (!unload_measurement_env() || !reload_measurement_env())
    {
        std::fprintf(stderr, "SCOREP_BINDING_PYTHON WARNING: fork-reinit failed, the child "
                             "continues without a re-initialized measurement environment.\n");
        return;
    }
    set_exit_hook_finalize();
}

} // namespace

bool register_fork_reinit(void)
{
    if (registered)
    {
        return true;
    }
    if (!init_binding_measurement() || !measurement_relocatable())
    {
        std::fprintf(stderr, "SCOREP_BINDING_PYTHON WARNING: fork-reinit unavailable: the Score-P "
                             "runtime was not loaded by the binding (dlopen-mode required, see "
                             "SCOREP_PYTHON_BINDINGS_FORK_REINIT).\n");
        return false;
    }
    if (orig_dlopen() == nullptr || orig_dlsym() == nullptr)
    {
        std::fprintf(stderr, "SCOREP_BINDING_PYTHON WARNING: fork-reinit unavailable: the original "
                             "dl* symbols could not be captured.\n");
        return false;
    }
    // Optional: a Score-P build that provides
    // libwrap_subsystem_restore_original_bindings (or a test shim via
    // SCOREP_PYTHON_BINDINGS_RESTORE_LIB) unwinds the gotcha bindings with
    // Score-P internal knowledge. The binding-internal GOT snapshot
    // (see loader.hpp) covers the same ground on stock Score-P builds.
    restore_fn = find_restore_api();
    if (const char* hook = std::getenv("SCOREP_PYTHON_BINDINGS_EXIT_HOOK_LIB"))
    {
        if (hook[0] != '\0')
        {
            // Already preloaded: dlopen returns the same handle (and keeps it
            // referenced for the lifetime of this binding).
            exit_hook_handle = dlopen(hook, RTLD_NOW | RTLD_LOCAL);
        }
    }
    set_exit_hook_finalize();
    if (pthread_atfork(nullptr, nullptr, atfork_child) != 0)
    {
        std::fprintf(stderr, "SCOREP_BINDING_PYTHON WARNING: pthread_atfork failed.\n");
        return false;
    }
    registered = true;
    return true;
}

} // namespace scorepy
