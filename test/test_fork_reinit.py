#!/usr/bin/env pytest

import os

import pytest

import utils
from utils import OTF2_Trace, OTF2_Region

# Note: the fork-reinit feature works on stock Score-P builds: the binding
# restores the pre-interposition GOT state itself (see src/scorepy/loader.cpp).
# The shim under test/shim/ is only needed to exercise the optional
# Score-P-provided restore path (SCOREP_PYTHON_BINDINGS_RESTORE_LIB).


@pytest.fixture
def fork_reinit_env(tmp_path):
    env = os.environ.copy()
    env["SCOREP_ENABLE_PROFILING"] = "false"
    env["SCOREP_ENABLE_TRACING"] = "true"
    env["SCOREP_TOTAL_MEMORY"] = "3G"
    env["SCOREP_EXPERIMENT_DIRECTORY"] = str(tmp_path / "fork_dir")
    env["SCOREP_PYTHON_BINDINGS_FORK_REINIT"] = "on"
    return env


def test_fork_reinit_separate_experiment_dirs(fork_reinit_env):
    std_out, std_err = utils.call_with_scorep(
        "cases/fork.py", ["--nopython", "--keep-files"], env=fork_reinit_env)

    assert "WARNING" not in std_err
    parent_dir = None
    child_dir = None
    for line in std_out.splitlines():
        if line.startswith("PARENT_DIR "):
            parent_dir = line[len("PARENT_DIR "):]
        elif line.startswith("CHILD_DIR "):
            child_dir = line[len("CHILD_DIR "):]
    assert parent_dir is not None
    assert child_dir is not None
    assert parent_dir != child_dir
    assert os.path.isdir(child_dir)

    parent_trace = OTF2_Trace(parent_dir + "/traces.otf2")
    child_trace = OTF2_Trace(child_dir + "/traces.otf2")
    assert OTF2_Region("user:parent_before_fork") in parent_trace
    assert OTF2_Region("user:parent_after_fork") in parent_trace
    assert OTF2_Region("user:child_region") not in parent_trace
    assert OTF2_Region("user:child_region") in child_trace
    assert OTF2_Region("user:parent_before_fork") not in child_trace


def test_multiprocessing_fork_exit_hook(fork_reinit_env):
    std_out, std_err = utils.call_with_scorep(
        "cases/multiprocessing_fork.py", ["--nopython", "--keep-files"], env=fork_reinit_env)

    assert "WARNING" not in std_err
    assert "CHILD_EXITCODE 0" in std_out
    child_dir = None
    for line in std_out.splitlines():
        if line.startswith("CHILD_DIR "):
            child_dir = line[len("CHILD_DIR "):]
    assert child_dir is not None
    assert os.path.isdir(child_dir)

    child_trace = OTF2_Trace(child_dir + "/traces.otf2")
    assert OTF2_Region("user:mp_child_region") in child_trace


def test_fork_reinit_refused_with_preloaded_scorep(tmp_path):
    # An externally preloaded Score-P runtime cannot be unloaded, so the
    # fork-reinit mode must refuse to start. Simulating a fully working
    # external preload would require building the subsystem first, so we
    # trigger the guard with a LD_PRELOAD entry named like a Score-P library.
    env = os.environ.copy()
    env["SCOREP_ENABLE_PROFILING"] = "false"
    env["SCOREP_ENABLE_TRACING"] = "true"
    env["SCOREP_TOTAL_MEMORY"] = "3G"
    env["SCOREP_EXPERIMENT_DIRECTORY"] = str(tmp_path / "fork_dir")
    env["SCOREP_PYTHON_BINDINGS_FORK_REINIT"] = "on"
    env["LD_PRELOAD"] = "/nonexistent/libscorep_external_preload.so"

    with pytest.raises(AssertionError) as exc_info:
        utils.call_with_scorep("cases/fork.py", ["--nopython"], env=env)
    assert "Score-P is already loaded" in str(exc_info.value)


def test_fork_reinit_opt_in_disabled_by_default(tmp_path):
    env = os.environ.copy()
    env["SCOREP_ENABLE_PROFILING"] = "false"
    env["SCOREP_ENABLE_TRACING"] = "true"
    env["SCOREP_TOTAL_MEMORY"] = "3G"
    env["SCOREP_EXPERIMENT_DIRECTORY"] = str(tmp_path / "fork_dir")
    assert "SCOREP_PYTHON_BINDINGS_FORK_REINIT" not in env

    std_out, std_err = utils.call_with_scorep("cases/fork.py", ["--nopython"], env=env)

    # Without the opt-in, the child inherits the measurement environment.
    assert "WARNING" not in std_err
    assert "CHILD_DIR " + env["SCOREP_EXPERIMENT_DIRECTORY"] in std_out
