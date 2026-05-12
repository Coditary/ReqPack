#!/usr/bin/env bash

scenario_prepare() {
    base_prepare
}

scenario_command() {
    printf 'cd %q && rqp --config %q sbom quill zeno missing --sbom-skip-missing-packages --format json --output sbom.json' "${WORKSPACE_DIR}" "${CONFIG_PATH}"
}

scenario_assert() {
    assert_equals "0" "$(read_status)" "sbom skip missing exits zero"
    assert_output_contains "sbom skipping missing package: quill:missing" "${STDOUT_PATH}"
    assert_output_contains "${WORKSPACE_DIR}/sbom.json" "${STDOUT_PATH}"
    assert_file_exists "${WORKSPACE_DIR}/sbom.json"
    assert_output_contains '"name": "zeno"' "${WORKSPACE_DIR}/sbom.json"
    assert_output_not_contains '"name": "missing"' "${WORKSPACE_DIR}/sbom.json"
}
