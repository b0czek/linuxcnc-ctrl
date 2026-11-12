# Reusable server TLS transport

This library constructs server credentials without depending on LinuxCNC
services. It supports an explicitly selected PEM file key or an external
P-384 key. External keys contain a public point and a signer reference; the
provider neither receives nor exports a private scalar. There is no automatic
fallback between key sources or to plaintext.

## Public API and ownership

Headers live under `include/linuxcnc_transport`, in namespace
`linuxcnc::transport`:

- `external_key.hpp`: `ExternalKeyReference`, `EncodeExternalKey`, and
  `DecodeExternalKey`. Each reference owns its socket path, key ID, and public
  point. Signer selection uses neither environment variables nor mutable
  process-wide key state.
- `signer.hpp`: typed `TlsServerSignRequest`, `TranscriptHash`, request/response
  codecs, `BuildServerCertificateVerify`, and `RequestServerSignature`.
- `credentials.hpp`: `FilePrivateKey`, `ServerKey`, `ServerIdentityOptions`, and
  `ServerIdentity::Load` / `Configure(SSL_CTX*)`.
- `grpc_credentials.hpp`: `InitializeGrpcServerPolicy` and
  `MakeGrpcServerCredentials`.

Functions throw `std::runtime_error` for configuration, decoding, and signer
failures; provider C callbacks translate failures into OpenSSL failure results.
The external provider is the module `linuxcnc-external-key.so`; pass its absolute
installed path to `ServerIdentity::Load`. No provider search-path or OpenSSL
configuration environment changes are required. File keys are loaded using the
default provider, cannot contain external references, and do not use the signer.
Encrypted file keys requiring passwords are unsupported.

An identity parses its certificate chain and trust bundle and validates its
public point and certificate/key match before use. Certificate contents and trust
roots are PEM strings, each bounded to 1 MiB. A file-key snapshot is bounded to
1 MiB. Identity
and root rotation require constructing a new identity/context/credentials and
replacing the listener; this API does not implement live rotation.

`Configure` applies the identity and policy to a fresh server `SSL_CTX`, including
a Boost.Asio context's `native_handle()`. It retains the identity implementation
and provider through context ex-data. The facade may then be released. Destroy
connections before destroying their context. If configuration throws, discard
the context. Do not mutate its identity, policy, or trust store after configuring.

For gRPC, the returned shared credentials retain the identity and provider.
Keep those credentials alive until the listener and all associated
handshakes have stopped. Application authorization remains the caller's concern.

```cpp
// Single-threaded daemon startup, before any SSL_CTX or gRPC initialization:
linuxcnc::transport::InitializeGrpcServerPolicy();
auto identity = linuxcnc::transport::ServerIdentity::Load(options, module_path);
auto credentials = linuxcnc::transport::MakeGrpcServerCredentials(identity);
// Keep credentials alive through server shutdown.
builder.AddListeningPort(endpoint, credentials);

// For another server frontend:
identity->Configure(asio_ssl_context.native_handle());
```

## TLS policy and gRPC process restriction

Both paths enforce TLS 1.3, zero early data, and zero session tickets. A fresh
connection must obtain a fresh CertificateVerify signature. Existing authenticated
connections are not revoked when a signer later becomes unavailable. Direct
SSL_CTX configuration additionally disables the session cache and ticket option.
Client certificates are required and verified by default; explicitly setting
`require_client_certificate=false` selects server-authenticated TLS and requires
empty `client_roots_pem`.

The qualified gRPC APIs do not expose their internal SSL_CTX. Therefore
`InitializeGrpcServerPolicy` installs an **in-memory process SSL_CONF default**
with `MinProtocol=MaxProtocol=TLSv1.3` and `NumTickets=0`, before any contexts,
gRPC initialization, or threads. This default remains installed until process
exit and is never restored. Reserve process-wide OpenSSL
SSL configuration for this module; reloading other configuration later can
invalidate the policy. Credential construction checks the effective defaults.

gRPC 1.80 and newer also receive the TLS 1.3 minimum and maximum through their
credential options. Earlier gRPC sets TLS 1.2 as the minimum on the SSL_CTX it
creates, overriding the process default, and has no option to raise it. There,
a gRPC listener with a **file key** also accepts TLS 1.2 (with TLS 1.2 session
tickets). External keys still complete only TLS 1.3, because the provider signs
nothing but a TLS 1.3 server CertificateVerify. Boost/WSS contexts are
configured directly and are TLS 1.3 only with either gRPC.

Use this initialization only in a dedicated server process. `NumTickets` is a
server-only SSL_CONF command: a later `TLS_client_method()` context in the same
process can fail when OpenSSL config diagnostics are enabled. Do not distribute
this configuration via `OPENSSL_CONF` or system files to client processes.
Separate Python/client processes retain their own configuration. Direct
`ServerIdentity::Configure` needs no process SSL_CONF default, and is appropriate
when a frontend exposes SSL_CTX. The library does not disable configuration
diagnostics to hide incompatible client initialization.

External identities support only `ecdsa_secp384r1_sha384` (0x0503). The transcript
hash can be SHA-256 or SHA-384, independently of the SHA-384 signature digest.
TLS 1.2, TLS client CertificateVerify, arbitrary messages, bare digests, and raw
EVP signing are refused. The provider validates the entire 130- or 146-byte
server CertificateVerify envelope before making a typed backend request.

## External key envelope

`EncodeExternalKey` emits canonical PEM with label
`LINUXCNC EXTERNAL TLS KEY`. Its decoded payload is, in order:

1. ASCII `LCK`, version byte `2`.
2. One-byte socket-path length, then one-byte key-ID length.
3. 97-byte SEC1 uncompressed P-384 point, validated on the curve.
4. Socket-path bytes, then key-ID bytes.

Paths are absolute filesystem Unix sockets, shorter than `sockaddr_un::sun_path`
and contain no NUL. IDs are 1..64 ASCII letters, digits, `_`, `-`, or `.`.
Unknown versions, invalid points, trailing data, noncanonical PEM, and arbitrary
URIs are rejected. The entire PEM input is at most 1024 bytes. The envelope is
public configuration, not an authorization credential or private-key container.

## Signer wire contract

AF_UNIX stream connections carry exactly one request and one response. Both
records start with a two-byte big-endian payload length, excluding the prefix;
the maximum complete record is 256 bytes. There is no multiplexing or retry.
The client handles fragmented reads/writes and uses one monotonic deadline for
connect, send, and receive, and sends with no SIGPIPE. It rejects
disconnects, denial, truncation, oversized records, malformed DER, and timeout.

TLS 1.3 servers sign before they receive a client certificate, so any peer that
reaches a listener can cause a signer request. The client does not limit
concurrent requests. While a signer's listen backlog is full, the client's
connect waits for room within its deadline instead of failing, so a signer that
serializes operations queues a burst in its backlog and the waiting connects.
This relies on Linux AF_UNIX connect semantics.
Listeners must abandon handshakes after `kServerHandshakeTimeout` (five
seconds). Each signer request has the shorter fixed deadline `kSignerTimeout`
(four seconds), including time queued at the signer, so a signature can still
complete a handshake and a serial signer can serve a burst of concurrent
handshakes. A slow signer can hold each handshaking listener thread for up to
four seconds.

The request payload is:

1. ASCII `LCS`, version byte `1`.
2. Operation byte `1`: TLS 1.3 **server** CertificateVerify.
3. Two-byte signature scheme `05 03`.
4. Transcript-hash byte: `1` for SHA-256, `2` for SHA-384.
5. One-byte key-ID length, key-ID bytes, then exactly 32 or 48 transcript bytes.

The backend must decode and authorize this typed operation and selected key,
independently construct `64 spaces || "TLS 1.3, server CertificateVerify" ||
0x00 || transcript_hash`, then sign that message with P-384/SHA-384. The helper
`BuildServerCertificateVerify` provides this reconstruction after validation;
it is not an authorization decision. Do not provide a raw digest/message signing
operation on this endpoint. The backend owns private-key protection and caller
authorization. Protect the socket and its parent directory against substitution;
the client does not authenticate a remote socket owner or enforce OS access policy.

The response payload is ASCII `LCS`, version `1`, status byte, and signature.
Status `0` requires canonical DER ECDSA, 8..104 bytes, with nonzero r/s below
the P-384 order. Status `1` is denial with no signature. Other statuses fail.
Structural validation is local; the TLS peer cryptographically verifies the
signature against the certified public key.

## Build, install, and qualification

The library is standalone or can be added with `add_subdirectory(native/transport)`.
The repository root exposes opt-in `LINUXCNC_BUILD_TRANSPORT`. Build options are
`LINUXCNC_TRANSPORT_BUILD_GRPC` (default ON) and
`LINUXCNC_TRANSPORT_BUILD_TESTS` (default `BUILD_TESTING`). Tests additionally
need Boost headers and host Python with `cryptography`. Production targets do
not depend on Boost or Python.

```sh
cmake -S native/transport -B /tmp/linuxcnc-transport-build \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLINUXCNC_TRANSPORT_BUILD_TESTS=ON
cmake --build /tmp/linuxcnc-transport-build
ctest --test-dir /tmp/linuxcnc-transport-build --output-on-failure
./scripts/native-format.sh check
./scripts/native-lint.sh /tmp/linuxcnc-transport-build
cmake --install /tmp/linuxcnc-transport-build --prefix /tmp/linuxcnc-transport-install
```

The installation provides `find_package(linuxcnc-transport CONFIG REQUIRED)`,
exported targets `linuxcnc::transport`, `linuxcnc::transport-grpc` (if built),
and `linuxcnc::external-key`. The latter exposes the provider module path through
`$<TARGET_FILE:linuxcnc::external-key>`. Dependency discovery uses OpenSSL,
Threads, and pkg-config `grpc++`. Cross builds and consumers must set the target
`PKG_CONFIG_LIBDIR` and `PKG_CONFIG_SYSROOT_DIR` rather than discovering host gRPC.
Set `CMAKE_CROSSCOMPILING_EMULATOR` to run the target CTest executables and pass
the same launcher to the Python integration harness.

Qualified library combinations: host gRPC 1.51.1 / OpenSSL 3.5.5, and AArch64
gRPC 1.80.0 / OpenSSL 3.6.4 / Boost 1.83 under qemu. Real produced credentials
complete HTTP/2 and WebSocket TLS handshakes with independently generated
ephemeral keys. The tests check peer verification, both transcript hashes,
certificate/key mismatch, strict reference decoding, no private export or raw
signing, signer faults, deadlines, concurrent handshakes, and no
ticket/resumption authentication bypass.
File-key handshakes verify that explicit file mode remains independent of the
external signer. Private test material stays in memory or anonymous memfd files;
temporary directories contain public certificates and socket paths only.

These tests qualify the software transport with a test signer. They do not
establish a production backend's key isolation, authorization, latency, or
behavior under real-time workloads.
