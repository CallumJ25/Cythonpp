# IndentationPass — design

Date: 2026-08-08
Status: approved, not yet implemented

## Context

Cythonpp's lexer is complete, but nothing downstream can parse a statement. Python's grammar is
block-structured and the lexer emits no `INDENT`/`DEDENT`: leading whitespace arrives as one
`SPACE` or `TAB` token per character (`src/domain/lexer/lexer.h:38` flags this as a TODO). Block
structure therefore has to be recovered before a parser can exist.

This is the first of five specs leading to a working front end:

1. **This spec** — `IndentationPass` + shared diagnostics infrastructure + pipeline wiring
2. AST node model (`domain/ast/`) — hierarchy, visitor, source spans
3. Expression parser — precedence climbing over the `OPERATOR` category, soft-keyword
   re-classification, an open-delimiter stack used for diagnostics
4. Statement parser — compound statements over `INDENT`/`DEDENT`, `def`/`class`/imports → `Module`
5. Symbol table and name resolution (`domain/semantic/`) — scopes, imports, forward references

Two decisions were made up front and shape this spec:

- **Import and function-use validation is not a parser concern.** Python permits forward
  references — a module-level function may call one defined later — so no single left-to-right
  parse can decide whether a name is valid. That is a symbol-table pass over the finished AST,
  deferred to spec 5.
- **Bracket matching does not need an explicit stack for correctness.** Recursive descent matches
  delimiters structurally through the call stack, which is strictly more informative (it knows
  which closer was expected). An explicit stack of open delimiters is still worth keeping in
  spec 3, purely so a diagnostic can say "unclosed `(` opened at line 12:8" rather than
  "unexpected EOF".

## Goal

A pass that consumes a `TokenStream` and returns a `TokenStream` in which leading whitespace runs
have been replaced by `INDENT`/`DEDENT` tokens, reporting `TabError` and `IndentationError`
through a diagnostics sink, and never throwing.

## Placement

`src/domain/lexer/indentation_pass.{h,cpp}`, namespace `cythonpp::domain::lexer`.

```cpp
class IndentationPass {
public:
    // Total: never throws. Diagnostics are appended to `sink` in source order.
    // The sink is a parameter rather than a member so that running the pass
    // twice cannot accumulate duplicate diagnostics.
    TokenStream run(const TokenStream& tokens, diagnostics::DiagnosticSink& sink) const;
};
```

A class with one `const` method rather than a free function, matching how `Lexer` and
`ScanContext` are shaped, and leaving room for options (a tab width, a strictness flag) without
changing call sites.

A separate pass rather than logic inside `Lexer`. CPython does this inside its tokenizer, but this
codebase deliberately chose otherwise: `lexer.h:24-26` explains that `SPACE`/`TAB` are preserved
"so that a later pass can apply Python's tab/space rules and report `TabError`", and
`scan_line_start` (`lexer.cpp:168-172`) already excludes blank and comment-only lines specifically
to avoid misleading "the future INDENT/DEDENT pass". A separate pass also tests without source
strings — feed it a synthetic token vector — and leaves every existing lexer test untouched.

## Input contract

Verified by tracing `src/domain/lexer/lexer.cpp`, not assumed.

- `SPACE`/`TAB` are emitted from exactly one place (`lexer.cpp:183`, inside `scan_line_start`), so
  they appear only as a contiguous run at the start of a logical line that has content.
- `NEWLINE` appears only at the end of a logical line. It is suppressed inside brackets
  (`lexer.cpp:198-203`) and after a `\` continuation (`lexer.cpp:222`).
- Blank lines emit nothing. A comment-only line emits a bare `COMMENT_SINGLE` with **no** leading
  whitespace and **no** `NEWLINE` (`lexer.cpp:175-178`, `lexer.cpp:102-103`, `lexer.cpp:209`).
- The stream ends with an optional synthesized `NEWLINE`, then `TOKEN_EOF` — but a
  default-constructed `TokenStream` is empty (`token_stream.h:22`), so the pass must not assume a
  terminal `TOKEN_EOF` exists.
- Semicolon-separated statements are one logical line with one `NEWLINE`, hence one indentation
  decision. So is `if x: y = 1`, which correctly produces no `INDENT`.

Consequences that constrain the implementation:

- **A whitespace run and the first significant token of its line can be on different physical
  lines.** With `if a:` / `    \` / `    x`, the `SPACE` tokens are on line 2 and `IDENTIFIER(x)`
  is on line 3. Runs must be grouped by token adjacency, never by `Token::line_number()`.
- **A dedent to column 0 emits no whitespace tokens at all.** The comparison must run once per
  logical line, including when the run is empty, or dedents to column 0 go undetected until the
  EOF flush — leaving block structure wrong but still balanced, which no balance assertion would
  catch.
- **The first logical line of a file has no preceding `NEWLINE`.** A loop keyed on "just saw a
  `NEWLINE`" silently misses its indentation.
- **The only reachable token order after a `NEWLINE` is `COMMENT_SINGLE*` then `(SPACE|TAB)*` then
  the first significant token.** A comment can never appear inside a whitespace run. An
  order-agnostic accumulator is used anyway, for robustness.
- **"First significant token" here is not the lexer's own notion** (`lexer.cpp:102-103` counts
  `NEWLINE` as significant). The pass's predicate is: not `SPACE`, not `TAB`, not
  `COMMENT_SINGLE`, not `NEWLINE`, not `TOKEN_EOF`.
- **No `SPACE`/`TAB` can escape from inside a string.** `scan_string` and `scan_fstring` consume
  newlines through `advance()` without re-arming `at_line_start_`, and `advance()` keeps `line_`
  correct throughout, so the line after a multi-line docstring is measured correctly.

## Output contract

The same tokens in the same order, with every leading whitespace run deleted and replaced by zero
or more `INDENT`/`DEDENT` tokens inserted **immediately before the first significant token of that
logical line**. Comments stay where they are, so an `INDENT` follows any comment tokens that
preceded it. All open levels are flushed as `DEDENT`s immediately before `TOKEN_EOF`, located by
scanning for the `TOKEN_EOF` token rather than by "after the last `NEWLINE`" — for
`if x:\n    y\n# c\n` the stream ends `NEWLINE, COMMENT_SINGLE, TOKEN_EOF` and the `DEDENT` belongs
after the comment.

Synthesized tokens carry an empty lexeme, matching the convention `finish()` already uses
(`lexer.cpp:546`), and the line and column of the token they precede.

### New token types

```cpp
INDENT = detail::make_token(detail::SPECIAL_FLAGS, 5),
DEDENT = detail::make_token(detail::SPECIAL_FLAGS, 6),
```

Subtypes 5 and 6 are the next free indices in the `SPECIAL_FLAGS` space (0-4 are in use). Per
`CLAUDE.md`, adding an enumerator also requires updating `ALL_TOKEN_TYPES` in
`tests/domain/lexer/token_test.cpp` and adding cases to `token_type_name.cpp`. The comment at
`token_type.h:52-54` claiming `INDENT`/`DEDENT` are "deliberately absent" becomes false and must be
rewritten.

## Algorithm

State is a stack of `IndentLevel { int col; int alt_col; }` seeded with an immutable sentinel
`{0, 0}`. Accumulating a line's whitespace run:

```
SPACE: col += 1;                  alt_col += 1;
TAB:   col = (col / 8 + 1) * 8;   alt_col += 1;
```

This is CPython's `col`/`altcol` scheme verbatim. Note the tab rule is *advance to the next
multiple of 8*, not *add 8*: `  \t` is column 8, not 10. `col` is computed from the tokens, never
from `Token::column_number()` — the lexer's column counter advances over form feeds that produce
no token, so the two disagree.

Comparison against the stack top, once per logical line:

| Relation          | Action                                                                                                                                   |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `col == top.col`  | `alt_col != top.alt_col` → **TabError**; otherwise emit nothing                                                                            |
| `col > top.col`   | `alt_col <= top.alt_col` → **TabError**; otherwise emit `INDENT` and push                                                                  |
| `col < top.col`   | while `size() > 1 && col < top.col`: pop and emit `DEDENT`. Then `col != top.col` → **IndentationError**; then `alt_col != top.alt_col` → **TabError** |

One `INDENT` is emitted per level regardless of how many columns the jump spans.

## Error recovery

The pass is **total**: it never throws, and always returns a structurally balanced stream where
the `INDENT` count equals the `DEDENT` count. The parser will depend on that balance.

- **TabError** — report and continue under the `col` interpretation. Balance-neutral: the
  equal-column case pushes and pops nothing, and the greater-column case still pushes exactly one
  `INDENT`.
- **Unmatched dedent** — report `IndentationError`, then overwrite the stack top's widths with the
  current line's, so depth stays correct and one bad line does not cascade into every line below
  it. **The sentinel at index 0 is exempt**: at the base level, report and leave it at `{0, 0}`.

That exemption is load-bearing. Without it:

```python
if a:
    x        # col 4 > 0  -> INDENT,  stack [0, 4]
  y          # col 2 < 4  -> DEDENT,  stack [0]
             #   2 != 0   -> overwrite base -> stack [2]
z            # col 0 < 2  -> DEDENT,  stack []   <-- underflow
```

produces one `INDENT` against two `DEDENT`s and pops an empty stack. With the sentinel immutable
and the pop loop guarded on `size() > 1`, overwriting a non-base top is balance-safe — it changes
a width, not the depth — and the stack stays strictly increasing, because the overwrite only runs
when `col > top.col` after popping.

The EOF flush emits `stack.size() - 1` `DEDENT`s, not `stack.size()`; the sentinel never had a
matching `INDENT`.

The pass must be re-runnable without accumulating duplicate diagnostics, matching
`Lexer::tokenize()`'s reset-on-entry contract (`lexer.cpp:62`). The sink is a per-call parameter
rather than a member, which makes this structural rather than a thing to remember.

## Diagnostics infrastructure

`src/domain/diagnostics/diagnostic.h`:

```cpp
enum class Severity { Warning, Error };

struct Diagnostic {
    Severity severity;
    std::string code;      // Python exception name: "TabError", "IndentationError"
    std::string message;
    int line;
    int column;
};
```

`Warning`/`Error` rather than the `SCREAMING_CASE` used by `token_category` and `token_type`,
deliberately: `ERROR` is an object-like macro in `wingdi.h`, and macro substitution happens before
scoping, so `Severity::ERROR` would fail to compile in any translation unit that ever pulls in
`windows.h` — on a Windows-primary project that is a latent build break, not a hypothetical. The
existing SCREAMING enums are bit-flag and packed-value constants where the C-style spelling
carries meaning; `Severity` is an ordinary scoped enum, so nothing is lost.

`code` carries the Python exception name so a later front end can match CPython's wording, and
because the pass needs to distinguish the two error kinds anyway.

`src/domain/diagnostics/diagnostic_sink.{h,cpp}` — a concrete collector (`report`,
`diagnostics()`, `has_errors()`), preserving report order. Concrete rather than an interface:
domain code must not depend on `ports/`.

`src/ports/diagnostics_reporter.h` changes from `report_error(message, line, column)` to
`report(const std::string& path, const Diagnostic&)`. The port needs to say which file, and needs
severity and code. It has no implementors, call sites, or tests today, so the change is free. This
gives `ports/` its first include of `domain/` — normal hexagonal practice (a driven port speaks the
domain's language), but a new edge in the dependency graph, so `CLAUDE.md` should record it.

`src/adapters/cli/console_diagnostics_reporter.{h,cpp}` — prints `path:line:col: error: message`
to stderr.

## Wiring

`CompilePipeline` takes a `ports::DiagnosticsReporter&` as a third constructor argument.
`compile_one()` becomes `Lexer` → `IndentationPass` → drain the sink into the reporter. Reporting
as each file is processed, rather than buffering into `CompileResult`, is what a multi-file
compile wants.

`CompileResult` gains `bool has_errors`, and `CliAdapter` returns exit code 1 when it is set. A
compiler that prints errors and exits 0 is a broken compiler.

The arity change touches nine construction sites: `cli_adapter.cpp:67` and eight in
`tests/application/compile_pipeline_test.cpp`. The tests gain a `RecordingDiagnosticsReporter`
test double, which is useful in its own right for asserting pipeline-level diagnostics.

No existing test expectation breaks from inserting the pass into `compile_one`: every fixture in
`compile_pipeline_test.cpp` is an unindented one-liner, and `DEDENT`s are inserted *before*
`TOKEN_EOF`, so the trailing-token assertions still hold. Lexer tests construct `Lexer` directly
and are unaffected, provided `lexer.cpp` is not modified.

## Testing

`tests/domain/lexer/indentation_pass_test.cpp`, test-driven, in the existing
`TEST(Suite, DescriptiveSentence)` style. Two fixtures: unit tests over synthetic token vectors,
and integration tests running real source strings through `Lexer` → `IndentationPass` to prove the
input contract above actually holds rather than merely being documented.

Coverage:

- **Shape** — empty stream; whitespace-only file; comment-only file; whitespace tokens removed;
  `INDENT` placed before the first significant token, not before the `NEWLINE`; synthesized tokens
  carry the position of the token they precede.
- **Levels** — one `INDENT` per level regardless of width; multi-level dedent emits one `DEDENT`
  per level; equal indentation emits neither; EOF flushes all open levels; dedent to column 0
  detected despite no whitespace tokens existing; an indented first line of a file is measured.
- **Tabs** — tab advances to the next multiple of 8; equal columns with a different tab/space mix
  is a `TabError`; deeper column with non-increasing alt column is a `TabError`; consistent
  all-tab indentation produces no diagnostic; `TabError` recovery continues under the `col`
  interpretation.
- **Errors** — unmatched dedent reports and continues; unmatched dedent does not unbalance the
  stream (the four-line case above, as a regression test); diagnostics carry the offending line
  and column; the pass never throws; running the pass twice does not duplicate diagnostics.
- **Lexer interaction** — blank lines and comment-only lines between statements do not change the
  level; a comment-only line at column 0 inside a block does not dedent; continuation-line
  indentation is not measured; backslash continuation measures only the first physical line;
  semicolon-separated statements are one logical line; indentation after a multi-line
  triple-quoted string and after a multi-line f-string is measured correctly; an unterminated
  bracket, an unterminated triple-quoted string, and a `\r`-only line ending each leave a balanced
  stream.
- **Global invariant** — a helper asserting `#INDENT == #DEDENT` across every fixture in the
  suite, including the malformed ones, since balance is what the parser depends on and individual
  expectations will not catch a regression in the recovery path.

Plus `tests/domain/diagnostics/diagnostic_sink_test.cpp`: starts empty, preserves report order,
records severity, code, message and position.

## Known deviations, deliberately not fixed here

Both require modifying `lexer.cpp`, which is outside this spec's boundary. Each gets a test
pinning the current behaviour so the divergence is visible rather than forgotten.

- **Form feed.** `scan_line_start`'s emit loop accepts only `' '` and `'\t'` (`lexer.cpp:179`)
  while the surrounding probe and skip loops also consume `'\f'` (`lexer.cpp:172`,
  `lexer.cpp:186-188`). So `"\f    x"` produces zero whitespace tokens where CPython computes
  column 4, and `"    \f  x"` produces four where CPython computes 2 (it resets the column on a
  form feed). The information is absent from the stream, so no implementation of this pass can be
  faithful here.
- **Bare `\r` line endings.** `scan_end_of_line` is reached only for `'\n'` (`lexer.cpp:129`); a
  lone `'\r'` is swallowed as inter-token whitespace. A classic-Mac-line-ending file therefore
  produces no `NEWLINE` and no whitespace tokens at all, and lexes as a single logical line. CRLF
  is handled correctly.

## Also stale once this lands

`src/domain/lexer/lexer.h:38-40` (the INDENT/DEDENT TODO), `CLAUDE.md:123`
("INDENT/DEDENT are not implemented yet"), `CLAUDE.md:51-53` (the lexer file list), and
`CLAUDE.md:62` ("stub, not yet consumed anywhere", about `DiagnosticsReporter`).
