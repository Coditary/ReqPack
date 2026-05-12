#!/usr/bin/env bash

scenario_prepare() {
    local map_entries="      rqp = 'rqp-osv',
      anvil = 'anvil-osv',
      glyph = 'glyph-osv',"
    base_prepare_audit_fixture "empty" false "${map_entries}"
}

scenario_command() {
    printf 'cd %q && rqp --config %q audit' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "audit all known systems without findings exits zero"
    assert_output_contains "No vulnerabilities or audit findings detected." "${STDOUT_PATH}"
    assert_output_not_contains "CVE-TEST-" "${STDOUT_PATH}"
    assert_output_not_contains "unsupported ecosystem mapping" "${STDOUT_PATH}"
}
