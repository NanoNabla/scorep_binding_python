#pragma once

namespace scorepy
{

/** Register the atfork handler that re-initializes the Score-P measurement
 * environment in forked children (fork-reinit).
 *
 * Requires the dlopen-mode runtime (see loader.hpp). The interposition of the
 * old environment is undone via a pre-interposition GOT snapshot taken by the
 * binding; if a Score-P build additionally provides
 * `libwrap_subsystem_restore_original_bindings` (or a test shim passed via
 * SCOREP_PYTHON_BINDINGS_RESTORE_LIB), it is called first as well.
 *
 * @return true if the atfork handler is registered.
 */
bool register_fork_reinit(void);

} // namespace scorepy
