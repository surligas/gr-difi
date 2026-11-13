# gr-difi

GNU Radio out-of-tree module for the DIFI (Digital IF Interoperability, ISTO Std
4900-2021) protocol, a VITA 49 profile. Provides two blocks: a **source** that
receives DIFI packets from the network and emits samples, and a **sink** that
packs samples into DIFI packets and transmits them. The source reads packets
with 8- or 16-bit samples and outputs `gr_complex` (fc32) or
`std::complex<char>` (sc8).
The sink takes integer samples: `std::complex<char>` (sc8) at 8 bits or
`std::complex<short>` (sc16) at 16 bits.

Requires GNU Radio >= 3.9. C++20.

## Build and test

```bash
mkdir -p build && cd build
cmake .. && make -j $(nproc)
```

```bash
ctest                                             # everything
ctest -R difi_qa_udp_server --output-on-failure   # one C++ suite
ctest -R difi_qa_certification -V                 # DIFI compliance certification suite
cd ../python && python -m pytest qa_*             # Python only
python -m pytest qa_difi_blocks_cpp.py -k test_multi_packet_correct   # one test
```

Run under `ctest` and the generated wrapper points `PYTHONPATH` and
`LD_LIBRARY_PATH` at the build tree; run `pytest` by hand and you are testing
whatever was last `make install`ed. Either way a C++ change needs a rebuild
first. C++ suites are skipped silently when Boost.Test is absent.

The certification suite exists only when the `test/DIFI-Certification`
submodule is checked out (`git submodule update --init`). The Python packages
that the suite needs are installed in a virtual environment. Run
`make difi_cert_setup_venv` once, then run `cmake ..` again so that the suite
uses this environment.

Detailed output lands in `build/Testing/Temporary/LastTest.log`.

**The Python QA needs the same pybind11 as GNU Radio.** Python modules built
with different pybind11 ABI versions cannot share types, so `import difi` fails
with `referenced unknown base type "gr::sync_block"`. This happens when a
distribution updates pybind11 before it rebuilds GNU Radio. `cmake` checks for
this mismatch and prints a warning. The C++ suites still work. To run the
Python QA, use the container, where pybind11 and GNU Radio come from the same
Debian release:

```bash
docker build -t gr-difi .
docker run --rm gr-difi sh -c 'cd /src/gr-difi/build && ctest --output-on-failure'
```

The image contains a copy of the source taken when it was built, so rebuild
the image after every change.

**Assume no CI.** A workflow exists at `.github/workflows/.check-in.yaml`, but
the leading dot in the filename means GitHub Actions never picks it up, and it
only targets `main` anyway. Its container is `gnuradio:3.9-ubuntu20`, which
predates the C++20 requirement. Run the tests locally.

## Layout

| Path | Contents |
| --- | --- |
| `include/difi/` | Installed public headers: the two blocks, `difi.hpp` (`class difi`, nested enums/structs, wire-format constants) and the transport classes. |
| `lib/` | Block implementations, transports and the C++ test suites (`qa_*.cc`). Headers here are **not** installed. |
| `test/` | Certification test tools (`difi_cert_tx`, `difi_cert_rx`), runner (`run_difi_certification.py`), and `DIFI-Certification` submodule. |
| `grc/*.block.yml` | GRC block definitions. Parameter values are part of the public contract. |
| `python/bindings/` | pybind11 glue. Hand-maintained despite `bind_oot_file.py` sitting next to it — see the traps. |
| `python/qa_difi_blocks_cpp.py` | Python QA, end-to-end through real sockets. |
| `examples/*.grc` | Saved flowgraphs. They embed block parameter values, so changing a GRC enum breaks them. |

## How the blocks work

**The source: one template, two instantiations.** `difi_source_cpp<T>` is a
template over `gr_complex` and `std::complex<char>`, explicitly instantiated at
the bottom of `difi_source_cpp_impl.cc`. Bit depth (8 or 16) is a *separate*
runtime parameter, not the template type: it sets `d_unpack_idx_size` (bytes
per I or Q component) and picks the `d_unpacker` function pointer in the
constructor. Getting the pair wrong is a warning ("not divisible by the number
bytes per sample"), not an error, so it shows up as garbage samples.

**The sink: the wire format is set by template parameters.**
`difi_sink_cpp<V, Transport, BitDepth, ByteOrder>` takes the context version
(standard or alternative), the transport (`udp_client` or `tcp_client`), the
bit depth and the byte order of the I/Q samples, so the packet path has no
runtime branching. The bit depth also sets the input type: sc8 at 8 bits, sc16
at 16 bits. `DIFI_SINK_INSTANTIATE` at the bottom of `difi_sink_cpp_impl.cc`
instantiates every combination. `make()` returns `difi_sink_base::sptr`, the
non-template base class that holds the setters and getters. The Python
bindings pick the instantiation at runtime from the constructor arguments.

**The protocol engine is `class difi`.** `difi.hpp` consolidates wire-format
constants, header operations, context parsing, timestamp arithmetic, SIMD sample
conversion, and PMT tag generation. Wire constants are `static constexpr`
members of `class difi` (with backwards-compatible aliases for legacy blocks,
e.g. `difi::CONTEXT_PACKET_OFFSETS`, `difi::CONTEXT_PACKET_ALT_OFFSETS`,
`difi::DIFI_HEADER_SIZE`, `difi::DATA_START_IDX`, `difi::VITA_PKT_MOD`).

Enums and structs are nested inside `class difi` (with namespace-level `using`
aliases for backwards compatibility):

- `difi::packet_type`: `data = 1`, `context = 4`, `version = 5` (DIFI 1.1 / 1.2.1), `command = 6` (flow control), `unknown = 0`.
- `difi::bit_depth`: `bits_8 = 8`, `bits_16 = 16`.
- `difi::byte_order`: `network` (same as `big`), `big`, `little`. It applies to the I/Q samples only; header and context words are always big-endian.
- `difi::context_version`: `standard`, `alternative`.
- `difi::context_behavior`: `throw_exe`, `ignore`, `warnings_forward`, `warnings_no_forward`.
- `difi::header`: 28-byte header representation (alias `difi_header`).
- `difi::flow_control`: a parsed flow control packet: buffer size, buffer level (0 to 4095) and the overflow, nearly full, nearly empty and underflow flags.

**Context packets are templated by version:**
`template <context_version V = context_version::standard> struct context;`

- `context<context_version::standard>` (`standard_context`, `difi_context`):
  represents the 108-byte standard context packet with 16 engineering fields.
- `context<context_version::alternative>` (`alternative_context`):
  represents the 72-byte alternative context packet with 9 fields.
- `difi::parse_context<V>(buf, len)` / `difi::parse_context<V>(span)`: compile-time version selection.
- `difi::parse_context(buf, len)` / `difi::parse_context(span)`: runtime dispatch returning `std::variant<standard_context, alternative_context>`.
- `difi::pack_context(ctx, ...)`: overloads for typed contexts and variants.

Header fields come out by shifting: type is bits 31-28 (`type == 1` is data,
`type == 4` is context, `type == 5` is version context, `type == 6` is flow
control), packet count bits 19-16, size in 32-bit words bits 15-0, and bits
31-20 are the "static part" whose change emits a tag.

**The source emits three stream tags; the sink does not use them.** The tags
are for blocks downstream of the source:

| tag | emitted when | carries |
| --- | --- | --- |
| `pck_n` | first data packet, or a gap in the mod-16 counter | `pck_n`, `data_len`, `full`, `frac` — lets a consumer resync timestamps after loss |
| `context` | a context packet arrived (attached to the *next* data samples) | raw context bytes plus decoded `full`/`frac`/`stream_num` |
| `static_change` | header bits 31-20 changed | the new static bits |

The sink ignores all tags in streaming mode, and reads only the length tag in
burst mode. It builds its own context packets from its parameters and sends one
every `context_interval` data packets. So a sink connected to a source does not
pass on the context packets that the source received.

**Sink rate control.** With rate control on, the sink follows the flow control
reports that the receiver (a digitizer) sends back on the same socket. A
receive thread (`rx_loop`) parses them. The sink starts to send at once, and
does not wait for the first report. At startup, and again after an underflow,
the sink sends data at twice real time until a report shows more than 2
seconds of samples in the buffer, or has the nearly full flag set. The
digitizer starts to transmit only when its buffer holds about 2 seconds of
samples. After that, the sink sends only while the reports have the nearly
empty flag set. This keeps the latency low. Rate control also sets the
information class in the Class ID field of every packet to
`bit_padding_data_plane`. Every report and every received context packet is
posted on the `status` message port.
`lib/qa_sink_rate_control.cc` tests the rate control against a fake digitizer.

**`work()` blocks, and the source's recurses.** `difi_source_cpp_impl::work`
delegates straight to `buffer_and_send`, which reads exactly one packet and — on
a context packet — recurses to keep hunting for data rather than returning 0.
The legacy transports block, so the flowgraph thread blocks with them. The new
transports cap `recv` at 100 ms (`RECV_TIMEOUT_US`) so that a caller can still
notice shutdown; keep that property in any new transport. While rate control
holds the sink back, the sink's `work()` waits for flow control reports with no
time limit.

## Two coexisting conventions in lib/

The transport layer is being rewritten. New code follows the second column; do
not copy the first when adding files.

| | Legacy (`difi_*_impl`, `udp_socket`, `tcp_*_legacy`) | Current (`difi.cpp`, `transport`, `udp_*`, `tcp_*`) |
| --- | --- | --- |
| Extensions | `.h` / `.cc` | `.hpp` / `.cpp` |
| Namespace | nested `namespace gr { namespace difi {` | `namespace gr::difi {` |
| Members | `d_foo`, raw `new`/`delete` | `m_foo`, RAII |
| Copyright | Microsoft / Welkin Sciences | Libre Space Foundation |
| Errors | `std::cerr` then throw | throw with `strerror(errno)` |

The sink implementation is only partly migrated. It keeps the legacy file
names, namespaces and copyright, but the members added later use the `m_foo`
naming and RAII.

Both are GPL-3.0-or-later with an SPDX tag. New files use:

```text
/*
 * Copyright (C) <year> Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
```

## Transport restructure (in progress)

`include/difi/transport.hpp` declares the interface every transport implements:

```cpp
virtual void send(const void *data, size_t len) = 0;      // throws on failure
virtual ssize_t recv(void *buf, size_t len) = 0;          // negated errno on failure
```

It also provides the socket setup that every transport needs, so a derived class
only has to add what makes it different:

| member | purpose |
| --- | --- |
| `create_socket(type, protocol)` | opens a socket and configures it, closing it again if the configuration fails |
| `apply_socket_options(fd)` | sets the buffer sizes and the receive timeout. Call it on a socket returned by `accept()`, which does not inherit them |
| `errno_msg(what)` | formats a failure as `"<what>: <strerror(errno)>"` |
| `RECV_TIMEOUT_US` | 100 ms, so a blocking `recv` still returns often enough to notice shutdown |
| `m_recv_buffer_size` / `m_send_buffer_size` | protected consts, default `DEFAULT_BUFFER_SIZE` (2 MB) |

A derived class calls `create_socket()`, then does its own `bind()`, `connect()`
or `listen()`, and closes the socket by hand if that step throws. The object is
not fully constructed at that point, so its destructor will not run.

One class per role, so protocol choice collapses to a single decision at block
construction and the runtime path has no per-packet branching:

- `udp_server` — bound endpoint, replies to the last peer heard from
- `udp_client` — connected to a fixed remote endpoint (used by the sink)
- `tcp_server` — serves one peer at a time
- `tcp_client` — connects on demand and reconnects (used by the sink)

The two TCP classes share `tcp_transport`, which holds the framing, the send
loop and the connection handling. A derived class implements one method,
`ensure_connected()`: the server accepts a waiting peer, the client reaches out
to its endpoint. `tcp_client` connects when first used rather than in its
constructor, and connects again after a connection is lost, because the peer of
a sink is often started later than the flowgraph.

**The sink uses the new transports; the source does not yet.** The next step is
wiring `difi_source_cpp_impl` to `udp_server` and `tcp_server`. Until then,
`difi_source_cpp_impl` uses the legacy `udp_socket` and `tcp_server_legacy`. `tcp_client_legacy` is no
longer used by any block. The legacy TCP classes carry the `_legacy` suffix
because the rewritten ones needed their names; delete the legacy classes once
nothing includes them. The Python QA covers the TCP path of the source with the
`test_tcp_*` cases, and `examples/difi_paired_example.grc` uses
`protocol: '1'`, so run these cases and this flowgraph before trusting the
wiring.

**TCP framing now exists in two places, on purpose.** TCP delivers a stream and
does not keep packet boundaries, so somebody has to read the size from the DIFI
header and cut the stream into packets. The legacy path still does this inside
the source block, at `difi_source_cpp_impl.cc` in `buffer_and_send`.
`tcp_transport::recv` does the same thing internally and returns exactly one
packet, so a block using it does not need to know which protocol it is on.
Delete the copy in `buffer_and_send` when the source block is wired to the new
transport.

## Traps

**The protocol is a magic number, not a `SOCK_*` constant.** The source's
`make()` takes `uint8_t socket_type`, and the sink's Python constructors take
`protocol`. In both, `1` = TCP and anything else = UDP. These values happen to match
Linux `SOCK_STREAM` / `SOCK_DGRAM`, which is why passing `socket.SOCK_DGRAM`
from Python works. The C++ sink takes the transport as a template parameter
instead. The values are baked into `grc/*.block.yml` (`protocol`, with
`option_labels: ['TCP', 'UDP']`) and into every saved `.grc` flowgraph, so
replacing the magic number means migrating all of them.

**The source block ignores its `ip_addr` in UDP mode.** Legacy `udp_socket`
always binds `INADDR_ANY` when acting as a server, so existing flowgraphs set
that field to the *sender's* address. `udp_server(port)` preserves this;
`udp_server(ip_addr, port)` binds the given interface and would break them.

**`udp_client` ignores `ECONNREFUSED` when sending.** Its socket is connected, so
when no socket listens on the remote port, the kernel reports the resulting ICMP
port unreachable message as an error on the *next* `send`, not on the one that
caused it. The earlier datagram was still transmitted, and the receiver may be
started at any time, so `send()` returns normally in this case instead of
throwing. Every other error still throws. `recv()` reports the same condition as
`-ECONNREFUSED` and lets the caller decide.

**The bindings are hand-written, and nothing catches them drifting.**
`bind_oot_file.py` is present but was not used for the current files. The
source file defines a `bind_difi_source_cpp_template<T>` helper and calls it
twice to register `difi_source_cpp_fc32` and `difi_source_cpp_sc8`. The sink
file registers one class, `difi_sink_base`, and three factory functions
(`difi_sink_cpp`, `difi_sink_cpp_sc8`, `difi_sink_cpp_sc16`) that pick the
template instantiation from their arguments. The generator emits neither of these layouts.
GNU Radio's "bindings are out of sync" check only fires when the binding `.cc`
carries `BINDTOOL_HEADER_FILE` / `BINDTOOL_HEADER_FILE_HASH` markers; these
carry neither (`python3 header_utils.py all difi_source_cpp_python.cc` prints
`False;False;None;None;`), so a stale binding compiles happily and fails later as
a Python `TypeError` on the constructor. Edit them by hand, and mirror the change
in the `docstrings/*_pydoc_template.h` that `D()` expands.

**A `make()` change ripples into five places**: the public header plus impl,
`python/bindings/*_python.cc` plus its docstring template, `grc/*.block.yml`,
the tests, and `examples/*.grc`, which embed literal parameter values. Only the
C++ side fails loudly. The sink's Python factories take extra arguments that
select the template (`context_version`, `protocol`, and `bit_depth` for
`difi_sink_cpp`), so their argument order differs from the C++ `make()`. The
GRC `make` template, the Python QA and `lib/qa_sink_rate_control.cc` all pass
arguments by position. A change of the argument order breaks the first two only
at runtime.

**Waits inside `work()` must use `gr::thread` types.** GNU Radio stops a
flowgraph by interrupting the threads of its blocks. Only the Boost-based waits
of `gr::thread` return when the thread is interrupted. A
`std::condition_variable` wait in `work()` can keep the flowgraph from
stopping. Code that changes a flag used by such a wait must take the mutex
after it changes the flag and before it notifies, as `wake_rate_control()` in
the sink does. Otherwise the notification can arrive
between the check and the wait, and the wait never ends.

**C++ test targets need exported symbols.** The top-level CMakeLists sets
`-fvisibility=hidden`, so a class without `DIFI_API` (from `include/difi/api.h`)
is absent from `libgnuradio-difi.so` and the test binary fails to link. Mark new
transport classes `DIFI_API`. Verify with `nm -DC build/lib/libgnuradio-difi.so`.

**Adding a C++ test suite** means appending the file to `test_difi_sources` in
`lib/CMakeLists.txt`; `GR_ADD_CPP_TEST` handles Boost.Test wiring from there.
Include `lib/qa_transport_utils.hpp` for the shared helpers instead of copying
them: `free_port()` (always use it rather than a literal port, mirroring
`get_open_ports()` in the Python QA; `free_port()` returns a port that is free
for both UDP and TCP), `peer` for the socket on the other end,
`fd_opened_by()` to reach the private socket of a transport, and `sock_opt()` /
`local_port()` to inspect it. Each suite is a separate binary, which is why the
helpers are inline in a header rather than a library.

**A single-argument constructor inside a Boost check macro is a vexing parse.**
`BOOST_CHECK_THROW(udp_server(port), ...)` declares a variable; write
`udp_server{ port }`.

## Known issues

None

## Git workflow

Do not commit automatically. Never run `git commit` unless explicitly instructed
by the user. When committing, follow strict atomic commits: one logical change
per commit, no unrelated edits mixed in.

## Documentation

The documentation within the code (comments, block of comments, etc) as well as
documentation for the end user (doxygen, sphinx, etc) should follow a strict,
direct, clear and understandable language even from non-native English users.
Avoid over-complicated phrases, slang or phrases that do not make any sense.
If necessary, ask for user guidance and feedback interactively.
