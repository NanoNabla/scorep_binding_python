import os
from setuptools import setup, Extension
# from scorep.instrumenter import has_c_instrumenter



import sys
import subprocess
import re
import platform

def call(arguments):
    """
    return a triple with (returncode, stdout, stderr) from the call to subprocess
    """
    if sys.version_info > (3, 5):
        out = subprocess.run(
            arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        result = (
            out.returncode,
            out.stdout.decode("utf-8"),
            out.stderr.decode("utf-8"))
    else:
        p = subprocess.Popen(
            arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        stdout, stderr = p.communicate()
        p.wait()
        result = (p.returncode, stdout.decode("utf-8"), stderr.decode("utf-8"))
    return result



def has_c_instrumenter():
    """Return true if the C instrumenter(s) are available"""
    # We are using the UTF-8 string features from Python 3
    # The C Instrumenter functions are not available on PyPy
    return platform.python_implementation() != 'PyPy'

def get_scorep_config(config_line=None):
    (return_code, std_out, std_err) = call(["scorep-info", "config-summary"])
    if (return_code != 0):
        raise RuntimeError("Cannot call Score-P, reason {}".format(std_err))
    if config_line is None:
        return std_out.split("\n")
    else:
        for line in std_out.split("\n"):
            if config_line in line:
                return line
    return None


def generate_compile_deps(config):
    """
    Generates the data needed for compilation.
    """

    scorep_config = ["scorep-config"] + config

    (return_code, stdout, stderr) = call(scorep_config)
    if return_code != 0:
        raise ValueError(
            "given config {} is not supported\nstdout: {}\nstrerr: {}".format(config, stdout, stderr))

    (_, ldflags, _) = call(scorep_config + ["--ldflags"])
    (_, libs, _) = call(scorep_config + ["--libs"])
    (_, mgmt_libs, _) = call(scorep_config + ["--mgmt-libs"])
    (_, cflags, _) = call(scorep_config + ["--cflags"])

    libs = " " + libs + " " + mgmt_libs
    ldflags = " " + ldflags
    cflags = " " + cflags

    lib_dir = re.findall(r" -L[/+-@.\w]*", ldflags)
    lib = re.findall(r" -l[/+-@.\w]*", libs)
    include = re.findall(r" -I[/+-@.\w]*", cflags)
    macro = re.findall(r" -D[/+-@.\w]*", cflags)
    linker_flags = re.findall(r" -Wl[/+-@.\w]*", ldflags)

    def remove_flag3(x): return x[3:]

    def remove_space1(x): return x[1:]

    lib_dir = list(map(remove_flag3, lib_dir))
    lib = list(map(remove_space1, lib))
    include = list(map(remove_flag3, include))
    macro = list(map(remove_flag3, macro))
    linker_flags = list(map(remove_space1, linker_flags))

    macro = list(map(lambda x: tuple([x, 1]), macro))

    return (include, lib, lib_dir, macro, linker_flags)

def get_scorep_version():
    (return_code, std_out, std_err) = call(["scorep", "--version"])
    if (return_code != 0):
        raise RuntimeError("Cannot call Score-P, reason {}".format(std_err))
    me = re.search("([0-9.]+)", std_out)
    version_str = me.group(1)
    try:
        version = float(version_str)
    except TypeError:
        raise RuntimeError(
            "Can not decode the Score-P Version. The version string is: \"{}\"".format(std_out))
    return version








if get_scorep_version() < 5.0:
    raise RuntimeError("Score-P version less than 5.0, plase use Score-P >= 5.0")

link_mode = get_scorep_config("Link mode:")
if not ("shared=yes" in link_mode):
    raise RuntimeError(
        'Score-P not build with "--enable-shared". Link mode is:\n{}'.format(link_mode)
    )

check_compiler = get_scorep_config("C99 compiler used:")
if check_compiler is None:
    check_compiler = get_scorep_config("C99 compiler:")
if check_compiler is None:
    raise RuntimeError("Can not parse the C99 compiler, aborting!")
if "gcc" in check_compiler:
    gcc_plugin = get_scorep_config("GCC plug-in support:")
    if not ("yes" in gcc_plugin):
        raise RuntimeError(
            "Score-P uses GCC but is not build with GCC Compiler Plugin. "
            "GCC plug-in support is:\n{}".format(gcc_plugin)
        )


cmodules = []
(include, _, _, _, _) = generate_compile_deps([])
src_folder = os.path.abspath("src")
include += [src_folder]
sources = [
    "src/methods.cpp",
    "src/scorep_bindings.cpp",
    "src/scorepy/events.cpp",
    "src/scorepy/pathUtils.cpp",
]
define_macros = [("PY_SSIZE_T_CLEAN", "1")]
# We are using the UTF-8 string features from Python 3
# The C Instrumenter functions are not available on PyPy
if has_c_instrumenter():
    sources.extend(
        [
            "src/classes.cpp",
            "src/scorepy/cInstrumenter.cpp",
            "src/scorepy/pythonHelpers.cpp",
        ]
    )
    define_macros.append(("SCOREPY_ENABLE_CINSTRUMENTER", "1"))
else:
    define_macros.append(("SCOREPY_ENABLE_CINSTRUMENTER", "0"))

cmodules.append(
    Extension(
        "scorep._bindings",
        include_dirs=include,
        define_macros=define_macros,
        extra_compile_args=["-std=c++17"],
        sources=sources,
    )
)

setup(
    packages=["scorep", "scorep._instrumenters"],
)
