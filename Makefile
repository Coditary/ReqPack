.PHONY: all clean run build test test-unit test-smoke

# Standard-Aktion: Kompilieren
all: build
	cmake --build build -j$(nproc)

# Erstellt den build-Ordner, falls er nicht existiert, und konfiguriert CMake
build:
	mkdir -p build
	cd build && cmake ..

# Startet das Programm
run: all
	@echo
	@echo "---------------------- Running ReqPack ----------------------"
	@echo
	@echo
	@./build/ReqPack ${ARGS}

# Führt alle Tests aus
test: all
	@cd build && ctest --output-on-failure

# Führt nur Unit-Tests aus
test-unit: all
	@cd build && ctest --output-on-failure -R "^unit::"

# Führt nur Smoke-Tests aus
test-smoke: all
	@cd build && ctest --output-on-failure -R "^integration::"

# Löscht den Build-Ordner
clean:
	rm -rf build
