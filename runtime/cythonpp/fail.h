#ifndef CYTHONPP_RUNTIME_FAIL_H
#define CYTHONPP_RUNTIME_FAIL_H

#include <cstdio>
#include <cstdlib>

namespace py {

// How an emitted program reports a condition it cannot represent: a message
// on stderr and a non-zero exit.
//
// The message text is deliberately NOT an attempt to reproduce CPython's
// traceback. The equivalence claim this project makes is about STDOUT and the
// EXIT CODE; both interpreters write nothing further to stdout and both exit
// non-zero, and that is what the end-to-end tests assert.
[[noreturn]] inline void fail(const char* message) {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::exit(1);
}

} // namespace py

#endif // CYTHONPP_RUNTIME_FAIL_H
