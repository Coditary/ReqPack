.PHONY: all clean run build

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

# Löscht den Build-Ordner
clean:
	rm -rf build
