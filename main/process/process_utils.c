#include "../identity.h"
#include "../jade_assert.h"
#include "../jade_wally_verify.h"
#include "../keychain.h"
#include "../multisig.h"
#include "../ui.h"
#include "../utils/cbor_rpc.h"

#include <sys/time.h>
#include <wally_script.h>

#include "process_utils.h"

// Sanity check extended-data payload fields
bool check_extended_data_fields(CborValue* params, const char* expected_origid, const char* expected_orig,
    const size_t expected_seqnum, const size_t expected_seqlen)
{
    JADE_ASSERT(params);
    JADE_ASSERT(expected_origid);
    JADE_ASSERT(expected_orig);

    const char* orig = NULL;
    size_t origlen = 0;
    rpc_get_string_ptr("orig", params, &orig, &origlen);

    char origid[MAXLEN_ID];
    size_t origidlen = 0;
    rpc_get_string("origid", sizeof(origid), params, origid, &origidlen);

    const size_t len = strlen(expected_orig);
    if (origlen != len || strncmp(orig, expected_orig, len) || strcmp(origid, expected_origid)) {
        JADE_LOGE("Extended data origin fields mismatch");
        return false;
    }

    size_t nextseq = 0;
    size_t seqlen = 0;
    if (!rpc_get_sizet("seqlen", params, &seqlen) || seqlen != expected_seqlen
        || !rpc_get_sizet("seqnum", params, &nextseq) || nextseq != expected_seqnum) {
        JADE_LOGE("Extended data sequence fields mismatch");
        return false;
    }

    // Appears consistent and as expected
    return true;
}

// Extract 'epoch' field from message and use to set internal clock
int params_set_epoch_time(CborValue* params, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_INIT_OUT_PPTR(errmsg);

    uint64_t epoch = 0;
    if (!rpc_get_uint64_t("epoch", params, &epoch)) {
        *errmsg = "Failed to extract valid epoch value from parameters";
        return CBOR_RPC_BAD_PARAMETERS;
    }

    // Set the epoch time
    const struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    const int res = settimeofday(&tv, NULL);
    if (res) {
        JADE_LOGE("settimeofday() failed error: %d", res);
        *errmsg = "Failed to set time";
        return CBOR_RPC_INTERNAL_ERROR;
    }

    // Return no-error
    return 0;
}

// Identity, curve and index are always needed by the 'identity' functions.
bool params_identity_curve_index(CborValue* params, const char** identity, size_t* identity_len, const char** curve,
    size_t* curve_len, size_t* index, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_INIT_OUT_PPTR(identity);
    JADE_INIT_OUT_SIZE(identity_len);
    JADE_INIT_OUT_PPTR(curve);
    JADE_INIT_OUT_SIZE(curve_len);
    JADE_INIT_OUT_SIZE(index);
    JADE_INIT_OUT_PPTR(errmsg);

    rpc_get_string_ptr("identity", params, identity, identity_len);
    if (!*identity || *identity_len >= MAX_DISPLAY_MESSAGE_LEN
        || !is_identity_protocol_valid(*identity, *identity_len)) {
        *errmsg = "Failed to extract valid identity from parameters";
        return false;
    }

    rpc_get_string_ptr("curve", params, curve, curve_len);
    if (!*curve || !is_identity_curve_valid(*curve, *curve_len)) {
        *errmsg = "Failed to extract valid curve name from parameters";
        return false;
    }

    // index is optional
    if (rpc_has_field_data("index", params)) {
        if (!rpc_get_sizet("index", params, index)) {
            *errmsg = "Failed to extract valid index from parameters";
            return false;
        }
    }

    return true;
}

// Hash-prevouts and output index are needed to generate deterministic blinding factors.
bool params_hashprevouts_outputindex(CborValue* params, const uint8_t** hash_prevouts, size_t* hash_prevouts_len,
    size_t* output_index, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_INIT_OUT_PPTR(hash_prevouts);
    JADE_INIT_OUT_SIZE(hash_prevouts_len);
    JADE_INIT_OUT_SIZE(output_index);
    JADE_INIT_OUT_PPTR(errmsg);

    rpc_get_bytes_ptr("hash_prevouts", params, hash_prevouts, hash_prevouts_len);
    if (*hash_prevouts_len != SHA256_LEN) {
        *errmsg = "Failed to extract hash_prevouts from parameters";
        return false;
    }

    if (!rpc_get_sizet("output_index", params, output_index)) {
        *errmsg = "Failed to extract output index from parameters";
        return false;
    }

    return true;
}

// Read multisig name and load the registration record.
bool params_load_multisig(CborValue* params, char* multisig_name, const size_t multisig_name_len,
    multisig_data_t* multisig_data, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_ASSERT(multisig_name);
    JADE_ASSERT(multisig_name_len);
    JADE_ASSERT(multisig_data);
    JADE_INIT_OUT_PPTR(errmsg);

    size_t written = 0;
    rpc_get_string("multisig_name", multisig_name_len, params, multisig_name, &written);
    if (written == 0) {
        *errmsg = "Invalid multisig name parameter";
        return false;
    }

    if (!multisig_load_from_storage(multisig_name, multisig_data, errmsg)) {
        // 'errmsg' populated by above call
        return false;
    }

    return true;
}

// Take a multisig record as above, then read out the signer path suffixes and derive the relevant pubkeys.
// Output any warning messages associated with the signer paths (eg. if they are non-standard, mismatch, etc)
// Required for generating multisig receive addresses and also change addresses (when auto-validating change).
bool params_multisig_pubkeys(const bool is_change, CborValue* params, multisig_data_t* multisig_data, uint8_t* pubkeys,
    const size_t pubkeys_len, size_t* pubkeys_written, char* warningmsg, size_t warningmsg_len, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_ASSERT(multisig_data);
    JADE_ASSERT(pubkeys);
    JADE_ASSERT(pubkeys_len == MAX_MULTISIG_SIGNERS * EC_PUBLIC_KEY_LEN);
    JADE_INIT_OUT_SIZE(pubkeys_written);
    JADE_ASSERT(warningmsg);
    JADE_ASSERT(warningmsg_len);
    JADE_INIT_OUT_PPTR(errmsg);

    // Validate paths
    CborValue all_signer_paths;
    bool all_paths_as_expected;
    bool final_elements_consistent;
    if (!rpc_get_array("paths", params, &all_signer_paths)
        || !multisig_validate_paths(is_change, &all_signer_paths, &all_paths_as_expected, &final_elements_consistent)) {
        *errmsg = "Failed to extract signer paths from parameters";
        return false;
    }

    // If paths are not as expected, see if we have a valid change/non-change path (when expecting the other)
    bool flipped_change_element = false;
    if (!all_paths_as_expected) {
        bool unused;
        multisig_validate_paths(!is_change, &all_signer_paths, &flipped_change_element, &unused);
    }

    // If paths not as expected show a warning message and ask the user to confirm
    if (!all_paths_as_expected || !final_elements_consistent) {
        const char* msg1 = "";
        if (!all_paths_as_expected) {
            if (flipped_change_element) {
                msg1 = is_change ? "Non-change multisig path." : "Multisig change path.";
            } else {
                msg1 = is_change ? "Unusual multisig change path." : "Unusual multisig path.";
            }
        }
        const char* msg2
            = !final_elements_consistent ? "Non-standard multisig with different paths across signers." : "";
        const char* maybe_space = !all_paths_as_expected && !final_elements_consistent ? " " : "";

        const int ret = snprintf(
            warningmsg, warningmsg_len, "Warning: %s%s%s Proceed at your own risk.", msg1, maybe_space, msg2);
        JADE_ASSERT(ret > 0 && ret < warningmsg_len);
    }

    if (!multisig_get_pubkeys(
            multisig_data->xpubs, multisig_data->num_xpubs, &all_signer_paths, pubkeys, pubkeys_len, pubkeys_written)
        || *pubkeys_written != multisig_data->num_xpubs * EC_PUBLIC_KEY_LEN) {
        *errmsg = "Unexpected number of signer paths or invalid path for multisig";
        return false;
    }

    return true;
}

// Get the relevant master blinding key (padded to 64-bytes for low-level calls).
// This may be the master key with a multisig registration (if indicated in the parameters).
// Otherwise, defaults to the master blinding key directly associated with this wallet/signer.
bool params_get_master_blindingkey(
    CborValue* params, uint8_t* master_blinding_key, const size_t master_blinding_key_len, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_ASSERT(master_blinding_key);
    JADE_ASSERT(master_blinding_key_len == HMAC_SHA512_LEN);
    JADE_INIT_OUT_PPTR(errmsg);

    // If no 'multisig_name' parameter, default to the signer's own master blinding key
    if (!rpc_has_field_data("multisig_name", params)) {
        memcpy(master_blinding_key, keychain_get()->master_unblinding_key, master_blinding_key_len);
        return true;
    }

    // If is multisig, extract master key from multisig record
    size_t written = 0;
    char multisig_name[MAX_MULTISIG_NAME_SIZE];
    rpc_get_string("multisig_name", sizeof(multisig_name), params, multisig_name, &written);
    if (written == 0) {
        *errmsg = "Invalid multisig name parameter";
        return false;
    }

    multisig_data_t multisig_data = { 0 };
    if (!multisig_load_from_storage(multisig_name, &multisig_data, errmsg)) {
        // 'errmsg' populated by above call
        return false;
    }

    if (!multisig_get_master_blinding_key(&multisig_data, master_blinding_key, master_blinding_key_len, errmsg)) {
        // 'errmsg' populated by above call
        return false;
    }

    return true;
}

// For now just return 'single-sig' or 'other'.
// In future may extend to inlcude eg. 'green', 'other-multisig', etc.
script_flavour_t get_script_flavour(const uint8_t* script, const size_t script_len)
{
    size_t script_type;
    JADE_WALLY_VERIFY(wally_scriptpubkey_get_type(script, script_len, &script_type));
    if (script_type == WALLY_SCRIPT_TYPE_P2PKH || script_type == WALLY_SCRIPT_TYPE_P2WPKH) {
        return SCRIPT_FLAVOUR_SINGLESIG;
    } else if (script_type == WALLY_SCRIPT_TYPE_MULTISIG) {
        return SCRIPT_FLAVOUR_MULTISIG;
    } else {
        // eg. ga-csv script
        return SCRIPT_FLAVOUR_OTHER;
    }
}

// Track the types of the input prevout scripts
void update_aggregate_scripts_flavour(
    const script_flavour_t new_script_flavour, script_flavour_t* aggregate_scripts_flavour)
{
    JADE_ASSERT(aggregate_scripts_flavour);
    if (*aggregate_scripts_flavour == SCRIPT_FLAVOUR_NONE) {
        // First script sets the 'aggregate_scripts_flavour'
        *aggregate_scripts_flavour = new_script_flavour;
    } else if (*aggregate_scripts_flavour != new_script_flavour) {
        // As soon as we see something differet, set to 'mixed'
        *aggregate_scripts_flavour = SCRIPT_FLAVOUR_MIXED;
    }
}