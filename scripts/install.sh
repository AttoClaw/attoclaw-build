#!/usr/bin/env sh
set -eu

echo "AttoClaw install helper"
echo "======================"

need() { command -v "$1" >/dev/null 2>&1; }

install_termux() {
  echo "Detected Termux (pkg). Installing deps..."
  pkg update -y
  pkg install -y cmake clang make libcurl openssl pkg-config git python || true
}

install_apt() {
  echo "Detected apt. Installing deps..."
  sudo apt-get update
  sudo apt-get install -y --no-install-recommends cmake g++ libcurl4-openssl-dev pkg-config git python3
}

install_brew() {
  echo "Detected Homebrew. Installing deps..."
  brew install cmake curl pkg-config || true
}

if need pkg; then
  install_termux
elif need apt-get; then
  install_apt
elif need brew; then
  install_brew
else
  echo "No supported package manager found (pkg/apt-get/brew). Install cmake + c++ compiler + libcurl dev."
fi

echo "Building..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

echo "Running tests..."
ctest --test-dir build --output-on-failure || true

echo "Binary: ./build/attoclaw"

