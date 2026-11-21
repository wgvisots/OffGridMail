# Contributing

Thanks for your interest.  Bug reports, captures from real radios and
gateways, and pull requests are all welcome.

## Building and testing

```sh
cmake -S . -B build -G Ninja -DOGM_SANITIZE=ON
cmake --build build && ctest --test-dir build --output-on-failure
```

Fuzzers (Clang only):

```sh
CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja -DOGM_FUZZ=ON
cmake --build build-fuzz
build-fuzz/fuzz_b2f -max_total_time=60 fuzz/corpus/b2f
```

If a fuzzer finds a crash, add the input to `fuzz/corpus/<name>/` as
`regress-<something>` along with the fix, and a unit test if the cause is
worth spelling out.

## Ground rules

- The core (`core/`) does no I/O: no sockets, no clock, no threads.  Shells
  pass in bytes and the time.  Keep it that way; it is what makes the whole
  stack testable.
- Treat everything from the radio or a gateway as hostile input.
- Don't hard-code callsigns other than `N0CALL`/`W1AW` examples, and never
  commit passwords, API keys or real users' mail.
- Never make the program identify itself (its SID) as another Winlink
  client.
- New behaviour comes with tests; CI must pass (GCC and Clang, sanitizers,
  Android NDK build, fuzzing).

By contributing you agree that your work is licensed under the Apache License
2.0, like the rest of the project.
