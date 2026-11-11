#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/core_object.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <string_view>

#include "linuxcnc_transport/external_key.hpp"
#include "linuxcnc_transport/signer.hpp"

namespace {
using linuxcnc::transport::ExternalKeyReference;
struct Provider {
  OSSL_FUNC_BIO_read_ex_fn* read = nullptr;
};
struct Key {
  ExternalKeyReference reference;
  bool external = false;
  bool public_set = false;
};
void* KeyNew(void*) { return new (std::nothrow) Key; }
void KeyFree(void* key) { delete static_cast<Key*>(key); }
void* KeyDup(const void* key, int) {
  try {
    return new Key(*static_cast<const Key*>(key));
  } catch (...) {
    return nullptr;
  }
}
void* KeyLoad(const void* reference, size_t size) {
  if (size != sizeof(Key*)) return nullptr;
  Key* key = nullptr;
  std::memcpy(static_cast<void*>(&key), reference, size);
  return key ? KeyDup(key, 0) : nullptr;
}
int KeyHas(const void* ptr, int selection) {
  const auto& key = *static_cast<const Key*>(ptr);
  if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) && !key.external) return 0;
  return key.public_set ? 1 : 0;
}
int KeyMatch(const void* left, const void* right, int selection) {
  const auto& a = *static_cast<const Key*>(left);
  const auto& b = *static_cast<const Key*>(right);
  if (!a.public_set || !b.public_set ||
      a.reference.public_point != b.reference.public_point)
    return 0;
  if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY)
    return a.external && b.external &&
           a.reference.key_id == b.reference.key_id &&
           a.reference.socket_path == b.reference.socket_path;
  return 1;
}
int KeyGet(void* ptr, OSSL_PARAM out[]) {
  const auto& key = *static_cast<Key*>(ptr);
  if (OSSL_PARAM_locate(out, OSSL_PKEY_PARAM_PRIV_KEY)) return 0;
  auto* p = OSSL_PARAM_locate(out, OSSL_PKEY_PARAM_BITS);
  if (p && !OSSL_PARAM_set_int(p, 384)) return 0;
  p = OSSL_PARAM_locate(out, OSSL_PKEY_PARAM_SECURITY_BITS);
  if (p && !OSSL_PARAM_set_int(p, 192)) return 0;
  p = OSSL_PARAM_locate(out, OSSL_PKEY_PARAM_MAX_SIZE);
  if (p && !OSSL_PARAM_set_int(p, 104)) return 0;
  p = OSSL_PARAM_locate(out, OSSL_PKEY_PARAM_GROUP_NAME);
  if (p && !OSSL_PARAM_set_utf8_string(p, "secp384r1")) return 0;
  p = OSSL_PARAM_locate(out, OSSL_PKEY_PARAM_DEFAULT_DIGEST);
  if (p && !OSSL_PARAM_set_utf8_string(p, "SHA384")) return 0;
  for (const char* name :
       {OSSL_PKEY_PARAM_PUB_KEY, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY}) {
    p = OSSL_PARAM_locate(out, name);
    if (p &&
        !OSSL_PARAM_set_octet_string(p, key.reference.public_point.data(), 97))
      return 0;
  }
  return 1;
}
const OSSL_PARAM* KeyGettable(void*) {
  static const OSSL_PARAM params[] = {
      OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS, nullptr),
      OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS, nullptr),
      OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE, nullptr),
      OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, nullptr, 0),
      OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_DEFAULT_DIGEST, nullptr, 0),
      OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0),
      OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, nullptr, 0),
      OSSL_PARAM_END};
  return params;
}
int KeyExport(void* ptr, int selection, OSSL_CALLBACK* callback,
              void* argument) {
  if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) return 0;
  auto& key = *static_cast<Key*>(ptr);
  if (!key.public_set) return 0;
  char group[] = "secp384r1";
  OSSL_PARAM params[] = {
      OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group)),
      OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY,
                              key.reference.public_point.data(), 97),
      OSSL_PARAM_END};
  return callback(params, argument);
}
const OSSL_PARAM* KeyTypes(int selection) {
  static const OSSL_PARAM params[] = {
      OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, nullptr, 0),
      OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0),
      OSSL_PARAM_END};
  return selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY ? nullptr : params;
}
int KeyImport(void* ptr, int selection, const OSSL_PARAM params[]) {
  if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) return 0;
  const auto* point = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PUB_KEY);
  const auto* group =
      OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_GROUP_NAME);
  if (!point || point->data_type != OSSL_PARAM_OCTET_STRING ||
      point->data_size != 97 || !group ||
      group->data_type != OSSL_PARAM_UTF8_STRING || !group->data ||
      std::string_view(static_cast<const char*>(group->data),
                       group->data_size) != "secp384r1")
    return 0;
  auto& key = *static_cast<Key*>(ptr);
  std::memcpy(key.reference.public_point.data(), point->data, 97);
  key.public_set = true;
  key.external = false;
  return 1;
}
const char* KeyOperation(int operation) {
  return operation == OSSL_OP_SIGNATURE ? "ECDSA" : nullptr;
}

void* DecoderNew(void* context) { return context; }
void DecoderFree(void*) {}
int DecoderSelection(void*, int selection) {
  return (selection & OSSL_KEYMGMT_SELECT_KEYPAIR) != 0;
}
int Decode(void* context, OSSL_CORE_BIO* input, int, OSSL_CALLBACK* callback,
           void* argument, OSSL_PASSPHRASE_CALLBACK*, void*) {
  try {
    auto* provider = static_cast<Provider*>(context);
    std::array<char, 1025> bytes{};
    size_t total = 0;
    while (total < bytes.size()) {
      size_t count = 0;
      const int ok = provider->read(input, bytes.data() + total,
                                    bytes.size() - total, &count);
      total += count;
      if (!ok || count == 0) break;
    }
    if (total == bytes.size()) return 1;
    Key key;
    key.reference =
        linuxcnc::transport::DecodeExternalKey({bytes.data(), total});
    key.external = true;
    key.public_set = true;
    Key* reference = &key;
    int type = OSSL_OBJECT_PKEY;
    char name[] = "EC";
    OSSL_PARAM params[] = {
        OSSL_PARAM_int(OSSL_OBJECT_PARAM_TYPE, &type),
        OSSL_PARAM_utf8_string(OSSL_OBJECT_PARAM_DATA_TYPE, name, sizeof(name)),
        OSSL_PARAM_octet_string(OSSL_OBJECT_PARAM_REFERENCE, &reference,
                                sizeof(Key*)),
        OSSL_PARAM_END};
    return callback(params, argument);
  } catch (...) {
    return 1;
  }  // unrecognized input; no object produced
}

struct Signature {
  Key key;
  std::array<unsigned char, 146> message{};
  size_t size = 0;
  bool initialized = false;
};
void* SignatureNew(void*, const char*) { return new (std::nothrow) Signature; }
void SignatureFree(void* ptr) { delete static_cast<Signature*>(ptr); }
void* SignatureDup(void* ptr) {
  try {
    return new Signature(*static_cast<Signature*>(ptr));
  } catch (...) {
    return nullptr;
  }
}
bool IsSha384(std::string_view name) {
  return name == "SHA384" || name == "SHA2-384" || name == "SHA-384";
}
int SignatureSet(void*, const OSSL_PARAM params[]) {
  if (!params) return 1;
  const auto* digest =
      OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_DIGEST);
  if (!digest) return 1;
  const char* name = nullptr;
  return OSSL_PARAM_get_utf8_string_ptr(digest, &name) && name &&
         IsSha384(name);
}
const OSSL_PARAM* SignatureSettable(void*, void*) {
  static const OSSL_PARAM params[] = {
      OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, nullptr, 0),
      OSSL_PARAM_END};
  return params;
}
int SignatureInit(void* ptr, const char* digest, void* key,
                  const OSSL_PARAM params[]) {
  auto& signature = *static_cast<Signature*>(ptr);
  signature.initialized = false;
  signature.size = 0;
  if (!digest || !IsSha384(digest) || !key ||
      !static_cast<Key*>(key)->external || !SignatureSet(ptr, params))
    return 0;
  try {
    signature.key = *static_cast<Key*>(key);
    signature.initialized = true;
    return 1;
  } catch (...) {
    return 0;
  }
}
int SignatureUpdate(void* ptr, const unsigned char* bytes, size_t size) {
  auto& signature = *static_cast<Signature*>(ptr);
  if (!signature.initialized ||
      size > signature.message.size() - signature.size) {
    signature.initialized = false;
    return 0;
  }
  if (size) std::memcpy(signature.message.data() + signature.size, bytes, size);
  signature.size += size;
  return 1;
}
int SignatureFinal(void* ptr, unsigned char* output, size_t* size,
                   size_t capacity) {
  auto& signature = *static_cast<Signature*>(ptr);
  if (!signature.initialized || !size) return 0;
  if (!output) {
    *size = 104;
    return 1;
  }
  if (capacity < 104) return 0;
  signature.initialized = false;
  constexpr std::string_view context = "TLS 1.3, server CertificateVerify";
  if ((signature.size != 130 && signature.size != 146) ||
      !std::all_of(signature.message.begin(), signature.message.begin() + 64,
                   [](unsigned char c) { return c == 0x20; }) ||
      std::memcmp(signature.message.data() + 64, context.data(),
                  context.size()) != 0 ||
      signature.message[97] != 0)
    return 0;
  try {
    linuxcnc::transport::TlsServerSignRequest request;
    request.key_id = signature.key.reference.key_id;
    request.hash = signature.size == 130
                       ? linuxcnc::transport::TranscriptHash::kSha256
                       : linuxcnc::transport::TranscriptHash::kSha384;
    request.transcript_hash.assign(signature.message.begin() + 98,
                                   signature.message.begin() + signature.size);
    const auto der = linuxcnc::transport::RequestServerSignature(
        signature.key.reference, request);
    std::memcpy(output, der.data(), der.size());
    *size = der.size();
    return 1;
  } catch (...) {
    return 0;
  }
}
int SignatureOneShot(void* ptr, unsigned char* output, size_t* size,
                     size_t capacity, const unsigned char* bytes,
                     size_t count) {
  if (!output) return SignatureFinal(ptr, output, size, capacity);
  return SignatureUpdate(ptr, bytes, count) &&
         SignatureFinal(ptr, output, size, capacity);
}

#define FUNCTION(id, fn) {id, reinterpret_cast<void (*)(void)>(fn)}
const OSSL_DISPATCH kKeyFunctions[] = {
    FUNCTION(OSSL_FUNC_KEYMGMT_NEW, KeyNew),
    FUNCTION(OSSL_FUNC_KEYMGMT_FREE, KeyFree),
    FUNCTION(OSSL_FUNC_KEYMGMT_DUP, KeyDup),
    FUNCTION(OSSL_FUNC_KEYMGMT_LOAD, KeyLoad),
    FUNCTION(OSSL_FUNC_KEYMGMT_HAS, KeyHas),
    FUNCTION(OSSL_FUNC_KEYMGMT_MATCH, KeyMatch),
    FUNCTION(OSSL_FUNC_KEYMGMT_GET_PARAMS, KeyGet),
    FUNCTION(OSSL_FUNC_KEYMGMT_GETTABLE_PARAMS, KeyGettable),
    FUNCTION(OSSL_FUNC_KEYMGMT_EXPORT, KeyExport),
    FUNCTION(OSSL_FUNC_KEYMGMT_EXPORT_TYPES, KeyTypes),
    FUNCTION(OSSL_FUNC_KEYMGMT_IMPORT, KeyImport),
    FUNCTION(OSSL_FUNC_KEYMGMT_IMPORT_TYPES, KeyTypes),
    FUNCTION(OSSL_FUNC_KEYMGMT_QUERY_OPERATION_NAME, KeyOperation),
    {0, nullptr}};
const OSSL_DISPATCH kDecoderFunctions[] = {
    FUNCTION(OSSL_FUNC_DECODER_NEWCTX, DecoderNew),
    FUNCTION(OSSL_FUNC_DECODER_FREECTX, DecoderFree),
    FUNCTION(OSSL_FUNC_DECODER_DOES_SELECTION, DecoderSelection),
    FUNCTION(OSSL_FUNC_DECODER_DECODE, Decode),
    {0, nullptr}};
const OSSL_DISPATCH kSignatureFunctions[] = {
    FUNCTION(OSSL_FUNC_SIGNATURE_NEWCTX, SignatureNew),
    FUNCTION(OSSL_FUNC_SIGNATURE_FREECTX, SignatureFree),
    FUNCTION(OSSL_FUNC_SIGNATURE_DUPCTX, SignatureDup),
    FUNCTION(OSSL_FUNC_SIGNATURE_DIGEST_SIGN_INIT, SignatureInit),
    FUNCTION(OSSL_FUNC_SIGNATURE_DIGEST_SIGN_UPDATE, SignatureUpdate),
    FUNCTION(OSSL_FUNC_SIGNATURE_DIGEST_SIGN_FINAL, SignatureFinal),
    FUNCTION(OSSL_FUNC_SIGNATURE_DIGEST_SIGN, SignatureOneShot),
    FUNCTION(OSSL_FUNC_SIGNATURE_SET_CTX_PARAMS, SignatureSet),
    FUNCTION(OSSL_FUNC_SIGNATURE_SETTABLE_CTX_PARAMS, SignatureSettable),
    {0, nullptr}};
const OSSL_ALGORITHM kKeys[] = {
    {"EC:id-ecPublicKey:1.2.840.10045.2.1", "provider=linuxcnc_external",
     kKeyFunctions, "external P-384 TLS server key"},
    {nullptr, nullptr, nullptr, nullptr}};
const OSSL_ALGORITHM kDecoders[] = {
    {"EC", "provider=linuxcnc_external,input=pem", kDecoderFunctions,
     "versioned external TLS key reference"},
    {nullptr, nullptr, nullptr, nullptr}};
const OSSL_ALGORITHM kSignatures[] = {
    {"ECDSA", "provider=linuxcnc_external", kSignatureFunctions,
     "TLS 1.3 server CertificateVerify only"},
    {nullptr, nullptr, nullptr, nullptr}};
const OSSL_ALGORITHM* Query(void*, int operation, int* no_cache) {
  *no_cache = 0;
  switch (operation) {
    case OSSL_OP_KEYMGMT:
      return kKeys;
    case OSSL_OP_DECODER:
      return kDecoders;
    case OSSL_OP_SIGNATURE:
      return kSignatures;
    default:
      return nullptr;
  }
}
void Teardown(void* context) { delete static_cast<Provider*>(context); }
const OSSL_DISPATCH kProviderFunctions[] = {
    FUNCTION(OSSL_FUNC_PROVIDER_QUERY_OPERATION, Query),
    FUNCTION(OSSL_FUNC_PROVIDER_TEARDOWN, Teardown),
    {0, nullptr}};
#undef FUNCTION
}  // namespace

extern "C" int OSSL_provider_init(const OSSL_CORE_HANDLE*,
                                  const OSSL_DISPATCH* in,
                                  const OSSL_DISPATCH** out, void** context) {
  auto* provider = new (std::nothrow) Provider;
  if (!provider) return 0;
  for (; in->function_id; ++in)
    if (in->function_id == OSSL_FUNC_BIO_READ_EX)
      provider->read = OSSL_FUNC_BIO_read_ex(in);
  if (!provider->read) {
    delete provider;
    return 0;
  }
  *context = provider;
  *out = kProviderFunctions;
  return 1;
}
