#include <gssapi.h>

gss_OID GSS_C_NT_HOSTBASED_SERVICE = (gss_OID)0;

OM_uint32 gss_display_status(OM_uint32 *minor_status,
                             OM_uint32 status_value,
                             int status_type,
                             const gss_OID mech_type,
                             OM_uint32 *message_context,
                             gss_buffer_t status_string) {
    (void)minor_status; (void)status_value; (void)status_type;
    (void)mech_type; (void)message_context; (void)status_string;
    return 0;
}

OM_uint32 gss_release_buffer(OM_uint32 *minor_status,
                             gss_buffer_t buffer) {
    (void)minor_status; (void)buffer;
    return 0;
}

OM_uint32 gss_delete_sec_context(OM_uint32 *minor_status,
                                 gss_ctx_id_t *context_handle,
                                 gss_buffer_t output_token) {
    (void)minor_status; (void)context_handle; (void)output_token;
    return 0;
}

OM_uint32 gss_import_name(OM_uint32 *minor_status,
                          const gss_buffer_t input_name_buffer,
                          const gss_OID input_name_type,
                          gss_name_t *output_name) {
    (void)minor_status; (void)input_name_buffer;
    (void)input_name_type; (void)output_name;
    return 0;
}

OM_uint32 gss_release_name(OM_uint32 *minor_status,
                           gss_name_t *name) {
    (void)minor_status; (void)name;
    return 0;
}

OM_uint32 gss_init_sec_context(OM_uint32 *minor_status,
                               const gss_cred_id_t claimant_cred_handle,
                               gss_ctx_id_t *context_handle,
                               const gss_name_t target_name,
                               const gss_OID mech_type,
                               OM_uint32 req_flags,
                               OM_uint32 time_req,
                               const gss_channel_bindings_t input_chan_bindings,
                               const gss_buffer_t input_token,
                               gss_OID *actual_mech_type,
                               gss_buffer_t output_token,
                               OM_uint32 *ret_flags,
                               OM_uint32 *time_rec) {
    (void)minor_status; (void)claimant_cred_handle; (void)context_handle;
    (void)target_name; (void)mech_type; (void)req_flags; (void)time_req;
    (void)input_chan_bindings; (void)input_token; (void)actual_mech_type;
    (void)output_token; (void)ret_flags; (void)time_rec;
    return 0;
}

OM_uint32 gss_indicate_mechs(OM_uint32 *minor_status,
                             gss_OID_set *mech_set) {
    (void)minor_status; (void)mech_set;
    return 0;
}

OM_uint32 gss_release_oid_set(OM_uint32 *minor_status,
                              gss_OID_set *set) {
    (void)minor_status; (void)set;
    return 0;
}
