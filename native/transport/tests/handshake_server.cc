#define BOOST_ERROR_CODE_HEADER_ONLY 1
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <fstream>
#include <iostream>
#include <iterator>

#include "linuxcnc_transport/credentials.hpp"
#include "linuxcnc_transport/grpc_credentials.hpp"
#include "linuxcnc_transport/signer.hpp"

namespace {
using namespace linuxcnc::transport;
using Bytes = std::vector<unsigned char>;
void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  Check(file.good(), "cannot read test public data");
  return {std::istreambuf_iterator<char>(file), {}};
}
void Probe(const std::shared_ptr<ServerIdentity>& identity) {
  const auto& pem = identity->key_pem();
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
      BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr),
      EVP_PKEY_free);
  Check(key != nullptr, "provider PEM decode failed");
  auto other_reference = DecodeExternalKey(pem);
  other_reference.socket_path += ".unavailable";
  other_reference.key_id = "other.identity";
  const auto other_pem = EncodeExternalKey(other_reference);
  std::unique_ptr<BIO, decltype(&BIO_free)> other_bio(
      BIO_new_mem_buf(other_pem.data(), static_cast<int>(other_pem.size())),
      BIO_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> other(
      PEM_read_bio_PrivateKey(other_bio.get(), nullptr, nullptr, nullptr),
      EVP_PKEY_free);
  Check(other != nullptr, "second independent reference failed to decode");
  BIGNUM* scalar = nullptr;
  const int exported =
      EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_PRIV_KEY, &scalar);
  BN_clear_free(scalar);
  Check(exported == 0, "private scalar exported");
  OSSL_PARAM* params = nullptr;
  const int exported_pair =
      EVP_PKEY_todata(key.get(), EVP_PKEY_KEYPAIR, &params);
  OSSL_PARAM_free(params);
  Check(exported_pair == 0, "private keypair exported");
  std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()),
                                                   BIO_free);
  Check(PEM_write_bio_PrivateKey(output.get(), key.get(), nullptr, nullptr, 0,
                                 nullptr, nullptr) == 0,
        "private PEM exported");
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> raw(
      EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr),
      EVP_PKEY_CTX_free);
  Check(raw && EVP_PKEY_sign_init(raw.get()) <= 0, "raw signing enabled");
  ERR_clear_error();
  const auto sign = [&](const Bytes& message, const char* digest,
                        EVP_PKEY* selected = nullptr) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(),
                                                               EVP_MD_CTX_free);
    Bytes signature(104);
    size_t size = signature.size();
    const bool ok =
        EVP_DigestSignInit_ex(md.get(), nullptr, digest, nullptr, nullptr,
                              selected ? selected : key.get(), nullptr) > 0 &&
        EVP_DigestSignUpdate(md.get(), message.data(), message.size()) > 0 &&
        EVP_DigestSignFinal(md.get(), signature.data(), &size) > 0;
    ERR_clear_error();
    if (!ok) return Bytes{};
    signature.resize(size);
    return signature;
  };
  std::unique_ptr<BIO, decltype(&BIO_free)> cert_bio(
      BIO_new_mem_buf(
          identity->certificate_chain_pem().data(),
          static_cast<int>(identity->certificate_chain_pem().size())),
      BIO_free);
  std::unique_ptr<X509, decltype(&X509_free)> cert(
      PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr), X509_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key(
      X509_get_pubkey(cert.get()), EVP_PKEY_free);
  for (const auto hash : {TranscriptHash::kSha256, TranscriptHash::kSha384}) {
    TlsServerSignRequest request{
        "identity", hash,
        Bytes(hash == TranscriptHash::kSha256 ? 32 : 48, 0xa5)};
    auto message = BuildServerCertificateVerify(request);
    Check(sign(message, "SHA384", other.get()).empty(),
          "second reference used the first key's signer");
    const auto signature = sign(message, "SHA384");
    Check(!signature.empty(), "typed server signature failed");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> verify(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    Check(EVP_DigestVerifyInit_ex(verify.get(), nullptr, "SHA384", nullptr,
                                  "provider=default", public_key.get(),
                                  nullptr) > 0 &&
              EVP_DigestVerify(verify.get(), signature.data(), signature.size(),
                               message.data(), message.size()) == 1,
          "external signature did not verify");
    Check(sign(message, "SHA256").empty(), "SHA256 signature accepted");
    Check(sign(request.transcript_hash, "SHA384").empty(),
          "bare digest accepted");
    auto malformed = message;
    malformed.pop_back();
    Check(sign(malformed, "SHA384").empty(), "truncated context accepted");
    for (size_t index : {size_t{0}, size_t{70}, size_t{73}, size_t{97}}) {
      malformed = message;
      malformed[index] ^= 1;
      Check(sign(malformed, "SHA384").empty(), "invalid TLS context accepted");
    }
    Check(sign(Bytes(message.size(), 0), "SHA384").empty(),
          "untyped message accepted");
  }
  // A provider may remain registered, but arbitrary URIs and malformed PEM must
  // still fail via the actual OpenSSL decoder path.
  for (const std::string& bad :
       {std::string("pkcs11:object=identity"), pem + "garbage"}) {
    std::unique_ptr<BIO, decltype(&BIO_free)> invalid(
        BIO_new_mem_buf(bad.data(), static_cast<int>(bad.size())), BIO_free);
    EVP_PKEY* decoded =
        PEM_read_bio_PrivateKey(invalid.get(), nullptr, nullptr, nullptr);
    const bool accepted = decoded != nullptr;
    EVP_PKEY_free(decoded);
    Check(!accepted, "invalid key reference accepted by OpenSSL");
    ERR_clear_error();
  }
  std::cout
      << "provider purpose, real signatures and no-export checks passed\n";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 8,
          "expected mode module cert roots point-or-file socket cipher");
    const std::string mode = argv[1];
    // This runs before contexts, gRPC initialization or worker threads.
    if (mode == "grpc" || mode == "grpc-file") InitializeGrpcServerPolicy();
    ServerIdentityOptions options;
    options.certificate_chain_pem = Read(argv[3]);
    options.client_roots_pem = Read(argv[4]);
    if (mode == "grpc-file" || mode == "wss-file") {
      options.key = FilePrivateKey{argv[5]};
    } else {
      ExternalKeyReference reference;
      reference.socket_path = argv[6];
      reference.key_id = "identity";
      const auto point = Read(argv[5]);
      Check(point.size() == 97, "invalid public test point");
      std::copy(point.begin(), point.end(), reference.public_point.begin());
      options.key = reference;
    }
    if (mode == "release") {
      // A released or failed identity must leave OpenSSL usable in a process
      // that had not used it before.
      try {
        (void)ServerIdentity::Load(std::move(options), argv[2]);
      } catch (const std::exception& error) {
        std::cout << "identity load failed: " << error.what() << '\n';
      }
      std::unique_ptr<EVP_MD, decltype(&EVP_MD_free)> digest(
          EVP_MD_fetch(nullptr, "SHA384", nullptr), EVP_MD_free);
      Check(digest != nullptr, "released identity disabled OpenSSL defaults");
      return 0;
    }
    auto identity = ServerIdentity::Load(std::move(options), argv[2]);
    if (mode == "probe") {
      Probe(identity);
      return 0;
    }
    if (mode == "grpc" || mode == "grpc-file") {
      grpc::EnableDefaultHealthCheckService(true);
      grpc::ServerBuilder builder;
      grpc::AsyncGenericService service;
      builder.RegisterAsyncGenericService(&service);
      auto queue = builder.AddCompletionQueue();
      auto credentials = MakeGrpcServerCredentials(identity);
      int port = 0;
      builder.AddListeningPort("127.0.0.1:0", credentials, &port);
      auto server = builder.BuildAndStart();
      Check(server && port > 0, "gRPC server could not start");
      std::cout << "READY " << port << '\n' << std::flush;
      server->Wait();
    } else {
      namespace asio = boost::asio;
      namespace websocket = boost::beast::websocket;
      using tcp = asio::ip::tcp;
      asio::ssl::context context(asio::ssl::context::tls_server);
      identity->Configure(context.native_handle());
      Check(SSL_CTX_set_ciphersuites(context.native_handle(), argv[7]) == 1,
            "test cipher configuration failed");
      // Configure retains the provider even when the identity facade goes away.
      identity.reset();
      asio::io_context io;
      tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
      std::cout << "READY " << listener.local_endpoint().port() << '\n'
                << std::flush;
      for (;;) {
        try {
          tcp::socket socket(io);
          listener.accept(socket);
          websocket::stream<asio::ssl::stream<tcp::socket>> ws(
              std::move(socket), context);
          ws.next_layer().handshake(asio::ssl::stream_base::server);
          ws.accept();
          ws.write(asio::buffer("verified", 8));
        } catch (const std::exception&) {
          ERR_clear_error();
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    ERR_print_errors_fp(stderr);
    return 1;
  }
}
