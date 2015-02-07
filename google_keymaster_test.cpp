/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <openssl/engine.h>

#include <keymaster/google_keymaster_utils.h>
#include <keymaster/keymaster_tags.h>
#include <keymaster/soft_keymaster_device.h>

#include "google_keymaster_test_utils.h"

using std::ifstream;
using std::istreambuf_iterator;
using std::string;
using std::vector;

int main(int argc, char** argv) {
    ERR_load_crypto_strings();
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    // Clean up stuff OpenSSL leaves around, so Valgrind doesn't complain.
    CRYPTO_cleanup_all_ex_data();
    ERR_remove_thread_state(NULL);
    ERR_free_strings();
    return result;
}

template <typename T> std::ostream& operator<<(std::ostream& os, const std::vector<T>& vec) {
    os << "{ ";
    bool first = true;
    for (T t : vec) {
        os << (first ? "" : ", ") << t;
        if (first)
            first = false;
    }
    os << " }";
    return os;
}

namespace keymaster {
namespace test {

/**
 * Utility class to make construction of AuthorizationSets easy, and readable.  Use like:
 *
 * ParamBuilder()
 *     .Option(TAG_ALGORITHM, KM_ALGORITHM_RSA)
 *     .Option(TAG_KEY_SIZE, 512)
 *     .Option(TAG_DIGEST, KM_DIGEST_NONE)
 *     .Option(TAG_PADDING, KM_PAD_NONE)
 *     .Option(TAG_SINGLE_USE_PER_BOOT, true)
 *     .build();
 *
 * In addition there are methods that add common sets of parameters, like RsaSigningKey().
 */
class ParamBuilder {
  public:
    template <typename TagType, typename ValueType>
    ParamBuilder& Option(TagType tag, ValueType value) {
        set.push_back(tag, value);
        return *this;
    }

    template <keymaster_tag_t Tag> ParamBuilder& Option(TypedTag<KM_BOOL, Tag> tag) {
        set.push_back(tag);
        return *this;
    }

    ParamBuilder& RsaKey(uint32_t key_size = 0, uint64_t public_exponent = 0) {
        Option(TAG_ALGORITHM, KM_ALGORITHM_RSA);
        if (key_size != 0)
            Option(TAG_KEY_SIZE, key_size);
        if (public_exponent != 0)
            Option(TAG_RSA_PUBLIC_EXPONENT, public_exponent);
        return *this;
    }

    ParamBuilder& EcdsaKey(uint32_t key_size = 0) {
        Option(TAG_ALGORITHM, KM_ALGORITHM_ECDSA);
        if (key_size != 0)
            Option(TAG_KEY_SIZE, key_size);
        return *this;
    }

    ParamBuilder& AesKey(uint32_t key_size) {
        Option(TAG_ALGORITHM, KM_ALGORITHM_AES);
        return Option(TAG_KEY_SIZE, key_size);
    }

    ParamBuilder& HmacKey(uint32_t key_size, keymaster_digest_t digest, uint32_t mac_length) {
        Option(TAG_ALGORITHM, KM_ALGORITHM_HMAC);
        Option(TAG_KEY_SIZE, key_size);
        SigningKey();
        Option(TAG_DIGEST, digest);
        return Option(TAG_MAC_LENGTH, mac_length);
    }

    ParamBuilder& RsaSigningKey(uint32_t key_size = 0, keymaster_digest_t digest = KM_DIGEST_NONE,
                                keymaster_padding_t padding = KM_PAD_NONE,
                                uint64_t public_exponent = 0) {
        RsaKey(key_size, public_exponent);
        SigningKey();
        Option(TAG_DIGEST, digest);
        return Option(TAG_PADDING, padding);
    }

    ParamBuilder& RsaEncryptionKey(uint32_t key_size = 0,
                                   keymaster_padding_t padding = KM_PAD_RSA_OAEP,
                                   uint64_t public_exponent = 0) {
        RsaKey(key_size, public_exponent);
        EncryptionKey();
        return Option(TAG_PADDING, padding);
    }

    ParamBuilder& EcdsaSigningKey(uint32_t key_size = 0) {
        EcdsaKey(key_size);
        return SigningKey();
    }

    ParamBuilder& AesEncryptionKey(uint32_t key_size = 128) {
        AesKey(key_size);
        return EncryptionKey();
    }

    ParamBuilder& SigningKey() {
        Option(TAG_PURPOSE, KM_PURPOSE_SIGN);
        return Option(TAG_PURPOSE, KM_PURPOSE_VERIFY);
    }

    ParamBuilder& EncryptionKey() {
        Option(TAG_PURPOSE, KM_PURPOSE_ENCRYPT);
        return Option(TAG_PURPOSE, KM_PURPOSE_DECRYPT);
    }

    ParamBuilder& NoDigestOrPadding() {
        Option(TAG_DIGEST, KM_DIGEST_NONE);
        return Option(TAG_PADDING, KM_PAD_NONE);
    }

    ParamBuilder& OcbMode(uint32_t chunk_length, uint32_t mac_length) {
        Option(TAG_BLOCK_MODE, KM_MODE_OCB);
        Option(TAG_CHUNK_LENGTH, chunk_length);
        return Option(TAG_MAC_LENGTH, mac_length);
    }

    AuthorizationSet build() const { return set; }

  private:
    AuthorizationSet set;
};

inline string make_string(const uint8_t* data, size_t length) {
    return string(reinterpret_cast<const char*>(data), length);
}

template <size_t N> string make_string(const uint8_t(&a)[N]) {
    return make_string(a, N);
}

static string make_string(const uint8_t(&)[0]) {
    return string();
}

StdoutLogger logger;

const uint64_t OP_HANDLE_SENTINEL = 0xFFFFFFFFFFFFFFFF;
class KeymasterTest : public testing::Test {
  protected:
    KeymasterTest() : op_handle_(OP_HANDLE_SENTINEL), characteristics_(NULL) {
        blob_.key_material = NULL;
        RAND_seed("foobar", 6);
        blob_.key_material = 0;
    }

    ~KeymasterTest() {
        FreeCharacteristics();
        FreeKeyBlob();
    }

    keymaster1_device_t* device() {
        return reinterpret_cast<keymaster1_device_t*>(device_.hw_device());
    }

    keymaster_error_t GenerateKey(const ParamBuilder& builder) {
        AuthorizationSet params(builder.build());
        params.push_back(UserAuthParams());
        params.push_back(ClientParams());

        FreeKeyBlob();
        FreeCharacteristics();
        return device()->generate_key(device(), params.data(), params.size(), &blob_,
                                      &characteristics_);
    }

    keymaster_error_t ImportKey(const ParamBuilder& builder, keymaster_key_format_t format,
                                const string& key_material) {
        AuthorizationSet params(builder.build());
        params.push_back(UserAuthParams());
        params.push_back(ClientParams());

        FreeKeyBlob();
        FreeCharacteristics();
        return device()->import_key(device(), params.data(), params.size(), format,
                                    reinterpret_cast<const uint8_t*>(key_material.c_str()),
                                    key_material.length(), &blob_, &characteristics_);
    }

    AuthorizationSet UserAuthParams() {
        AuthorizationSet set;
        set.push_back(TAG_USER_ID, 7);
        set.push_back(TAG_USER_AUTH_ID, 8);
        set.push_back(TAG_AUTH_TIMEOUT, 300);
        return set;
    }

    AuthorizationSet ClientParams() {
        AuthorizationSet set;
        set.push_back(TAG_APPLICATION_ID, "app_id", 6);
        return set;
    }

    keymaster_error_t BeginOperation(keymaster_purpose_t purpose) {
        keymaster_key_param_t* out_params = NULL;
        size_t out_params_count = 0;
        keymaster_error_t error =
            device()->begin(device(), purpose, &blob_, client_params_, array_length(client_params_),
                            &out_params, &out_params_count, &op_handle_);
        EXPECT_EQ(0, out_params_count);
        EXPECT_TRUE(out_params == NULL);
        return error;
    }

    keymaster_error_t BeginOperation(keymaster_purpose_t purpose, const AuthorizationSet& input_set,
                                     AuthorizationSet* output_set = NULL) {
        AuthorizationSet additional_params(client_params_, array_length(client_params_));
        additional_params.push_back(input_set);

        keymaster_key_param_t* out_params;
        size_t out_params_count;
        keymaster_error_t error =
            device()->begin(device(), purpose, &blob_, additional_params.data(),
                            additional_params.size(), &out_params, &out_params_count, &op_handle_);
        if (output_set) {
            output_set->Reinitialize(out_params, out_params_count);
        } else {
            EXPECT_EQ(0, out_params_count);
            EXPECT_TRUE(out_params == NULL);
        }
        keymaster_free_param_values(out_params, out_params_count);
        free(out_params);
        return error;
    }

    keymaster_error_t UpdateOperation(const string& message, string* output,
                                      size_t* input_consumed) {
        uint8_t* out_tmp = NULL;
        size_t out_length;
        EXPECT_NE(op_handle_, OP_HANDLE_SENTINEL);
        keymaster_error_t error =
            device()->update(device(), op_handle_, NULL /* params */, 0 /* params_count */,
                             reinterpret_cast<const uint8_t*>(message.c_str()), message.length(),
                             input_consumed, &out_tmp, &out_length);
        if (out_tmp)
            output->append(reinterpret_cast<char*>(out_tmp), out_length);
        free(out_tmp);
        return error;
    }

    keymaster_error_t UpdateOperation(const AuthorizationSet& additional_params,
                                      const string& message, string* output,
                                      size_t* input_consumed) {
        uint8_t* out_tmp = NULL;
        size_t out_length;
        EXPECT_NE(op_handle_, OP_HANDLE_SENTINEL);
        keymaster_error_t error = device()->update(
            device(), op_handle_, additional_params.data(), additional_params.size(),
            reinterpret_cast<const uint8_t*>(message.c_str()), message.length(), input_consumed,
            &out_tmp, &out_length);
        if (out_tmp)
            output->append(reinterpret_cast<char*>(out_tmp), out_length);
        free(out_tmp);
        return error;
    }

    keymaster_error_t FinishOperation(string* output) { return FinishOperation("", output); }

    keymaster_error_t FinishOperation(const string& signature, string* output) {
        AuthorizationSet additional_params;
        return FinishOperation(additional_params, signature, output);
    }

    keymaster_error_t FinishOperation(const AuthorizationSet& additional_params,
                                      const string& signature, string* output) {
        uint8_t* out_tmp = NULL;
        size_t out_length;
        keymaster_error_t error = device()->finish(
            device(), op_handle_, additional_params.data(), additional_params.size(),
            reinterpret_cast<const uint8_t*>(signature.c_str()), signature.length(), &out_tmp,
            &out_length);
        if (out_tmp)
            output->append(reinterpret_cast<char*>(out_tmp), out_length);
        free(out_tmp);
        return error;
    }

    template <typename T>
    bool ResponseContains(const vector<T>& expected, const T* values, size_t len) {
        return expected.size() == len &&
               std::is_permutation(values, values + len, expected.begin());
    }

    template <typename T> bool ResponseContains(T expected, const T* values, size_t len) {
        return (len == 1 && *values == expected);
    }

    keymaster_error_t AbortOperation() { return device()->abort(device(), op_handle_); }

    string ProcessMessage(keymaster_purpose_t purpose, const string& message) {
        AuthorizationSet input_params;
        EXPECT_EQ(KM_ERROR_OK, BeginOperation(purpose, input_params, NULL /* output_params */));

        string result;
        size_t input_consumed;
        EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
        EXPECT_EQ(message.size(), input_consumed);
        EXPECT_EQ(KM_ERROR_OK, FinishOperation(&result));
        return result;
    }

    string ProcessMessage(keymaster_purpose_t purpose, const string& message,
                          const AuthorizationSet& begin_params,
                          const AuthorizationSet& update_params,
                          AuthorizationSet* output_params = NULL) {
        EXPECT_EQ(KM_ERROR_OK, BeginOperation(purpose, begin_params, output_params));

        string result;
        size_t input_consumed;
        EXPECT_EQ(KM_ERROR_OK, UpdateOperation(update_params, message, &result, &input_consumed));
        EXPECT_EQ(message.size(), input_consumed);
        EXPECT_EQ(KM_ERROR_OK, FinishOperation(update_params, "", &result));
        return result;
    }

    string ProcessMessage(keymaster_purpose_t purpose, const string& message,
                          const string& signature) {
        AuthorizationSet input_params;
        EXPECT_EQ(KM_ERROR_OK, BeginOperation(purpose, input_params, NULL /* output_params */));

        string result;
        size_t input_consumed;
        EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
        EXPECT_EQ(message.size(), input_consumed);
        EXPECT_EQ(KM_ERROR_OK, FinishOperation(signature, &result));
        return result;
    }

    void SignMessage(const string& message, string* signature) {
        SCOPED_TRACE("SignMessage");
        *signature = ProcessMessage(KM_PURPOSE_SIGN, message);
        EXPECT_GT(signature->size(), 0);
    }

    void VerifyMessage(const string& message, const string& signature) {
        SCOPED_TRACE("VerifyMessage");
        ProcessMessage(KM_PURPOSE_VERIFY, message, signature);
    }

    string EncryptMessage(const string& message, string* generated_nonce = NULL) {
        AuthorizationSet update_params;
        return EncryptMessage(update_params, message, generated_nonce);
    }

    string EncryptMessage(const AuthorizationSet& update_params, const string& message,
                          string* generated_nonce = NULL) {
        SCOPED_TRACE("EncryptMessage");
        AuthorizationSet begin_params, output_params;
        string ciphertext = ProcessMessage(KM_PURPOSE_ENCRYPT, message, begin_params, update_params,
                                           &output_params);
        if (generated_nonce) {
            keymaster_blob_t nonce_blob;
            EXPECT_TRUE(output_params.GetTagValue(TAG_NONCE, &nonce_blob));
            *generated_nonce = make_string(nonce_blob.data, nonce_blob.data_length);
        } else {
            EXPECT_EQ(-1, output_params.find(TAG_NONCE));
        }
        return ciphertext;
    }

    string EncryptMessageWithParams(const string& message, const AuthorizationSet& begin_params,
                                    const AuthorizationSet& update_params,
                                    AuthorizationSet* output_params) {
        SCOPED_TRACE("EncryptMessageWithParams");
        return ProcessMessage(KM_PURPOSE_ENCRYPT, message, begin_params, update_params,
                              output_params);
    }

    string DecryptMessage(const string& ciphertext) {
        SCOPED_TRACE("DecryptMessage");
        return ProcessMessage(KM_PURPOSE_DECRYPT, ciphertext);
    }

    string DecryptMessage(const string& ciphertext, const string& nonce) {
        SCOPED_TRACE("DecryptMessage");
        AuthorizationSet update_params;
        return DecryptMessage(update_params, ciphertext, nonce);
    }

    string DecryptMessage(const AuthorizationSet& update_params, const string& ciphertext,
                          const string& nonce) {
        SCOPED_TRACE("DecryptMessage");
        AuthorizationSet begin_params;
        begin_params.push_back(TAG_NONCE, nonce.data(), nonce.size());
        return ProcessMessage(KM_PURPOSE_DECRYPT, ciphertext, begin_params, update_params);
    }

    keymaster_error_t GetCharacteristics() {
        FreeCharacteristics();
        return device()->get_key_characteristics(device(), &blob_, &client_id_, NULL /* app_data */,
                                                 &characteristics_);
    }

    keymaster_error_t ExportKey(keymaster_key_format_t format, string* export_data) {
        uint8_t* export_data_tmp;
        size_t export_data_length;

        keymaster_error_t error =
            device()->export_key(device(), format, &blob_, &client_id_, NULL /* app_data */,
                                 &export_data_tmp, &export_data_length);

        if (error != KM_ERROR_OK)
            return error;

        *export_data = string(reinterpret_cast<char*>(export_data_tmp), export_data_length);
        free(export_data_tmp);
        return error;
    }

    keymaster_error_t GetVersion(uint8_t* major, uint8_t* minor, uint8_t* subminor) {
        GetVersionRequest request;
        GetVersionResponse response;
        device_.GetVersion(request, &response);
        if (response.error != KM_ERROR_OK)
            return response.error;
        *major = response.major_ver;
        *minor = response.minor_ver;
        *subminor = response.subminor_ver;
        return response.error;
    }

    void CheckHmacTestVector(string key, string message, keymaster_digest_t digest,
                             string expected_mac) {
        ASSERT_EQ(KM_ERROR_OK,
                  ImportKey(ParamBuilder().HmacKey(key.size() * 8, digest, expected_mac.size()),
                            KM_KEY_FORMAT_RAW, key));
        string signature;
        SignMessage(message, &signature);
        EXPECT_EQ(expected_mac, signature) << "Test vector didn't match for digest " << digest;
    }

    void CheckAesOcbTestVector(const string& key, const string& nonce,
                               const string& associated_data, const string& message,
                               const string& expected_ciphertext) {
        ASSERT_EQ(KM_ERROR_OK, ImportKey(ParamBuilder()
                                             .AesEncryptionKey(key.size() * 8)
                                             .OcbMode(4096 /* chunk length */, 16 /* tag length */)
                                             .Option(TAG_CALLER_NONCE),
                                         KM_KEY_FORMAT_RAW, key));

        AuthorizationSet begin_params, update_params, output_params;
        begin_params.push_back(TAG_NONCE, nonce.data(), nonce.size());
        update_params.push_back(TAG_ASSOCIATED_DATA, associated_data.data(),
                                associated_data.size());
        string ciphertext =
            EncryptMessageWithParams(message, begin_params, update_params, &output_params);
        EXPECT_EQ(expected_ciphertext, ciphertext);
    }

    AuthorizationSet hw_enforced() {
        EXPECT_TRUE(characteristics_ != NULL);
        return AuthorizationSet(characteristics_->hw_enforced);
    }

    AuthorizationSet sw_enforced() {
        EXPECT_TRUE(characteristics_ != NULL);
        return AuthorizationSet(characteristics_->sw_enforced);
    }

    void FreeCharacteristics() {
        keymaster_free_characteristics(characteristics_);
        free(characteristics_);
        characteristics_ = NULL;
    }

    void FreeKeyBlob() {
        free(const_cast<uint8_t*>(blob_.key_material));
        blob_.key_material = NULL;
    }

    void corrupt_key_blob() {
        assert(blob_.key_material);
        uint8_t* tmp = const_cast<uint8_t*>(blob_.key_material);
        ++tmp[blob_.key_material_size / 2];
    }

  private:
    SoftKeymasterDevice device_;
    keymaster_blob_t client_id_ = {.data = reinterpret_cast<const uint8_t*>("app_id"),
                                   .data_length = 6};
    keymaster_key_param_t client_params_[1] = {
        Authorization(TAG_APPLICATION_ID, client_id_.data, client_id_.data_length)};

    uint64_t op_handle_;

    keymaster_key_blob_t blob_;
    keymaster_key_characteristics_t* characteristics_;
};

typedef KeymasterTest CheckSupported;
TEST_F(CheckSupported, SupportedAlgorithms) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_algorithms(device(), NULL, NULL));

    size_t len;
    keymaster_algorithm_t* algorithms;
    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_algorithms(device(), &algorithms, &len));
    EXPECT_TRUE(ResponseContains(
        {KM_ALGORITHM_RSA, KM_ALGORITHM_ECDSA, KM_ALGORITHM_AES, KM_ALGORITHM_HMAC}, algorithms,
        len));
    free(algorithms);
}

TEST_F(CheckSupported, SupportedBlockModes) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_block_modes(device(), KM_ALGORITHM_RSA, KM_PURPOSE_ENCRYPT,
                                                  NULL, NULL));

    size_t len;
    keymaster_block_mode_t* modes;
    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_block_modes(device(), KM_ALGORITHM_RSA,
                                                               KM_PURPOSE_ENCRYPT, &modes, &len));
    EXPECT_EQ(0, len);
    free(modes);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_ALGORITHM,
              device()->get_supported_block_modes(device(), KM_ALGORITHM_DSA, KM_PURPOSE_ENCRYPT,
                                                  &modes, &len));

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_PURPOSE,
              device()->get_supported_block_modes(device(), KM_ALGORITHM_ECDSA, KM_PURPOSE_ENCRYPT,
                                                  &modes, &len));

    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_block_modes(device(), KM_ALGORITHM_AES,
                                                               KM_PURPOSE_ENCRYPT, &modes, &len));
    EXPECT_TRUE(ResponseContains({KM_MODE_OCB, KM_MODE_ECB, KM_MODE_CBC}, modes, len));
    free(modes);
}

TEST_F(CheckSupported, SupportedPaddingModes) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA, KM_PURPOSE_ENCRYPT,
                                                    NULL, NULL));

    size_t len;
    keymaster_padding_t* modes;
    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA,
                                                                 KM_PURPOSE_SIGN, &modes, &len));
    EXPECT_TRUE(
        ResponseContains({KM_PAD_NONE, KM_PAD_RSA_PKCS1_1_5_SIGN, KM_PAD_RSA_PSS}, modes, len));
    free(modes);

    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA,
                                                                 KM_PURPOSE_ENCRYPT, &modes, &len));
    EXPECT_TRUE(ResponseContains({KM_PAD_RSA_OAEP, KM_PAD_RSA_PKCS1_1_5_ENCRYPT}, modes, len));
    free(modes);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_ALGORITHM,
              device()->get_supported_padding_modes(device(), KM_ALGORITHM_DSA, KM_PURPOSE_SIGN,
                                                    &modes, &len));

    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_padding_modes(device(), KM_ALGORITHM_ECDSA,
                                                                 KM_PURPOSE_SIGN, &modes, &len));
    EXPECT_EQ(0, len);
    free(modes);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_PURPOSE,
              device()->get_supported_padding_modes(device(), KM_ALGORITHM_AES, KM_PURPOSE_SIGN,
                                                    &modes, &len));
}

TEST_F(CheckSupported, SupportedDigests) {
    EXPECT_EQ(
        KM_ERROR_OUTPUT_PARAMETER_NULL,
        device()->get_supported_digests(device(), KM_ALGORITHM_RSA, KM_PURPOSE_SIGN, NULL, NULL));

    size_t len;
    keymaster_digest_t* digests;
    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_digests(device(), KM_ALGORITHM_RSA,
                                                           KM_PURPOSE_SIGN, &digests, &len));
    EXPECT_TRUE(ResponseContains({KM_DIGEST_NONE, KM_DIGEST_SHA_2_256}, digests, len));
    free(digests);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_ALGORITHM,
              device()->get_supported_digests(device(), KM_ALGORITHM_DSA, KM_PURPOSE_SIGN, &digests,
                                              &len));

    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_digests(device(), KM_ALGORITHM_ECDSA,
                                                           KM_PURPOSE_SIGN, &digests, &len));
    EXPECT_EQ(0, len);
    free(digests);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_PURPOSE,
              device()->get_supported_digests(device(), KM_ALGORITHM_AES, KM_PURPOSE_SIGN, &digests,
                                              &len));

    EXPECT_EQ(KM_ERROR_OK, device()->get_supported_digests(device(), KM_ALGORITHM_HMAC,
                                                           KM_PURPOSE_SIGN, &digests, &len));
    EXPECT_TRUE(ResponseContains({KM_DIGEST_SHA_2_224, KM_DIGEST_SHA_2_256, KM_DIGEST_SHA_2_384,
                                  KM_DIGEST_SHA_2_512, KM_DIGEST_SHA1},
                                 digests, len));
    free(digests);
}

TEST_F(CheckSupported, SupportedImportFormats) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_RSA, NULL, NULL));

    size_t len;
    keymaster_key_format_t* formats;
    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_RSA, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_PKCS8, formats, len));
    free(formats);

    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_AES, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_RAW, formats, len));
    free(formats);

    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_import_formats(device(), KM_ALGORITHM_HMAC, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_RAW, formats, len));
    free(formats);
}

TEST_F(CheckSupported, SupportedExportFormats) {
    EXPECT_EQ(KM_ERROR_OUTPUT_PARAMETER_NULL,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_RSA, NULL, NULL));

    size_t len;
    keymaster_key_format_t* formats;
    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_RSA, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_X509, formats, len));
    free(formats);

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_ALGORITHM,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_DSA, &formats, &len));

    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_ECDSA, &formats, &len));
    EXPECT_TRUE(ResponseContains(KM_KEY_FORMAT_X509, formats, len));
    free(formats);

    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_AES, &formats, &len));
    EXPECT_EQ(0, len);
    free(formats);

    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_AES, &formats, &len));
    EXPECT_EQ(0, len);
    free(formats);

    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_export_formats(device(), KM_ALGORITHM_HMAC, &formats, &len));
    EXPECT_EQ(0, len);
    free(formats);
}

class NewKeyGeneration : public KeymasterTest {
  protected:
    void CheckBaseParams() {
        EXPECT_EQ(0U, hw_enforced().size());
        EXPECT_EQ(12U, hw_enforced().SerializedSize());

        AuthorizationSet auths = sw_enforced();
        EXPECT_GT(auths.SerializedSize(), 12U);

        EXPECT_TRUE(contains(auths, TAG_PURPOSE, KM_PURPOSE_SIGN));
        EXPECT_TRUE(contains(auths, TAG_PURPOSE, KM_PURPOSE_VERIFY));
        EXPECT_TRUE(contains(auths, TAG_USER_ID, 7));
        EXPECT_TRUE(contains(auths, TAG_USER_AUTH_ID, 8));
        EXPECT_TRUE(contains(auths, TAG_AUTH_TIMEOUT, 300));

        // Verify that App ID, App data and ROT are NOT included.
        EXPECT_FALSE(contains(auths, TAG_ROOT_OF_TRUST));
        EXPECT_FALSE(contains(auths, TAG_APPLICATION_ID));
        EXPECT_FALSE(contains(auths, TAG_APPLICATION_DATA));

        // Just for giggles, check that some unexpected tags/values are NOT present.
        EXPECT_FALSE(contains(auths, TAG_PURPOSE, KM_PURPOSE_ENCRYPT));
        EXPECT_FALSE(contains(auths, TAG_PURPOSE, KM_PURPOSE_DECRYPT));
        EXPECT_FALSE(contains(auths, TAG_AUTH_TIMEOUT, 301));

        // Now check that unspecified, defaulted tags are correct.
        EXPECT_TRUE(contains(auths, TAG_ORIGIN, KM_ORIGIN_SOFTWARE));
        EXPECT_TRUE(contains(auths, KM_TAG_CREATION_DATETIME));
    }
};

TEST_F(NewKeyGeneration, Rsa) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaSigningKey(256, KM_DIGEST_NONE, KM_PAD_NONE, 3)));
    CheckBaseParams();

    // Check specified tags are all present in auths
    AuthorizationSet auths(sw_enforced());
    EXPECT_TRUE(contains(auths, TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_TRUE(contains(auths, TAG_KEY_SIZE, 256));
    EXPECT_TRUE(contains(auths, TAG_RSA_PUBLIC_EXPONENT, 3));
}

TEST_F(NewKeyGeneration, RsaDefaultSize) {
    // TODO(swillden): Remove support for defaulting RSA parameter size and pub exponent.
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey()));
    CheckBaseParams();

    // Check specified tags are all present in unenforced characteristics
    EXPECT_TRUE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_RSA));

    // Now check that unspecified, defaulted tags are correct.
    EXPECT_TRUE(contains(sw_enforced(), TAG_RSA_PUBLIC_EXPONENT, 65537));
    EXPECT_TRUE(contains(sw_enforced(), TAG_KEY_SIZE, 2048));
}

TEST_F(NewKeyGeneration, Ecdsa) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().EcdsaSigningKey(224)));
    CheckBaseParams();

    // Check specified tags are all present in unenforced characteristics
    EXPECT_TRUE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_ECDSA));
    EXPECT_TRUE(contains(sw_enforced(), TAG_KEY_SIZE, 224));
}

TEST_F(NewKeyGeneration, EcdsaDefaultSize) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().EcdsaSigningKey()));
    CheckBaseParams();

    // Check specified tags are all present in unenforced characteristics
    EXPECT_TRUE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_ECDSA));

    // Now check that unspecified, defaulted tags are correct.
    EXPECT_TRUE(contains(sw_enforced(), TAG_KEY_SIZE, 224));
}

TEST_F(NewKeyGeneration, EcdsaInvalidSize) {
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_KEY_SIZE, GenerateKey(ParamBuilder().EcdsaSigningKey(190)));
}

TEST_F(NewKeyGeneration, EcdsaAllValidSizes) {
    size_t valid_sizes[] = {224, 256, 384, 521};
    for (size_t size : valid_sizes) {
        EXPECT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().EcdsaSigningKey(size)))
            << "Failed to generate size: " << size;
    }
}

TEST_F(NewKeyGeneration, AesOcb) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
}

TEST_F(NewKeyGeneration, AesOcbInvalidKeySize) {
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_KEY_SIZE,
              GenerateKey(ParamBuilder().AesEncryptionKey(136).OcbMode(4096, 16)));
}

TEST_F(NewKeyGeneration, AesOcbAllValidSizes) {
    size_t valid_sizes[] = {128, 192, 256};
    for (size_t size : valid_sizes) {
        EXPECT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(size)))
            << "Failed to generate size: " << size;
    }
}

TEST_F(NewKeyGeneration, HmacSha256) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_256, 16)));
}

typedef KeymasterTest GetKeyCharacteristics;
TEST_F(GetKeyCharacteristics, SimpleRsa) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    AuthorizationSet original(sw_enforced());

    ASSERT_EQ(KM_ERROR_OK, GetCharacteristics());
    EXPECT_EQ(original, sw_enforced());
}

typedef KeymasterTest SigningOperationsTest;
TEST_F(SigningOperationsTest, RsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature);
}

TEST_F(SigningOperationsTest, RsaSha256DigestSuccess) {
    // Note that without padding, key size must exactly match digest size.
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256, KM_DIGEST_SHA_2_256)));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
}

TEST_F(SigningOperationsTest, RsaPssSha256Success) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
}

TEST_F(SigningOperationsTest, RsaPkcs1Sha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256,
                                                                    KM_PAD_RSA_PKCS1_1_5_SIGN)));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
}

TEST_F(SigningOperationsTest, RsaPssSha256TooSmallKey) {
    // Key must be at least 10 bytes larger than hash, to provide minimal random salt, so verify
    // that 9 bytes larger than hash won't work.
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(
                               256 + 9 * 8, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS)));
    string message(1024, 'a');
    string signature;

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_SIGN));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_INCOMPATIBLE_DIGEST, FinishOperation(signature, &result));
}

TEST_F(SigningOperationsTest, EcdsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().EcdsaSigningKey(224)));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature);
}

TEST_F(SigningOperationsTest, RsaAbort) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    AuthorizationSet input_params, output_params;
    ASSERT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_SIGN));
    EXPECT_EQ(KM_ERROR_OK, AbortOperation());
    // Another abort should fail
    EXPECT_EQ(KM_ERROR_INVALID_OPERATION_HANDLE, AbortOperation());
}

TEST_F(SigningOperationsTest, RsaUnsupportedDigest) {
    GenerateKey(
        ParamBuilder().RsaSigningKey(256, KM_DIGEST_MD5, KM_PAD_RSA_PSS /* supported padding */));
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_DIGEST, BeginOperation(KM_PURPOSE_SIGN));
}

TEST_F(SigningOperationsTest, RsaUnsupportedPadding) {
    GenerateKey(ParamBuilder().RsaSigningKey(256, KM_DIGEST_SHA_2_256 /* supported digest */,
                                             KM_PAD_PKCS7));
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_PADDING_MODE, BeginOperation(KM_PURPOSE_SIGN));
}

TEST_F(SigningOperationsTest, RsaNoDigest) {
    // Digest must be specified.
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaKey(256).SigningKey().Option(
                               TAG_PADDING, KM_PAD_NONE)));
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_DIGEST, BeginOperation(KM_PURPOSE_SIGN));
    // PSS requires a digest.
    GenerateKey(ParamBuilder().RsaSigningKey(256, KM_DIGEST_NONE, KM_PAD_RSA_PSS));
    ASSERT_EQ(KM_ERROR_INCOMPATIBLE_DIGEST, BeginOperation(KM_PURPOSE_SIGN));
}

TEST_F(SigningOperationsTest, RsaNoPadding) {
    // Padding must be specified
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaKey(256).SigningKey().Option(
                               TAG_DIGEST, KM_DIGEST_NONE)));
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_PADDING_MODE, BeginOperation(KM_PURPOSE_SIGN));
}

TEST_F(SigningOperationsTest, HmacSha1Success) {
    GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA1, 20));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature);
    ASSERT_EQ(20, signature.size());
}

TEST_F(SigningOperationsTest, HmacSha224Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_224, 28)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature);
    ASSERT_EQ(28, signature.size());
}

TEST_F(SigningOperationsTest, HmacSha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_256, 32)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature);
    ASSERT_EQ(32, signature.size());
}

TEST_F(SigningOperationsTest, HmacSha384Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_384, 48)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature);
    ASSERT_EQ(48, signature.size());
}

TEST_F(SigningOperationsTest, HmacSha512Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_512, 64)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature);
    ASSERT_EQ(64, signature.size());
}

TEST_F(SigningOperationsTest, HmacRfc4231TestCase1) {
    uint8_t key_data[] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    };
    string message = "Hi There";
    uint8_t sha_224_expected[] = {
        0x89, 0x6f, 0xb1, 0x12, 0x8a, 0xbb, 0xdf, 0x19, 0x68, 0x32, 0x10, 0x7c, 0xd4, 0x9d,
        0xf3, 0x3f, 0x47, 0xb4, 0xb1, 0x16, 0x99, 0x12, 0xba, 0x4f, 0x53, 0x68, 0x4b, 0x22,
    };
    uint8_t sha_256_expected[] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf,
        0xce, 0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83,
        0x3d, 0xa7, 0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
    };
    uint8_t sha_384_expected[] = {
        0xaf, 0xd0, 0x39, 0x44, 0xd8, 0x48, 0x95, 0x62, 0x6b, 0x08, 0x25, 0xf4,
        0xab, 0x46, 0x90, 0x7f, 0x15, 0xf9, 0xda, 0xdb, 0xe4, 0x10, 0x1e, 0xc6,
        0x82, 0xaa, 0x03, 0x4c, 0x7c, 0xeb, 0xc5, 0x9c, 0xfa, 0xea, 0x9e, 0xa9,
        0x07, 0x6e, 0xde, 0x7f, 0x4a, 0xf1, 0x52, 0xe8, 0xb2, 0xfa, 0x9c, 0xb6,
    };
    uint8_t sha_512_expected[] = {
        0x87, 0xaa, 0x7c, 0xde, 0xa5, 0xef, 0x61, 0x9d, 0x4f, 0xf0, 0xb4, 0x24, 0x1a,
        0x1d, 0x6c, 0xb0, 0x23, 0x79, 0xf4, 0xe2, 0xce, 0x4e, 0xc2, 0x78, 0x7a, 0xd0,
        0xb3, 0x05, 0x45, 0xe1, 0x7c, 0xde, 0xda, 0xa8, 0x33, 0xb7, 0xd6, 0xb8, 0xa7,
        0x02, 0x03, 0x8b, 0x27, 0x4e, 0xae, 0xa3, 0xf4, 0xe4, 0xbe, 0x9d, 0x91, 0x4e,
        0xeb, 0x61, 0xf1, 0x70, 0x2e, 0x69, 0x6c, 0x20, 0x3a, 0x12, 0x68, 0x54,
    };

    string key = make_string(key_data);

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));
}

TEST_F(SigningOperationsTest, HmacRfc4231TestCase2) {
    string key = "Jefe";
    string message = "what do ya want for nothing?";
    uint8_t sha_224_expected[] = {
        0xa3, 0x0e, 0x01, 0x09, 0x8b, 0xc6, 0xdb, 0xbf, 0x45, 0x69, 0x0f, 0x3a, 0x7e, 0x9e,
        0x6d, 0x0f, 0x8b, 0xbe, 0xa2, 0xa3, 0x9e, 0x61, 0x48, 0x00, 0x8f, 0xd0, 0x5e, 0x44,
    };
    uint8_t sha_256_expected[] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
        0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
        0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
    };
    uint8_t sha_384_expected[] = {
        0xaf, 0x45, 0xd2, 0xe3, 0x76, 0x48, 0x40, 0x31, 0x61, 0x7f, 0x78, 0xd2,
        0xb5, 0x8a, 0x6b, 0x1b, 0x9c, 0x7e, 0xf4, 0x64, 0xf5, 0xa0, 0x1b, 0x47,
        0xe4, 0x2e, 0xc3, 0x73, 0x63, 0x22, 0x44, 0x5e, 0x8e, 0x22, 0x40, 0xca,
        0x5e, 0x69, 0xe2, 0xc7, 0x8b, 0x32, 0x39, 0xec, 0xfa, 0xb2, 0x16, 0x49,
    };
    uint8_t sha_512_expected[] = {
        0x16, 0x4b, 0x7a, 0x7b, 0xfc, 0xf8, 0x19, 0xe2, 0xe3, 0x95, 0xfb, 0xe7, 0x3b,
        0x56, 0xe0, 0xa3, 0x87, 0xbd, 0x64, 0x22, 0x2e, 0x83, 0x1f, 0xd6, 0x10, 0x27,
        0x0c, 0xd7, 0xea, 0x25, 0x05, 0x54, 0x97, 0x58, 0xbf, 0x75, 0xc0, 0x5a, 0x99,
        0x4a, 0x6d, 0x03, 0x4f, 0x65, 0xf8, 0xf0, 0xe6, 0xfd, 0xca, 0xea, 0xb1, 0xa3,
        0x4d, 0x4a, 0x6b, 0x4b, 0x63, 0x6e, 0x07, 0x0a, 0x38, 0xbc, 0xe7, 0x37,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));
}

TEST_F(SigningOperationsTest, HmacRfc4231TestCase3) {
    string key(20, 0xaa);
    string message(50, 0xdd);
    uint8_t sha_224_expected[] = {
        0x7f, 0xb3, 0xcb, 0x35, 0x88, 0xc6, 0xc1, 0xf6, 0xff, 0xa9, 0x69, 0x4d, 0x7d, 0x6a,
        0xd2, 0x64, 0x93, 0x65, 0xb0, 0xc1, 0xf6, 0x5d, 0x69, 0xd1, 0xec, 0x83, 0x33, 0xea,
    };
    uint8_t sha_256_expected[] = {
        0x77, 0x3e, 0xa9, 0x1e, 0x36, 0x80, 0x0e, 0x46, 0x85, 0x4d, 0xb8,
        0xeb, 0xd0, 0x91, 0x81, 0xa7, 0x29, 0x59, 0x09, 0x8b, 0x3e, 0xf8,
        0xc1, 0x22, 0xd9, 0x63, 0x55, 0x14, 0xce, 0xd5, 0x65, 0xfe,
    };
    uint8_t sha_384_expected[] = {
        0x88, 0x06, 0x26, 0x08, 0xd3, 0xe6, 0xad, 0x8a, 0x0a, 0xa2, 0xac, 0xe0,
        0x14, 0xc8, 0xa8, 0x6f, 0x0a, 0xa6, 0x35, 0xd9, 0x47, 0xac, 0x9f, 0xeb,
        0xe8, 0x3e, 0xf4, 0xe5, 0x59, 0x66, 0x14, 0x4b, 0x2a, 0x5a, 0xb3, 0x9d,
        0xc1, 0x38, 0x14, 0xb9, 0x4e, 0x3a, 0xb6, 0xe1, 0x01, 0xa3, 0x4f, 0x27,
    };
    uint8_t sha_512_expected[] = {
        0xfa, 0x73, 0xb0, 0x08, 0x9d, 0x56, 0xa2, 0x84, 0xef, 0xb0, 0xf0, 0x75, 0x6c,
        0x89, 0x0b, 0xe9, 0xb1, 0xb5, 0xdb, 0xdd, 0x8e, 0xe8, 0x1a, 0x36, 0x55, 0xf8,
        0x3e, 0x33, 0xb2, 0x27, 0x9d, 0x39, 0xbf, 0x3e, 0x84, 0x82, 0x79, 0xa7, 0x22,
        0xc8, 0x06, 0xb4, 0x85, 0xa4, 0x7e, 0x67, 0xc8, 0x07, 0xb9, 0x46, 0xa3, 0x37,
        0xbe, 0xe8, 0x94, 0x26, 0x74, 0x27, 0x88, 0x59, 0xe1, 0x32, 0x92, 0xfb,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));
}

TEST_F(SigningOperationsTest, HmacRfc4231TestCase4) {
    uint8_t key_data[25] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    };
    string key = make_string(key_data);
    string message(50, 0xcd);
    uint8_t sha_224_expected[] = {
        0x6c, 0x11, 0x50, 0x68, 0x74, 0x01, 0x3c, 0xac, 0x6a, 0x2a, 0xbc, 0x1b, 0xb3, 0x82,
        0x62, 0x7c, 0xec, 0x6a, 0x90, 0xd8, 0x6e, 0xfc, 0x01, 0x2d, 0xe7, 0xaf, 0xec, 0x5a,
    };
    uint8_t sha_256_expected[] = {
        0x82, 0x55, 0x8a, 0x38, 0x9a, 0x44, 0x3c, 0x0e, 0xa4, 0xcc, 0x81,
        0x98, 0x99, 0xf2, 0x08, 0x3a, 0x85, 0xf0, 0xfa, 0xa3, 0xe5, 0x78,
        0xf8, 0x07, 0x7a, 0x2e, 0x3f, 0xf4, 0x67, 0x29, 0x66, 0x5b,
    };
    uint8_t sha_384_expected[] = {
        0x3e, 0x8a, 0x69, 0xb7, 0x78, 0x3c, 0x25, 0x85, 0x19, 0x33, 0xab, 0x62,
        0x90, 0xaf, 0x6c, 0xa7, 0x7a, 0x99, 0x81, 0x48, 0x08, 0x50, 0x00, 0x9c,
        0xc5, 0x57, 0x7c, 0x6e, 0x1f, 0x57, 0x3b, 0x4e, 0x68, 0x01, 0xdd, 0x23,
        0xc4, 0xa7, 0xd6, 0x79, 0xcc, 0xf8, 0xa3, 0x86, 0xc6, 0x74, 0xcf, 0xfb,
    };
    uint8_t sha_512_expected[] = {
        0xb0, 0xba, 0x46, 0x56, 0x37, 0x45, 0x8c, 0x69, 0x90, 0xe5, 0xa8, 0xc5, 0xf6,
        0x1d, 0x4a, 0xf7, 0xe5, 0x76, 0xd9, 0x7f, 0xf9, 0x4b, 0x87, 0x2d, 0xe7, 0x6f,
        0x80, 0x50, 0x36, 0x1e, 0xe3, 0xdb, 0xa9, 0x1c, 0xa5, 0xc1, 0x1a, 0xa2, 0x5e,
        0xb4, 0xd6, 0x79, 0x27, 0x5c, 0xc5, 0x78, 0x80, 0x63, 0xa5, 0xf1, 0x97, 0x41,
        0x12, 0x0c, 0x4f, 0x2d, 0xe2, 0xad, 0xeb, 0xeb, 0x10, 0xa2, 0x98, 0xdd,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));
}

TEST_F(SigningOperationsTest, HmacRfc4231TestCase5) {
    string key(20, 0x0c);
    string message = "Test With Truncation";

    uint8_t sha_224_expected[] = {
        0x0e, 0x2a, 0xea, 0x68, 0xa9, 0x0c, 0x8d, 0x37,
        0xc9, 0x88, 0xbc, 0xdb, 0x9f, 0xca, 0x6f, 0xa8,
    };
    uint8_t sha_256_expected[] = {
        0xa3, 0xb6, 0x16, 0x74, 0x73, 0x10, 0x0e, 0xe0,
        0x6e, 0x0c, 0x79, 0x6c, 0x29, 0x55, 0x55, 0x2b,
    };
    uint8_t sha_384_expected[] = {
        0x3a, 0xbf, 0x34, 0xc3, 0x50, 0x3b, 0x2a, 0x23,
        0xa4, 0x6e, 0xfc, 0x61, 0x9b, 0xae, 0xf8, 0x97,
    };
    uint8_t sha_512_expected[] = {
        0x41, 0x5f, 0xad, 0x62, 0x71, 0x58, 0x0a, 0x53,
        0x1d, 0x41, 0x79, 0xbc, 0x89, 0x1d, 0x87, 0xa6,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));
}

TEST_F(SigningOperationsTest, HmacRfc4231TestCase6) {
    string key(131, 0xaa);
    string message = "Test Using Larger Than Block-Size Key - Hash Key First";

    uint8_t sha_224_expected[] = {
        0x95, 0xe9, 0xa0, 0xdb, 0x96, 0x20, 0x95, 0xad, 0xae, 0xbe, 0x9b, 0x2d, 0x6f, 0x0d,
        0xbc, 0xe2, 0xd4, 0x99, 0xf1, 0x12, 0xf2, 0xd2, 0xb7, 0x27, 0x3f, 0xa6, 0x87, 0x0e,
    };
    uint8_t sha_256_expected[] = {
        0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f, 0x0d, 0x8a, 0x26,
        0xaa, 0xcb, 0xf5, 0xb7, 0x7f, 0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28,
        0xc5, 0x14, 0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54,
    };
    uint8_t sha_384_expected[] = {
        0x4e, 0xce, 0x08, 0x44, 0x85, 0x81, 0x3e, 0x90, 0x88, 0xd2, 0xc6, 0x3a,
        0x04, 0x1b, 0xc5, 0xb4, 0x4f, 0x9e, 0xf1, 0x01, 0x2a, 0x2b, 0x58, 0x8f,
        0x3c, 0xd1, 0x1f, 0x05, 0x03, 0x3a, 0xc4, 0xc6, 0x0c, 0x2e, 0xf6, 0xab,
        0x40, 0x30, 0xfe, 0x82, 0x96, 0x24, 0x8d, 0xf1, 0x63, 0xf4, 0x49, 0x52,
    };
    uint8_t sha_512_expected[] = {
        0x80, 0xb2, 0x42, 0x63, 0xc7, 0xc1, 0xa3, 0xeb, 0xb7, 0x14, 0x93, 0xc1, 0xdd,
        0x7b, 0xe8, 0xb4, 0x9b, 0x46, 0xd1, 0xf4, 0x1b, 0x4a, 0xee, 0xc1, 0x12, 0x1b,
        0x01, 0x37, 0x83, 0xf8, 0xf3, 0x52, 0x6b, 0x56, 0xd0, 0x37, 0xe0, 0x5f, 0x25,
        0x98, 0xbd, 0x0f, 0xd2, 0x21, 0x5d, 0x6a, 0x1e, 0x52, 0x95, 0xe6, 0x4f, 0x73,
        0xf6, 0x3f, 0x0a, 0xec, 0x8b, 0x91, 0x5a, 0x98, 0x5d, 0x78, 0x65, 0x98,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));
}

TEST_F(SigningOperationsTest, HmacRfc4231TestCase7) {
    string key(131, 0xaa);
    string message = "This is a test using a larger than block-size key and a larger than "
                     "block-size data. The key needs to be hashed before being used by the HMAC "
                     "algorithm.";

    uint8_t sha_224_expected[] = {
        0x3a, 0x85, 0x41, 0x66, 0xac, 0x5d, 0x9f, 0x02, 0x3f, 0x54, 0xd5, 0x17, 0xd0, 0xb3,
        0x9d, 0xbd, 0x94, 0x67, 0x70, 0xdb, 0x9c, 0x2b, 0x95, 0xc9, 0xf6, 0xf5, 0x65, 0xd1,
    };
    uint8_t sha_256_expected[] = {
        0x9b, 0x09, 0xff, 0xa7, 0x1b, 0x94, 0x2f, 0xcb, 0x27, 0x63, 0x5f,
        0xbc, 0xd5, 0xb0, 0xe9, 0x44, 0xbf, 0xdc, 0x63, 0x64, 0x4f, 0x07,
        0x13, 0x93, 0x8a, 0x7f, 0x51, 0x53, 0x5c, 0x3a, 0x35, 0xe2,
    };
    uint8_t sha_384_expected[] = {
        0x66, 0x17, 0x17, 0x8e, 0x94, 0x1f, 0x02, 0x0d, 0x35, 0x1e, 0x2f, 0x25,
        0x4e, 0x8f, 0xd3, 0x2c, 0x60, 0x24, 0x20, 0xfe, 0xb0, 0xb8, 0xfb, 0x9a,
        0xdc, 0xce, 0xbb, 0x82, 0x46, 0x1e, 0x99, 0xc5, 0xa6, 0x78, 0xcc, 0x31,
        0xe7, 0x99, 0x17, 0x6d, 0x38, 0x60, 0xe6, 0x11, 0x0c, 0x46, 0x52, 0x3e,
    };
    uint8_t sha_512_expected[] = {
        0xe3, 0x7b, 0x6a, 0x77, 0x5d, 0xc8, 0x7d, 0xba, 0xa4, 0xdf, 0xa9, 0xf9, 0x6e,
        0x5e, 0x3f, 0xfd, 0xde, 0xbd, 0x71, 0xf8, 0x86, 0x72, 0x89, 0x86, 0x5d, 0xf5,
        0xa3, 0x2d, 0x20, 0xcd, 0xc9, 0x44, 0xb6, 0x02, 0x2c, 0xac, 0x3c, 0x49, 0x82,
        0xb1, 0x0d, 0x5e, 0xeb, 0x55, 0xc3, 0xe4, 0xde, 0x15, 0x13, 0x46, 0x76, 0xfb,
        0x6d, 0xe0, 0x44, 0x60, 0x65, 0xc9, 0x74, 0x40, 0xfa, 0x8c, 0x6a, 0x58,
    };

    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_224, make_string(sha_224_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_256, make_string(sha_256_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_384, make_string(sha_384_expected));
    CheckHmacTestVector(key, message, KM_DIGEST_SHA_2_512, make_string(sha_512_expected));
}

TEST_F(SigningOperationsTest, HmacSha256NoMacLength) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder()
                                           .Option(TAG_ALGORITHM, KM_ALGORITHM_HMAC)
                                           .Option(TAG_KEY_SIZE, 128)
                                           .SigningKey()
                                           .Option(TAG_DIGEST, KM_DIGEST_SHA_2_256)));
    EXPECT_EQ(KM_ERROR_UNSUPPORTED_MAC_LENGTH, BeginOperation(KM_PURPOSE_SIGN));
}

TEST_F(SigningOperationsTest, HmacSha256TooLargeMacLength) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_256, 33)));
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_MAC_LENGTH, BeginOperation(KM_PURPOSE_SIGN));
}

TEST_F(SigningOperationsTest, RsaTooShortMessage) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    ASSERT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_SIGN));

    string message = "1234567890123456789012345678901";
    string result;
    size_t input_consumed;
    ASSERT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(0U, result.size());
    EXPECT_EQ(31U, input_consumed);

    string signature;
    ASSERT_EQ(KM_ERROR_UNKNOWN_ERROR, FinishOperation(&signature));
    EXPECT_EQ(0U, signature.length());
}

// TODO(swillden): Add more verification failure tests.

typedef KeymasterTest VerificationOperationsTest;
TEST_F(VerificationOperationsTest, RsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    string message = "12345678901234567890123456789012";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, RsaSha256DigestSuccess) {
    // Note that without padding, key size must exactly match digest size.
    GenerateKey(ParamBuilder().RsaSigningKey(256, KM_DIGEST_SHA_2_256));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, RsaSha256CorruptSignature) {
    GenerateKey(ParamBuilder().RsaSigningKey(256, KM_DIGEST_SHA_2_256));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    ++signature[signature.size() / 2];

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));
}

TEST_F(VerificationOperationsTest, RsaPssSha256Success) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, RsaPssSha256CorruptSignature) {
    GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    ++signature[signature.size() / 2];

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));
}

TEST_F(VerificationOperationsTest, RsaPssSha256CorruptInput) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PSS)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    ++message[message.size() / 2];

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));
}

TEST_F(VerificationOperationsTest, RsaPkcs1Sha256Success) {
    GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PKCS1_1_5_SIGN));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, RsaPkcs1Sha256CorruptSignature) {
    GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256, KM_PAD_RSA_PKCS1_1_5_SIGN));
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    ++signature[signature.size() / 2];

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));
}

TEST_F(VerificationOperationsTest, RsaPkcs1Sha256CorruptInput) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(512, KM_DIGEST_SHA_2_256,
                                                                    KM_PAD_RSA_PKCS1_1_5_SIGN)));
    // Use large message, which won't work without digesting.
    string message(1024, 'a');
    string signature;
    SignMessage(message, &signature);
    ++message[message.size() / 2];

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_VERIFY));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(signature, &result));
}

template <typename T> vector<T> make_vector(const T* array, size_t len) {
    return vector<T>(array, array + len);
}

TEST_F(VerificationOperationsTest, RsaAllDigestAndPadCombinations) {
    // Get all supported digests and padding modes.
    size_t digests_len;
    keymaster_digest_t* digests;
    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_digests(device(), KM_ALGORITHM_RSA, KM_PURPOSE_SIGN, &digests,
                                              &digests_len));

    size_t padding_modes_len;
    keymaster_padding_t* padding_modes;
    EXPECT_EQ(KM_ERROR_OK,
              device()->get_supported_padding_modes(device(), KM_ALGORITHM_RSA, KM_PURPOSE_SIGN,
                                                    &padding_modes, &padding_modes_len));

    // Try them.
    for (keymaster_padding_t padding_mode : make_vector(padding_modes, padding_modes_len)) {
        for (keymaster_digest_t digest : make_vector(digests, digests_len)) {
            // Compute key & message size that will work.
            size_t key_bits = 256;
            size_t message_len = 1000;
            switch (digest) {
            case KM_DIGEST_NONE:
                switch (padding_mode) {
                case KM_PAD_NONE:
                    // Match key size.
                    message_len = key_bits / 8;
                    break;
                case KM_PAD_RSA_PKCS1_1_5_SIGN:
                    message_len = key_bits / 8 - 11;
                    break;
                case KM_PAD_RSA_PSS:
                    // PSS requires a digest.
                    continue;
                default:
                    FAIL() << "Missing padding";
                    break;
                }
                break;

            case KM_DIGEST_SHA_2_256:
                switch (padding_mode) {
                case KM_PAD_NONE:
                    // Key size matches digest size
                    break;
                case KM_PAD_RSA_PKCS1_1_5_SIGN:
                    key_bits += 8 * 11;
                    break;
                case KM_PAD_RSA_PSS:
                    key_bits += 8 * 10;
                    break;
                default:
                    FAIL() << "Missing padding";
                    break;
                }
                break;
            default:
                FAIL() << "Missing digest";
            }

            GenerateKey(ParamBuilder().RsaSigningKey(key_bits, digest, padding_mode));
            string message(message_len, 'a');
            string signature;
            SignMessage(message, &signature);
            VerifyMessage(message, signature);
        }
    }

    free(padding_modes);
    free(digests);
}

TEST_F(VerificationOperationsTest, EcdsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().EcdsaSigningKey(256)));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, HmacSha1Success) {
    GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA1, 16));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, HmacSha224Success) {
    GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_224, 16));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, HmacSha256Success) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_256, 16)));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, HmacSha384Success) {
    GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_384, 16));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(VerificationOperationsTest, HmacSha512Success) {
    GenerateKey(ParamBuilder().HmacKey(128, KM_DIGEST_SHA_2_512, 16));
    string message = "123456789012345678901234567890123456789012345678";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

typedef VerificationOperationsTest ExportKeyTest;
TEST_F(ExportKeyTest, RsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    string export_data;
    ASSERT_EQ(KM_ERROR_OK, ExportKey(KM_KEY_FORMAT_X509, &export_data));
    EXPECT_GT(export_data.length(), 0);

    // TODO(swillden): Verify that the exported key is actually usable to verify signatures.
}

TEST_F(ExportKeyTest, EcdsaSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().EcdsaSigningKey(224)));
    string export_data;
    ASSERT_EQ(KM_ERROR_OK, ExportKey(KM_KEY_FORMAT_X509, &export_data));
    EXPECT_GT(export_data.length(), 0);

    // TODO(swillden): Verify that the exported key is actually usable to verify signatures.
}

TEST_F(ExportKeyTest, RsaUnsupportedKeyFormat) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    string export_data;
    ASSERT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_PKCS8, &export_data));
}

TEST_F(ExportKeyTest, RsaCorruptedKeyBlob) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaSigningKey(256)));
    corrupt_key_blob();
    string export_data;
    ASSERT_EQ(KM_ERROR_INVALID_KEY_BLOB, ExportKey(KM_KEY_FORMAT_X509, &export_data));
}

TEST_F(ExportKeyTest, AesKeyExportFails) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128)));
    string export_data;

    EXPECT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_X509, &export_data));
    EXPECT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_PKCS8, &export_data));
    EXPECT_EQ(KM_ERROR_UNSUPPORTED_KEY_FORMAT, ExportKey(KM_KEY_FORMAT_RAW, &export_data));
}

static string read_file(const string& file_name) {
    ifstream file_stream(file_name, std::ios::binary);
    istreambuf_iterator<char> file_begin(file_stream);
    istreambuf_iterator<char> file_end;
    return string(file_begin, file_end);
}

typedef VerificationOperationsTest ImportKeyTest;
TEST_F(ImportKeyTest, RsaSuccess) {
    string pk8_key = read_file("rsa_privkey_pk8.der");
    ASSERT_EQ(633U, pk8_key.size());

    ASSERT_EQ(KM_ERROR_OK, ImportKey(ParamBuilder().RsaSigningKey().NoDigestOrPadding(),
                                     KM_KEY_FORMAT_PKCS8, pk8_key));

    // Check values derived from the key.
    EXPECT_TRUE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_RSA));
    EXPECT_TRUE(contains(sw_enforced(), TAG_KEY_SIZE, 1024));
    EXPECT_TRUE(contains(sw_enforced(), TAG_RSA_PUBLIC_EXPONENT, 65537U));

    // And values provided by GoogleKeymaster
    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message(1024 / 8, 'a');
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(ImportKeyTest, RsaKeySizeMismatch) {
    string pk8_key = read_file("rsa_privkey_pk8.der");
    ASSERT_EQ(633U, pk8_key.size());
    ASSERT_EQ(KM_ERROR_IMPORT_PARAMETER_MISMATCH,
              ImportKey(ParamBuilder()
                            .RsaSigningKey(2048)  // Size doesn't match key
                            .NoDigestOrPadding(),
                        KM_KEY_FORMAT_PKCS8, pk8_key));
}

TEST_F(ImportKeyTest, RsaPublicExponenMismatch) {
    string pk8_key = read_file("rsa_privkey_pk8.der");
    ASSERT_EQ(633U, pk8_key.size());
    ASSERT_EQ(KM_ERROR_IMPORT_PARAMETER_MISMATCH,
              ImportKey(ParamBuilder()
                            .RsaSigningKey()
                            .Option(TAG_RSA_PUBLIC_EXPONENT, 3)  // Doesn't match key
                            .NoDigestOrPadding(),
                        KM_KEY_FORMAT_PKCS8, pk8_key));
}

TEST_F(ImportKeyTest, EcdsaSuccess) {
    string pk8_key = read_file("ec_privkey_pk8.der");
    ASSERT_EQ(138U, pk8_key.size());

    ASSERT_EQ(KM_ERROR_OK,
              ImportKey(ParamBuilder().EcdsaSigningKey(), KM_KEY_FORMAT_PKCS8, pk8_key));

    // Check values derived from the key.
    EXPECT_TRUE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_ECDSA));
    EXPECT_TRUE(contains(sw_enforced(), TAG_KEY_SIZE, 256));

    // And values provided by GoogleKeymaster
    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message(1024 / 8, 'a');
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(ImportKeyTest, EcdsaSizeSpecified) {
    string pk8_key = read_file("ec_privkey_pk8.der");
    ASSERT_EQ(138U, pk8_key.size());

    ASSERT_EQ(KM_ERROR_OK,
              ImportKey(ParamBuilder().EcdsaSigningKey(256), KM_KEY_FORMAT_PKCS8, pk8_key));

    // Check values derived from the key.
    EXPECT_TRUE(contains(sw_enforced(), TAG_ALGORITHM, KM_ALGORITHM_ECDSA));
    EXPECT_TRUE(contains(sw_enforced(), TAG_KEY_SIZE, 256));

    // And values provided by GoogleKeymaster
    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message(1024 / 8, 'a');
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

TEST_F(ImportKeyTest, EcdsaSizeMismatch) {
    string pk8_key = read_file("ec_privkey_pk8.der");
    ASSERT_EQ(138U, pk8_key.size());
    ASSERT_EQ(KM_ERROR_IMPORT_PARAMETER_MISMATCH,
              ImportKey(ParamBuilder().EcdsaSigningKey(224),  // Size does not match key
                        KM_KEY_FORMAT_PKCS8, pk8_key));
}

TEST_F(ImportKeyTest, AesKeySuccess) {
    char key_data[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    string key(key_data, sizeof(key_data));
    ASSERT_EQ(KM_ERROR_OK, ImportKey(ParamBuilder().AesEncryptionKey().OcbMode(4096, 16),
                                     KM_KEY_FORMAT_RAW, key));

    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message = "Hello World!";
    string nonce;
    string ciphertext = EncryptMessage(message, &nonce);
    string plaintext = DecryptMessage(ciphertext, nonce);
    EXPECT_EQ(message, plaintext);
}

TEST_F(ImportKeyTest, HmacSha256KeySuccess) {
    char key_data[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    string key(key_data, sizeof(key_data));
    ASSERT_EQ(KM_ERROR_OK,
              ImportKey(ParamBuilder().HmacKey(sizeof(key_data) * 8, KM_DIGEST_SHA_2_256, 32),
                        KM_KEY_FORMAT_RAW, key));

    EXPECT_TRUE(contains(sw_enforced(), TAG_ORIGIN, KM_ORIGIN_IMPORTED));
    EXPECT_TRUE(contains(sw_enforced(), KM_TAG_CREATION_DATETIME));

    string message = "Hello World!";
    string signature;
    SignMessage(message, &signature);
    VerifyMessage(message, signature);
}

typedef KeymasterTest VersionTest;
TEST_F(VersionTest, GetVersion) {
    uint8_t major, minor, subminor;
    ASSERT_EQ(KM_ERROR_OK, GetVersion(&major, &minor, &subminor));
    EXPECT_EQ(1, major);
    EXPECT_EQ(0, minor);
    EXPECT_EQ(0, subminor);
}

typedef KeymasterTest EncryptionOperationsTest;
TEST_F(EncryptionOperationsTest, RsaOaepSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_OAEP)));

    string message = "Hello World!";
    string ciphertext1 = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext2.size());

    // OAEP randomizes padding so every result should be different.
    EXPECT_NE(ciphertext1, ciphertext2);
}

TEST_F(EncryptionOperationsTest, RsaOaepRoundTrip) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_OAEP)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext.size());

    string plaintext = DecryptMessage(ciphertext);
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, RsaOaepTooLarge) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_OAEP)));
    string message = "12345678901234567890123";
    string result;
    size_t input_consumed;

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&result));
    EXPECT_EQ(0, result.size());
}

TEST_F(EncryptionOperationsTest, RsaOaepCorruptedDecrypt) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_OAEP)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext.size());

    // Corrupt the ciphertext
    ciphertext[512 / 8 / 2]++;

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_UNKNOWN_ERROR, FinishOperation(&result));
    EXPECT_EQ(0, result.size());
}

TEST_F(EncryptionOperationsTest, RsaPkcs1Success) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "Hello World!";
    string ciphertext1 = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext2.size());

    // PKCS1 v1.5 randomizes padding so every result should be different.
    EXPECT_NE(ciphertext1, ciphertext2);
}

TEST_F(EncryptionOperationsTest, RsaPkcs1RoundTrip) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext.size());

    string plaintext = DecryptMessage(ciphertext);
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, RsaPkcs1TooLarge) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "12345678901234567890123456789012345678901234567890123";
    string result;
    size_t input_consumed;

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&result));
    EXPECT_EQ(0, result.size());
}

TEST_F(EncryptionOperationsTest, RsaPkcs1CorruptedDecrypt) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().RsaEncryptionKey(512, KM_PAD_RSA_PKCS1_1_5_ENCRYPT)));
    string message = "Hello World!";
    string ciphertext = EncryptMessage(string(message));
    EXPECT_EQ(512 / 8, ciphertext.size());

    // Corrupt the ciphertext
    ciphertext[512 / 8 / 2]++;

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT));
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &result, &input_consumed));
    EXPECT_EQ(KM_ERROR_UNKNOWN_ERROR, FinishOperation(&result));
    EXPECT_EQ(0, result.size());
}

TEST_F(EncryptionOperationsTest, AesOcbSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string message = "Hello World!";
    string nonce1;
    string ciphertext1 = EncryptMessage(message, &nonce1);
    EXPECT_EQ(12, nonce1.size());
    EXPECT_EQ(message.size() + 16 /* tag */, ciphertext1.size());

    string nonce2;
    string ciphertext2 = EncryptMessage(message, &nonce2);
    EXPECT_EQ(12, nonce2.size());
    EXPECT_EQ(message.size() + 16 /* tag */, ciphertext2.size());

    // Nonces should be random
    EXPECT_NE(nonce1, nonce2);

    // Therefore ciphertexts are different
    EXPECT_NE(ciphertext1, ciphertext2);
}

TEST_F(EncryptionOperationsTest, AesOcbRoundTripSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string message = "Hello World!";
    string nonce;
    string ciphertext = EncryptMessage(message, &nonce);
    EXPECT_EQ(12, nonce.size());
    EXPECT_EQ(message.length() + 16 /* tag */, ciphertext.size());

    string plaintext = DecryptMessage(ciphertext, nonce);
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, AesOcbRoundTripCorrupted) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string message = "Hello World!";
    string nonce;
    string ciphertext = EncryptMessage(message, &nonce);
    EXPECT_EQ(message.size() + 16 /* tag */, ciphertext.size());

    ciphertext[ciphertext.size() / 2]++;

    AuthorizationSet input_set, output_set;
    input_set.push_back(TAG_NONCE, nonce.data(), nonce.size());
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, input_set, &output_set));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &result, &input_consumed));
    EXPECT_EQ(ciphertext.length(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(&result));
}

TEST_F(EncryptionOperationsTest, AesDecryptGarbage) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string ciphertext(128, 'a');
    AuthorizationSet input_params;
    input_params.push_back(TAG_NONCE, "aaaaaaaaaaaa", 12);
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, input_params));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &result, &input_consumed));
    EXPECT_EQ(ciphertext.length(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(&result));
}

TEST_F(EncryptionOperationsTest, AesDecryptTooShortNonce) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));

    // Try decrypting garbage ciphertext with too-short nonce
    string ciphertext(15, 'a');
    AuthorizationSet input_params;
    input_params.push_back(TAG_NONCE, "aaaaaaaaaaa", 11);
    EXPECT_EQ(KM_ERROR_INVALID_ARGUMENT, BeginOperation(KM_PURPOSE_DECRYPT, input_params));
}

TEST_F(EncryptionOperationsTest, AesOcbRoundTripEmptySuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string message = "";
    string nonce;
    string ciphertext = EncryptMessage(message, &nonce);
    EXPECT_EQ(12, nonce.size());
    EXPECT_EQ(message.size() + 16 /* tag */, ciphertext.size());

    string plaintext = DecryptMessage(ciphertext, nonce);
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, AesOcbRoundTripEmptyCorrupted) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string message = "";
    string nonce;
    string ciphertext = EncryptMessage(message, &nonce);
    EXPECT_EQ(12, nonce.size());
    EXPECT_EQ(message.size() + 16 /* tag */, ciphertext.size());

    ciphertext[ciphertext.size() / 2]++;

    AuthorizationSet input_set;
    input_set.push_back(TAG_NONCE, nonce.data(), nonce.size());
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT, input_set));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &result, &input_consumed));
    EXPECT_EQ(ciphertext.length(), input_consumed);
    EXPECT_EQ(KM_ERROR_VERIFICATION_FAILED, FinishOperation(&result));
}

TEST_F(EncryptionOperationsTest, AesOcbFullChunk) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string message(4096, 'a');
    string nonce;
    string ciphertext = EncryptMessage(message, &nonce);
    EXPECT_EQ(message.length() + 16 /* tag */, ciphertext.size());

    string plaintext = DecryptMessage(ciphertext, nonce);
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, AesOcbVariousChunkLengths) {
    for (unsigned chunk_length = 1; chunk_length <= 128; ++chunk_length) {
        ASSERT_EQ(KM_ERROR_OK,
                  GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(chunk_length, 16)));
        string message(128, 'a');
        string nonce;
        string ciphertext = EncryptMessage(message, &nonce);
        int expected_tag_count = (message.length() + chunk_length - 1) / chunk_length;
        EXPECT_EQ(message.length() + 16 * expected_tag_count, ciphertext.size())
            << "Unexpected ciphertext size for chunk length " << chunk_length
            << " expected tag count was " << expected_tag_count
            << " but actual tag count was probably "
            << (ciphertext.size() - message.length() - 12) / 16;

        string plaintext = DecryptMessage(ciphertext, nonce);
        EXPECT_EQ(message, plaintext);
    }
}

TEST_F(EncryptionOperationsTest, AesOcbAbort) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16)));
    string message = "Hello";

    AuthorizationSet input_set, output_set;
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT, input_set, &output_set));
    EXPECT_EQ(1, output_set.size());
    EXPECT_EQ(0, output_set.find(TAG_NONCE));

    string result;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &result, &input_consumed));
    EXPECT_EQ(message.length(), input_consumed);
    EXPECT_EQ(KM_ERROR_OK, AbortOperation());
}

TEST_F(EncryptionOperationsTest, AesOcbNoChunkLength) {
    EXPECT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder()
                                           .AesEncryptionKey(128)
                                           .Option(TAG_BLOCK_MODE, KM_MODE_OCB)
                                           .Option(TAG_MAC_LENGTH, 16)));
    EXPECT_EQ(KM_ERROR_INVALID_ARGUMENT, BeginOperation(KM_PURPOSE_ENCRYPT));
}

TEST_F(EncryptionOperationsTest, AesOcbPaddingUnsupported) {
    ASSERT_EQ(KM_ERROR_OK,
              GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 16).Option(
                  TAG_PADDING, KM_PAD_ZERO)));
    EXPECT_EQ(KM_ERROR_UNSUPPORTED_PADDING_MODE, BeginOperation(KM_PURPOSE_ENCRYPT));
}

TEST_F(EncryptionOperationsTest, AesOcbInvalidMacLength) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).OcbMode(4096, 17)));
    EXPECT_EQ(KM_ERROR_INVALID_ARGUMENT, BeginOperation(KM_PURPOSE_ENCRYPT));
}

uint8_t rfc_7523_test_key_data[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
};
string rfc_7523_test_key = make_string(rfc_7523_test_key_data);

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector1) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    };
    uint8_t expected_ciphertext[] = {
        0x78, 0x54, 0x07, 0xBF, 0xFF, 0xC8, 0xAD, 0x9E,
        0xDC, 0xC5, 0x52, 0x0A, 0xC9, 0x11, 0x1E, 0xE6,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), "" /* associated_data */,
                          "" /* plaintext */, make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector2) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x01,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    };
    uint8_t expected_ciphertext[] = {
        0x68, 0x20, 0xB3, 0x65, 0x7B, 0x6F, 0x61, 0x5A, 0x57, 0x25, 0xBD, 0xA0,
        0xD3, 0xB4, 0xEB, 0x3A, 0x25, 0x7C, 0x9A, 0xF1, 0xF8, 0xF0, 0x30, 0x09,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector3) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x02,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    };
    uint8_t expected_ciphertext[] = {
        0x81, 0x01, 0x7F, 0x82, 0x03, 0xF0, 0x81, 0x27,
        0x71, 0x52, 0xFA, 0xDE, 0x69, 0x4A, 0x0A, 0x00,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          "" /* plaintext */, make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector4) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x03,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    };
    uint8_t expected_ciphertext[] = {
        0x45, 0xDD, 0x69, 0xF8, 0xF5, 0xAA, 0xE7, 0x24, 0x14, 0x05, 0x4C, 0xD1,
        0xF3, 0x5D, 0x82, 0x76, 0x0B, 0x2C, 0xD0, 0x0D, 0x2F, 0x99, 0xBF, 0xA9,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), "" /* associated_data */,
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector5) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x04,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    uint8_t expected_ciphertext[] = {
        0x57, 0x1D, 0x53, 0x5B, 0x60, 0xB2, 0x77, 0x18, 0x8B, 0xE5, 0x14,
        0x71, 0x70, 0xA9, 0xA2, 0x2C, 0x3A, 0xD7, 0xA4, 0xFF, 0x38, 0x35,
        0xB8, 0xC5, 0x70, 0x1C, 0x1C, 0xCE, 0xC8, 0xFC, 0x33, 0x58,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector6) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x05,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    uint8_t expected_ciphertext[] = {
        0x8C, 0xF7, 0x61, 0xB6, 0x90, 0x2E, 0xF7, 0x64,
        0x46, 0x2A, 0xD8, 0x64, 0x98, 0xCA, 0x6B, 0x97,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          "" /* plaintext */, make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector7) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x06,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    };
    uint8_t expected_ciphertext[] = {
        0x5C, 0xE8, 0x8E, 0xC2, 0xE0, 0x69, 0x27, 0x06, 0xA9, 0x15, 0xC0,
        0x0A, 0xEB, 0x8B, 0x23, 0x96, 0xF4, 0x0E, 0x1C, 0x74, 0x3F, 0x52,
        0x43, 0x6B, 0xDF, 0x06, 0xD8, 0xFA, 0x1E, 0xCA, 0x34, 0x3D,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), "" /* associated_data */,
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector8) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x07,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    };
    uint8_t expected_ciphertext[] = {
        0x1C, 0xA2, 0x20, 0x73, 0x08, 0xC8, 0x7C, 0x01, 0x07, 0x56, 0x10, 0x4D, 0x88, 0x40,
        0xCE, 0x19, 0x52, 0xF0, 0x96, 0x73, 0xA4, 0x48, 0xA1, 0x22, 0xC9, 0x2C, 0x62, 0x24,
        0x10, 0x51, 0xF5, 0x73, 0x56, 0xD7, 0xF3, 0xC9, 0x0B, 0xB0, 0xE0, 0x7F,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector9) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x08,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    };
    uint8_t expected_ciphertext[] = {
        0x6D, 0xC2, 0x25, 0xA0, 0x71, 0xFC, 0x1B, 0x9F,
        0x7C, 0x69, 0xF9, 0x3B, 0x0F, 0x1E, 0x10, 0xDE,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          "" /* plaintext */, make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector10) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x09,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    };
    uint8_t expected_ciphertext[] = {
        0x22, 0x1B, 0xD0, 0xDE, 0x7F, 0xA6, 0xFE, 0x99, 0x3E, 0xCC, 0xD7, 0x69, 0x46, 0x0A,
        0x0A, 0xF2, 0xD6, 0xCD, 0xED, 0x0C, 0x39, 0x5B, 0x1C, 0x3C, 0xE7, 0x25, 0xF3, 0x24,
        0x94, 0xB9, 0xF9, 0x14, 0xD8, 0x5C, 0x0B, 0x1E, 0xB3, 0x83, 0x57, 0xFF,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), "" /* associated_data */,
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector11) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x0A,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    uint8_t expected_ciphertext[] = {
        0xBD, 0x6F, 0x6C, 0x49, 0x62, 0x01, 0xC6, 0x92, 0x96, 0xC1, 0x1E, 0xFD,
        0x13, 0x8A, 0x46, 0x7A, 0xBD, 0x3C, 0x70, 0x79, 0x24, 0xB9, 0x64, 0xDE,
        0xAF, 0xFC, 0x40, 0x31, 0x9A, 0xF5, 0xA4, 0x85, 0x40, 0xFB, 0xBA, 0x18,
        0x6C, 0x55, 0x53, 0xC6, 0x8A, 0xD9, 0xF5, 0x92, 0xA7, 0x9A, 0x42, 0x40,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector12) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x0B,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    uint8_t plaintext[] = {};
    uint8_t expected_ciphertext[] = {
        0xFE, 0x80, 0x69, 0x0B, 0xEE, 0x8A, 0x48, 0x5D,
        0x11, 0xF3, 0x29, 0x65, 0xBC, 0x9D, 0x2A, 0x32,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          "" /* plaintext */, make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector13) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x0C,
    };
    uint8_t associated_data[] = {};
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
        0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    };
    uint8_t expected_ciphertext[] = {
        0x29, 0x42, 0xBF, 0xC7, 0x73, 0xBD, 0xA2, 0x3C, 0xAB, 0xC6, 0xAC, 0xFD,
        0x9B, 0xFD, 0x58, 0x35, 0xBD, 0x30, 0x0F, 0x09, 0x73, 0x79, 0x2E, 0xF4,
        0x60, 0x40, 0xC5, 0x3F, 0x14, 0x32, 0xBC, 0xDF, 0xB5, 0xE1, 0xDD, 0xE3,
        0xBC, 0x18, 0xA5, 0xF8, 0x40, 0xB5, 0x2E, 0x65, 0x34, 0x44, 0xD5, 0xDF,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), "" /* associated_data */,
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector14) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x0D,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
        0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
        0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    };
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
        0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
        0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    };
    uint8_t expected_ciphertext[] = {
        0xD5, 0xCA, 0x91, 0x74, 0x84, 0x10, 0xC1, 0x75, 0x1F, 0xF8, 0xA2, 0xF6, 0x18, 0x25,
        0x5B, 0x68, 0xA0, 0xA1, 0x2E, 0x09, 0x3F, 0xF4, 0x54, 0x60, 0x6E, 0x59, 0xF9, 0xC1,
        0xD0, 0xDD, 0xC5, 0x4B, 0x65, 0xE8, 0x62, 0x8E, 0x56, 0x8B, 0xAD, 0x7A, 0xED, 0x07,
        0xBA, 0x06, 0xA4, 0xA6, 0x94, 0x83, 0xA7, 0x03, 0x54, 0x90, 0xC5, 0x76, 0x9E, 0x60,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector15) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x0E,
    };
    uint8_t associated_data[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
        0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
        0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    };
    uint8_t plaintext[] = {};
    uint8_t expected_ciphertext[] = {
        0xC5, 0xCD, 0x9D, 0x18, 0x50, 0xC1, 0x41, 0xE3,
        0x58, 0x64, 0x99, 0x94, 0xEE, 0x70, 0x1B, 0x68,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), make_string(associated_data),
                          "" /* plaintext */, make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesOcbRfc7253TestVector16) {
    uint8_t nonce[] = {
        0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x0F,
    };
    uint8_t associated_data[] = {};
    uint8_t plaintext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
        0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
        0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    };
    uint8_t expected_ciphertext[] = {
        0x44, 0x12, 0x92, 0x34, 0x93, 0xC5, 0x7D, 0x5D, 0xE0, 0xD7, 0x00, 0xF7, 0x53, 0xCC,
        0xE0, 0xD1, 0xD2, 0xD9, 0x50, 0x60, 0x12, 0x2E, 0x9F, 0x15, 0xA5, 0xDD, 0xBF, 0xC5,
        0x78, 0x7E, 0x50, 0xB5, 0xCC, 0x55, 0xEE, 0x50, 0x7B, 0xCB, 0x08, 0x4E, 0x47, 0x9A,
        0xD3, 0x63, 0xAC, 0x36, 0x6B, 0x95, 0xA9, 0x8C, 0xA5, 0xF3, 0x00, 0x0B, 0x14, 0x79,
    };
    CheckAesOcbTestVector(rfc_7523_test_key, make_string(nonce), "" /* associated_data */,
                          make_string(plaintext), make_string(expected_ciphertext));
}

TEST_F(EncryptionOperationsTest, AesEcbRoundTripSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).Option(TAG_BLOCK_MODE,
                                                                                   KM_MODE_ECB)));
    // Two-block message.
    string message = "12345678901234567890123456789012";
    string ciphertext1 = EncryptMessage(message);
    EXPECT_EQ(message.size(), ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message));
    EXPECT_EQ(message.size(), ciphertext2.size());

    // ECB is deterministic.
    EXPECT_EQ(ciphertext1, ciphertext2);

    string plaintext = DecryptMessage(ciphertext1);
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, AesEcbNoPaddingWrongInputSize) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).Option(TAG_BLOCK_MODE,
                                                                                   KM_MODE_ECB)));
    // Message is slightly shorter than two blocks.
    string message = "1234567890123456789012345678901";

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT));
    string ciphertext;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(message, &ciphertext, &input_consumed));
    EXPECT_EQ(message.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_INVALID_INPUT_LENGTH, FinishOperation(&ciphertext));
}

TEST_F(EncryptionOperationsTest, AesEcbPkcs7Padding) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder()
                                           .AesEncryptionKey(128)
                                           .Option(TAG_BLOCK_MODE, KM_MODE_ECB)
                                           .Option(TAG_PADDING, KM_PAD_PKCS7)));

    // Try various message lengths; all should work.
    for (int i = 0; i < 32; ++i) {
        string message(i, 'a');
        string ciphertext = EncryptMessage(message);
        EXPECT_EQ(i + 16 - (i % 16), ciphertext.size());
        string plaintext = DecryptMessage(ciphertext);
        EXPECT_EQ(message, plaintext);
    }
}

TEST_F(EncryptionOperationsTest, AesEcbPkcs7PaddingCorrupted) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder()
                                           .AesEncryptionKey(128)
                                           .Option(TAG_BLOCK_MODE, KM_MODE_ECB)
                                           .Option(TAG_PADDING, KM_PAD_PKCS7)));

    string message = "a";
    string ciphertext = EncryptMessage(message);
    EXPECT_EQ(16, ciphertext.size());
    EXPECT_NE(ciphertext, message);
    ++ciphertext[ciphertext.size() / 2];

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT));
    string plaintext;
    size_t input_consumed;
    EXPECT_EQ(KM_ERROR_OK, UpdateOperation(ciphertext, &plaintext, &input_consumed));
    EXPECT_EQ(ciphertext.size(), input_consumed);
    EXPECT_EQ(KM_ERROR_INVALID_ARGUMENT, FinishOperation(&plaintext));
}

TEST_F(EncryptionOperationsTest, AesCbcRoundTripSuccess) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).Option(TAG_BLOCK_MODE,
                                                                                   KM_MODE_CBC)));
    // Two-block message.
    string message = "12345678901234567890123456789012";
    string ciphertext1 = EncryptMessage(message);
    EXPECT_EQ(message.size() + 16, ciphertext1.size());

    string ciphertext2 = EncryptMessage(string(message));
    EXPECT_EQ(message.size() + 16, ciphertext2.size());

    // CBC uses random IVs, so ciphertexts shouldn't match.
    EXPECT_NE(ciphertext1, ciphertext2);

    string plaintext = DecryptMessage(ciphertext1);
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, AesCbcIncrementalNoPadding) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder().AesEncryptionKey(128).Option(TAG_BLOCK_MODE,
                                                                                   KM_MODE_CBC)));

    int increment = 15;
    string message(240, 'a');
    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_ENCRYPT));
    string ciphertext;
    size_t input_consumed;
    for (size_t i = 0; i < message.size(); i += increment)
        EXPECT_EQ(KM_ERROR_OK,
                  UpdateOperation(message.substr(i, increment), &ciphertext, &input_consumed));
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&ciphertext));
    EXPECT_EQ(message.size() + 16, ciphertext.size());

    EXPECT_EQ(KM_ERROR_OK, BeginOperation(KM_PURPOSE_DECRYPT));
    string plaintext;
    for (size_t i = 0; i < ciphertext.size(); i += increment)
        EXPECT_EQ(KM_ERROR_OK,
                  UpdateOperation(ciphertext.substr(i, increment), &plaintext, &input_consumed));
    EXPECT_EQ(KM_ERROR_OK, FinishOperation(&plaintext));
    EXPECT_EQ(ciphertext.size() - 16, plaintext.size());
    EXPECT_EQ(message, plaintext);
}

TEST_F(EncryptionOperationsTest, AesCbcPkcs7Padding) {
    ASSERT_EQ(KM_ERROR_OK, GenerateKey(ParamBuilder()
                                           .AesEncryptionKey(128)
                                           .Option(TAG_BLOCK_MODE, KM_MODE_CBC)
                                           .Option(TAG_PADDING, KM_PAD_PKCS7)));

    // Try various message lengths; all should work.
    for (int i = 0; i < 32; ++i) {
        string message(i, 'a');
        string ciphertext = EncryptMessage(message);
        EXPECT_EQ(i + 32 - (i % 16), ciphertext.size());
        string plaintext = DecryptMessage(ciphertext);
        EXPECT_EQ(message, plaintext);
    }
}

}  // namespace test
}  // namespace keymaster
