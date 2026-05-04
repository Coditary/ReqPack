#include "output/command_output.h"

#include "output/plain_display.h"
#include "output/logger.h"

#include <sstream>
#include <utility>

namespace {

class PlainStreamDisplay : public PlainDisplay {
public:
	explicit PlainStreamDisplay(std::ostream& stream) noexcept
	    : stream_(stream) {}

protected:
	std::ostream& out() const override {
		return stream_;
	}

private:
	std::ostream& stream_;
};

void append_field(std::vector<CommandOutputField>& fields,
	              const std::string& key,
	              const std::string& value) {
	if (!value.empty()) {
		fields.push_back(CommandOutputField{.key = key, .value = value});
	}
}

void append_list_field(std::vector<CommandOutputField>& fields,
	                  const std::string& key,
	                  const std::vector<std::string>& values) {
	const std::string joined = join_command_output_values(values);
	if (!joined.empty()) {
		fields.push_back(CommandOutputField{.key = key, .value = joined});
	}
}

} // namespace

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

CommandOutputBlock make_command_message_block(const std::string& text,
	                                          const std::string& source) {
	CommandOutputBlock block;
	block.type = CommandOutputBlockType::MESSAGE;
	block.message = CommandOutputMessage{.text = text, .source = source};
	return block;
}

CommandOutputBlock make_command_artifact_block(const std::string& label,
	                                           const std::string& value) {
	CommandOutputBlock block;
	block.type = CommandOutputBlockType::ARTIFACT;
	block.artifact = CommandOutputArtifact{.label = label, .value = value};
	return block;
}

CommandOutputBlock make_command_table_block(const std::vector<std::string>& headers,
	                                        const std::vector<std::vector<std::string>>& rows) {
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

std::vector<CommandOutputField> package_info_to_fields(const PackageInfo& info) {
	std::vector<CommandOutputField> fields;
	append_field(fields, "System", info.system);
	append_field(fields, "Name", info.name);
	append_field(fields, "Package ID", info.packageId);
	append_field(fields, "Version", info.version);
	append_field(fields, "Latest Version", info.latestVersion);
	append_field(fields, "Status", info.status);
	append_field(fields, "Installed", info.installed);
	append_field(fields, "Summary", info.summary);
	append_field(fields, "Description", info.description);
	append_field(fields, "Homepage", info.homepage);
	append_field(fields, "Documentation", info.documentation);
	append_field(fields, "Source URL", info.sourceUrl);
	append_field(fields, "Repository", info.repository);
	append_field(fields, "Channel", info.channel);
	append_field(fields, "Section", info.section);
	append_field(fields, "Architecture", info.architecture);
	append_field(fields, "License", info.license);
	append_field(fields, "Author", info.author);
	append_field(fields, "Maintainer", info.maintainer);
	append_field(fields, "Email", info.email);
	append_field(fields, "Published", info.publishedAt);
	append_field(fields, "Updated", info.updatedAt);
	append_field(fields, "Size", info.size);
	append_field(fields, "Installed Size", info.installedSize);
	append_list_field(fields, "Dependencies", info.dependencies);
	append_list_field(fields, "Optional Dependencies", info.optionalDependencies);
	append_list_field(fields, "Provides", info.provides);
	append_list_field(fields, "Conflicts", info.conflicts);
	append_list_field(fields, "Replaces", info.replaces);
	append_list_field(fields, "Binaries", info.binaries);
	append_list_field(fields, "Tags", info.tags);
	for (const auto& [key, value] : info.extraFields) {
		append_field(fields, key, value);
	}
	return fields;
}

std::vector<std::vector<std::string>> package_infos_to_rows(const std::vector<PackageInfo>& items,
	                                                      bool includeSystem) {
	std::vector<std::vector<std::string>> rows;
	rows.reserve(items.size());
	for (const auto& item : items) {
		std::vector<std::string> row;
		if (includeSystem) {
			row.push_back(item.system);
		}
		row.push_back(item.name);
		row.push_back(item.version);
		row.push_back(item.summary.empty() ? item.description : item.summary);
		rows.push_back(std::move(row));
	}
	return rows;
}

void render_command_output(const CommandOutput& output) {
	Logger& logger = Logger::instance();
	logger.displaySessionBegin(output.mode, output.sessionItems);
	for (const auto& block : output.blocks) {
		switch (block.type) {
			case CommandOutputBlockType::MESSAGE:
				logger.emit(OutputAction::DISPLAY_MESSAGE,
				            OutputContext{.message = block.message.text, .source = block.message.source});
				break;
			case CommandOutputBlockType::ARTIFACT:
				logger.emit(OutputAction::DISPLAY_MESSAGE,
				            OutputContext{.message = block.artifact.label + ": " + block.artifact.value});
				break;
			case CommandOutputBlockType::TABLE:
				logger.displayTableHeader(block.table.headers);
				for (const auto& row : block.table.rows) {
					logger.displayTableRow(row);
				}
				logger.displayTableEnd();
				break;
			case CommandOutputBlockType::FIELD_VALUE: {
				logger.displayTableHeader({"Field", "Value"});
				for (const auto& field : block.fields) {
					logger.displayTableRow({field.key, field.value});
				}
				logger.displayTableEnd();
				break;
			}
			case CommandOutputBlockType::RAW_TEXT: {
				std::istringstream stream(block.rawText);
				std::string line;
				bool emitted = false;
				while (std::getline(stream, line)) {
					emitted = true;
					logger.emit(OutputAction::DISPLAY_MESSAGE,
					            OutputContext{.message = line});
				}
				if (!block.rawText.empty() && block.rawText.back() == '\n') {
					break;
				}
				if (!emitted && !block.rawText.empty()) {
					logger.emit(OutputAction::DISPLAY_MESSAGE,
					            OutputContext{.message = block.rawText});
				}
				break;
			}
		}
	}
	logger.displaySessionEnd(output.success, output.succeeded, output.skipped, output.failed);
}

std::string render_command_output_text(const CommandOutput& output) {
	if (output.blocks.empty()) {
		return {};
	}
	std::ostringstream stream;
	PlainStreamDisplay display(stream);
	display.onSessionBegin(output.mode, output.sessionItems);
	for (const auto& block : output.blocks) {
		switch (block.type) {
			case CommandOutputBlockType::MESSAGE:
				display.onMessage(block.message.text, block.message.source);
				break;
			case CommandOutputBlockType::ARTIFACT:
				display.onMessage(block.artifact.label + ": " + block.artifact.value);
				break;
			case CommandOutputBlockType::TABLE:
				display.onTableBegin(block.table.headers);
				for (const auto& row : block.table.rows) {
					display.onTableRow(row);
				}
				display.onTableEnd();
				break;
			case CommandOutputBlockType::FIELD_VALUE:
				display.onTableBegin({"Field", "Value"});
				for (const auto& field : block.fields) {
					display.onTableRow({field.key, field.value});
				}
				display.onTableEnd();
				break;
			case CommandOutputBlockType::RAW_TEXT:
				stream << block.rawText;
				if (!block.rawText.empty() && block.rawText.back() != '\n') {
					stream << '\n';
				}
				break;
		}
	}
	display.onSessionEnd(output.success, output.succeeded, output.skipped, output.failed);
	display.flush();
	return stream.str();
}

std::unique_ptr<IDisplay> create_plain_stream_display(std::ostream& stream) {
	return std::make_unique<PlainStreamDisplay>(stream);
}
