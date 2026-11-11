"""Execute daemon credential composition and telemetry admission over real sockets."""
import argparse
import base64
import contextlib
import datetime
import os
import pathlib
import select
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


def exact(connection, size):
    data = b""
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            raise EOFError("peer closed")
        data += chunk
    return data


class Signer:
    """Ephemeral backend for the public typed TLS signer protocol."""
    def __init__(self, path, key):
        self.path, self.key = str(path), key
        self.deny = False
        self.delay = 0
        self.requests = 0
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(self.path)
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(2)
                try:
                    size = struct.unpack("!H", exact(connection, 2))[0]
                    data = exact(connection, size)
                    assert data[:7] == b"LCS\x01\x01\x05\x03"
                    hash_id, id_size = data[7:9]
                    assert data[9:9 + id_size] == b"identity"
                    digest = data[9 + id_size:]
                    assert len(digest) == {1: 32, 2: 48}[hash_id]
                    self.requests += 1
                    time.sleep(self.delay)
                    if self.deny:
                        payload = b"LCS\x01\x01"
                    else:
                        message = b" " * 64 + b"TLS 1.3, server CertificateVerify\0" + digest
                        payload = b"LCS\x01\x00" + self.key.sign(message, ec.ECDSA(hashes.SHA384()))
                    connection.sendall(struct.pack("!H", len(payload)) + payload)
                except (OSError, EOFError):
                    pass

    def close(self):
        self.stopped.set()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive()
        self.listener.close()


def free_port():
    with socket.socket() as connection:
        connection.bind(("127.0.0.1", 0))
        return connection.getsockname()[1]


def rejected(operation):
    try:
        result = operation()
        if hasattr(result, "close"):
            result.close()
    except (OSError, EOFError):
        return
    raise AssertionError("unauthorized connection succeeded")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--grpc-version", required=True)
    parser.add_argument("--runner", nargs=argparse.REMAINDER, default=[])
    args = parser.parse_args()
    # gRPC before 1.80 cannot hold a file-key listener to TLS 1.3.
    grpc_tls13_only = tuple(int(part) for part in args.grpc_version.split(".")[:2]) >= (1, 80)
    shutdown_timeout = 10 if args.runner else 3
    now = datetime.datetime.now(datetime.timezone.utc)
    root_key, server_key, unknown_key = [ec.generate_private_key(ec.SECP384R1()) for _ in range(3)]
    # gRPC offers only the P-256 group, and TLS 1.2 also checks the client
    # certificate curve against it, so a P-256 client is needed for a TLS 1.2
    # attempt to reach the version check.
    client_key = ec.generate_private_key(ec.SECP256R1())
    name = lambda value: x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, value)])

    def certificate(key, subject, *, authority=False, server=False, issuer=None):
        builder = (x509.CertificateBuilder().subject_name(name(subject))
                   .issuer_name(name("Listener test CA")).public_key(key.public_key())
                   .serial_number(x509.random_serial_number())
                   .not_valid_before(now - datetime.timedelta(days=1))
                   .not_valid_after(now + datetime.timedelta(days=1))
                   .add_extension(x509.BasicConstraints(ca=authority, path_length=None), True))
        if server:
            builder = builder.add_extension(x509.SubjectAlternativeName([x509.DNSName("listener.test")]), False)
        if not authority:
            builder = builder.add_extension(x509.ExtendedKeyUsage([
                ExtendedKeyUsageOID.SERVER_AUTH if server else ExtendedKeyUsageOID.CLIENT_AUTH]), False)
        return builder.sign(issuer or root_key, hashes.SHA384()).public_bytes(serialization.Encoding.PEM)

    descriptors = []

    def memory_file(data):
        descriptor = os.memfd_create("ephemeral-listener-fixture")
        descriptors.append(descriptor)
        os.write(descriptor, data)
        return f"/proc/self/fd/{descriptor}"

    def private_pem(key):
        return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                 serialization.NoEncryption())

    try:
        with tempfile.TemporaryDirectory(prefix="linuxcnc-listeners-") as temporary:
            folder = pathlib.Path(temporary)
            root = folder / "ca.pem"
            root.write_bytes(certificate(root_key, "Listener test CA", authority=True))
            cert = folder / "server.pem"
            cert.write_bytes(certificate(server_key, "listener.test", server=True))
            private = memory_file(private_pem(server_key))
            # Likewise a P-256 file identity, so rejecting TLS 1.2 with it tests the
            # version policy, not curve support.
            file_key = ec.generate_private_key(ec.SECP256R1())
            file_cert = folder / "file-server.pem"
            file_cert.write_bytes(certificate(file_key, "listener.test", server=True))
            file_private = memory_file(private_pem(file_key))
            client_cert = memory_file(certificate(client_key, "client"))
            unknown_cert = memory_file(certificate(client_key, "unknown", issuer=unknown_key))
            client_private = memory_file(private_pem(client_key))
            signer = Signer(folder / "signer.sock", server_key)
            try:
                reference = folder / "external.pem"
                point = server_key.public_key().public_bytes(serialization.Encoding.X962,
                                                             serialization.PublicFormat.UncompressedPoint)
                endpoint = signer.path.encode()
                data = b"LCK\x02" + struct.pack("!BB", len(endpoint), 8) + point + endpoint + b"identity"
                encoded = base64.encodebytes(data).decode().replace("\n", "")
                reference.write_text("-----BEGIN LINUXCNC EXTERNAL TLS KEY-----\n" +
                                     "\n".join(encoded[i:i+64] for i in range(0, len(encoded), 64)) +
                                     "\n-----END LINUXCNC EXTERNAL TLS KEY-----\n")

                def command(options):
                    return args.runner + [args.server,
                        f"--endpoint=127.0.0.1:{grpc_port}", f"--telemetry-endpoint=127.0.0.1:{ws_port}",
                        f"--workspace-root={folder / 'workspaces'}",
                        f"--active-program-directory={folder / 'active'}"] + options

                def context(*, client=True, unknown=False, version=ssl.TLSVersion.TLSv1_3):
                    result = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    result.minimum_version = result.maximum_version = version
                    result.load_verify_locations(root)
                    result.set_alpn_protocols(["h2"])
                    if client:
                        result.load_cert_chain(unknown_cert if unknown else client_cert, client_private)
                    return result

                def tls(port, **options):
                    raw = socket.create_connection(("127.0.0.1", port), timeout=2)
                    try:
                        return context(**options).wrap_socket(raw, server_hostname="listener.test")
                    except BaseException:
                        raw.close()
                        raise

                def upgrade(connection, path):
                    connection.sendall((f"GET {path} HTTP/1.1\r\nHost: listener.test\r\n"
                        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n").encode())
                    response = b""
                    while not response.endswith(b"\r\n\r\n"):
                        response += exact(connection, 1)
                        assert len(response) < 16384
                    assert response.startswith(b"HTTP/1.1 101"), response
                    return connection

                def ws(path, **options):
                    connection = tls(ws_port, **options)
                    try:
                        return upgrade(connection, path)
                    except BaseException:
                        connection.close()
                        raise

                def binary(connection):
                    first, length = exact(connection, 2)
                    assert first == 0x82 and length & 0x80 == 0
                    if length == 126:
                        length = struct.unpack("!H", exact(connection, 2))[0]
                    elif length == 127:
                        length = struct.unpack("!Q", exact(connection, 8))[0]
                    assert 0 < length < 16384
                    return exact(connection, length)

                @contextlib.contextmanager
                def running(options):
                    with tempfile.TemporaryFile(mode="w+") as log:
                        process = subprocess.Popen(command(options), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=log, text=True, pass_fds=descriptors)
                        try:
                            ready, _, _ = select.select([process.stdout], [], [], 10)
                            line = process.stdout.readline() if ready else ""
                            if not line.startswith("READY "):
                                log.seek(0)
                                raise AssertionError(line + log.read())
                            yield process, line.strip().split()[1]
                        finally:
                            if process.poll() is None:
                                process.stdin.write("stop\n")
                                process.stdin.flush()
                            try:
                                process.wait(timeout=shutdown_timeout)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait()
                                log.seek(0)
                                raise AssertionError("listener shutdown exceeded deadline: " + log.read())
                            log.seek(0)
                            assert process.returncode == 0, log.read()

                # Credential and option failures must precede either bind.
                grpc_port, ws_port = free_port(), free_port()
                base = ["--tls", f"--tls-certificate={cert}"]
                external = [f"--tls-external-key-reference={reference}", f"--tls-provider-module={args.provider}"]
                invalid = [[], ["--mtls", "--allow-plaintext"],
                           ["--allow-plaintext", "--health-endpoint=0.0.0.0:50053"], base, base + external + [f"--tls-private-key={private}"],
                           base + [f"--tls-external-key-reference={reference}", "--tls-provider-module=relative.so"],
                           base + [f"--tls-external-key-reference={private}", f"--tls-provider-module={args.provider}"],
                           base + [f"--tls-private-key={reference}"],
                           base + [f"--tls-private-key={private}", "--mtls"],
                           base + ["--tls-external-key-reference=/missing/reference", f"--tls-provider-module={args.provider}"]]
                for options in invalid:
                    result = subprocess.run(command(options), capture_output=True, pass_fds=descriptors, timeout=5)
                    assert result.returncode == 2, result.stderr.decode()
                    for port in (grpc_port, ws_port):
                        with socket.socket() as probe:
                            probe.bind(("127.0.0.1", port))

                for certificate_path, key_options in ((file_cert, [f"--tls-private-key={file_private}"]),
                                                      (cert, external)):
                    signer.deny = False
                    grpc_port, ws_port = free_port(), free_port()
                    with running(["--tls", f"--tls-certificate={certificate_path}"] + key_options +
                                 ["--mtls", f"--tls-client-ca={root}"]) as (process, hal_path):
                        for port in (grpc_port, ws_port):
                            def plaintext():
                                with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
                                    connection.sendall(b"GET /v1/position-history HTTP/1.1\r\nHost: localhost\r\n\r\n")
                                    if not connection.recv(4096):
                                        raise EOFError()
                            rejected(plaintext)
                        for path in ("/v1/position-history", hal_path, "/v1/scope/invalid",
                                     "/v1/program-preview?workspace_id=invalid&relative_path=file.ngc"):
                            rejected(lambda: ws(path, client=False))
                            rejected(lambda: ws(path, unknown=True))
                        rejected(lambda: ws("/v1/position-history", version=ssl.TLSVersion.TLSv1_2))
                        with ws("/v1/position-history") as connection:
                            assert connection.version() == "TLSv1.3"
                            assert not connection.session.has_ticket
                            binary(connection)
                            # A valid authenticated client still cannot send application data.
                            connection.sendall(b"\x81\x81\x01\x02\x03\x04\x79")
                            opcode, length = exact(connection, 2)
                            assert opcode == 0x88
                            assert struct.unpack("!H", exact(connection, length)[:2])[0] == 1008
                        with ws(hal_path) as connection:
                            binary(connection)
                            rejected(lambda: ws(hal_path))
                        for path in ("/invalid", "/v1/hal-values/invalid", "/v1/scope/invalid"):
                            rejected(lambda: ws(path))
                        with tls(grpc_port) as connection:
                            connection.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n\x00\x00\x00\x04\x00\x00\x00\x00\x00")
                            assert len(connection.recv(1024)) >= 9
                            assert connection.selected_alpn_protocol() == "h2"
                        def grpc_preface(**options):
                            with tls(grpc_port, **options) as connection:
                                connection.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
                                if not connection.recv(1024):
                                    raise EOFError()
                        rejected(lambda: grpc_preface(client=False))
                        rejected(lambda: grpc_preface(unknown=True))
                        if key_options == external or grpc_tls13_only:
                            rejected(lambda: grpc_preface(version=ssl.TLSVersion.TLSv1_2))
                        if key_options == external:
                            before = signer.requests
                            with ws("/v1/position-history") as connection:
                                binary(connection)
                            assert signer.requests > before
                            signer.deny = True
                            rejected(lambda: ws("/v1/position-history"))
                            signer.deny = False
                        # Half-open TLS cannot occupy an admission slot forever, nor
                        # hold a gRPC handshake past the transport's deadline.
                        for port in (ws_port, grpc_port):
                            with socket.create_connection(("127.0.0.1", port), timeout=2) as idle:
                                idle.settimeout(7)
                                assert idle.recv(1) == b"", port
                        if key_options == external:
                            # Handshakes waiting at a slow serial signer must not stall
                            # sessions that are already streaming: a ping is answered
                            # on the streaming session's own worker.
                            with ws("/v1/position-history") as streaming:
                                binary(streaming)
                                before = signer.requests
                                signer.delay = 0.6

                                def waiting_handshake():
                                    try:
                                        with ws("/v1/position-history"):
                                            pass
                                    except (OSError, EOFError):
                                        pass

                                waiting = [threading.Thread(target=waiting_handshake) for _ in range(6)]
                                for thread in waiting:
                                    thread.start()
                                deadline = time.monotonic() + 2
                                while signer.requests == before and time.monotonic() < deadline:
                                    time.sleep(0.005)
                                assert signer.requests > before
                                time.sleep(0.2)
                                start = time.monotonic()
                                streaming.sendall(b"\x89\x80\x00\x00\x00\x00")  # masked empty ping
                                while True:
                                    first, length = exact(streaming, 2)
                                    length &= 0x7F
                                    if length == 126:
                                        length = struct.unpack("!H", exact(streaming, 2))[0]
                                    elif length == 127:
                                        length = struct.unpack("!Q", exact(streaming, 8))[0]
                                    exact(streaming, length)
                                    if first == 0x8A:
                                        break
                                elapsed = time.monotonic() - start
                                for thread in waiting:
                                    thread.join(timeout=6)
                                    assert not thread.is_alive()
                                signer.delay = 0
                                assert elapsed < 0.4, f"streaming session stalled {elapsed:.2f}s"
                        # Slow signing must not block accept/admission, and shutdown must
                        # drain upgraded, handshaking, and half-open sessions before exit.
                        live = ws("/v1/position-history")
                        idle = []
                        pending = None
                        if key_options == external:
                            before = signer.requests
                            signer.delay = 0.8
                            def pending_handshake():
                                try:
                                    with ws("/v1/position-history"):
                                        pass
                                except (OSError, EOFError):
                                    pass
                            pending = threading.Thread(target=pending_handshake)
                            pending.start()
                            deadline = time.monotonic() + 2
                            while signer.requests == before and time.monotonic() < deadline:
                                time.sleep(0.005)
                            assert signer.requests > before
                            idle = [socket.create_connection(("127.0.0.1", ws_port), timeout=2)
                                    for _ in range(14)]
                            start = time.monotonic()
                            with socket.create_connection(("127.0.0.1", ws_port), timeout=0.4) as overflow:
                                assert overflow.recv(1) == b""
                            assert time.monotonic() - start < 0.4, "signer blocked accept/admission"
                        else:
                            idle = [socket.create_connection(("127.0.0.1", ws_port), timeout=2)]
                        start = time.monotonic()
                        process.stdin.write("stop\n")
                        process.stdin.flush()
                        process.wait(timeout=shutdown_timeout)
                        assert time.monotonic() - start < shutdown_timeout
                        live.close()
                        for connection in idle:
                            connection.close()
                        if pending:
                            pending.join(timeout=2)
                            assert not pending.is_alive()
                            signer.delay = 0
                    print("TLS/WSS listener admission and shutdown:", "external key" if key_options == external else "file key", flush=True)

                # TLS without mTLS remains available deliberately; plaintext needs opt-in.
                grpc_port, ws_port = free_port(), free_port()
                with running(base + [f"--tls-private-key={private}"]):
                    with ws("/v1/position-history", client=False) as connection:
                        binary(connection)
                grpc_port, ws_port = free_port(), free_port()
                with running(["--allow-plaintext"]):
                    with socket.create_connection(("127.0.0.1", ws_port), timeout=2) as connection:
                        upgrade(connection, "/v1/position-history")
                        binary(connection)
                print("invalid startup leaves both ports unbound; explicit development WS works", flush=True)
            finally:
                signer.close()
    finally:
        for descriptor in descriptors:
            os.close(descriptor)


if __name__ == "__main__":
    main()
