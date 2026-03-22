#!/usr/bin/env bash
# test.sh — Integration test for Pidgin compression across all servers.
#
# Tests nginx (port 9401), Apache (port 9402), and Kong (port 9403).
# For each server:
#   1. Send N requests (learning phase)
#   2. Send request with Accept-Encoding: pidgin
#   3. Verify compressed response has Content-Encoding: pidgin
#   4. Fetch profile from /.well-known/pidgin/<endpoint>
#   5. Compare original vs compressed sizes
#
# Usage:
#   ./test.sh               # test all servers
#   ./test.sh nginx         # test only nginx
#   ./test.sh apache kong   # test specific servers

set -euo pipefail

# ------------------------------------------------------------------ #
# Configuration                                                       #
# ------------------------------------------------------------------ #

BACKEND_PORT=9400
LEARN_REQUESTS=35    # slightly more than the default 30 threshold
ENDPOINTS=("/api/users" "/api/orders" "/api/products")
TIMEOUT=5

declare -A SERVER_PORTS
SERVER_PORTS[nginx]=9401
SERVER_PORTS[apache]=9402
SERVER_PORTS[kong]=9403
SERVER_PORTS[caddy]=9405
SERVER_PORTS[traefik]=9406
SERVER_PORTS[haproxy]=9407

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color
BOLD='\033[1m'

# Counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0

# ------------------------------------------------------------------ #
# Helpers                                                              #
# ------------------------------------------------------------------ #

log_header() {
    echo ""
    echo -e "${BOLD}${BLUE}════════════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}${BLUE}  $1${NC}"
    echo -e "${BOLD}${BLUE}════════════════════════════════════════════════════════${NC}"
}

log_section() {
    echo ""
    echo -e "${CYAN}── $1 ──${NC}"
}

log_pass() {
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    PASSED_TESTS=$((PASSED_TESTS + 1))
    echo -e "  ${GREEN}PASS${NC} $1"
}

log_fail() {
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    FAILED_TESTS=$((FAILED_TESTS + 1))
    echo -e "  ${RED}FAIL${NC} $1"
    if [ -n "${2:-}" ]; then
        echo -e "       ${RED}$2${NC}"
    fi
}

log_skip() {
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
    echo -e "  ${YELLOW}SKIP${NC} $1"
}

log_info() {
    echo -e "  ${CYAN}INFO${NC} $1"
}

wait_for_service() {
    local name="$1"
    local port="$2"
    local max_wait=30
    local waited=0

    while [ $waited -lt $max_wait ]; do
        if curl -sf --max-time 2 "http://localhost:${port}/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done

    echo -e "${RED}Service ${name} on port ${port} did not become ready in ${max_wait}s${NC}"
    return 1
}

get_body_size() {
    # Returns the size of the response body in bytes
    local url="$1"
    shift
    curl -sf --max-time $TIMEOUT "$@" "$url" | wc -c | tr -d ' '
}

# ------------------------------------------------------------------ #
# Test functions                                                       #
# ------------------------------------------------------------------ #

test_health() {
    local name="$1"
    local port="$2"

    if curl -sf --max-time $TIMEOUT "http://localhost:${port}/health" > /dev/null 2>&1; then
        log_pass "${name} health check"
        return 0
    else
        log_fail "${name} health check" "Server not responding on port ${port}"
        return 1
    fi
}

test_learning_phase() {
    local name="$1"
    local port="$2"
    local endpoint="$3"

    log_info "Sending ${LEARN_REQUESTS} learning requests to ${endpoint}..."

    local success=0
    for i in $(seq 1 $LEARN_REQUESTS); do
        local status
        status=$(curl -sf --max-time $TIMEOUT -o /dev/null -w "%{http_code}" \
            "http://localhost:${port}${endpoint}" 2>/dev/null || echo "000")
        if [ "$status" = "200" ]; then
            success=$((success + 1))
        fi
    done

    if [ $success -eq $LEARN_REQUESTS ]; then
        log_pass "${name} learning phase for ${endpoint} (${success}/${LEARN_REQUESTS} requests)"
    else
        log_fail "${name} learning phase for ${endpoint}" "Only ${success}/${LEARN_REQUESTS} succeeded"
    fi
}

test_compression() {
    local name="$1"
    local port="$2"
    local endpoint="$3"

    # Get original size (no pidgin encoding)
    local orig_size
    orig_size=$(curl -sf --max-time $TIMEOUT "http://localhost:${port}${endpoint}" | wc -c | tr -d ' ')

    # Request with Accept-Encoding: pidgin
    local tmp_headers
    tmp_headers=$(mktemp)
    local tmp_body
    tmp_body=$(mktemp)

    curl -sf --max-time $TIMEOUT \
        -H "Accept-Encoding: pidgin" \
        -D "$tmp_headers" \
        -o "$tmp_body" \
        "http://localhost:${port}${endpoint}" 2>/dev/null

    local comp_size
    comp_size=$(wc -c < "$tmp_body" | tr -d ' ')

    # Check Content-Encoding header
    local has_pidgin_encoding
    has_pidgin_encoding=$(grep -ci "content-encoding.*pidgin" "$tmp_headers" 2>/dev/null || echo "0")

    # Check X-Pidgin-Ratio header
    local ratio
    ratio=$(grep -i "x-pidgin-ratio" "$tmp_headers" 2>/dev/null | head -1 | awk '{print $2}' | tr -d '\r' || echo "N/A")

    rm -f "$tmp_headers" "$tmp_body"

    if [ "$has_pidgin_encoding" -gt 0 ]; then
        log_pass "${name} compression on ${endpoint}"
        log_info "Original: ${orig_size} bytes, Compressed: ${comp_size} bytes, Ratio: ${ratio}"

        if [ "$comp_size" -lt "$orig_size" ] && [ "$comp_size" -gt 0 ]; then
            local savings
            savings=$(echo "scale=1; (1 - $comp_size / $orig_size) * 100" | bc 2>/dev/null || echo "?")
            log_pass "${name} size reduction on ${endpoint} (${savings}% smaller)"
        else
            log_fail "${name} size reduction on ${endpoint}" \
                "Compressed (${comp_size}) not smaller than original (${orig_size})"
        fi
    else
        log_fail "${name} compression on ${endpoint}" \
            "No Content-Encoding: pidgin header (orig=${orig_size}, resp=${comp_size})"
    fi
}

test_profile_endpoint() {
    local name="$1"
    local port="$2"
    local endpoint="$3"

    # Strip leading slash for the well-known URL
    local ep_path="${endpoint#/}"
    local profile_url="http://localhost:${port}/.well-known/pidgin/${ep_path}"

    local tmp_headers
    tmp_headers=$(mktemp)
    local tmp_body
    tmp_body=$(mktemp)

    local status
    status=$(curl -sf --max-time $TIMEOUT \
        -D "$tmp_headers" \
        -o "$tmp_body" \
        -w "%{http_code}" \
        "$profile_url" 2>/dev/null || echo "000")

    local profile_size
    profile_size=$(wc -c < "$tmp_body" | tr -d ' ')

    rm -f "$tmp_headers" "$tmp_body"

    if [ "$status" = "200" ] && [ "$profile_size" -gt 10 ]; then
        log_pass "${name} profile endpoint for ${endpoint} (${profile_size} bytes)"
    elif [ "$status" = "404" ]; then
        log_skip "${name} profile for ${endpoint} (not ready yet)"
    else
        log_fail "${name} profile endpoint for ${endpoint}" \
            "HTTP ${status}, body size ${profile_size}"
    fi
}

test_no_compression_without_header() {
    local name="$1"
    local port="$2"
    local endpoint="$3"

    # Request WITHOUT Accept-Encoding: pidgin
    local tmp_headers
    tmp_headers=$(mktemp)

    curl -sf --max-time $TIMEOUT \
        -D "$tmp_headers" \
        -o /dev/null \
        "http://localhost:${port}${endpoint}" 2>/dev/null

    local has_pidgin_encoding
    has_pidgin_encoding=$(grep -ci "content-encoding.*pidgin" "$tmp_headers" 2>/dev/null || echo "0")

    rm -f "$tmp_headers"

    if [ "$has_pidgin_encoding" -eq 0 ]; then
        log_pass "${name} no compression without Accept-Encoding header"
    else
        log_fail "${name} no compression without Accept-Encoding header" \
            "Got Content-Encoding: pidgin even without Accept-Encoding"
    fi
}

# ------------------------------------------------------------------ #
# Server test runner                                                   #
# ------------------------------------------------------------------ #

test_server() {
    local name="$1"
    local port="$2"

    log_header "Testing ${name} (port ${port})"

    # Health check
    if ! test_health "$name" "$port"; then
        log_skip "Skipping ${name} — server not available"
        return
    fi

    # For each endpoint
    for endpoint in "${ENDPOINTS[@]}"; do
        log_section "${name} — ${endpoint}"

        # Phase 1: Learning
        test_learning_phase "$name" "$port" "$endpoint"

        # Phase 2: Test compression
        test_compression "$name" "$port" "$endpoint"

        # Phase 3: Test profile endpoint
        test_profile_endpoint "$name" "$port" "$endpoint"

        # Phase 4: No compression without header
        test_no_compression_without_header "$name" "$port" "$endpoint"
    done
}

# ------------------------------------------------------------------ #
# Main                                                                 #
# ------------------------------------------------------------------ #

log_header "Pidgin Compression Integration Tests"
echo ""
echo -e "${CYAN}Backend:${NC}  http://localhost:${BACKEND_PORT}"
echo -e "${CYAN}nginx:${NC}    http://localhost:${SERVER_PORTS[nginx]}"
echo -e "${CYAN}Apache:${NC}   http://localhost:${SERVER_PORTS[apache]}"
echo -e "${CYAN}Kong:${NC}     http://localhost:${SERVER_PORTS[kong]}"
echo -e "${CYAN}Caddy:${NC}    http://localhost:${SERVER_PORTS[caddy]}"
echo -e "${CYAN}Traefik:${NC}  http://localhost:${SERVER_PORTS[traefik]}"
echo -e "${CYAN}HAProxy:${NC}  http://localhost:${SERVER_PORTS[haproxy]}"

# Wait for backend
log_section "Waiting for backend..."
if ! wait_for_service "backend" $BACKEND_PORT; then
    echo -e "${RED}Backend not available. Start with: docker compose up -d${NC}"
    exit 1
fi
log_pass "Backend is ready"

# Determine which servers to test
SERVERS_TO_TEST=()
if [ $# -gt 0 ]; then
    SERVERS_TO_TEST=("$@")
else
    SERVERS_TO_TEST=("nginx" "apache" "kong" "caddy" "traefik" "haproxy")
fi

# Test each server
for server in "${SERVERS_TO_TEST[@]}"; do
    port="${SERVER_PORTS[$server]:-}"
    if [ -z "$port" ]; then
        echo -e "${RED}Unknown server: ${server}${NC}"
        continue
    fi

    # Wait for server
    log_section "Waiting for ${server}..."
    if wait_for_service "$server" "$port"; then
        test_server "$server" "$port"
    else
        log_skip "Skipping ${server} — not available"
    fi
done

# ------------------------------------------------------------------ #
# Summary                                                              #
# ------------------------------------------------------------------ #

log_header "Test Summary"
echo ""
echo -e "  Total:   ${BOLD}${TOTAL_TESTS}${NC}"
echo -e "  Passed:  ${GREEN}${PASSED_TESTS}${NC}"
echo -e "  Failed:  ${RED}${FAILED_TESTS}${NC}"
echo -e "  Skipped: ${YELLOW}${SKIPPED_TESTS}${NC}"
echo ""

if [ $FAILED_TESTS -gt 0 ]; then
    echo -e "${RED}${BOLD}Some tests failed.${NC}"
    exit 1
else
    echo -e "${GREEN}${BOLD}All tests passed!${NC}"
    exit 0
fi
