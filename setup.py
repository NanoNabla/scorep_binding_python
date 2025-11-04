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
class FixLinkerBuildExt(build_ext):
    def build_extension(self, ext):
        compiler = self.compiler

        # If linker is missing, set it explicitly
        if getattr(compiler, "linker_so", None) is None:
            compiler.linker_so = compiler.compiler_cxx  # usually g++
        if getattr(compiler, "linker_exe", None) is None:
            compiler.linker_exe = compiler.compiler_cxx  # same fallback

        super().build_extension(ext)        

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
