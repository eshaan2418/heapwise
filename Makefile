BUILD ?= build
JOBS ?= $(shell n=$$(sysctl -n hw.ncpu 2>/dev/null || nproc); echo $$(( n / 2 > 2 ? n / 2 : 2 )))
TRACES ?= bench/traces
REPS ?= 7

.PHONY: build test bench traces lint format demo asan tsan clean

build:
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j$(JOBS)

test: build
	ctest --test-dir $(BUILD) --output-on-failure -j$(JOBS)

asan:
	cmake -S . -B build-asan -G Ninja -DHW_SANITIZE=address -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build build-asan -j$(JOBS)
	ctest --test-dir build-asan --output-on-failure -j$(JOBS)

tsan:
	cmake -S . -B build-tsan -G Ninja -DHW_SANITIZE=thread -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build build-tsan -j$(JOBS)
	ctest --test-dir build-tsan --output-on-failure -j$(JOBS)

traces: build
	bench/scripts/record_traces.sh $(BUILD) $(TRACES)

bench: build
	@test -f $(TRACES)/python-ast.hwt || $(MAKE) traces
	bench/scripts/env.sh > bench/results/ENV.md
	bench/scripts/run.sh $(BUILD) $(TRACES) bench/results/raw.jsonl $(REPS)
	python3 bench/scripts/summarize.py bench/results/raw.jsonl | tee bench/results/summary.md

lint:
	clang-format --dry-run -Werror src/*.cpp src/*.h include/heapwise/*.h tests/*.cpp bench/*.cpp bench/*.h
	@if command -v clang-tidy >/dev/null; then clang-tidy -p $(BUILD) --quiet src/heapwise.cpp src/os.cpp; \
	else echo "clang-tidy not installed; skipping"; fi

format:
	clang-format -i src/*.cpp src/*.h include/heapwise/*.h tests/*.cpp bench/*.cpp bench/*.h

demo:
	@$(MAKE) -s build > /dev/null
	@bench/scripts/demo.sh $(BUILD)

clean:
	rm -rf build build-asan build-tsan build-prof
