#include "loader.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <string>
#include <sys/mman.h>
#include <utility>
#include <vector>

#include "scorep_api.hpp"

namespace scorepy
{

namespace
{

struct MeasurementEnv
{
    bool initialized = false;
    bool relocatable = false;
    void* subsystem_handle = nullptr;
    void* constructor_handle = nullptr;
    std::vector<void*> interposition_handles;
    void* orig_dlopen_ptr = nullptr;
    void* orig_dlsym_ptr = nullptr;
    void* orig_dlclose_ptr = nullptr;
};

MeasurementEnv env;

bool open_runtime(bool take_got_snapshot);
void capture_orig_dl(void* subsystem_handle);

const char* env_value(const char* name)
{
    const char* value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

/// dlopen with the original dlopen if available, as the gotcha-interposed
/// dlopen might misbehave after the runtime it wraps was unloaded.
void* open_lib(const char* path, int flags)
{
    if (env.orig_dlopen_ptr != nullptr)
    {
        using dlopen_t = void* (*)(const char*, int);
        return reinterpret_cast<dlopen_t>(env.orig_dlopen_ptr)(path, flags);
    }
    return dlopen(path, flags);
}

void close_lib(void* handle)
{
    if (handle == nullptr)
    {
        return;
    }
    if (env.orig_dlclose_ptr != nullptr)
    {
        using dlclose_t = int (*)(void*);
        reinterpret_cast<dlclose_t>(env.orig_dlclose_ptr)(handle);
    }
    else
    {
        dlclose(handle);
    }
}

bool init_dlopen_mode(void)
{
    if (!open_runtime(true))
    {
        return false;
    }
    capture_orig_dl(env.subsystem_handle);
    if (!load_scorep_api(env.subsystem_handle))
    {
        return false;
    }
    // Register the measurement exit handler explicitly: patched Score-P
    // builds for the reloadable variant do not register it during
    // initialization. Score-P guards against double finalization, so this is
    // also safe with stock builds that already registered the handler.
    scorep_api().register_exit_handler();
    return true;
}

void capture_orig_dl(void* subsystem_handle)
{
    if (subsystem_handle == nullptr)
    {
        return;
    }
    using getter_t = void* (*)(void);
    const auto lookup = [&](const char* name) {
        return reinterpret_cast<getter_t>(dlsym(subsystem_handle, name));
    };
    if (auto* get = lookup("scorep_py_get_orig_dlopen"))
    {
        env.orig_dlopen_ptr = get();
    }
    if (auto* get = lookup("scorep_py_get_orig_dlsym"))
    {
        env.orig_dlsym_ptr = get();
    }
    if (auto* get = lookup("scorep_py_get_orig_dlclose"))
    {
        env.orig_dlclose_ptr = get();
    }
}

/// One captured GOT entry: address of the GOT slot and its value.
struct GotEntry
{
    void** cell;
    void* value;
};

static std::vector<GotEntry> got_snapshot;

/// Cached /proc/self/maps snapshot for page protection lookups.
struct MapRange
{
    uintptr_t start;
    uintptr_t end;
    int prot;
};
static std::vector<MapRange> maps_cache;

static void refresh_maps_cache(void)
{
    maps_cache.clear();
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr)
    {
        return;
    }
    char line[4096];
    while (std::fgets(line, sizeof(line), maps) != nullptr)
    {
        uintptr_t start = 0;
        uintptr_t end = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
        {
            continue;
        }
        int prot = PROT_NONE;
        if (std::strchr(perms, 'r') != nullptr)
        {
            prot |= PROT_READ;
        }
        if (std::strchr(perms, 'w') != nullptr)
        {
            prot |= PROT_WRITE;
        }
        if (std::strchr(perms, 'x') != nullptr)
        {
            prot |= PROT_EXEC;
        }
        maps_cache.push_back(MapRange{start, end, prot});
    }
    std::fclose(maps);
}

/// Current protection of the page containing `address`, based on the cached
/// /proc/self/maps snapshot.
static int page_protection(uintptr_t address)
{
    for (const MapRange& range : maps_cache)
    {
        if (address >= range.start && address < range.end)
        {
            return range.prot;
        }
    }
    return PROT_NONE;
}

static bool write_got_cell(void** cell, void* value)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(cell);
    const uintptr_t page_start = address & ~uintptr_t(0xfff);
    const uintptr_t page_end = (address + sizeof(void*) + 0xfff) & ~uintptr_t(0xfff);
    const int prot = page_protection(page_start);
    if ((prot & PROT_WRITE) == 0 &&
        mprotect(reinterpret_cast<void*>(page_start), page_end - page_start,
                 prot | PROT_WRITE) != 0)
    {
        return false;
    }
    *cell = value;
    if ((prot & PROT_WRITE) == 0)
    {
        mprotect(reinterpret_cast<void*>(page_start), page_end - page_start, prot);
    }
    return true;
}

static int snapshot_callback(struct dl_phdr_info* info, size_t size, void* data)
{
    (void)size;
    (void)data;
    // glibc adjusts most DT_ pointer entries (e.g. DT_JMPREL, DT_RELA) to
    // absolute addresses in place, while others (e.g. DT_FINI_ARRAY) keep
    // their relative virtual address. Detect the convention per entry: if the
    // value already points into the object's mapping it is absolute.
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
        ElfW(Rela)* jmprel = nullptr;
        size_t pltrelsz = 0;
        ElfW(Rela)* rela = nullptr;
        size_t relasz = 0;
        for (ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++)
        {
            switch (d->d_tag)
            {
                case DT_JMPREL:
                    jmprel = static_cast<ElfW(Rela)*>(dyn_ptr(d->d_un.d_ptr));
                    break;
                case DT_PLTRELSZ:
                    pltrelsz = d->d_un.d_val;
                    break;
                case DT_RELA:
                    rela = static_cast<ElfW(Rela)*>(dyn_ptr(d->d_un.d_ptr));
                    break;
                case DT_RELASZ:
                    relasz = d->d_un.d_val;
                    break;
                default:
                    break;
            }
        }
        const auto collect = [&](ElfW(Rela)* relocations, size_t bytes) {
            if (relocations == nullptr)
            {
                return;
            }
            for (size_t off = 0; off + sizeof(ElfW(Rela)) <= bytes; off += sizeof(ElfW(Rela)))
            {
                // The relocation table may cross into a PROT_NONE alignment
                // hole; stop there.
                if ((page_protection(reinterpret_cast<uintptr_t>(&relocations[off])) & PROT_READ)
                    == 0)
                {
                    break;
                }
                const auto type = ELF64_R_TYPE(relocations[off].r_info);
                if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT)
                {
                    continue;
                }
                auto* cell = reinterpret_cast<void**>(info->dlpi_addr + relocations[off].r_offset);
                // Skip cells on PROT_NONE pages (segment alignment holes).
                if ((page_protection(reinterpret_cast<uintptr_t>(cell)) & PROT_READ) == 0)
                {
                    continue;
                }
                got_snapshot.push_back(GotEntry{cell, *cell});
            }
        };
        collect(jmprel, pltrelsz);
        collect(rela, relasz);
    }
    return 0;
}

/** dlopen the Score-P runtime in the correct order.
 *
 * The subsystem must be loaded first: glibc relocates it together with its
 * DT_NEEDED closure (libscorep_measurement, the adapters, ...) as one set,
 * which resolves their mutual data-symbol references (e.g.
 * `scorep_subsystems`). The interposition libraries are opened afterwards
 * with RTLD_LAZY, as they may reference data symbols of the already loaded
 * set (they usually resolve to already loaded objects). Finally the
 * constructor library is loaded, whose initialization routine runs the
 * measurement initialization.
 *
 * When `take_got_snapshot` is set, the GOT entries of all loaded objects are
 * captured after the Score-P runtime libraries are mapped but before the
 * measurement initialization installs the gotcha interposition. */
bool open_runtime(bool take_got_snapshot)
{
    const char* interposition_libs = env_value("SCOREP_PYTHON_BINDINGS_INTERPOSITION_LIBS");
    const char* subsystem_lib = env_value("SCOREP_PYTHON_BINDINGS_SUBSYSTEM_LIB");
    const char* constructor_lib = env_value("SCOREP_PYTHON_BINDINGS_CONSTRUCTOR_LIB");
    if (subsystem_lib == nullptr || constructor_lib == nullptr)
    {
        return false;
    }

    env.subsystem_handle = open_lib(subsystem_lib, RTLD_NOW | RTLD_GLOBAL);
    if (env.subsystem_handle == nullptr)
    {
        return false;
    }
    if (interposition_libs != nullptr)
    {
        std::string libs(interposition_libs);
        size_t pos = 0;
        while (pos < libs.size())
        {
            size_t end = libs.find(' ', pos);
            if (end == std::string::npos)
            {
                end = libs.size();
            }
            if (end > pos)
            {
                std::string path = libs.substr(pos, end - pos);
                void* handle = open_lib(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
                if (handle == nullptr)
                {
                    return false;
                }
                env.interposition_handles.push_back(handle);
            }
            pos = end + 1;
        }
    }
    if (take_got_snapshot)
    {
        got_snapshot.clear();
        dl_iterate_phdr(snapshot_callback, nullptr);
    }
    // Loading the constructor library runs its initialization routine, which
    // triggers the measurement initialization.
    env.constructor_handle = open_lib(constructor_lib, RTLD_NOW | RTLD_GLOBAL);
    if (env.constructor_handle == nullptr)
    {
        return false;
    }
    env.relocatable = true;
    return true;
}

} // namespace

bool init_binding_measurement(void)
{
    if (env.initialized)
    {
        return true;
    }

    const char* dlopen_mode = env_value("SCOREP_PYTHON_BINDINGS_DLOPEN");
    bool loaded = false;
    if (dlopen_mode != nullptr)
    {
        loaded = init_dlopen_mode();
    }
    else
    {
        loaded = load_scorep_api(nullptr);
        if (loaded)
        {
            // See init_dlopen_mode: register the exit handler explicitly.
            scorep_api().register_exit_handler();
        }
    }

    env.initialized = loaded;
    return loaded;
}

bool measurement_relocatable(void)
{
    return env.relocatable;
}

bool unload_measurement_env(void)
{
    if (!env.relocatable)
    {
        return false;
    }
    close_lib(env.constructor_handle);
    env.constructor_handle = nullptr;
    close_lib(env.subsystem_handle);
    env.subsystem_handle = nullptr;
    for (auto it = env.interposition_handles.rbegin(); it != env.interposition_handles.rend();
         ++it)
    {
        close_lib(*it);
    }
    env.interposition_handles.clear();
    env.relocatable = false;
    return true;
}

bool reload_measurement_env(void)
{
    if (env.initialized && env.relocatable)
    {
        return false; // still loaded, nothing to reload
    }
    if (!open_runtime(true))
    {
        return false;
    }
    capture_orig_dl(env.subsystem_handle);
    if (!load_scorep_api(env.subsystem_handle))
    {
        return false;
    }
    // See init_dlopen_mode: the fresh environment needs its own exit handler.
    scorep_api().register_exit_handler();
    return true;
}

bool snapshot_gots(void)
{
    refresh_maps_cache();
    got_snapshot.clear();
    dl_iterate_phdr(snapshot_callback, nullptr);
    return !got_snapshot.empty();
}

bool restore_gots_from_snapshot(void)
{
    refresh_maps_cache();
    bool success = true;
    for (const GotEntry& entry : got_snapshot)
    {
        if (*entry.cell != entry.value && !write_got_cell(entry.cell, entry.value))
        {
            success = false;
        }
    }
    return success;
}

void* subsystem_lib_handle(void)
{
    return env.subsystem_handle;
}

void* constructor_lib_handle(void)
{
    return env.constructor_handle;
}

void* orig_dlopen(void)
{
    return env.orig_dlopen_ptr;
}

void* orig_dlsym(void)
{
    return env.orig_dlsym_ptr;
}

void* orig_dlclose(void)
{
    return env.orig_dlclose_ptr;
}

} // namespace scorepy
