CMAKE ?= cmake
CARGO ?= cargo
PYTHON ?= python3
BUILD_DIR ?= build
IREZ_DEPS_DIR ?= $(abspath ../Software_Repos)
TMPDIR ?= /tmp
E2E_STATE ?= $(TMPDIR)/irez-v01-e2e-state

# Windows (MSVC) builds use the scripts in scripts/ instead; see
# docs/BUILDING.md.

.PHONY: build mcp test quality verify e2e package-linux clean
build:
	$(CMAKE) -S . -B $(BUILD_DIR) -DIREZ_DEPS_DIR="$(IREZ_DEPS_DIR)" -DCMAKE_BUILD_TYPE=RelWithDebInfo
	$(CMAKE) --build $(BUILD_DIR) -j

mcp:
	cd mcp && $(CARGO) build --release --locked

quality:
	cd mcp && $(CARGO) fmt --check
	cd mcp && $(CARGO) clippy --locked --all-targets -- -D warnings
	$(CARGO) metadata --manifest-path mcp/Cargo.toml --locked --format-version 1 >/dev/null
	$(PYTHON) scripts/generate_contract.py --check
	$(PYTHON) scripts/generate_third_party_notices.py --check
	$(PYTHON) scripts/release_notes.py --check
	PYTHONPYCACHEPREFIX="$(TMPDIR)/irez-pycache" $(PYTHON) -m compileall -q scripts

package-linux: build mcp
	$(PYTHON) scripts/package_release.py --platform linux-x86_64 --cpp-bin "$(BUILD_DIR)" --mcp-bin mcp/target/release/irez-mcp

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure
	cd mcp && IREZ_CLI="$(abspath $(BUILD_DIR))/irez" $(CARGO) test --locked

verify: quality test

e2e: build
	rm -rf "$(E2E_STATE)"
	$(BUILD_DIR)/irez --state-dir "$(E2E_STATE)" init --name fixture
	$(BUILD_DIR)/irez --state-dir "$(E2E_STATE)" ingest llvm fixtures/nonfloating.ll --index full

clean:
	$(CMAKE) -E remove_directory $(BUILD_DIR)
