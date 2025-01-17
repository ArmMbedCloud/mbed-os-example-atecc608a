/*
 * Copyright (c) 2019, Arm Limited and affiliates
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#if defined(ATCA_HAL_I2C)
#include "psa/crypto.h"
#include "atecc608a_se.h"
#include "atecc608a_utils.h"
#include "atca_helpers.h"
#include "atecc508a_config_dev.h"

/** This macro checks if the result of an `expression` is equal to an
 *  `expected` value and sets a `status` variable of type `psa_status_t` to
 *  `PSA_SUCCESS`. If they are not equal, the `status` is set to
 *  `psa_error instead`, the error details are printed, and the code jumps
 *  to the `exit` label. */
#define ASSERT_STATUS_PSA(expression, expected, psa_error)            \
    do                                                                \
    {                                                                 \
        psa_status_t ASSERT_result = (expression);                    \
        psa_status_t ASSERT_expected = (expected);                    \
        if ((ASSERT_result) != (ASSERT_expected))                     \
        {                                                             \
            printf("assertion failed at %s:%d "                       \
                   "(actual=%ld expected=%ld)\n", __FILE__, __LINE__, \
                   ASSERT_result, ASSERT_expected);                   \
            status = (psa_error);                                     \
            goto exit;                                                \
        }                                                             \
        status = PSA_SUCCESS;                                         \
    } while(0)

#define USAGE \
    "\n\nAvailable commands:\n"       \
    " - info - print configuration information;\n" \
    " - test - run all tests on the device;\n"\
    " - exit - exit the interactive loop;\n"\
    " - generate_private[=%%d] - generate a private key in a given slot (0-15),\n"\
    "                          default slot - 0.\n"\
    " - generate_public=%%d_%%d - generate a public key in a given slot\n"\
    "                           (0-15, first argument) using a private key\n"\
    "                           from a given slot (0-15, second argument);\n"\
    " - private_slot=%%d - designate a slot to be used as a private key in tests;\n"\
    " - public_slot=%%d - designate a slot to be used as a public key in tests;\n"\
    " - write_lock_config - write a hardcoded configuration to the device,\n"\
    "                       lock it;\n"\
    " - lock_data - lock the data zone;\n\n"

#define WARNING_CONFIG \
    "\n\nWarning! Locking a configuration zone is irreversible.\n"\
    "Please make sure that a desired configuration is used in the process.\n"\
    "Are you sure you want to proceed? [y/n]: "

#define WARNING_DATA \
    "\n\nWarning! Locking the data/OTP zone is irreversible.\n"\
    "Please note that locking the data/OTP zone does not mean that\n"\
    "the values in these zones cannot be modified; locking indicates that\n"\
    "the slot now behaves according to the policies set by the associated\n"\
    "configuration zone’s values. [y/n]: "

/* Data used by tests */
psa_key_slot_number_t atecc608a_private_key_slot = 0;
psa_key_slot_number_t atecc608a_public_key_slot = 9;

enum {
    key_type = PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_CURVE_SECP256R1),
    keypair_type = PSA_KEY_TYPE_ECC_KEYPAIR(PSA_ECC_CURVE_SECP256R1),
    key_bits = 256,
    hash_alg = PSA_ALG_SHA_256,
    alg = PSA_ALG_ECDSA(hash_alg),
    sig_size = PSA_ASYMMETRIC_SIGN_OUTPUT_SIZE(key_type, key_bits, alg),
    pubkey_size = PSA_KEY_EXPORT_ECC_PUBLIC_KEY_MAX_SIZE(key_bits),
    hash_size = PSA_HASH_SIZE(hash_alg),
};

psa_status_t atecc608a_hash_sha256(const uint8_t *input, size_t input_size,
                                   const uint8_t *expected_hash,
                                   size_t expected_hash_size)
{
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    uint8_t actual_hash[ATCA_SHA_DIGEST_SIZE] = {0};

    ASSERT_SUCCESS_PSA(atecc608a_init());
    ASSERT_SUCCESS(atcab_hw_sha2_256(input, input_size, actual_hash));

    ASSERT_STATUS(memcmp(actual_hash, expected_hash, sizeof(actual_hash)), 0,
                  PSA_ERROR_HARDWARE_FAILURE);

exit:
    atecc608a_deinit();
    return status;
}

psa_status_t atecc608a_print_locked_zones()
{
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    bool locked;
    printf("--- Device locks information ---\n");
    ASSERT_SUCCESS_PSA(atecc608a_init());
    ASSERT_SUCCESS(atcab_is_locked(LOCK_ZONE_CONFIG, &locked));
    printf("  - Config locked: %d\n", locked);
    ASSERT_SUCCESS(atcab_is_locked(LOCK_ZONE_DATA, &locked));
    printf("  - Data locked: %d\n", locked);
    for (uint8_t i = 0; i < 16; i++) {
        ASSERT_SUCCESS(atcab_is_slot_locked(i, &locked));
        printf("  - Slot %d locked: %d\n", i, locked);
    }
    printf("--------------------------------\n");

exit:
    atecc608a_deinit();
    return status;
}

psa_status_t atecc608a_print_serial_number()
{
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    uint8_t serial[ATCA_SERIAL_NUM_SIZE];
    size_t buffer_length;

    ASSERT_SUCCESS_PSA(atecc608a_get_serial_number(serial,
                                                   ATCA_SERIAL_NUM_SIZE,
                                                   &buffer_length));
    printf("Serial Number:\n");
    atcab_printbin_sp(serial, buffer_length);
    printf("\n");
exit:
    return status;
}

psa_status_t atecc608a_print_config_zone()
{
    uint8_t config_buffer[ATCA_ECC_CONFIG_SIZE] = {0};
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    ASSERT_SUCCESS_PSA(atecc608a_init());
    ASSERT_SUCCESS(atcab_read_config_zone(config_buffer));
    atcab_printbin_label("Config zone: ", config_buffer, ATCA_ECC_CONFIG_SIZE);
exit:
    atecc608a_deinit();
    return status;
}

/* Test that a 32 byte clear text write and read can be performed on a slot. */
psa_status_t test_write_read_slot(uint16_t slot)
{
    const uint8_t test_write_read_size = 32;
    psa_status_t status = PSA_ERROR_GENERIC_ERROR;
    uint8_t data_write[test_write_read_size];
    uint8_t data_read[test_write_read_size];

    ASSERT_SUCCESS_PSA(atecc608a_random_32_bytes(data_write, test_write_read_size));
    ASSERT_SUCCESS_PSA(atecc608a_write(slot, 0, data_write, test_write_read_size));
    ASSERT_SUCCESS_PSA(atecc608a_read(slot, 0, data_read, test_write_read_size));
    ASSERT_STATUS(memcmp(data_write, data_read, test_write_read_size),
                  0, PSA_ERROR_HARDWARE_FAILURE);

    printf("test_write_read_slot succesful!\n");
exit:
    return status;
}

/* Test that a signature from hardware can be verified by PSA with a public
 * key imported to PSA. */
psa_status_t test_psa_import_verify()
{
    psa_status_t status;
    psa_key_handle_t verify_handle;
    psa_key_policy_t policy = PSA_KEY_POLICY_INIT;
    static uint8_t pubkey[pubkey_size];
    size_t pubkey_len = 0;
    uint8_t signature[sig_size];
    size_t signature_length = 0;
    const uint8_t hash[hash_size] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    };

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_asym->p_sign(
                           atecc608a_private_key_slot, alg, hash, sizeof(hash),
                           signature, sizeof(signature), &signature_length));

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_export(
                           atecc608a_private_key_slot, pubkey, sizeof(pubkey),
                           &pubkey_len));
    /*
     * Import the secure element's public key into a volatile key slot.
     */
    ASSERT_SUCCESS_PSA(psa_allocate_key(&verify_handle));

    psa_key_policy_set_usage(&policy, PSA_KEY_USAGE_VERIFY, alg);
    ASSERT_SUCCESS_PSA(psa_set_key_policy(verify_handle, &policy));

    ASSERT_SUCCESS_PSA(psa_import_key(verify_handle, key_type, pubkey,
                                      pubkey_len));

    /* Verify that the signature produced by the secure element is valid. */
    ASSERT_SUCCESS_PSA(psa_asymmetric_verify(verify_handle, alg, hash,
                                             sizeof(hash), signature,
                                             signature_length));

    printf("test_psa_import_verify succesful!\n");
exit:
    return status;
}

/* Test that a public key generated while generating a private key can
 * be imported. */
psa_status_t test_generate_import()
{
    /* Valid values */
    psa_status_t status;
    static uint8_t pubkey[pubkey_size];
    size_t pubkey_len = 0;

    /* Invalid values */
    const uint16_t bad_key_id = 16;
    const psa_key_type_t bad_key_type = PSA_KEY_TYPE_RSA_PUBLIC_KEY;
    const size_t bad_key_bits = 5;
    const size_t bad_buffer_size = 64;

    /* Passing an invalid key slot should fail. */
    ASSERT_STATUS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                          bad_key_id, keypair_type,
                          PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                          key_bits, NULL, 0, pubkey, pubkey_size, &pubkey_len),
                      PSA_ERROR_INVALID_ARGUMENT,
                      PSA_ERROR_HARDWARE_FAILURE);

    /* Passing an invalid key type should fail. */
    ASSERT_STATUS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                          atecc608a_private_key_slot, bad_key_type,
                          PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                          key_bits, NULL, 0, pubkey, pubkey_size,
                          &pubkey_len),
                      PSA_ERROR_NOT_SUPPORTED,
                      PSA_ERROR_HARDWARE_FAILURE);

    /* Passing invalid key bits should fail. */
    ASSERT_STATUS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                          atecc608a_private_key_slot, keypair_type,
                          PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                          bad_key_bits, NULL, 0, pubkey, pubkey_size,
                          &pubkey_len),
                      PSA_ERROR_NOT_SUPPORTED,
                      PSA_ERROR_HARDWARE_FAILURE);

    /* Passing an invalid size should fail. */
    ASSERT_STATUS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                          atecc608a_private_key_slot, keypair_type,
                          PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                          key_bits, NULL, 0, pubkey, bad_buffer_size,
                          &pubkey_len),
                      PSA_ERROR_BUFFER_TOO_SMALL,
                      PSA_ERROR_HARDWARE_FAILURE);

    /* Passing a NULL public key buffer should work, regardless of its size. */
    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                           atecc608a_private_key_slot, keypair_type,
                           PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                           key_bits, NULL, 0, NULL, pubkey_size,
                           &pubkey_len));

    /* Passing a NULL pubkey_len should work, even when exporting a public key. */
    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                           atecc608a_private_key_slot, keypair_type,
                           PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                           key_bits, NULL, 0, pubkey, pubkey_size, NULL));

    /* Test that a public key received during a private key generation
     * can be imported. */
    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                           atecc608a_private_key_slot, keypair_type,
                           PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                           key_bits, NULL, 0, pubkey, pubkey_size,
                           &pubkey_len));

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_import(
                           atecc608a_public_key_slot,
                           atecc608a_drv_info.lifetime,
                           key_type, alg, PSA_KEY_USAGE_VERIFY, pubkey,
                           pubkey_len));

    /* Importing with a bad size should fail. */
    ASSERT_STATUS_PSA(atecc608a_drv_info.p_key_management->p_import(
                          atecc608a_public_key_slot,
                          atecc608a_drv_info.lifetime,
                          key_type, alg, PSA_KEY_USAGE_VERIFY, pubkey,
                          0),
                      PSA_ERROR_INVALID_ARGUMENT,
                      PSA_ERROR_HARDWARE_FAILURE);


    printf("test_generate_import succesful!\n");
exit:
    return status;
}

/* Test that a public key that is exported from a private key can be
 * imported to a public key slot by the driver. */
psa_status_t test_export_import()
{
    psa_status_t status;
    static uint8_t pubkey[pubkey_size];
    size_t pubkey_len = 0;

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_export(
                           atecc608a_private_key_slot, pubkey,
                           sizeof(pubkey), &pubkey_len));

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_import(
                           atecc608a_public_key_slot,
                           atecc608a_drv_info.lifetime,
                           key_type, alg, PSA_KEY_USAGE_VERIFY, pubkey,
                           pubkey_len));
    printf("test_export_import succesful!\n");
exit:
    return status;
}

/* Test that signing using the generated private key and verifying using
 * the exported public key works. */
psa_status_t test_sign_verify()
{
    psa_status_t status;
    const uint8_t hash[hash_size] = {};
    uint8_t signature[sig_size];
    size_t signature_length = 0;
    static uint8_t pubkey[pubkey_size];
    size_t pubkey_len = 0;

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_generate(
                           atecc608a_private_key_slot, keypair_type,
                           PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                           key_bits, NULL, 0, pubkey, pubkey_size,
                           &pubkey_len));

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_key_management->p_import(
                           atecc608a_public_key_slot,
                           atecc608a_drv_info.lifetime,
                           key_type, alg, PSA_KEY_USAGE_VERIFY, pubkey,
                           pubkey_len));

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_asym->p_sign(
                           atecc608a_private_key_slot, alg, hash,
                           sizeof(hash), signature, sizeof(signature),
                           &signature_length));

    ASSERT_SUCCESS_PSA(atecc608a_drv_info.p_asym->p_verify(
                           atecc608a_public_key_slot, alg, hash, sizeof(hash),
                           signature, signature_length));
    printf("test_sign_verify succesful!\n");
exit:
    return status;
}

/* Test that hardware sha256 works. */
psa_status_t test_hash_sha256()
{
    psa_status_t status;
    const uint8_t hash_input1[] = "abc";
    /* SHA-256 hash of ['a','b','c'] */
    const uint8_t sha256_expected_hash1[] = {
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD
    };

    const uint8_t hash_input2[] = "";
    /* SHA-256 hash of an empty string */
    const uint8_t sha256_expected_hash2[] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };

    ASSERT_SUCCESS_PSA(atecc608a_hash_sha256(hash_input1,
                                             sizeof(hash_input1) - 1,
                                             sha256_expected_hash1,
                                             sizeof(sha256_expected_hash1)));

    ASSERT_SUCCESS_PSA(atecc608a_hash_sha256(hash_input2,
                                             sizeof(hash_input2) - 1,
                                             sha256_expected_hash2,
                                             sizeof(sha256_expected_hash2)));

    printf("test_hash_sha256 succesful!\n");
exit:
    return status;
}

psa_status_t run_tests()
{
    psa_status_t status;

    printf("Running tests...\n");
    ASSERT_SUCCESS_PSA(test_hash_sha256());

    /* Verify that the device has a locked config zone before running tests
     * that use slots. */
    ASSERT_SUCCESS_PSA(atecc608a_check_zone_locked(LOCK_ZONE_CONFIG));

    ASSERT_SUCCESS_PSA(test_generate_import());
    ASSERT_SUCCESS_PSA(test_export_import());
    ASSERT_SUCCESS_PSA(test_sign_verify());
    ASSERT_SUCCESS_PSA(test_psa_import_verify());

    /* Verify that the device has a locked data zone before running tests
     * that use clear text read. */
    ASSERT_SUCCESS_PSA(atecc608a_check_zone_locked(LOCK_ZONE_DATA));

    /* Slot 8 is usually used as a clear write and read certificate
     * or signature slot, as it is the biggest one (416 bytes of space). */
    ASSERT_SUCCESS_PSA(test_write_read_slot(8));

exit:
    return status;
}

void print_device_info()
{
    atecc608a_print_serial_number();
    atecc608a_print_config_zone();
    atecc608a_print_locked_zones();
    printf("\nPrivate key slot in use: %lu, public: %lu\n",
           atecc608a_private_key_slot, atecc608a_public_key_slot);
}

bool prompt_confirmation(char *message)
{
    char confirmation[2];
    printf(message);
    scanf("%1s", confirmation);
    printf("\n");
    if (confirmation[0] == 'y' || confirmation[0] == 'Y') {
        return true;
    }
    return false;
}

bool interactive_loop()
{
    char command[80];
    char *arg;
    size_t len;

    printf(USAGE);
    scanf("%79s", command);
    len = strlen(command);

    arg = strchr(command, '=');

    if (strcmp(command, "info") == 0) {
        print_device_info();
    } else if (strcmp(command, "exit") == 0) {
        return true;
    } else if (strcmp(command, "test") == 0) {
        run_tests();
    } else if (strncmp(command, "generate_private", strlen("generate_private") - 1) == 0) {
        uint16_t slot = 0;
        psa_status_t status;

        // If there is an argument supplied
        if (len > strlen("generate_private=0") - 1 && arg != NULL) {
            slot = (uint16_t) atoi(arg + 1);
        }

        if (slot > 15) {
            printf("Invalid slot %u provided for generate_private command.\n", slot);
            return false;
        }
        printf("Generating a private key in slot %u... ", slot);
        status = atecc608a_drv_info.p_key_management->p_generate(
                     slot, keypair_type,
                     PSA_KEY_USAGE_SIGN | PSA_KEY_USAGE_VERIFY,
                     key_bits, NULL, 0, NULL, 0, NULL);
        if (status != PSA_SUCCESS) {
            printf("Failed! Error %ld.\n", status);
            return false;
        }
        printf("Done.\n");
    } else if (strncmp(command, "generate_public", strlen("generate_public") - 1) == 0) {
        uint16_t slot_private = 0;
        uint16_t slot_public = 9;
        static uint8_t pubkey[pubkey_size];
        size_t pubkey_len = 0;
        psa_status_t status;

        // Check if an argument is missing
        if (len <= strlen("generate_public=0_9") - 1) {
            printf("Please specify both slots for public key generation.\n");
            return false;
        }
        slot_private = (uint16_t) atoi(arg + 1);
        slot_public = (uint16_t) atoi(strrchr(command, '_') + 1);

        if (slot_private > 15 || slot_public > 15) {
            printf("Invalid slots provided for generate_public command: %u, %u\n",
                   slot_private, slot_public);
            return false;
        }

        printf("Exporting a public key from private key in slot %u... ",
               slot_private);
        status = atecc608a_drv_info.p_key_management->p_export(
                     slot_private, pubkey, sizeof(pubkey),
                     &pubkey_len);
        if (status != PSA_SUCCESS) {
            printf("Failed! Error %ld.\n", status);
            return false;
        }
        printf("Done.\n");

        printf("Importing public key to slot %u... ", slot_public);
        status = atecc608a_drv_info.p_key_management->p_import(
                     slot_public,
                     atecc608a_drv_info.lifetime,
                     key_type, alg, PSA_KEY_USAGE_VERIFY, pubkey,
                     pubkey_len);
        if (status != PSA_SUCCESS) {
            printf("Failed! Error %ld.\n", status);
            return false;
        }
        printf("Done.\n");
    } else if (strcmp(command, "write_lock_config") == 0) {
        psa_status_t status;
        if (!prompt_confirmation(WARNING_CONFIG)) {
            return false;
        }
        printf("Writing configuration and locking the config zone... ");
        status = atecc608a_write_lock_config(template_config_508a_dev,
                                             sizeof(template_config_508a_dev));
        if (status != PSA_SUCCESS) {
            printf("Failed! Error %ld.\n", status);
            return false;
        }
        printf("Done.\n");
    } else if (strcmp(command, "lock_data") == 0) {
        psa_status_t status;
        if (!prompt_confirmation(WARNING_DATA)) {
            return false;
        }
        printf("Locking the data/OTP zone... ");
        status = atecc608a_lock_data_zone();
        if (status != PSA_SUCCESS) {
            printf("Failed! Error %ld.\n", status);
            return false;
        }
        printf("Done.\n");
    } else if (strncmp(command, "private_slot", strlen("private_slot") - 1) == 0) {
        uint16_t slot = 0;

        // If there is no argument
        if (len <= strlen("private_slot=0") - 1) {
            printf("Please specify a slot that will be used as a private key in tests.\n");
            return false;
        }

        slot = (uint16_t) atoi(arg + 1);
        if (slot > 15) {
            printf("Invalid slot %u provided as a private key slot.\n", slot);
            return false;
        }
        atecc608a_private_key_slot = slot;

        printf("The private key slot in use is now %u.\n", slot);
    } else if (strncmp(command, "public_slot", strlen("public_slot") - 1) == 0) {
        uint16_t slot = 9;

        // If there is no argument
        if (len <= strlen("public_slot=9") - 1) {
            printf("Please specify a slot that will be used as a public key in tests.\n");
            return false;
        }

        slot = (uint16_t) atoi(arg + 1);
        if (slot > 15) {
            printf("Invalid slot %u provided as a public key slot.\n", slot);
            return false;
        }
        atecc608a_public_key_slot = slot;

        printf("The public key slot in use is now %u.\n", slot);
    } else {
        printf("Unrecognized command - \'%s\'.\n", command);
    }
    return false;
}

int main(void)
{
    psa_status_t status;
    bool exit_application = false;

    print_device_info();
    ASSERT_SUCCESS_PSA(psa_crypto_init());
    run_tests();

    while (!exit_application) {
        exit_application = interactive_loop();
    }

exit:
    printf("Exiting application.\n");
    return status;
}
#else
int main(void)
{
    printf("Not all of the required options are defined:\n"
           "  - ATCA_HAL_I2C\n");
    return 0;
}
#endif
