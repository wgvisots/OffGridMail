# Third-party notices

Off Grid Mail is licensed under the Apache License 2.0 (see `LICENSE`).  It
includes or is derived from the following.

## Pat / wl2k-go (MIT)

From [wl2k-go](https://github.com/la5nta/wl2k-go), the Winlink library used by
the Pat client:

- `tests/fixtures/lzhuf/*`: LZHUF test vectors and a sample Winlink message,
  copied unchanged.
- The 64-byte salt in `core/src/secure_login.cpp` (first published in
  paclink-unix), and the protocol details the B2F and LZHUF code was checked
  against.  No wl2k-go source code is included.

```
The MIT License (MIT)

Copyright (c) 2014-2017 Martin Hebnes Pedersen (LA5NTA)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## LZHUF.C

`core/src/lzhuf.cpp` follows the LZHUF algorithm by Haruhiko Okumura and
Haruyasu Yoshizaki (1988, edited by Kenji Rikitake), whose authors released it
as "use, distribute, and modify this program freely".

## aprs_sms

The KISS and AX.25 frame code, the TH-D75 bring-up, the unit-test harness, and
`tests/fixtures/bench.kisslog` (frames received over the air by a TH-D75)
come from the same author's aprs_sms project, also under Apache 2.0.
