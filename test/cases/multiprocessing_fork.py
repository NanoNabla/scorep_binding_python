import multiprocessing
import os

import scorep._bindings
import scorep.user


def worker(queue):
    scorep.user.region_begin("mp_child_region")
    scorep.user.region_end("mp_child_region")
    queue.put(scorep._bindings.get_experiment_dir_name())
    # Bypasses atexit: the preloaded _exit hook must finalize the measurement.
    os._exit(0)


if __name__ == "__main__":
    ctx = multiprocessing.get_context("fork")
    queue = ctx.SimpleQueue()
    process = ctx.Process(target=worker, args=(queue,))
    process.start()
    process.join()
    print("CHILD_EXITCODE " + str(process.exitcode))
    print("CHILD_DIR " + queue.get())
