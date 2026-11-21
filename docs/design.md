# Off Grid Mail — Design

Nov 18, 2026 · SID `OffGridMail`; the code prefix is `ogm`

## Goal

Send and receive Winlink email from Linux and Android through amateur radio,
starting with the Kenwood TH-D75 on VHF/UHF packet and adding the Yaesu FT-710
on HF next.  The radio and modem sit behind a small abstraction layer so other
radios can be added without touching the mail code.

Phases:

1. **TH-D75, AX.25 packet at 1200 baud**, through the radio's built-in KISS TNC
   over Bluetooth.  Plus Telnet to the Winlink CMS, which needs no radio and is
   the quickest way to test an account and the mail code.
2. **FT-710 on HF with ARDOP and/or Mercury** over the radio's USB link (audio
   and CAT).  VARA stays possible through the same host-protocol driver, but it
   is not a goal.  See "Phase 2" below.

## Layers

```
 ┌──────────────────────── shells (own all I/O) ────────────────────────┐
 │ Linux: Qt 6 app, ogm         Android: Kotlin app + JNI            │
 │ Bluetooth RFCOMM, TCP sockets, serial, USB, audio, files, clock      │
 └───────────────▲──────────────────────────────────────────────────────┘
                 │ bytes in / bytes out, now()
 ┌───────────────┴──────────────── libogmcore (C++20, no I/O) ──────────┐
 │ Engine            front door: connect, compose, events, tick(now)    │
 │ Mailbox           in / out / sent folders of raw .b2f messages       │
 │ B2F session       handshake, secure login, proposals, LZHUF blocks   │
 │ ── Channel ──     a reliable byte pipe to one remote station ─────── │
 │   Ax25Channel     AX.25 v2.0 connected mode over a KISS TNC          │
 │   TelnetChannel   CMS Telnet login, then raw bytes                   │
 │   (phase 2)       ArdopChannel, VaraHostChannel (Mercury/VARA)       │
 │ ── Radio ──       what the rig can do: TNC, audio, CAT, PTT ──────── │
 │   TH-D75          KISS TNC over Bluetooth SPP; PC commands (FO, BC)  │
 │   (phase 2)       FT-710: CAT over USB serial, USB audio codec       │
 └──────────────────────────────────────────────────────────────────────┘
```

The rule from aprs_sms carries over: **the core never touches a socket, a
file descriptor, or the clock.**  Shells feed it received bytes and the current
time, and it hands back bytes to send and events to show.  So the whole stack,
down to AX.25 timers, runs in unit tests with a fake clock and a lossy fake
channel.  The one exception is the mailbox, which reads and writes files under
a directory the shell picks.

### Channel

A Channel is the boundary between "how do we reach the gateway" and "what do
we say to it":

| Call | Meaning |
| --- | --- |
| `connect(target, now)` | Start a link to a callsign (AX.25), a host (Telnet), or a modem target |
| `write(bytes)` | Queue session bytes; the channel segments, frames, and resends them |
| `disconnect(now)` | Close gracefully once the queued data has been acknowledged |
| `onLower(bytes, now)` | Bytes from the shell's socket (KISS stream, TCP, modem data port) |
| `tick(now)` | Timers |
| events | `Connected`, `Data(bytes)`, `Disconnected(reason)`, `Progress` |
| `lowerOut()` | Bytes the shell must write to its socket |

The B2F session only sees `write` and `Data`, so it doesn't care whether the
bytes travel over AX.25, TCP, ARDOP, or Mercury.

### Radio

A Radio describes what a rig offers: a built-in TNC, an audio path for a
soundcard modem, and rig control (frequency, mode, PTT).  For the TH-D75 the
only data path is the KISS TNC.  Its PC-command mode (before entering KISS)
can set the frequency, which lets a later version tune the data band to a
gateway's frequency with one tap.  The FT-710 offers CAT plus USB audio and
pairs with a soundcard modem in phase 2.

## AX.25 connected mode (phase 1's main new piece)

The D75's TNC only speaks KISS, so the app runs the AX.25 link layer itself.
It targets **AX.25 v2.0**, which is what packet RMS gateways (Linux kernel,
BPQ, JNOS) all answer to: modulo-8 sequence numbers, SABM rather than SABME,
and no XID negotiation.

- **Frames:** I, RR, RNR, REJ, SABM, DISC, UA, DM, FRMR.  Command and response
  are marked with the C bits in the address SSID bytes (v2.0 style).
- **Defaults for 1200 baud:** PACLEN 128, MAXFRAME (window k) 4, N2 10 retries,
  T3 (idle check) 180 s.  All are settings.
- **T1 (FRACK) counts from estimated end of transmission**, not from when a
  frame was handed to the TNC.  A KISS TNC queues frames and keys up when the
  channel is clear, so the link estimates air time from frame length, baud
  rate, and TXDELAY, then adds FRACK, scaled by the number of digipeaters.
- **Acknowledge after a short delay (T2)** so a burst of I-frames gets one RR,
  and piggy-back N(R) on outgoing I-frames whenever there are any.
- **Recovery:** on T1, send RR with P=1 and retransmit from the N(R) in the
  answer.  REJ on an out-of-sequence frame (once per gap).  After N2 tries,
  the link fails with a clear reason ("no answer from W1ABC-10 after 10 tries").
- **Digipeaters:** `connect W1ABC-10 via DIGI1,DIGI2`.  A received frame
  counts only when every digi in its path has its H bit set; that filters out
  hearing our own frames, or the gateway's, before the digi repeats them.
- **Incoming connects** (P2P) are refused with DM in phase 1.

## Winlink protocol (shared by every channel)

Taken from the published B2F spec, cross-checked against Pat's MIT-licensed
`wl2k-go`, which interoperates with the live system.

- **Handshake:** the gateway sends banner lines, its SID such as
  `[WL2K-5.0-B2FWIHJM$]`, an optional `;PQ: <challenge>`, then a line ending in
  `>`.  We answer `;FW: MYCALL`, our SID `[OffGridMail-0.1-B2FHM$]`, `;PR: <response>`
  when challenged, and `; GATEWAY DE MYCALL (GRID)`.
- **Secure login:** the response is the last 8 decimal digits of a 30-bit
  number taken from MD5(challenge + password + fixed 64-byte salt).  The
  password is the Winlink account password, kept on the device only.
- **Turns:** the client goes first.  Each turn is either a proposal block
  (up to 5 `FC EM <MID> <size> <csize> 0` lines, then `F> <checksum>`) or `FF`
  (nothing to send); `FQ` ends the session.  The receiver answers proposals
  with `FS +-=` (accept, already have, later).
- **Message blocks:** SOH + title + offset, then STX blocks of at most 125
  bytes (so a block plus header fits in a 128-byte AX.25 frame), then EOT and
  checksum.
- **Compression:** LZHUF (2 KB window, 60-byte lookahead, adaptive Huffman) with a
  CRC16 (XMODEM) of the compressed data in front, the B2 variant.
- **Message format:** `Mid`, `Date` (UTC, `YYYY/MM/DD HH:MM`), `Type`, `From`,
  `To`/`Cc` (callsigns or `SMTP:addr`), `Subject`, `Mbo`, `Body: <bytes>`,
  `File: <bytes> <name>` per attachment, CRLF lines, then body and files.
  Body charset defaults to ISO-8859-1; non-ASCII subjects are RFC 2047 encoded.
- **MID:** 12 characters of base32(MD5(time + callsign)).

## Mailbox

Messages are stored as raw `.b2f` files, one per message, in `in/`, `out/`,
`sent/` and `archive/` folders under a per-callsign directory.  This is the
same layout Pat uses, so a mailbox can be inspected or shared with Pat, and a
crash mid-session never corrupts more than the message being written (each
file is written to a temporary name and renamed).

## Phase 2: FT-710 on HF

- **Mercury** speaks VARA's TCP host protocol (command port 8300, data port
  8301), so one `VaraHostChannel` covers Mercury (and VARA, if anyone wants
  it).  On Linux the modem runs as its own program and the app connects over
  TCP, which keeps the app's license independent of Mercury's GPL-3.
- **ARDOP** (ardopcf, MIT) has a similar host interface; `ArdopChannel`.
- **Rig control:** frequency and mode over the FT-710's USB CAT port (the
  "Enhanced" COM port), PTT by CAT or RTS.  Driver choice (own drivers or
  Hamlib) is still open.
- **Android:** running a modem inside the app (direct USB audio and CAT) is
  the long-term goal; building Mercury in would make the app GPL-3, which is
  a decision to make before starting.

## Open decisions

- Rig-control drivers: own, or Hamlib.
- License if Mercury is ever built into the Android app.
