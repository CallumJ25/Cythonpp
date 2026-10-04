# Cythonpp

**A Python → C++ compiler for strictly-typed, mypy-compliant Python.**

Cythonpp takes Python source written under `mypy --strict` and turns it into a standalone `.cpp`
file that any C++23 compiler can build into a native executable. Think of it as mypyc, except the
result runs directly on your hardware rather than inside CPython's virtual machine.

> 🚧 **Work in progress.** Cythonpp is under active development.

---

## Why Cythonpp?

### The contract

A compiled program **prints the same output and exits with the same code** as the same script run
under CPython. Cythonpp accepts a program only if **both** `mypy --strict` **and** CPython accept
it. When the compiler can't be sure it will get something right, it says so with a
`NotImplementedError` rather than guessing. A refusal is always acceptable; a binary that quietly
computes the wrong answer never is.

### Compared with plain Python

- **Native speed.** No interpreter and no bytecode dispatch, and values aren't boxed objects. On
  the Collatz example below (scanning the first million integers), the compiled binary ran in
  **~1.7 s** against CPython 3.14's **~13.9 s**. That's one benchmark on one machine, so treat it
  as an illustration rather than a promise.
- **No runtime dependency.** The output is one `.cpp` file plus a small header-only runtime. You
  can ship the binary to a machine with no Python installed, or drop the source into an existing
  C++ build.
- **Type errors before run time.** The compiler has its own mypy-compatible type checker, so a
  program that would fail `mypy --strict` is reported at compile time instead of turning up in
  production.

### Compared with writing C++ directly

- **You write Python.** Readable, concise source with Python's semantics, and the C++ is generated
  for you.
- **Python's arithmetic, not C++'s.** `//` and `%` round toward negative infinity
  (`-7 % 3 == 2`), `int / int` gives a `float`, `float` prints with Python's `repr`, and `and`/`or`
  return an operand, all matching CPython exactly. Getting any of these subtly wrong by hand is a
  classic source of C++ bugs.
- **Checked integers.** Integer overflow and division by zero fail loudly with a non-zero exit
  code. They are never undefined behaviour or a silently wrapped value.
- **Readable output.** Every Python operation becomes a named runtime call
  (`py::add(a, b)`, `py::floordiv(n, 2)`), and every identifier keeps its name behind a `cy_`
  prefix, so you can read the generated code and step through it in a debugger.

---

## Installation

### Requirements

| Tool | Version used in development | Notes |
|---|---|---|
| clang++ | 21 | Needs C++23. Builds the compiler and compiles its output. |
| CMake | 3.x / 4.x | |
| Ninja | any recent | |
| Python + mypy | 3.14 / mypy 1.18.1 | **Optional.** Only used by the corpus-verification script. Not needed to build, test, or use the compiler. |

GoogleTest is downloaded automatically through CMake's `FetchContent`, so there's nothing to
install by hand.

### Build

```sh
git clone <this-repo> Cythonpp
cd Cythonpp
cmake -S . -B build -G Ninja
cmake --build build
```

This produces `build/cythonpp` (`build/cythonpp.exe` on Windows) and the `cythonpp_tests` test
binary.

### Run the tests

```sh
ctest --test-dir build --output-on-failure
```

Among other things, the suite compiles every sample in `test_files/codegen/` with clang++, runs
the result, and checks its output.

---

## Usage

```sh
cythonpp [--tokens | --types] [--emit-cpp] <file.py | directory>
```

| Flag | Effect |
|---|---|
| *(none)* | Lex, parse and type-check, then print each file's AST as an s-expression. |
| `--tokens` | Print the token stream instead of the AST. |
| `--types` | Print the AST with each expression annotated with its inferred type. |
| `--emit-cpp` | **Compile.** Write `<file>.cpp` next to each input. Works alongside any of the modes above. |

Passing a directory processes every `*.py` file under it recursively. Diagnostics go to **stderr**
in the familiar `path:line:col: error: Code: message` format, so editors pick them up without any
configuration. The exit code is non-zero if any file had an error.

Compiling the emitted file only needs the runtime on the include path:

```sh
cythonpp --emit-cpp prog.py
clang++ -std=c++23 -O2 -I runtime prog.cpp -o prog
./prog
```

### Example

`collatz.py`:

```python
def collatz_steps(n: int) -> int:
    steps: int = 0
    while n != 1:
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
        steps = steps + 1
    return steps


def describe(n: int) -> str:
    if collatz_steps(n) > 100:
        return "long"
    return "short"


best: int = 0
best_steps: int = 0
n: int = 1
while n < 10000:
    s: int = collatz_steps(n)
    if s > best_steps:
        best = n
        best_steps = s
    n = n + 1

print(best, best_steps)
print(describe(27), describe(8))
print(7 / 2, 7 // 2, -7 % 3, 2 ** 10)
```

Compile and run it:

```sh
cythonpp --emit-cpp collatz.py
clang++ -std=c++23 -O2 -I runtime collatz.cpp -o collatz
./collatz
```

```
6171 261
long short
3.5 3 2 1024
```

That is byte-for-byte what `python collatz.py` prints. Here's an excerpt of the generated
`collatz.cpp`:

```cpp
py::int_ cy_collatz_steps(py::int_ cy_n) {
  py::int_ cy_steps;
  cy_steps = py::int_(0);
  while (py::truthy(py::ne(cy_n, py::int_(1)))) {
    if (py::truthy(py::eq(py::mod(cy_n, py::int_(2)), py::int_(0)))) {
      cy_n = py::floordiv(cy_n, py::int_(2));
    } else {
      cy_n = py::add(py::mul(py::int_(3), cy_n), py::int_(1));
    }
    cy_steps = py::add(cy_steps, py::int_(1));
  }
  return cy_steps;
}
```

---

## How it works

```
 .py ─► Lexer ─► IndentationPass ─► StatementParser ─► TypeChecker ─► Emitter ─► .cpp
                                                                                    │
                                              runtime/cythonpp/*.h  ◄── #include ──┘
```

- **`src/domain/lexer`, `parser`, `ast`.** Tokens, an immutable AST, and a recursive-descent
  parser that reports `SyntaxError` and keeps going instead of throwing.
- **`src/domain/semantic`.** A mypy-compatible type model and checker. It's validated against a
  labelled corpus of 130+ programs in `test_files/semantic/`, each recording what `mypy --strict`
  and CPython say about it.
- **`src/domain/codegen`.** The `Emitter`, which turns a type-checked AST into C++ or refuses it.
- **`runtime/cythonpp`.** A header-only runtime (`py::int_`, `py::float_`, `py::str`, `py::print`,
  …) that implements Python's semantics in C++. This is the only code that ends up inside the
  compiled program.
- **`src/application`, `ports`, `adapters`.** A hexagonal architecture: the compiler core does no
  I/O, and the filesystem and CLI are plugged in from the outside.

### Verifying the corpus against real Python (optional)

```sh
python scripts/verify_corpus_labels.py --check-corpus
```

This re-runs every labelled sample under real `mypy --strict` and CPython and confirms the
recorded expectations still hold. It's the only part of the project that needs Python installed.

---

## License

[MIT](LICENSE) © 2026 Callum Johnson
