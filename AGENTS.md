# gr-difi

GNU Radio out-of-tree module for the DIFI (Digital IF Interoperability, ISTO Std
4900-2021) protocol, a VITA 49 profile. Provides two blocks: a **source** that
receives DIFI packets from the network and emits samples, and a **sink** that
packs samples into DIFI packets and transmits them. Both are templates over
`gr_complex` (fc32) and `std::complex<char>` (sc8), at 8- or 16-bit depth.

Requires GNU Radio >= 3.9 (3.10 in use here). C++20.

## Build and test

```bash
mkdir -p build && cd build
cmake .. && make -j $(nproc)
```

```bash
ctest                                             # everything
ctest -R difi_qa_udp_server --output-on-failure   # one C++ suite
cd ../python && python -m pytest qa_*             # Python only
python -m pytest qa_difi_blocks_cpp.py -k test_multi_packet_correct   # one test
```

Run under `ctest` and the generated wrapper points `PYTHONPATH` and
`LD_LIBRARY_PATH` at the build tree; run `pytest` by hand and you are testing
whatever was last `make install`ed. Either way a C++ change needs a rebuild
first. C++ suites are skipped silently when Boost.Test is absent.

Detailed output lands in `build/Testing/Temporary/LastTest.log`.

**Assume no CI.** A workflow exists at `.github/workflows/.check-in.yaml`, but
the leading dot in the filename means GitHub Actions never picks it up, and it
only targets `main` anyway. Its container is `gnuradio:3.9-ubuntu20`, which
predates the C++20 requirement. Run the tests locally.

## Layout

| Path | Contents |
| --- | --- |
| `include/difi/` | Installed public headers. `difi_common.h` holds the wire-format offsets and constants. |
| `lib/` | Block implementations and transports. Headers here are **not** installed. |
| `grc/*.block.yml` | GRC block definitions. Parameter values are part of the public contract. |
| `python/bindings/` | pybind11 glue. Hand-maintained despite `bind_oot_file.py` sitting next to it — see the traps. |
| `python/qa_difi_blocks_cpp.py` | Python QA, end-to-end through real sockets. |
| `examples/*.grc` | Saved flowgraphs. They embed block parameter values, so changing a GRC enum breaks them. |

## How the blocks work

**One template, two instantiations.** Both blocks are templates over
`gr_complex` and `std::complex<char>`, explicitly instantiated at the bottom of
each `_impl.cc`. Bit depth (8 or 16) is a *separate* runtime parameter, not the
template type: it sets `d_unpack_idx_size` (bytes per I or Q component) and
picks the `d_unpacker` function pointer in the constructor. Getting the pair
wrong is a warning ("not divisible by the number bytes per sample"), not an
error, so it shows up as garbage samples.

**The wire format is offset tables, not structs.** `difi_common.h` holds a
28-byte header size, the mod-16 packet counter, and two offset tables:
`CONTEXT_PACKET_OFFSETS` (16 entries, standard layout) and
`CONTEXT_PACKET_ALT_OFFSETS` (9 entries, a workaround for a non-compliant
device, selected by `context_pack_size == 72`). The sink packer and the source's
`unpack_context_alt` both branch on that 72; a change to one table has to be
mirrored on the other side. Header fields come out by shifting: type is bits
31-28 (`type == 1` is data, anything else is treated as context), packet count
bits 19-16, size in 32-bit words bits 15-0, and bits 31-20 are the "static part"
whose change emits a tag.

**Tags are the source→sink contract.** Everything the sink needs in paired mode
arrives as stream tags, not through a signature. The source emits three keys and
`difi_sink_cpp_impl::process_tags` consumes the same three:

| tag | emitted when | carries |
| --- | --- | --- |
| `pck_n` | first data packet, or a gap in the mod-16 counter | `pck_n`, `data_len`, `full`, `frac` — lets the sink resync timestamps after loss |
| `context` | a context packet arrived (attached to the *next* data samples) | raw context bytes plus decoded `full`/`frac`/`stream_num`; the sink forwards the raw bytes verbatim |
| `static_change` | header bits 31-20 changed | the new static bits |

In standalone mode the sink ignores all of it and packs a context packet from
its own constructor arguments at a fixed interval. So renaming a tag key or a
dict field breaks paired mode while every signature still compiles.

**`work()` blocks, and recurses.** `difi_source_cpp_impl::work` delegates
straight to `buffer_and_send`, which reads exactly one packet and — on a context
packet — recurses to keep hunting for data rather than returning 0. The legacy
transports block, so the flowgraph thread blocks with them. `udp_server` caps
`recv` at 100 ms (`RECV_TIMEOUT_US`) precisely so a caller can still notice
shutdown; preserve that property in the transports still to be written.

## Two coexisting conventions in lib/

The transport layer is being rewritten. New code follows the second column; do
not copy the first when adding files.

| | Legacy (`difi_*_impl`, `udp_socket`, `tcp_*`) | Current (`transport`, `udp_server`) |
| --- | --- | --- |
| Extensions | `.h` / `.cc` | `.hpp` / `.cpp` |
| Namespace | nested `namespace gr { namespace difi {` | `namespace gr::difi {` |
| Members | `d_foo`, raw `new`/`delete` | `m_foo`, RAII |
| Copyright | Microsoft / Welkin Sciences | Libre Space Foundation |
| Errors | `std::cerr` then throw | throw with `strerror(errno)` |

Both are GPL-3.0-or-later with an SPDX tag. New files use:

```text
/*
 * Copyright (C) <year> Libre Space Foundation <https://libre.space>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
```

## Transport restructure (in progress)

`lib/transport.hpp` declares the interface every transport implements:

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

- `udp_server` — **done**, bound endpoint, replies to the last peer heard from
- `udp_client` — **done**, connected to a fixed remote endpoint (used by the sink)
- `tcp_server` — not started (replaces `lib/tcp_server.*`, used by the source)
- `tcp_client` — not started (replaces `lib/tcp_client.*`, used by the sink)

Until all four exist, `difi_source_cpp_impl` and `difi_sink_cpp_impl` still use
the legacy classes; `lib/udp_socket.*`, `lib/tcp_server.*` and `lib/tcp_client.*`
stay until then.

When wiring transports into the blocks, note that **TCP packet framing currently
lives in the source block**, at `difi_source_cpp_impl.cc` in `buffer_and_send`:
it reads a 4-byte word, decodes the packet size from header bits 0-15, then reads
the remainder. That belongs inside the TCP transport's `recv`, so the block stops
knowing which protocol it is on.

## Traps

**`socket_type` is a magic number, not a `SOCK_*` constant.** `make()` takes
`uint8_t socket_type` where `1` = TCP and anything else = UDP. Those happen to
match Linux `SOCK_STREAM` / `SOCK_DGRAM`, which is why passing `socket.SOCK_DGRAM`
from Python works. The values are baked into `grc/*.block.yml` (`protocol`, with
`option_labels: ['TCP', 'UDP']`) and into 4 saved `.grc` flowgraphs, so replacing
the magic number means migrating every consumer below.

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
`bind_oot_file.py` is present but was not used for the current files: each one
defines a `bind_difi_*_cpp_template<T>` helper and calls it twice to register the
`_fc32` and `_sc8` classes, which the generator does not emit. GNU Radio's
"bindings are out of sync" check only fires when the binding `.cc` carries
`BINDTOOL_HEADER_FILE` / `BINDTOOL_HEADER_FILE_HASH` markers; these carry neither
(`python3 header_utils.py all difi_source_cpp_python.cc` prints
`False;False;None;None`), so a stale binding compiles happily and fails later as
a Python `TypeError` on the constructor. Edit them by hand, and mirror the change
in the `docstrings/*_pydoc_template.h` that `D()` expands.

**A `make()` change ripples into five places**: the public header plus impl,
`python/bindings/*_python.cc` plus its docstring template, `grc/*.block.yml`,
the 27 Python QA tests, which construct blocks positionally, and `examples/*.grc`,
which embed literal parameter values. Only the C++ side fails loudly.

**C++ test targets need exported symbols.** The top-level CMakeLists sets
`-fvisibility=hidden`, so a class without `DIFI_API` (from `include/difi/api.h`)
is absent from `libgnuradio-difi.so` and the test binary fails to link. Mark new
transport classes `DIFI_API`. Verify with `nm -DC build/lib/libgnuradio-difi.so`.

**Adding a C++ test suite** means appending the file to `test_difi_sources` in
`lib/CMakeLists.txt`; `GR_ADD_CPP_TEST` handles Boost.Test wiring from there.
Include `lib/qa_transport_utils.hpp` for the shared helpers instead of copying
them: `free_port()` (always use it rather than a literal port, mirroring
`get_open_ports()` in the Python QA), `peer` for the socket on the other end,
`fd_opened_by()` to reach the private socket of a transport, and `sock_opt()` /
`local_port()` to inspect it. Each suite is a separate binary, which is why the
helpers are inline in a header rather than a library.

**A single-argument constructor inside a Boost check macro is a vexing parse.**
`BOOST_CHECK_THROW(udp_server(port), ...)` declares a variable; write
`udp_server{ port }`.

## Known-red tests

`ctest -R qa_difi_blocks_cpp` fails with ~15 assertion errors. It is red on both
`endianess` (HEAD `WIP: Fix endianess issues`) and the `redesign` branch built on
top of it, and was already red before any transport work — verified by stashing
all uncommitted changes and re-running. Do not treat it as a regression from a
transport change; do re-check it if you touch packing or unpacking.

## Git workflow

Strict atomic commits: one logical change per commit, no unrelated edits mixed
in. Finish a unit of work, then commit it before starting the next.

## Documentation

The documentation within the code (comments, block of comments, etc) as well as
documentation for the end user (doxygen, sphinx, etc) should follow a strict,
direct, clear and understandable language even from non-native English users.
Avoid over-complicated phrases, slang or phrases that do not make any sense.
If necessary, ask for user guidance and feedback interactively.
