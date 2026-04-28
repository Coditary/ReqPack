#include "core/sbom_exporter.h"

#include <boost/graph/graph_traits.hpp>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string json_escape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (unsigned char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                escaped.push_back(static_cast<char>(c));
                break;
        }
    }
    return escaped;
}

std::string sbom_component_ref(const Package& package) {
    std::ostringstream stream;
    stream << package.system << ':' << package.name;
    if (!package.version.empty()) {
        stream << '@' << package.version;
    }
    if (package.localTarget && !package.sourcePath.empty()) {
        stream << '#' << package.sourcePath;
    }
    return stream.str();
}

std::string purl_name_for(const Package& package, const PluginSecurityMetadata& metadata) {
    if (package.name.empty()) {
        return {};
    }

    if (metadata.purlType == "maven") {
        const std::size_t separator = package.name.find(':');
        if (separator == std::string::npos || separator == 0 || separator == package.name.size() - 1) {
            return {};
        }
        return package.name.substr(0, separator) + "/" + package.name.substr(separator + 1);
    }

    return package.name;
}

std::string purl_for(const Package& package, PluginMetadataProvider* metadataProvider) {
    if (package.name.empty() || metadataProvider == nullptr) {
        return {};
    }

    const auto metadata = metadataProvider->getPluginSecurityMetadata(package.system);
    if (!metadata.has_value() || metadata->purlType.empty()) {
        return {};
    }

    const std::string purlName = purl_name_for(package, metadata.value());
    if (purlName.empty()) {
        return {};
    }

    std::string purl = "pkg:" + metadata->purlType + "/" + purlName;
    if (!package.version.empty()) {
        purl += '@' + package.version;
    }
    return purl;
}

std::vector<Graph::vertex_descriptor> ordered_vertices(const Graph& graph) {
    std::vector<Graph::vertex_descriptor> vertices;
    auto [it, end] = boost::vertices(graph);
    for (; it != end; ++it) {
        vertices.push_back(*it);
    }
    return vertices;
}

std::string package_display_name(const Package& package) {
    if (package.localTarget && !package.sourcePath.empty()) {
        return std::filesystem::path(package.sourcePath).filename().string();
    }
    return package.name;
}

}  // namespace

SbomExporter::SbomExporter(PluginMetadataProvider* metadataProvider, const ReqPackConfig& config)
    : config(config), metadataProvider(metadataProvider) {}

SbomOutputFormat SbomExporter::resolveFormat(const Request& request) const {
    if (!request.outputFormat.empty()) {
        const auto parsed = sbom_output_format_from_string(request.outputFormat);
        if (parsed.has_value()) {
            return parsed.value();
        }
    }

    if (!request.outputPath.empty()) {
        return SbomOutputFormat::CYCLONEDX_JSON;
    }

    return this->config.sbom.defaultFormat;
}

std::string SbomExporter::resolveOutputPath(const Request& request) const {
    if (!request.outputPath.empty()) {
        return request.outputPath;
    }

    return this->config.sbom.defaultOutputPath;
}

std::string SbomExporter::renderTable(const Graph& graph) const {
    std::ostringstream stream;
    stream << "SYSTEM\tNAME\tVERSION\tSOURCE\n";
    for (const Graph::vertex_descriptor vertex : ordered_vertices(graph)) {
        const Package& package = graph[vertex];
        stream << package.system << '\t'
               << package_display_name(package) << '\t'
               << (package.version.empty() ? "-" : package.version) << '\t'
               << (package.sourcePath.empty() ? "-" : package.sourcePath) << '\n';
    }
    return stream.str();
}

std::string SbomExporter::renderJson(const Graph& graph, const bool cyclonedx) const {
    std::ostringstream stream;
    if (cyclonedx) {
        stream << "{\n";
        stream << "  \"bomFormat\": \"CycloneDX\",\n";
        stream << "  \"specVersion\": \"1.5\",\n";
        stream << "  \"version\": 1,\n";
        stream << "  \"components\": [\n";
        const std::vector<Graph::vertex_descriptor> vertices = ordered_vertices(graph);
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            const Package& package = graph[vertices[index]];
            stream << "    {\n";
            stream << "      \"type\": \"library\",\n";
            stream << "      \"bom-ref\": \"" << json_escape(sbom_component_ref(package)) << "\",\n";
            stream << "      \"name\": \"" << json_escape(package_display_name(package)) << "\"";
            if (!package.version.empty()) {
                stream << ",\n      \"version\": \"" << json_escape(package.version) << "\"";
            }

            const std::string purl = purl_for(package, this->metadataProvider);
            if (!purl.empty()) {
                stream << ",\n      \"purl\": \"" << json_escape(purl) << "\"";
            }

            stream << ",\n      \"properties\": [\n";
            stream << "        {\"name\": \"reqpack:system\", \"value\": \"" << json_escape(package.system) << "\"}";
            if (!package.sourcePath.empty()) {
                stream << ",\n        {\"name\": \"reqpack:sourcePath\", \"value\": \"" << json_escape(package.sourcePath) << "\"}";
            }
            if (package.localTarget) {
                stream << ",\n        {\"name\": \"reqpack:localTarget\", \"value\": \"true\"}";
            }
            stream << "\n      ]\n";
            stream << "    }";
            if (index + 1 < vertices.size()) {
                stream << ',';
            }
            stream << '\n';
        }
        stream << "  ]";
        if (this->config.sbom.includeDependencyEdges) {
            stream << ",\n  \"dependencies\": [\n";
            bool firstDependency = true;
            auto [it, end] = boost::vertices(graph);
            for (; it != end; ++it) {
                const Package& package = graph[*it];
                if (!firstDependency) {
                    stream << ",\n";
                }
                firstDependency = false;
                stream << "    {\n";
                stream << "      \"ref\": \"" << json_escape(sbom_component_ref(package)) << "\",\n";
                stream << "      \"dependsOn\": [";
                auto [adjacentIt, adjacentEnd] = boost::adjacent_vertices(*it, graph);
                bool firstEdge = true;
                for (; adjacentIt != adjacentEnd; ++adjacentIt) {
                    if (!firstEdge) {
                        stream << ", ";
                    }
                    firstEdge = false;
                    stream << '\"' << json_escape(sbom_component_ref(graph[*adjacentIt])) << '\"';
                }
                stream << "]\n";
                stream << "    }";
            }
            stream << "\n  ]";
        }
        stream << "\n}";
        return stream.str();
    }

    stream << "{\n";
    stream << "  \"packages\": [\n";
    const std::vector<Graph::vertex_descriptor> vertices = ordered_vertices(graph);
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const Package& package = graph[vertices[index]];
        stream << "    {\n";
        stream << "      \"system\": \"" << json_escape(package.system) << "\",\n";
        stream << "      \"name\": \"" << json_escape(package.name) << "\",\n";
        stream << "      \"displayName\": \"" << json_escape(package_display_name(package)) << "\",\n";
        stream << "      \"version\": \"" << json_escape(package.version) << "\",\n";
        stream << "      \"sourcePath\": \"" << json_escape(package.sourcePath) << "\",\n";
        stream << "      \"localTarget\": " << (package.localTarget ? "true" : "false") << "\n";
        stream << "    }";
        if (index + 1 < vertices.size()) {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "  ]";
    if (this->config.sbom.includeDependencyEdges) {
        stream << ",\n  \"dependencies\": [\n";
        bool firstEdge = true;
        auto [edgeIt, edgeEnd] = boost::edges(graph);
        for (; edgeIt != edgeEnd; ++edgeIt) {
            if (!firstEdge) {
                stream << ",\n";
            }
            firstEdge = false;
            const Package& from = graph[boost::source(*edgeIt, graph)];
            const Package& to = graph[boost::target(*edgeIt, graph)];
            stream << "    {\"from\": \"" << json_escape(sbom_component_ref(from))
                   << "\", \"to\": \"" << json_escape(sbom_component_ref(to)) << "\"}";
        }
        stream << "\n  ]";
    }
    stream << "\n}";
    return stream.str();
}

std::string SbomExporter::renderGraph(const Graph& graph, const Request& request) const {
    switch (resolveFormat(request)) {
        case SbomOutputFormat::JSON:
            return renderJson(graph, false);
        case SbomOutputFormat::CYCLONEDX_JSON:
            return renderJson(graph, true);
        case SbomOutputFormat::TABLE:
        default:
            return renderTable(graph);
    }
}

bool SbomExporter::exportGraph(const Graph& graph, const Request& request) const {
    const std::string rendered = renderGraph(graph, request);
    const std::string outputPath = resolveOutputPath(request);
    if (outputPath.empty()) {
        std::cout << rendered;
        if (rendered.empty() || rendered.back() != '\n') {
            std::cout << '\n';
        }
        std::cout.flush();
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path(), error);
    if (error) {
        std::cerr << "failed to create sbom output directory: " << outputPath << '\n';
        return false;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "failed to open sbom output path: " << outputPath << '\n';
        return false;
    }

    output << rendered;
    if (!output.good()) {
        std::cerr << "failed to write sbom output path: " << outputPath << '\n';
        return false;
    }

    std::cout << outputPath << '\n';
    std::cout.flush();
    return true;
}
