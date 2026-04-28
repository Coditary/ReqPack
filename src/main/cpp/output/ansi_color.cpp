#include "output/ansi_color.h"

#include <sstream>

namespace {

struct ParsedSpec {
	bool bold{false};
	bool dim{false};
	bool underline{false};
	int  fgCode{-1};   ///< -1 = no color; 30-37 = normal; 90-97 = bright.
};

ParsedSpec parse_spec(const std::string& spec) {
	if (spec.empty() || spec == "none") {
		return {};
	}

	ParsedSpec out;
	std::istringstream ss(spec);
	std::string token;

	while (ss >> token) {
		if      (token == "bold")             { out.bold      = true; }
		else if (token == "dim")              { out.dim       = true; }
		else if (token == "underline")        { out.underline = true; }
		else if (token == "black")            { out.fgCode = 30; }
		else if (token == "red")              { out.fgCode = 31; }
		else if (token == "green")            { out.fgCode = 32; }
		else if (token == "yellow")           { out.fgCode = 33; }
		else if (token == "blue")             { out.fgCode = 34; }
		else if (token == "magenta")          { out.fgCode = 35; }
		else if (token == "cyan")             { out.fgCode = 36; }
		else if (token == "white")            { out.fgCode = 37; }
		else if (token == "bright_black"  ||
		         token == "dark_gray")        { out.fgCode = 90; }
		else if (token == "bright_red")       { out.fgCode = 91; }
		else if (token == "bright_green")     { out.fgCode = 92; }
		else if (token == "bright_yellow")    { out.fgCode = 93; }
		else if (token == "bright_blue")      { out.fgCode = 94; }
		else if (token == "bright_magenta")   { out.fgCode = 95; }
		else if (token == "bright_cyan")      { out.fgCode = 96; }
		else if (token == "bright_white")     { out.fgCode = 97; }
	}

	return out;
}

bool has_any(const ParsedSpec& s) {
	return s.bold || s.dim || s.underline || (s.fgCode >= 0);
}

} // namespace

std::string ansi_open(const std::string& spec) {
	const ParsedSpec s = parse_spec(spec);
	if (!has_any(s)) return {};

	std::string codes;
	auto append = [&](int code) {
		if (!codes.empty()) codes += ';';
		codes += std::to_string(code);
	};

	if (s.bold)      append(1);
	if (s.dim)       append(2);
	if (s.underline) append(4);
	if (s.fgCode >= 0) append(s.fgCode);

	return "\033[" + codes + "m";
}

std::string ansi_reset(const std::string& spec) {
	const ParsedSpec s = parse_spec(spec);
	return has_any(s) ? "\033[0m" : std::string{};
}

std::string ansi_wrap(const std::string& text, const std::string& spec) {
	const std::string open = ansi_open(spec);
	if (open.empty()) return text;
	return open + text + "\033[0m";
}
