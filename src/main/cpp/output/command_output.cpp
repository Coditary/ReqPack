#include "output/command_output.h"

std::string join_command_output_values(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (value.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += value;
    }
    return out;
}

CommandOutputBlock make_command_message_block(const std::string& text, const std::string& source) {
    CommandOutputBlock block;
    block.type = CommandOutputBlockType::MESSAGE;
    block.message = CommandOutputMessage{.text = text, .source = source};
    return block;
}

CommandOutputBlock make_command_artifact_block(const std::string& label, const std::string& value) {
    CommandOutputBlock block;
    block.type = CommandOutputBlockType::ARTIFACT;
    block.artifact = CommandOutputArtifact{.label = label, .value = value};
    return block;
}

CommandOutputBlock make_command_table_block(
    const std::vector<std::string>& headers,
    const std::vector<std::vector<std::string>>& rows
) {
    CommandOutputBlock block;
    block.type = CommandOutputBlockType::TABLE;
    block.table = CommandOutputTable{.headers = headers, .rows = rows};
    return block;
}

CommandOutputBlock make_command_field_value_block(const std::vector<CommandOutputField>& fields) {
    CommandOutputBlock block;
    block.type = CommandOutputBlockType::FIELD_VALUE;
    block.fields = fields;
    return block;
}

CommandOutputBlock make_command_raw_text_block(const std::string& rawText) {
    CommandOutputBlock block;
    block.type = CommandOutputBlockType::RAW_TEXT;
    block.rawText = rawText;
    return block;
}
