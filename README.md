# Off Grid Mail

[![CI](https://github.com/wgvisots/OffGridMail/actions/workflows/ci.yml/badge.svg)](https://github.com/wgvisots/OffGridMail/actions/workflows/ci.yml)

Winlink email over amateur radio, for Linux and Android, starting with the
Kenwood TH-D75 on VHF/UHF packet and adding the Yaesu FT-710 on HF next.
Design: [docs/design.md](docs/design.md).

## Layout

| Path | What |
| --- | --- |
| `core/` | Portable C++20 library (`libogmcore`), no I/O: KISS, AX.25 frames and connected mode, LZHUF, B2F, Winlink messages, secure login, channels, radios, mailbox, and the `Engine` front door |
| `sim/` | `FakeRms`: a pretend RMS gateway built from the core, for tests and `ogmfake` |
| `apps/cli/` | `ogm`: compose, read, and connect from a terminal (Linux) |
| `apps/fake/` | `ogmfake`: the pretend RMS on a TCP port (AX.25 over KISS, or Telnet) |
| `fuzz/` | libFuzzer targets for every parser of outside data, with seed and regression corpora |
| `tests/fixtures/` | TH-D75 captures (from aprs_sms) and LZHUF reference vectors (from Pat's wl2k-go) |
| `tests/e2e_cli.sh` | `ogm` against `ogmfake` over real sockets |

## Build and test

```sh
cmake -S . -B build -G Ninja -DWLE_SANITIZE=ON
cmake --build build && ctest --test-dir build --output-on-failure
```

`connect d75` needs the BlueZ headers (`sudo apt install libbluetooth-dev`).

## Try it without a radio

```sh
build/ogmfake --password pw &                       # W1AW-10 on 127.0.0.1:8001
echo "Hello" | build/ogm --call N0CALL compose --to friend@example.com --subject Test
OGM_PASSWORD=pw build/ogm --call N0CALL connect kiss-tcp 127.0.0.1:8001 W1AW-10
build/ogm --call N0CALL list sent
```

`ogmfake --drop 5` loses every fifth frame, to watch the link recover.
`ogmfake --telnet` (port 8772) behaves like the CMS Telnet service instead.

Settings can live in `~/.config/ogm/config`:

```
call=N0CALL
grid=EM73
```

The Winlink password is looked up in this order: `OGM_PASSWORD`; the desktop
keyring (`secret-tool store --label "Winlink N0CALL" service ogm account N0CALL`,
from the `libsecret-tools` package); `~/.config/ogm/password-N0CALL`, which
must be readable by you only (`chmod 600`); otherwise it is asked for at
connect time.  Mail is kept in
`~/.local/share/ogm/mailbox/<CALL>/{in,out,sent,archive}` as one `.b2f`
file per message, the same layout Pat uses.

## On the air

**Telnet first** (no radio).  The production CMS only accepts client
programs the Winlink team has registered, and this one isn't yet, so use the
development server for now:

```sh
build/ogm connect telnet cms-z.winlink.org:8772
```

`build/ogm connect telnet` (server.winlink.org) answers "Unknown client
types are not allowed on production servers" until the program is registered.

**TH-D75**, paired as for aprs_sms (Menu 983 = Bluetooth, data band on the
gateway's frequency, 1200 baud, radio's own APRS beacons off):

```sh
build/ogm connect d75 28:3C:90:77:11:DF W1AW-10            # direct
build/ogm connect d75 28:3C:90:77:11:DF W1AW-10 --via RELAY # through a digipeater
```

Add `--monitor` to see every AX.25 frame and `--verbose` for the B2F lines.
The app puts the radio in KISS mode on connect and takes it back out at the
end, so the radio's own APRS works again between sessions.

## Contributing and security

See [CONTRIBUTING.md](CONTRIBUTING.md).  Please report security problems
privately, as described in [SECURITY.md](SECURITY.md).

## License

Apache 2.0, see [LICENSE](LICENSE).  Third-party material (Pat's wl2k-go test
vectors, MIT) is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
