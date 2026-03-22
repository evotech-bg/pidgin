.PHONY: help setup test benchmark example clean

VENV := .venv
PY   := $(VENV)/bin/python

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-14s\033[0m %s\n", $$1, $$2}'

setup: ## Create venv and install with C extension
	python3 -m venv $(VENV)
	$(PY) -m pip install -q -e ".[crypto,dev]"
	@echo "  Setup complete. C extension: $$($(PY) -c 'from pidgin._fast import encode_records; print("OK")' 2>/dev/null || echo 'not built')"

test: ## Run all tests
	$(PY) -m pytest tests/ -v

benchmark: ## Run realistic API benchmark (vs gzip, brotli, zstd, Protobuf, msgpack)
	$(PY) -m pip install -q msgpack zstandard orjson protobuf grpcio-tools 2>/dev/null
	$(PY) benchmarks/realistic_api.py

example: ## Run usage example
	$(PY) examples/usage.py

clean: ## Remove build artifacts
	rm -rf dist/ build/ *.egg-info .pytest_cache
	find . -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
	@echo "  Cleaned."
