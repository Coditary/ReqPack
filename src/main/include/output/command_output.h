#pragma once

#include "core/types.h"
#include "output/idisplay.h"

#include <memory>
#include <ostream>
#include <string>
#include <vector>

enum class CommandOutputBlockType {
	MESSAGE,
	TABLE,
	FIELD_VALUE,
	ARTIFACT,
	RAW_TEXT,
};

struct CommandOutputField {
	std::string key;
	std::string value;
};

struct CommandOutputMessage {
	std::string text;
	std::string source;
};

struct CommandOutputArtifact {
	std::string label;
	std::string value;
};

struct CommandOutputTable {
	std::vector<std::string> headers;
	std::vector<std::vector<std::string>> rows;
};

struct CommandOutputBlock {
	CommandOutputBlockType type{CommandOutputBlockType::MESSAGE};
	CommandOutputMessage message{};
	CommandOutputArtifact artifact{};
	CommandOutputTable table{};
	std::vector<CommandOutputField> fields{};
	std::string rawText{};
};

struct CommandOutput {
	DisplayMode mode{DisplayMode::IDLE};
	std::vector<std::string> sessionItems{};
	std::vector<CommandOutputBlock> blocks{};
	bool success{true};
	int succeeded{0};
	int skipped{0};
	int failed{0};
};

std::string join_command_output_values(const std::vector<std::string>& values);

CommandOutputBlock make_command_message_block(const std::string& text,
	                                          const std::string& source = {});
CommandOutputBlock make_command_artifact_block(const std::string& label,
	                                           const std::string& value);
CommandOutputBlock make_command_table_block(const std::vector<std::string>& headers,
	                                        const std::vector<std::vector<std::string>>& rows);
CommandOutputBlock make_command_field_value_block(const std::vector<CommandOutputField>& fields);
CommandOutputBlock make_command_raw_text_block(const std::string& rawText);

std::vector<CommandOutputField> package_info_to_fields(const PackageInfo& info);
std::vector<std::vector<std::string>> package_infos_to_rows(const std::vector<PackageInfo>& items,
	                                                      bool includeSystem);

void render_command_output(const CommandOutput& output);
std::string render_command_output_text(const CommandOutput& output);

std::unique_ptr<IDisplay> create_plain_stream_display(std::ostream& stream);
