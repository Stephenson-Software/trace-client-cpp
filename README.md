# trace-client (C++)

**One call to report that a program was used.**

A single-header C++11 client for a [trace](https://trace.danielstephenson.dev)
server — the central place a fleet of programs reports usage events to. The
whole library is one file, `trace_client.hpp`, with nothing to link, and the
integration on the program side is meant to stay one call. The Java and Python
counterparts are [trace-client-java](https://github.com/Stephenson-Software/trace-client-java)
and [trace-client-python](https://github.com/Stephenson-Software/trace-client-python);
all three speak the same wire format and make the same promises.

```cpp
#include "trace_client.hpp"

trace_client::TraceClient trace("https://trace.danielstephenson.dev", "MyGame",
                                settings.usageReportingKey,
                                settings.usageReportingEnabled);
trace.report("startup", {{"version", "1.4.0"}});
trace.report("level-complete", 3.0, {{"level", "forest"}});

// on shutdown -- also before a short-lived program exits, so the event is
// sent. The destructor does the same.
trace.close();
```

## What `report` promises

| Property | Meaning |
|---|---|
| **Returns immediately** | The HTTP call runs on one thread the client owns. A game loop can report from its main thread and no frame waits on the network. |
| **Never throws** | Nothing in the client throws. A server that is down, slow, or rejecting the key — or a machine with no `curl` — is a dropped report, not an exception or a crash in your program. Drops are passed to the optional `Logger` (a `std::function<void(const std::string&)>`, last constructor argument), otherwise not mentioned at all. |
| **Bounded** | At most 256 reports wait to be sent; past that, new ones are dropped. A trace server that is unreachable for a week costs a few kilobytes, not your memory. |
| **`close()` drains** | Reports already queued get up to the client timeout (5 s total) to be sent before the thread stops, so a CLI that reports and exits at once does not lose its event. Still bounded: an unreachable server delays exit by at most the timeout — a request still in flight then is abandoned and its `curl` process killed. `close(seconds)` takes a shorter bound. Once `close()` returns, the `Logger` is never called again. |
| **Fits the server's limits** | Names and tag keys/values are cut to 255 bytes (never mid-character), at most 32 tags are sent, blank tag keys are dropped, invalid UTF-8 becomes U+FFFD, and NaN or infinite values are left out — so a report is never rejected as a whole for one bad field. |

## Turning it off

Reporting is **opt-out**. Any one of these yields a client that does nothing,
starts no thread and costs nothing; the first that applies is the reason:

- **Environment, for every trace-reporting program at once:**
  `TRACE_USAGE_REPORTING=off` (also `false`, `0`, `no`; case-insensitive) or
  `DO_NOT_TRACK=1` (also `true`, `yes`; the
  [consoledonottrack.com](https://consoledonottrack.com) convention). The
  constructor checks these before anything else, so they win over the
  program's own setting. Any other value, or an unset variable, leaves that
  setting in charge. `trace_client::environmentOptsOut()` answers the same
  question on its own.
- **The program's own setting:** `enabled = false` (the fourth constructor
  argument).
- **No key** (or a blank one).

`client.disabledReason()` says which one applied — `"environment"`, `"config"`
or `"no key"`, or `"unavailable"` when the client cannot send at all (a
browser build, a blank base URL or application name, or the thread could
not be started) — and is empty when the client reports, so a program can print it.
A default-constructed `TraceClient` is disabled with reason `"config"`.

A program that runs on other people's machines should expose the `enabled`
switch in its settings — and say so once, the first time it runs, so the
player knows reporting is on, that `TRACE_USAGE_REPORTING=off` turns it off,
and where the details are:
<https://github.com/Stephenson-Software/trace#usage-reporting>.

## How a report is sent, and why that way

The server is HTTPS-only, and C++ has no HTTP client in its standard library.
The choices were:

1. **Link libcurl (or OpenSSL) directly.** The obvious one, but it turns a
   one-header drop-in into a build dependency: every consumer's Makefile, CMake
   file, CI image and Windows toolchain would need the development package, and
   a program that builds today would stop building for the sake of telemetry.
2. **Write TLS by hand.** Not a serious option.
3. **Run the system's `curl` executable.** macOS ships it, Windows ships it
   from Windows 10 1803 on (`C:\Windows\System32\curl.exe`), and nearly every
   Linux distribution has it. Nothing is added to the program's build, and TLS
   is whatever the operating system keeps up to date.

The client does **3** by default, and offers **1** as an opt-in: define
`TRACE_CLIENT_USE_LIBCURL` before including the header and link `-lcurl`.

Running an external program is only acceptable if nothing the program reports
can change what that program does, so the details are strict:

- The command line is fixed: `curl -q --silent --config -`. No value from the
  program or the report is ever an argument, and there is no shell — on POSIX
  it is `posix_spawnp`, on Windows `CreateProcess` with a constant command
  line. `-q` comes first so no `~/.curlrc` is read.
- Everything else — URL, headers, the key, the JSON body, the timeouts — goes
  to curl's standard input as a curl config file. Every value is quoted, with
  backslashes, quotes and control characters escaped, so no value can end its
  line and start another option. The key and application name are also
  stripped of control characters before they go into a header. The tests
  include an injection attempt through each of them.
- `proto = "=http,https"` stops a malformed base URL from reaching any other
  protocol; redirects are not followed.
- The key is never on a command line, so it does not show up in `ps`.
- On Windows `curl.exe` is taken from `System32`, or else from the directories
  on `PATH` — never from the current directory. It runs with
  `CREATE_NO_WINDOW`, so a GUI program does not flash a console.
- On POSIX the sending thread blocks `SIGPIPE`, so a curl that dies early is an
  error code, not a signal that kills the host program. curl itself starts with
  a clean signal mask.
- **No curl is an unreachable server:** the spawn fails, the report is dropped,
  and the `Logger` hears about it. Nothing is printed and nothing throws.

The cost is one short-lived process per report — fine for `startup` and a
handful of events per session, which is what trace is for. A program that
reports many events a second should use the libcurl transport.

Under Emscripten (a browser build) there are no processes to spawn, so the
client is always disabled there, with reason `"unavailable"` — nothing is
lost, the desktop build reports. With `TRACE_CLIENT_USE_LIBCURL` the header
also builds for platforms without `posix_spawn`.

## Getting it

**Copy the file.** `trace_client.hpp` has no dependencies. Drop it into your
source tree, keep the header comment so it can be found again, and you are
done. It uses `std::thread`, so link with `-pthread` as for any threaded
program (on Linux with glibc older than 2.34 the link fails without it; the
compiler says so at build time, never at run time).

The executable can be overridden before the include, e.g.
`#define TRACE_CLIENT_CURL "/opt/bin/curl"`.

## The wire format

`POST {baseUrl}/api/metrics` with `Authorization: Bearer <key>`,
`Content-Type: application/json; charset=utf-8`,
`User-Agent: trace-client-cpp/0.1.0 (<application>)` and a body of

```json
{"application":"MyGame","name":"startup","tags":{"version":"1.4.0"}}
```

`value` and `tags` are omitted when not given. The server assigns the
timestamp. A `201` is success; anything else is passed to the `Logger` and
dropped.

## Keys

A key identifies the program to the server and lets the operator revoke it;
it is scoped to *reporting only*. Because it ships inside the program, it
cannot prove anything — treat trace data as best-effort telemetry, which is
what it is. Ask the trace operator for a key for your program.

## Building

```
make test            # system curl transport
make test-libcurl    # the TRACE_CLIENT_USE_LIBCURL transport
```

or, where there is no make (MSVC), `cmake -S . -B build && cmake --build build`
and run `build/Release/test_trace_client`.

Tests run the client against a small HTTP server written with plain sockets
on a loopback port — no more dependencies than the client itself — and never
reach a real trace server. CI runs them on Linux (g++ and clang++, C++11, 17
and 20; libcurl; ThreadSanitizer and AddressSanitizer), macOS and Windows
(MSVC with the system `curl.exe`).

## License

MIT.
