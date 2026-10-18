#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
git config --local core.autocrlf false
git config --local core.eol lf
git config --local core.hooksPath .githooks
echo 'Enabled Linux verification before each git push in this clone.'
