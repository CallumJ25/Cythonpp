#ifndef CYTHONPP_RUNTIME_CYTHONPP_H
#define CYTHONPP_RUNTIME_CYTHONPP_H

// The single header emitted code includes. Header-only by design: an emitted
// .cpp is standalone -- one file plus an include path, with no library to
// build or link.
#include "bool_.h"
#include "compare.h"
#include "fail.h"
#include "float_.h"
#include "int_.h"
#include "logic.h"
#include "none.h"
#include "print.h"
#include "str.h"

#endif // CYTHONPP_RUNTIME_CYTHONPP_H
