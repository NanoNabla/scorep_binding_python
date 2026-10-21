import os
import sys

import scorep._bindings
import scorep.user

scorep.user.region_begin("parent_before_fork")
scorep.user.region_end("parent_before_fork")

pid = os.fork()
if pid == 0:
    # The fork-reinit support re-initializes the measurement environment in
    # the child, which measures into its own experiment directory.
    scorep.user.region_begin("child_region")
    scorep.user.region_end("child_region")
    print("CHILD_DIR " + scorep._bindings.get_experiment_dir_name())
    sys.exit(0)

scorep.user.region_begin("parent_after_fork")
scorep.user.region_end("parent_after_fork")
os.waitpid(pid, 0)
print("PARENT_DIR " + scorep._bindings.get_experiment_dir_name())
