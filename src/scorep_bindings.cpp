#include <Python.h>

#include <iostream>

#include "classes.hpp"
#include "methods.hpp"
#include "scorepy/loader.hpp"

namespace
{
/// Resolve the Score-P API (and in dlopen-mode load the measurement runtime).
/// Fails the module import when Score-P is not available, mirroring the
/// previous behavior where the missing Score-P symbols failed the import.
bool init_measurement_or_raise(void)
{
    if (scorepy::init_binding_measurement())
    {
        return true;
    }
    PyErr_SetString(PyExc_ImportError,
                    "scorep._bindings: Score-P runtime is not available. "
                    "Either run the application via 'python -m scorep' or ensure the Score-P "
                    "libraries are loaded (LD_PRELOAD).");
    return false;
}
} // namespace

#if PY_VERSION_HEX < 0x03000000
PyMODINIT_FUNC init_bindings(void)
{
    PyObject* m;
    if (!init_measurement_or_raise())
        return;
#if SCOREPY_ENABLE_CINSTRUMENTER
    static PyTypeObject ctracerType = scorepy::getCInstrumenterType();
    if (PyType_Ready(&ctracerType) < 0)
        return;
#endif
    m = Py_InitModule("_bindings", scorepy::getMethodTable());
#if SCOREPY_ENABLE_CINSTRUMENTER
    Py_INCREF(&ctracerType);
    PyModule_AddObject(m, "CInstrumenter", (PyObject*)&ctracerType);
#endif
}
#else /*python 3*/
static struct PyModuleDef scorepmodule = { PyModuleDef_HEAD_INIT, "_bindings", /* name of module */
                                           NULL, /* module documentation, may be NULL */
                                           -1,   /* size of per-interpreter state of the module,
                                                    or -1 if the module keeps state in global
                                                    variables. */
                                           scorepy::getMethodTable() };
PyMODINIT_FUNC PyInit__bindings(void)
{
    if (!init_measurement_or_raise())
        return nullptr;
#if SCOREPY_ENABLE_CINSTRUMENTER
    auto* ctracerType = &scorepy::getCInstrumenterType();
    if (PyType_Ready(ctracerType) < 0)
        return nullptr;
#endif

    auto* m = PyModule_Create(&scorepmodule);
    if (!m)
    {
        return nullptr;
    }

#if SCOREPY_ENABLE_CINSTRUMENTER
    Py_INCREF(ctracerType);
    if (PyModule_AddObject(m, "CInstrumenter", (PyObject*)ctracerType) < 0)
    {
        Py_DECREF(ctracerType);
        Py_DECREF(m);
        return nullptr;
    }
#endif

    return m;
}
#endif /*python 3*/
