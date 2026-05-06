.PHONY: all clean run build test test-unit test-smoke coverage-build test-coverage profile-build profile-tests

JOBS := $(shell command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 1)
COVERAGE_BUILD_DIR := build/coverage
PROFILE_BUILD_DIR := build/profile
PROFILE_TEST_BINARIES := core_unit_tests exec_rules_unit_tests core_integration_tests
PROFILE_GPROF_RUNS := 5
PYTHON := python3

# Standard-Aktion: Kompilieren
all: build
	cmake --build build -j$(JOBS)

# Erstellt den build-Ordner, falls er nicht existiert, und konfiguriert CMake
build:
	cmake -S . -B build

# Startet das Programm
run: all
	@echo
	@echo "---------------------- Running ReqPack ----------------------"
	@echo
	@echo
	@./build/ReqPack ${ARGS}

# Führt alle Tests aus
test: all
	@ctest --test-dir build --output-on-failure

# Führt nur Unit-Tests aus
test-unit: all
	@ctest --test-dir build --output-on-failure -R "^unit::"

# Führt nur Smoke-Tests aus
test-smoke: all
	@ctest --test-dir build --output-on-failure -R "^integration::"

# Baut Coverage-instrumentierte Targets in separatem Build-Ordner
coverage-build:
	cmake -S . -B $(COVERAGE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DREQPACK_ENABLE_COVERAGE=ON
	cmake --build $(COVERAGE_BUILD_DIR) -j$(JOBS)

# Führt Coverage-Tests aus und erzeugt CTest-Coverage-Report
test-coverage: coverage-build
	@ctest --test-dir $(COVERAGE_BUILD_DIR) --output-on-failure
	@ctest --test-dir $(COVERAGE_BUILD_DIR) -T Coverage
	@$(PYTHON) tests/coverage_summary.py $(COVERAGE_BUILD_DIR) .

# Baut Profiling-Targets in separatem Build-Ordner
profile-build:
	cmake -S . -B $(PROFILE_BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DREQPACK_ENABLE_PROFILING=ON
	cmake --build $(PROFILE_BUILD_DIR) -j$(JOBS)

# Führt Profiling pro Test-Binary aus, bevorzugt perf ohne Kindprozesse
profile-tests: profile-build
	@cmake -E make_directory $(PROFILE_BUILD_DIR)/profile-data
	@if command -v perf >/dev/null 2>&1; then \
		for binary in $(PROFILE_TEST_BINARIES); do \
			echo "Profiling $$binary with perf"; \
			"$(CURDIR)/$(PROFILE_BUILD_DIR)/$$binary" > "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/$$binary.log" 2>&1 || { cat "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/$$binary.log"; exit 1; }; \
			perf record --no-inherit --call-graph dwarf --output "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/$$binary.perf.data" -- "$(CURDIR)/$(PROFILE_BUILD_DIR)/$$binary" > "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/$$binary.perf.log" 2>&1 || { cat "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/$$binary.perf.log"; exit 1; }; \
			perf report --stdio --no-children --sort comm,dso,symbol --percent-limit 0.5 --dsos "$$binary" -i "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/$$binary.perf.data" > "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/$$binary.perf-report.txt"; \
			$(PYTHON) tests/profile_summary.py perf "$(PROFILE_BUILD_DIR)/profile-data/$$binary.perf-report.txt"; \
			echo "Full perf report: $(PROFILE_BUILD_DIR)/profile-data/$$binary.perf-report.txt"; \
		done; \
	elif command -v gprof >/dev/null 2>&1; then \
		for run in $$(seq 1 $(PROFILE_GPROF_RUNS)); do \
			cmake -E env GMON_OUT_PREFIX="$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/core_unit_tests" "$(CURDIR)/$(PROFILE_BUILD_DIR)/core_unit_tests" > /dev/null || exit 1; \
			cmake -E env GMON_OUT_PREFIX="$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/exec_rules_unit_tests" "$(CURDIR)/$(PROFILE_BUILD_DIR)/exec_rules_unit_tests" > /dev/null || exit 1; \
		done; \
		gprof "$(CURDIR)/$(PROFILE_BUILD_DIR)/core_unit_tests" "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/core_unit_tests."* > "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/core_unit_tests.gprof.txt"; \
		gprof "$(CURDIR)/$(PROFILE_BUILD_DIR)/exec_rules_unit_tests" "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/exec_rules_unit_tests."* > "$(CURDIR)/$(PROFILE_BUILD_DIR)/profile-data/exec_rules_unit_tests.gprof.txt"; \
		$(PYTHON) tests/profile_summary.py gprof "$(PROFILE_BUILD_DIR)/profile-data/core_unit_tests.gprof.txt"; \
		echo "Full gprof reports: $(PROFILE_BUILD_DIR)/profile-data/core_unit_tests.gprof.txt, $(PROFILE_BUILD_DIR)/profile-data/exec_rules_unit_tests.gprof.txt"; \
	else \
		echo "Neither perf nor gprof is available" >&2; \
		exit 1; \
	fi

# Löscht den Build-Ordner
clean:
	rm -rf build
