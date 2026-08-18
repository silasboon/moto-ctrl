idf_component_register(
    SRCS
        "mc_output.c"
        "mc_input.c"
        "mc_config.c"
        "mc_persist.c"
        "mc_crypto.c"
        "mc_keystore.c"
        "mc_status.c"
        "mc_config_json.c"
        "mc_session.c"
        "mc_lock.c"
        "mc_diag.c"
        "mc_power.c"
        "mc_ota.c"
        "mc_ota_release_key.c"
        "mc_event_log.c"
        "vendor/tweetnacl/tweetnacl.c"
        "vendor/cjson/cJSON.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "vendor/tweetnacl"
        "vendor/cjson"
    PRIV_REQUIRES esp_hw_support
)

# Vendored third-party sources are unmodified upstream — don't hold them to
# this project's -Werror / -Wall -Wextra. -w silences their warnings so the
# project's own strict flags still apply to everything else.
set_source_files_properties(
    "vendor/tweetnacl/tweetnacl.c"
    "vendor/cjson/cJSON.c"
    PROPERTIES COMPILE_OPTIONS "-w"
)
