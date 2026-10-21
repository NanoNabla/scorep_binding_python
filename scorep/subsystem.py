import os
import tempfile
import shutil

import scorep.helper
from scorep.helper import print_err


def _print_info(msg):
    """Print an info message with a prefix"""
    print_err("scorep: " + msg)


def generate_subsystem_lib_name():
    """
    generate the name for the subsystem lib.
    """
    mpi_lib_name = "libscorep_init_subsystem-{}.so".format(
        scorep.helper.get_python_version())
    return mpi_lib_name


def generate_ld_preload(scorep_config):
    """
    This functions generates a string that needs to be passed to $LD_PRELOAD.
    After this string is passed, the tracing needs to be restarted with this $LD_PRELOAD in env.

    @return ld_preload string which needs to be passed to LD_PRELOAD
    """

    (_, preload, _) = scorep.helper.call(["scorep-config"] + scorep_config + ["--preload-libs"])
    return preload.strip()


def generate_subsystem_code(config):
    """
    Generates the data needed to be preloaded.
    """

    scorep_config = ["scorep-config"] + config

    (return_code, _, _) = scorep.helper.call(scorep_config)
    if return_code != 0:
        raise ValueError(
            "given config {} is not supported".format(scorep_config))
    (_, scorep_adapter_init, _) = scorep.helper.call(
        scorep_config + ["--adapter-init"])

    return scorep_adapter_init


_orig_dl_capture_code = """\
/* scorep_binding_python: capture the original (non-interposed) dl* functions
 * before Score-P initializes its libwrap/gotcha interposition. The captured
 * pointers are used to reload the measurement environment in forked children. */
#define __USE_GNU
#include <dlfcn.h>
static void* scorep_py_orig_dlopen_ = NULL;
static void* scorep_py_orig_dlsym_ = NULL;
static void* scorep_py_orig_dlclose_ = NULL;

__attribute__((constructor(10))) static void scorep_py_capture_orig_dl(void)
{
    if (scorep_py_orig_dlopen_ == NULL)
        scorep_py_orig_dlopen_ = dlsym(RTLD_NEXT, "dlopen");
    if (scorep_py_orig_dlsym_ == NULL)
        scorep_py_orig_dlsym_ = dlsym(RTLD_NEXT, "dlsym");
    if (scorep_py_orig_dlclose_ == NULL)
        scorep_py_orig_dlclose_ = dlsym(RTLD_NEXT, "dlclose");
}

void* scorep_py_get_orig_dlopen(void) { return scorep_py_orig_dlopen_; }
void* scorep_py_get_orig_dlsym(void) { return scorep_py_orig_dlsym_; }
void* scorep_py_get_orig_dlclose(void) { return scorep_py_orig_dlclose_; }
"""

_exit_hook_code = """\
/* scorep_binding_python: finalize the measurement when the process exits via
 * _exit (e.g. os._exit in multiprocessing workers), which bypasses atexit. */
#define __USE_GNU
#include <dlfcn.h>
#include <signal.h>
#include <string.h>

static void (*scorep_py_finalize_)(void) = NULL;
static int scorep_py_finalized_ = 0;
static void (*scorep_py_real__exit_)(int) = NULL;

void scorep_py_set_finalize(void (*finalize)(void))
{
    scorep_py_finalize_ = finalize;
    scorep_py_finalized_ = 0;
}

__attribute__((constructor(5))) static void scorep_py_exit_hook_init(void)
{
    scorep_py_real__exit_ = (void (*)(int))dlsym(RTLD_NEXT, "_exit");
}

void _exit(int status) __attribute__((noreturn));
void _exit(int status)
{
    if (scorep_py_finalize_ != NULL && scorep_py_finalized_ == 0)
    {
        scorep_py_finalized_ = 1;
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_IGN;
        sigaction(SIGTERM, &action, NULL);
        scorep_py_finalize_();
    }
    if (scorep_py_real__exit_ == NULL)
        scorep_py_real__exit_ = (void (*)(int))dlsym(RTLD_NEXT, "_exit");
    if (scorep_py_real__exit_ != NULL)
        scorep_py_real__exit_(status);
    for (;;)
    {
    }
}
"""


def generate_exit_hook_lib(cc, temp_dir):
    """Compile the _exit-interposing helper library used for fork reinit.

    The library must be preloaded, as an interposer inside a dlopened library
    would not take precedence over libc for calls from the main executable.
    """
    hook_source = temp_dir + "/scorep_py_exit_hook.c"
    with open(hook_source, "w") as f:
        f.write(_exit_hook_code)
    hook_object = cc.compile([hook_source], output_dir=temp_dir)
    hook_lib_name = "libscorep_py_exit_hook.so"
    hook_lib = temp_dir + "/" + hook_lib_name
    cc.link(
        "scorep_py_exit_hook",
        objects=hook_object,
        output_filename=hook_lib_name,
        output_dir=temp_dir,
    )
    return hook_lib


def generate(scorep_config, keep_files=False, fork_reinit=False):
    """
    Uses the scorep_config to compile the scorep subsystem.
    Returns the name of the compiled subsystem, and the path of the temp folder, where the lib is located

    @param scorep_config scorep configuration to build subsystem
    @param keep_files whether to keep the generated files, or not.
    @param fork_reinit build the subsystem for the fork-reinit feature: embed
        the original dl* capture code and build the _exit-hook library.
    """

    (include, lib, lib_dir, macro,
     linker_flags_tmp) = scorep.helper.generate_compile_deps(scorep_config)
    scorep_adapter_init = generate_subsystem_code(scorep_config)
    # scorep_adapter_opari2_mgmt: Score-P v8.4 and lower
    # scorep_adapter_opari2_{openmp,user}_mgmt: Score-P v9.0 and newer
    if any(libname in lib for libname in [
        "-lscorep_adapter_opari2_mgmt",
        "-lscorep_adapter_opari2_openmp_mgmt",
        "-lscorep_adapter_opari2_user_mgmt"
    ]):
        scorep_adapter_init += "\n"
        scorep_adapter_init += "/* OPARI dependencies */\n"
        scorep_adapter_init += "void POMP2_Init_regions(){}\n"
        scorep_adapter_init += "size_t POMP2_Get_num_regions(){return 0;};\n"
        scorep_adapter_init += "void POMP2_USER_Init_regions(){};\n"
        scorep_adapter_init += "size_t POMP2_USER_Get_num_regions(){return 0;};\n"

    # add -Wl,-no-as-needed to tell the compiler that we really want to link these. Actually this should be default.
    # as distutils adds extra args at the very end we need to add all the libs
    # after this and skip the libs later in the extension module
    linker_flags = ["-Wl,-no-as-needed"]
    linker_flags.extend(lib)
    linker_flags.extend(linker_flags_tmp)

    temp_dir = tempfile.mkdtemp(prefix="scorep.")
    if keep_files:
        _print_info("Score-P files are kept at: " + temp_dir)

    with open(temp_dir + "/scorep_init.c", "w") as f:
        f.write(scorep_adapter_init)
        if fork_reinit:
            f.write("\n" + _orig_dl_capture_code)

    subsystem_lib_name = generate_subsystem_lib_name()

    # setuptools, which replaces distutils, calls uname in python < 3.9 during distutils bootstraping.
    # When LD_PRELOAD is set, this leads to preloading Score-P to uname, and crashes the later tracing.
    # To avoid this, we need to do the distutils bootstrap as late as possible.
    # Setuptools does not support ccompiler.new_compiler https://github.com/pypa/setuptools/issues/4540
    from setuptools._distutils.ccompiler import new_compiler
    cc = new_compiler()

    compiled_subsystem = cc.compile(
        [temp_dir + "/scorep_init.c"], output_dir=temp_dir)
    cc.link(
        "scorep_init_mpi",
        objects=compiled_subsystem,
        output_filename=subsystem_lib_name,
        output_dir=temp_dir,
        library_dirs=lib_dir,
        extra_postargs=linker_flags)

    os.environ["SCOREP_PYTHON_BINDINGS_TEMP_DIR"] = temp_dir

    # Export the library locations for the C-side loader (dlopen-mode) and the
    # fork-reinit support.
    preload_libs = generate_ld_preload(scorep_config).split()
    interposition_libs = [
        path for path in preload_libs
        if os.path.basename(path) != "libscorep_constructor.so"
    ]
    constructor_libs = [
        path for path in preload_libs
        if os.path.basename(path) == "libscorep_constructor.so"
    ]
    os.environ["SCOREP_PYTHON_BINDINGS_SUBSYSTEM_LIB"] = temp_dir + "/" + subsystem_lib_name
    if interposition_libs:
        os.environ["SCOREP_PYTHON_BINDINGS_INTERPOSITION_LIBS"] = " ".join(interposition_libs)
    if constructor_libs:
        os.environ["SCOREP_PYTHON_BINDINGS_CONSTRUCTOR_LIB"] = constructor_libs[0]

    if fork_reinit:
        os.environ["SCOREP_PYTHON_BINDINGS_EXIT_HOOK_LIB"] = generate_exit_hook_lib(cc, temp_dir)

    return subsystem_lib_name, temp_dir


def init_environment(scorep_config, keep_files=False, verbose=False, fork_reinit=False):
    """
    Set the inital needed environment variables, to get everything up an running.
    As a few variables interact with LD env vars, the program needs to be restarted after this.

    @param scorep_config configuration flags for score-p
    @param keep_files whether to keep the generated files, or not.
    @param verbose Set to True to output information about config used and environment variables set.
    @param fork_reinit Enable the fork-reinit mode: instead of preloading the
        Score-P runtime, only the tiny _exit-hook library is preloaded and the
        Score-P runtime is loaded via dlopen by the binding. This makes the
        runtime unloadable/reloadable, which the fork-reinit support requires.
    """

    if "libscorep" in os.environ.get("LD_PRELOAD", ""):
        raise RuntimeError("Score-P is already loaded. This should not happen at this point")

    if "--user" not in scorep_config:
        scorep_config.append("--user")

    if verbose:
        _print_info("Score-P config: %s" % scorep_config)

    old_env = os.environ.copy()

    subsystem_lib_name, temp_dir = generate(scorep_config, keep_files, fork_reinit)
    scorep_ld_preload = generate_ld_preload(scorep_config)

    if not os.access(temp_dir + "/" + subsystem_lib_name, os.X_OK):
        clean_up(keep_files=keep_files)
        raise RuntimeError(
            "The Score-P Subsystem Library at {} cannot be executed. Changing $TMP might help. "
            "Directory erased, use --keep-files to inspect the situation.".format(
                temp_dir + "/" + subsystem_lib_name))

    scorep.helper.add_to_ld_library_path(temp_dir)

    if fork_reinit:
        hook_lib = os.environ.get("SCOREP_PYTHON_BINDINGS_EXIT_HOOK_LIB")
        if not hook_lib or not os.access(hook_lib, os.X_OK):
            clean_up(keep_files=keep_files)
            raise RuntimeError("Cannot build the Score-P _exit-hook library.")
        os.environ["SCOREP_PYTHON_BINDINGS_DLOPEN"] = "1"
        preload_str = hook_lib
    else:
        preload_str = scorep_ld_preload + " " + subsystem_lib_name
    if os.environ.get("LD_PRELOAD"):
        print_err("LD_PRELOAD is already specified. If Score-P is already loaded this might lead to errors.")
        preload_str = os.environ["LD_PRELOAD"] + " " + preload_str
        os.environ["SCOREP_LD_PRELOAD_BACKUP"] = os.environ["LD_PRELOAD"]
    else:
        os.environ["SCOREP_LD_PRELOAD_BACKUP"] = ""
    os.environ["LD_PRELOAD"] = preload_str

    if verbose:
        for var in ("LD_LIBRARY_PATH", "LD_PRELOAD"):
            # Shorten the setting to e.g.: FOO=new:$FOO
            old_val = old_env.get(var)
            new_val = os.environ[var]
            if old_val:
                new_val = new_val.replace(old_val, '$' + var)
            _print_info('%s="%s"' % (var, new_val))


def reset_preload():
    """
    resets the environment variable `LD_PRELOAD` to the value before init_environment was called.
    """
    if "SCOREP_LD_PRELOAD_BACKUP" in os.environ:
        if os.environ["SCOREP_LD_PRELOAD_BACKUP"] == "":
            del os.environ["LD_PRELOAD"]
        else:
            os.environ["LD_PRELOAD"] = os.environ["SCOREP_LD_PRELOAD_BACKUP"]


def clean_up(keep_files=True):
    """
    deletes the files that are associated to subsystem

    @param keep_files do not delete the generated files. For debugging.
    """
    if keep_files:
        return
    temp_dir = os.environ.get("SCOREP_PYTHON_BINDINGS_TEMP_DIR")
    if not temp_dir:
        return
    # Forked children run this as well, so the directory might already be
    # gone; drop the variable to make repeated calls no-ops.
    del os.environ["SCOREP_PYTHON_BINDINGS_TEMP_DIR"]
    shutil.rmtree(temp_dir, ignore_errors=True)
