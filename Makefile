.PHONY: all clean run build build-rqp test test-unit test-smoke coverage-build test-coverage profile-build profile-tests profile-ground system-tests system-tests-parallel help install install-dev uninstall

NPROC := $(shell command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 2)
JOBS := $(shell expr $(NPROC) / 4)
BUILD_DIR := build
RQP_BIN := $(BUILD_DIR)/rqp

# Local beta install (same layout as Taiji): copy into ~/.local/opt/beta-bin and ~/.local/bin
INSTALL_DIR := $(HOME)/.local/opt/beta-bin
INSTALL_BIN_DIR := $(HOME)/.local/bin
INSTALL_RQP := $(INSTALL_DIR)/rqp
INSTALL_RQP_BIN := $(INSTALL_BIN_DIR)/rqp
# Dev entrypoint; kept when self-update owns ~/.local/bin/rqp (symlink to release bundle).
INSTALL_RQP_BETA := $(INSTALL_BIN_DIR)/rqp-beta
COVERAGE_BUILD_DIR := build/coverage
PROFILE_BUILD_DIR := build/profile
PROFILE_TEST_BINARIES := reqpack_cli_unit_tests
PROFILE_GPROF_RUNS := 5
PYTHON := python3
LOCAL_BUILD_CMAKE_ARGS := -DREQPACK_LINK_STATIC_LUA:BOOL=OFF -DCMAKE_EXE_LINKER_FLAGS:STRING=
LOCAL_SYSTEM_TEST_JOBS ?= $(JOBS)
LOCAL_SYSTEM_TEST_BUILD_JOBS ?=

all: build
	cmake --build $(BUILD_DIR) -j$(JOBS)

build:
	cmake -S . -B $(BUILD_DIR) $(LOCAL_BUILD_CMAKE_ARGS)

build-rqp: build
	cmake --build $(BUILD_DIR) -j$(JOBS) --target ReqPack

help:
	@echo "ReqPack (rqp)"
	@echo ""
	@echo "  make build        Configure CMake (build/)"
	@echo "  make build-rqp    Build only the rqp binary"
	@echo "  make install      Build rqp → ~/.local/opt/beta-bin/rqp and ~/.local/bin/rqp-beta"
	@echo "  make install      (overwrites ~/.local/bin/rqp only if not self-update symlink)"
	@echo "  make install-dev  Always install ~/.local/bin/rqp from build (overrides self-update link)"
	@echo "  make uninstall    Remove beta install (rqp-beta + opt/beta-bin; not release symlink)"
	@echo "  make run          Run ./build/rqp (pass ARGS='...')"
	@echo "  make test         Run ctest"
	@echo "  make clean        Remove build/"

install: build-rqp
	@test -f "$(RQP_BIN)" || { echo "Missing $(RQP_BIN); build failed?" >&2; exit 1; }
	mkdir -p "$(INSTALL_DIR)" "$(INSTALL_BIN_DIR)"
	install -m 755 "$(RQP_BIN)" "$(INSTALL_RQP)"
	ln -sf "$(INSTALL_RQP)" "$(INSTALL_RQP_BETA)"
	@if [ -L "$(INSTALL_RQP_BIN)" ] && readlink "$(INSTALL_RQP_BIN)" | grep -q 'reqpack/self/bin'; then \
		echo "Self-update owns $(INSTALL_RQP_BIN) → $$(readlink $(INSTALL_RQP_BIN))"; \
		echo "Use: rqp-beta   or   PATH=$(INSTALL_DIR):$$PATH rqp"; \
	else \
		install -m 755 "$(RQP_BIN)" "$(INSTALL_RQP_BIN)"; \
		echo "Installed $(INSTALL_RQP_BIN)"; \
	fi
	@echo "Installed $(INSTALL_RQP)"
	@echo "Installed $(INSTALL_RQP_BETA) → $(INSTALL_RQP)"

install-dev: build-rqp
	@test -f "$(RQP_BIN)" || { echo "Missing $(RQP_BIN); build failed?" >&2; exit 1; }
	mkdir -p "$(INSTALL_DIR)" "$(INSTALL_BIN_DIR)"
	install -m 755 "$(RQP_BIN)" "$(INSTALL_RQP)"
	ln -sf "$(INSTALL_RQP)" "$(INSTALL_RQP_BETA)"
	rm -f "$(INSTALL_RQP_BIN)"
	install -m 755 "$(RQP_BIN)" "$(INSTALL_RQP_BIN)"
	@echo "Installed dev override $(INSTALL_RQP_BIN) (self-update symlink removed)"

uninstall:
	rm -f "$(INSTALL_RQP)" "$(INSTALL_RQP_BETA)"
	@if [ -f "$(INSTALL_RQP_BIN)" ] && [ ! -L "$(INSTALL_RQP_BIN)" ]; then rm -f "$(INSTALL_RQP_BIN)"; fi
	@rmdir "$(INSTALL_DIR)" 2>/dev/null || true
	@echo "Removed beta install ($(INSTALL_DIR), $(INSTALL_RQP_BETA)); left self-update symlink at $(INSTALL_RQP_BIN) if present"

run: build-rqp
	@echo
	@echo "---------------------- Running ReqPack ----------------------"
	@echo
	@echo
	@$(RQP_BIN) ${ARGS}

test: all
	@ctest --test-dir build --output-on-failure

test-unit: all
	@ctest --test-dir build --output-on-failure -R "^cli::unit::"

test-smoke: all
	@ctest --test-dir build --output-on-failure -R "^integration::"

system-tests:
	@LOCAL_SYSTEM_TEST_BUILD_JOBS="$(LOCAL_SYSTEM_TEST_BUILD_JOBS)" bash scripts/run-all-local-system-tests.sh $(LOCAL_SYSTEM_TEST_SCENARIOS)

system-tests-parallel:
	@JOBS="$(LOCAL_SYSTEM_TEST_JOBS)" LOCAL_SYSTEM_TEST_BUILD_JOBS="$(LOCAL_SYSTEM_TEST_BUILD_JOBS)" bash scripts/run-all-local-system-tests-parallel.sh $(LOCAL_SYSTEM_TEST_SCENARIOS)

coverage-build:
	cmake -S . -B $(COVERAGE_BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DREQPACK_ENABLE_COVERAGE=ON -DRQP_SECURITY_CORE_BUILD_TESTS=ON
	cmake --build $(COVERAGE_BUILD_DIR) -j$(JOBS)

test-coverage: coverage-build
	@ctest --test-dir $(COVERAGE_BUILD_DIR) --output-on-failure -R '^cli::unit::'
	@$(PYTHON) tests/coverage_summary.py $(COVERAGE_BUILD_DIR) . \
		--badge-json .github/badges/coverage.json | tee coverage-summary.txt

profile-build:
	cmake -S . -B $(PROFILE_BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DREQPACK_ENABLE_PROFILING=ON
	cmake --build $(PROFILE_BUILD_DIR) -j$(JOBS)

profile-ground: profile-build
	@bash scripts/profile-ground.sh

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
	rm -rf $(BUILD_DIR)
