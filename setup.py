import os
import sys
from setuptools import setup, Extension

from setuptools.command.build_ext import build_ext
import platform
import sysconfig

import scorep.helper
from scorep.instrumenter import has_c_instrumenter

if scorep.helper.get_scorep_version() < 5.0:
    raise RuntimeError("Score-P version less than 5.0, plase use Score-P >= 5.0")

link_mode = scorep.helper.get_scorep_config("Link mode:")
if not ("shared=yes" in link_mode):
    raise RuntimeError(
        'Score-P not build with "--enable-shared". Link mode is:\n{}'.format(link_mode)
    )

install_requires= ["setuptools>=68.0.0"]

class DebugBuildExt(build_ext):
    def build_extension(self, ext):
        print("compiler_so:", self.compiler.compiler_so)
        print("linker_so:", self.compiler.linker_so)
        return super().build_extension(ext)
class FixLinkerBuildExt(_build_ext):
    """
    Ensure linker_so and linker_exe are properly initialized for PyPy
    variants (fixes PyPy 3.8/3.9 where setuptools/distutils leave them None).
    """
    def build_extension(self, ext):
        compiler = self.compiler

        # Helper to make a list form of an executable
        def _to_list(x):
            if x is None:
                return None
            if isinstance(x, (list, tuple)):
                return list(x)
            # split string into tokens if it contains whitespace; otherwise single-element list
            return x.split()

        # 1) Try obvious source: compiler.compiler_cxx
        cxx = getattr(compiler, "compiler_cxx", None)
        if cxx is None:
            # 2) Look at environment or sysconfig or PATH
            cxx_env = os.environ.get("CXX")
            cxx_conf = sysconfig.get_config_var("CXX")
            which_gpp = shutil.which("g++") or shutil.which("clang++")
            chosen = cxx_env or cxx_conf or which_gpp
            if chosen:
                compiler.compiler_cxx = _to_list(chosen)

        # Normalize compiler.compiler_cxx to a list (if present)
        if getattr(compiler, "compiler_cxx", None) is not None:
            compiler.compiler_cxx = _to_list(compiler.compiler_cxx)

        # 3) Ensure linker_so exists and uses CXX if appropriate
        if getattr(compiler, "linker_so", None) is None:
            # prefer compiler.compiler_cxx (g++) + '-shared'
            if getattr(compiler, "compiler_cxx", None):
                compiler.linker_so = compiler.compiler_cxx + ["-shared"]
            else:
                # fallback to LDSHARED env / sysconfig / g++
                ldshared = os.environ.get("LDSHARED") or sysconfig.get_config_var("LDSHARED")
                if ldshared:
                    compiler.linker_so = _to_list(ldshared)
                else:
                    which = shutil.which("g++") or shutil.which("gcc")
                    if which:
                        compiler.linker_so = [which, "-shared"]

        # 4) Ensure linker_exe is set (some distutils code indexes this)
        if getattr(compiler, "linker_exe", None) is None:
            # prefer linker_so if it is list-like
            if getattr(compiler, "linker_so", None):
                compiler.linker_exe = list(compiler.linker_so)
            elif getattr(compiler, "compiler_cxx", None):
                compiler.linker_exe = list(compiler.compiler_cxx)
            else:
                # fallback to g++
                which = shutil.which("g++") or shutil.which("gcc")
                compiler.linker_exe = [which] if which else None

        # 5) For safety, also ensure compiler_so uses CXX when building C++ ext
        if getattr(compiler, "compiler_so", None) is not None and getattr(compiler, "compiler_cxx", None):
            # When compiling C++ objects, some flows expect compiler_so to be compiler_cxx
            # But do not overwrite if it obviously contains g++
            cs = compiler.compiler_so
            if isinstance(cs, (list, tuple)) and "g++" not in " ".join(map(str, cs)):
                # only set if current compiler_so seems plain 'gcc' and we have a C++ compiler
                compiler.compiler_so = list(compiler.compiler_cxx)

        # Debug prints (remove before upstreaming if noisy)
        sys.stdout.write("DEBUG: compiler.compiler_cxx=%r\n" % getattr(compiler, "compiler_cxx", None))
        sys.stdout.write("DEBUG: compiler.compiler_so=%r\n" % getattr(compiler, "compiler_so", None))
        sys.stdout.write("DEBUG: compiler.linker_so=%r\n" % getattr(compiler, "linker_so", None))
        sys.stdout.write("DEBUG: compiler.linker_exe=%r\n" % getattr(compiler, "linker_exe", None))
        sys.stdout.flush()

        return super().build_extension(ext)    

# if platform.python_implementation() == "PyPy":
#     cc = sysconfig.get_config_var("CXX") or "g++"
#     # Override linker to use C++
#     os.environ.setdefault("LDSHARED", f"{cc} -shared")

cmodules = []
(include, _, _, _, _) = scorep.helper.generate_compile_deps([])
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
    name="scorep",
    version=scorep._version.__version__,
    description="This is a Score-P tracing package for python",
    author="Andreas Gocht",
    author_email="andreas.gocht@tu-dresden.de",
    url="https://github.com/score-p/scorep_binding_python",
    long_description="""
This package allows tracing of python code using Score-P.
A working Score-P version is required.
To enable tracing it uses LD_PRELOAD to load the Score-P runtime libraries.
Besides this, it uses the traditional python-tracing infrastructure.
""",
    packages=["scorep", "scorep._instrumenters"],
    install_requires=install_requires,
    ext_modules=cmodules,
    cmdclass={"build_ext": FixLinkerBuildExt},
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Environment :: Console",
        "Intended Audience :: Developers",
        "Topic :: Software Development :: Testing",
        "Topic :: Software Development :: Quality Assurance",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: Python :: 3.14",
        "Programming Language :: Python :: Implementation :: CPython",
        "Programming Language :: Python :: Implementation :: PyPy",
        "Operating System :: POSIX",
        "Operating System :: Unix",
    ],
)
