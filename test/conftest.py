import os
import subprocess

import utils

SHIM_SOURCE = os.path.join(os.path.dirname(__file__), "shim", "scorep_libwrap_restore_shim.c")
SHIM_LIBRARY = os.path.join(os.path.dirname(__file__), "shim", "libscorep_libwrap_restore_shim.so")


def pytest_sessionstart(session):
    """Build the fork-reinit test shim, if a compiler is available."""
    if os.path.isfile(SHIM_LIBRARY):
        return
    try:
        subprocess.run(
            ["cc", "-O2", "-shared", "-fPIC", "-o", SHIM_LIBRARY, SHIM_SOURCE, "-ldl"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except (OSError, subprocess.CalledProcessError):
        pass  # the shim is only needed for the optional Score-P restore path


def pytest_assertrepr_compare(op, left, right):
    if isinstance(left, utils.OTF2_Region) and isinstance(right, utils.OTF2_Trace):
        if op == "in":
            output = ["Region \"{}\" ENTER or LEAVE not found in trace:".format(left)]
            for line in str(right).split("\n"):
                output.append("\t" + line)
            return output
        elif op == "not in":
            output = ["Unexpected region \"{}\" ENTER or LEAVE found in trace:".format(left)]
            for line in str(right).split("\n"):
                output.append("\t" + line)
            return output
    elif isinstance(left, utils.OTF2_Parameter) and isinstance(right, utils.OTF2_Trace) and op == "in":
        output = ["Parameter \"{parameter}\" with Value \"{value}\" not found in trace:".format(
            parameter=left.parameter,
            value=left.value)]
        for line in str(right).split("\n"):
            output.append("\t" + line)
        return output
