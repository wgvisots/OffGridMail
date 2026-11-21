# Security policy

Off Grid Mail parses data that arrives over the radio and from Winlink
gateways, so bugs in the parsers matter: a malformed frame or message must
never crash the app, corrupt the mailbox, or write outside it.

## Reporting a vulnerability

Please report security problems privately through GitHub: open the
**Security** tab of this repository and choose **Report a vulnerability**.
Don't open a public issue for them.

Include what you sent (a frame capture, a `.b2f` file, a byte sequence) and
what happened.  You should hear back within a week.

## What is in scope

- Crashes, hangs or memory errors from input received over AX.25, KISS,
  Telnet or a modem.
- Anything that lets received data choose a file path, or overwrite or
  delete mail it shouldn't.
- Leaks of the Winlink password: it must only ever leave the device as the
  secure-login response, never in plain text.

Amateur radio transmissions are public by law, and Winlink messages travel
unencrypted.  That is by design and not a vulnerability.

## How the code defends itself

- Every parser of outside data (KISS, AX.25, B2F, LZHUF, the message format)
  has a fuzzer in `fuzz/`; CI replays the corpus and fuzzes each one on every
  push.
- Tests run under AddressSanitizer and UndefinedBehaviorSanitizer.
- Message IDs from the air are checked before they become file names.
