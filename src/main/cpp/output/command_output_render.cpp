#include "output/command_output.h"

#include "command_output_internal.h"

#include "output/logger.h"
#include "output/plain_display.h"

#include <sstream>

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

void render_field_value_block(Logger& logger, const std::vector<CommandOutputField>& fields) {
    logger.displayTableHeader({"Field", "Value"});
    for (const auto& field : fields) {
        logger.displayTableRow({field.key, field.value});
    }
    logger.displayTableEnd();
}

void render_raw_text_block(Logger& logger, const std::string& rawText) {
    std::istringstream stream(rawText);
    std::string line;
    bool emitted = false;
    while (std::getline(stream, line)) {
        emitted = true;
        logger.emit(OutputAction::DISPLAY_MESSAGE, OutputContext{.message = line});
    }
    if (!rawText.empty() && rawText.back() == '\n') {
        return;
    }
    if (!emitted && !rawText.empty()) {
        logger.emit(OutputAction::DISPLAY_MESSAGE, OutputContext{.message = rawText});
    }
}

void render_block(Logger& logger, const CommandOutputBlock& block) {
    switch (block.type) {
        case CommandOutputBlockType::MESSAGE:
            logger.emit(OutputAction::DISPLAY_MESSAGE, OutputContext{.message = block.message.text, .source = block.message.source});
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
        case CommandOutputBlockType::FIELD_VALUE:
            render_field_value_block(logger, block.fields);
            break;
        case CommandOutputBlockType::RAW_TEXT:
            render_raw_text_block(logger, block.rawText);
            break;
    }
}

void render_text_field_value_block(PlainDisplay& display, const std::vector<CommandOutputField>& fields) {
    display.onTableBegin({"Field", "Value"});
    for (const auto& field : fields) {
        display.onTableRow({field.key, field.value});
    }
    display.onTableEnd();
}

void render_text_block(std::ostream& stream, PlainDisplay& display, const CommandOutputBlock& block) {
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
            render_text_field_value_block(display, block.fields);
            break;
        case CommandOutputBlockType::RAW_TEXT:
            stream << block.rawText;
            if (!block.rawText.empty() && block.rawText.back() != '\n') {
                stream << '\n';
            }
            break;
    }
}

}  // namespace

namespace command_output_internal {

std::unique_ptr<IDisplay> make_plain_stream_display(std::ostream& stream) {
    return std::make_unique<PlainStreamDisplay>(stream);
}

}  // namespace command_output_internal

void render_command_output(const CommandOutput& output) {
    Logger& logger = Logger::instance();
    logger.displaySessionBegin(output.mode, output.sessionItems);
    for (const auto& block : output.blocks) {
        render_block(logger, block);
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
        render_text_block(stream, display, block);
    }
    display.onSessionEnd(output.success, output.succeeded, output.skipped, output.failed);
    display.flush();
    return stream.str();
}

std::unique_ptr<IDisplay> create_plain_stream_display(std::ostream& stream) {
    return command_output_internal::make_plain_stream_display(stream);
}
