#!/usr/bin/env bash
# =============================================================================
# tests/integration/run_all.sh
# =============================================================================
# Master runner for the ChimeraOS integration test suite.
#
# Usage
# -----
#   ./tests/integration/run_all.sh [--iso <path>] [--scenario <id>] [--help]
#
# Options
#   --iso <path>       Path to the integration test ISO.
#                      Default: <repo>/chimaera_integration_test.iso
#   --scenario <id>    Run only the named scenario (fresh_install, etc.)
#   --build            Rebuild the ISO before running tests
#   --verbose          Print full serial log for each scenario
#   --help             Show this help text
#
# Exit codes
#   0  All scenarios passed
#   1  One or more scenarios failed
#   2  Setup error (ISO not found, QEMU not installed, etc.)
#
# CI usage
# --------
#   # In .github/workflows/integration.yml:
#   - name: Run integration tests
#     run: ./tests/integration/run_all.sh --build
#
# =============================================================================

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISO_PATH="${REPO_ROOT}/chimaera_integration_test.iso"
PYTHON="${PYTHON:-python3}"
VERBOSE=0
BUILD=0
ONLY_SCENARIO=""

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
RESET='\033[0m'

pass()  { echo -e "${GREEN}  ✓ PASS${RESET}  $*"; }
fail()  { echo -e "${RED}  ✗ FAIL${RESET}  $*"; }
info()  { echo -e "${YELLOW}  ·${RESET}      $*"; }
title() { echo -e "\n${BOLD}$*${RESET}"; }

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iso)       ISO_PATH="$2"; shift 2 ;;
        --scenario)  ONLY_SCENARIO="$2"; shift 2 ;;
        --build)     BUILD=1; shift ;;
        --verbose)   VERBOSE=1; shift ;;
        --help|-h)
            sed -n '/^# Usage/,/^# ====/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

# ── Pre-flight checks ─────────────────────────────────────────────────────────
title "ChimeraOS Integration Test Suite"
echo "  Repo:    ${REPO_ROOT}"
echo "  ISO:     ${ISO_PATH}"
echo "  Python:  $(${PYTHON} --version 2>&1)"

if ! command -v qemu-system-i386 &>/dev/null; then
    echo -e "${RED}ERROR: qemu-system-i386 not found.${RESET}" >&2
    echo "  Install with: sudo apt-get install qemu-system-x86" >&2
    exit 2
fi

if ! command -v mkfs.fat &>/dev/null; then
    echo -e "${RED}ERROR: mkfs.fat not found.${RESET}" >&2
    echo "  Install with: sudo apt-get install dosfstools" >&2
    exit 2
fi

if ! command -v mcopy &>/dev/null; then
    echo -e "${RED}ERROR: mcopy not found.${RESET}" >&2
    echo "  Install with: sudo apt-get install mtools" >&2
    exit 2
fi

# ── Build ISO if requested ────────────────────────────────────────────────────
if [[ "${BUILD}" -eq 1 ]]; then
    title "Building integration test ISO..."
    (cd "${REPO_ROOT}" && make integration-test-iso)
fi

if [[ ! -f "${ISO_PATH}" ]]; then
    echo -e "${RED}ERROR: ISO not found: ${ISO_PATH}${RESET}" >&2
    echo "  Run: make integration-test-iso" >&2
    exit 2
fi

# ── Scenario list ─────────────────────────────────────────────────────────────
declare -a SCENARIOS=(
    "fresh_install"
    "existing_user"
    "full_disk"
    "no_network"
    "corrupted_fs"
    "panic_test"
    "usb_test"
    "gui_test"
    "acpi_test"
)

if [[ -n "${ONLY_SCENARIO}" ]]; then
    SCENARIOS=("${ONLY_SCENARIO}")
fi

# ── Run scenarios ─────────────────────────────────────────────────────────────
declare -A RESULTS
PASS_COUNT=0
FAIL_COUNT=0
START_TIME=$(date +%s)

for scenario in "${SCENARIOS[@]}"; do
    script="${SCRIPT_DIR}/${scenario}.py"
    if [[ ! -f "${script}" ]]; then
        echo -e "${RED}ERROR: Scenario script not found: ${script}${RESET}" >&2
        RESULTS["${scenario}"]="MISSING"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        continue
    fi

    title "Running: ${scenario}"
    scenario_start=$(date +%s)

    # Capture output
    tmp_out=$(mktemp)
    set +e
    ${PYTHON} "${script}" "${ISO_PATH}" 2>&1 | tee "${tmp_out}"
    rc=${PIPESTATUS[0]}
    set -e

    scenario_end=$(date +%s)
    elapsed=$((scenario_end - scenario_start))

    if [[ "${rc}" -eq 0 ]]; then
        pass "${scenario} (${elapsed}s)"
        RESULTS["${scenario}"]="PASS"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        fail "${scenario} (${elapsed}s)"
        RESULTS["${scenario}"]="FAIL"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        if [[ "${VERBOSE}" -eq 1 ]]; then
            echo "  --- Full output ---"
            cat "${tmp_out}"
        fi
    fi
    rm -f "${tmp_out}"
done

# ── Summary ───────────────────────────────────────────────────────────────────
END_TIME=$(date +%s)
TOTAL_ELAPSED=$((END_TIME - START_TIME))
TOTAL=$((PASS_COUNT + FAIL_COUNT))

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo -e "${BOLD}Results: ${PASS_COUNT}/${TOTAL} passed  (${TOTAL_ELAPSED}s)${RESET}"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

for scenario in "${SCENARIOS[@]}"; do
    result="${RESULTS[${scenario}]:-UNKNOWN}"
    if [[ "${result}" == "PASS" ]]; then
        echo -e "  ${GREEN}✓${RESET}  ${scenario}"
    else
        echo -e "  ${RED}✗${RESET}  ${scenario}  [${result}]"
    fi
done
echo ""

# ── Machine-readable output for CI ───────────────────────────────────────────
RESULTS_JSON="${SCRIPT_DIR}/integration_results.json"
{
    echo "{"
    echo "  \"timestamp\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
    echo "  \"iso\": \"${ISO_PATH}\","
    echo "  \"total\": ${TOTAL},"
    echo "  \"passed\": ${PASS_COUNT},"
    echo "  \"failed\": ${FAIL_COUNT},"
    echo "  \"elapsed_sec\": ${TOTAL_ELAPSED},"
    echo "  \"scenarios\": {"
    first=1
    for scenario in "${SCENARIOS[@]}"; do
        result="${RESULTS[${scenario}]:-UNKNOWN}"
        if [[ "${first}" -eq 0 ]]; then echo ","; fi
        printf "    \"%s\": \"%s\"" "${scenario}" "${result}"
        first=0
    done
    echo ""
    echo "  }"
    echo "}"
} > "${RESULTS_JSON}"

info "Results written to: ${RESULTS_JSON}"

if [[ "${FAIL_COUNT}" -gt 0 ]]; then
    exit 1
fi
exit 0
