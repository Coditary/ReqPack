.PHONY: all clean run build test test-unit test-smoke coverage-build test-coverage profile-build profile-tests

JOBS := $(shell command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 1)
COVERAGE_BUILD_DIR := build/coverage
PROFILE_BUILD_DIR := build/profile
PROFILE_TEST_BINARIES := core_unit_tests exec_rules_unit_tests core_integration_tests
PROFILE_GPROF_RUNS := 5
PYTHON := python3

all: build
	cmake --build build -j$(JOBS)

build:
	cmake -S . -B build

run: all
	@echo
	@echo "---------------------- Running ReqPack ----------------------"
	@echo
	@echo
	@./build/ReqPack ${ARGS}

test: all
	@ctest --test-dir build --output-on-failure

test-unit: all
	@ctest --test-dir build --output-on-failure -R "^unit::"

test-smoke: all
	@ctest --test-dir build --output-on-failure -R "^integration::"

coverage-build:
	cmake -S . -B $(COVERAGE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DREQPACK_ENABLE_COVERAGE=ON
	cmake --build $(COVERAGE_BUILD_DIR) -j$(JOBS)

test-coverage: coverage-build
	@ctest --test-dir $(COVERAGE_BUILD_DIR) --output-on-failure
	@ctest --test-dir $(COVERAGE_BUILD_DIR) -T Coverage
	@$(PYTHON) tests/coverage_summary.py $(COVERAGE_BUILD_DIR) .

profile-build:
	cmake -S . -B $(PROFILE_BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DREQPACK_ENABLE_PROFILING=ON
	cmake --build $(PROFILE_BUILD_DIR) -j$(JOBS)

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

clean:
	rm -rf build
