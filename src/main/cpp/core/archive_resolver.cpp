#include "core/archive_resolver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string escape_shell_arg(const std::string& value) {
    std::string escaped{"'"};
    for (char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
            continue;
        }
        escaped.push_back(ch);
    }
    escaped.push_back('\'');
    return escaped;
}

std::string trim_line(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

std::filesystem::path make_unique_directory(const std::filesystem::path& root, const std::string& prefix) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::runtime_error("failed to create archive temp root: " + root.string());
    }

    const std::filesystem::path pattern = root / (prefix + "-XXXXXX");
    std::string templateString = pattern.string();
    std::vector<char> buffer(templateString.begin(), templateString.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        throw std::runtime_error("failed to create archive temp directory");
    }
    return created;
}

std::string run_command_capture(const std::string& command) {
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to inspect archive");
    }

    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
        output += buffer;
    }

    const int status = ::pclose(pipe);
    if (status != 0) {
        throw std::runtime_error("failed to inspect archive");
    }
    return output;
}

bool path_has_invalid_segments(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return true;
    }

    for (const std::filesystem::path& part : path) {
        const std::string token = part.string();
        if (token.empty() || token == "." || token == "..") {
            return true;
        }
    }
    return false;
}

std::string normalize_archive_entry(std::string path) {
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        line = trim_line(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

std::vector<std::string> archive_entries(const std::filesystem::path& archivePath, const std::string& suffix) {
    if (suffix == ".zip") {
        return split_lines(run_command_capture("unzip -Z1 " + escape_shell_arg(archivePath.string())));
    }
    if (suffix == ".gz" || suffix == ".bz2" || suffix == ".xz" || suffix == ".zst") {
        return {"payload"};
    }
    return split_lines(run_command_capture("tar -tf " + escape_shell_arg(archivePath.string())));
}

void validate_archive_entries(const std::vector<std::string>& entries) {
    if (entries.empty()) {
        throw std::runtime_error("archive is empty");
    }

    for (std::string entry : entries) {
        entry = normalize_archive_entry(entry);
        if (entry.empty()) {
            continue;
        }

        if (path_has_invalid_segments(std::filesystem::path(entry))) {
            throw std::runtime_error("archive contains unsafe path: " + entry);
        }
    }
}

void verify_extracted_tree(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        throw std::runtime_error("archive extraction produced no output");
    }

    bool hasEntries = false;
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         it != std::filesystem::recursive_directory_iterator();
         it.increment(error)) {
        if (error) {
            throw std::runtime_error("failed to inspect extracted archive");
        }

        hasEntries = true;
        const std::filesystem::directory_entry& entry = *it;
        std::error_code typeError;
        if (entry.is_symlink(typeError) || typeError) {
            throw std::runtime_error("archive contains unsupported symlink entry");
        }
    }

    if (!hasEntries) {
        throw std::runtime_error("archive extraction produced no files");
    }
}

void extract_archive_to_directory(const std::filesystem::path& archivePath, const std::filesystem::path& outputDirectory) {
    const std::string suffix = generic_archive_suffix(archivePath);
    if (suffix.empty()) {
        return;
    }

    validate_archive_entries(archive_entries(archivePath, suffix));

    const std::string archive = escape_shell_arg(archivePath.string());
    const std::string output = escape_shell_arg(outputDirectory.string());
    std::string command;
    if (suffix == ".zip") {
        command = "unzip -oq " + archive + " -d " + output;
    } else if (suffix == ".tar" || suffix == ".tar.gz" || suffix == ".tgz" || suffix == ".tar.bz2" || suffix == ".tbz2" ||
               suffix == ".tar.xz" || suffix == ".txz" || suffix == ".tar.zst" || suffix == ".tzst") {
        command = "tar -xf " + archive + " -C " + output;
    } else if (suffix == ".gz") {
        command = "gzip -cd " + archive + " > " + output + "/payload";
    } else if (suffix == ".bz2") {
        command = "bzip2 -cd " + archive + " > " + output + "/payload";
    } else if (suffix == ".xz") {
        command = "xz -cd " + archive + " > " + output + "/payload";
    } else if (suffix == ".zst") {
        command = "zstd -q -d -c " + archive + " > " + output + "/payload";
    }

    if (command.empty() || std::system(command.c_str()) != 0) {
        throw std::runtime_error("failed to extract archive: " + archivePath.string());
    }

    verify_extracted_tree(outputDirectory);
}

}  // namespace

std::string generic_archive_suffix(const std::filesystem::path& path) {
    static const std::array<std::string, 13> suffixes{
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".pkg.tar.zst", ".pkg.tar.xz", ".pkg.tar.gz", ".tgz", ".tbz2", ".txz", ".tzst", ".zip", ".tar"
    };
    static const std::array<std::string, 4> compressedSuffixes{".gz", ".bz2", ".xz", ".zst"};

    const std::string filename = to_lower_copy(path.filename().string());
    for (const std::string& suffix : suffixes) {
        if (filename.size() >= suffix.size() && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return suffix;
        }
    }
    for (const std::string& suffix : compressedSuffixes) {
        if (filename.size() >= suffix.size() && filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return suffix;
        }
    }
    return {};
}

bool is_generic_archive_path(const std::filesystem::path& path) {
    return !generic_archive_suffix(path).empty();
}

ArchiveResolution extract_archive_to_temp_directory(const std::filesystem::path& path) {
    ArchiveResolution result;
    result.installPath = path;
    if (!is_generic_archive_path(path)) {
        return result;
    }

    const std::filesystem::path outputDirectory = make_unique_directory(std::filesystem::temp_directory_path() / "reqpack" / "archives", "extract");
    extract_archive_to_directory(path, outputDirectory);
    result.installPath = outputDirectory;
    result.cleanupPaths.push_back(outputDirectory);
    result.changed = true;
    return result;
}

bool extract_archive_in_place(const std::filesystem::path& path) {
    if (!is_generic_archive_path(path)) {
        return false;
    }

    const std::filesystem::path parent = path.parent_path().empty() ? std::filesystem::current_path() : path.parent_path();
    const std::filesystem::path stagingDirectory = make_unique_directory(parent, path.filename().string() + ".extract");
    extract_archive_to_directory(path, stagingDirectory);

    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
        std::filesystem::remove_all(stagingDirectory, error);
        throw std::runtime_error("failed to replace archive with extracted directory: " + path.string());
    }

    std::filesystem::rename(stagingDirectory, path, error);
    if (error) {
        std::filesystem::remove_all(stagingDirectory, error);
        throw std::runtime_error("failed to finalize extracted archive directory: " + path.string());
    }

    return true;
}
