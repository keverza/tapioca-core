#ifndef EVP_PYTHONEMBED_H
#define EVP_PYTHONEMBED_H

// The only sanctioned way to include Python.h in this add-on. Include it FIRST
// in any TU that needs the C API — before APIEnvir.h/ACAPinc.h.
//
// pyconfig.h keys two things off _DEBUG that would both be wrong for us:
//   * it defines Py_DEBUG, which is ABI-changing — a debug CPython lays objects
//     out differently, so a _DEBUG-compiled TU cannot talk to a release
//     python312.dll (silent memory corruption, not a link error);
//   * it auto-links python312_d.lib, which only ships with a debug CPython.
// We link the release DLL in every configuration, so _DEBUG is hidden from
// Python.h. The CRT headers are pulled in first, with the real _DEBUG state
// intact, so nothing else sees a misconfigured CRT.

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <cmath>

#define PY_SSIZE_T_CLEAN

#ifdef _DEBUG
    #define EVP_RESTORE_DEBUG
    #undef _DEBUG
#endif

#include <Python.h>

#ifdef EVP_RESTORE_DEBUG
    #define _DEBUG 1
    #undef EVP_RESTORE_DEBUG
#endif

#endif
