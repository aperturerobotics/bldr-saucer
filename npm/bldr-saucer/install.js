#!/usr/bin/env node

const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");
const os = require("os");

// Skip if binary exists and we're not forcing source build
if (process.env.BLDR_SAUCER_SKIP_BINARY === "true") {
  console.log("bldr-saucer: Skipping install (BLDR_SAUCER_SKIP_BINARY=true)");
  process.exit(0);
}

// Check if platform binary exists
const { getPlatformBinaryPath } = require("./index.js");
const platformBinary = getPlatformBinaryPath();
if (platformBinary && fs.existsSync(platformBinary)) {
  console.log("bldr-saucer: Using prebuilt binary");
  process.exit(0);
}

// Check for forced source build
if (process.env.BLDR_SAUCER_FROM_SOURCE !== "true") {
  console.log(
    "bldr-saucer: No prebuilt binary for this platform. " +
    "Set BLDR_SAUCER_FROM_SOURCE=true to build from source."
  );
  process.exit(0);
}

console.log("bldr-saucer: Building from source...");

// Check for required tools
function hasCommand(cmd) {
  try {
    execFileSync("which", [cmd], { stdio: "ignore" });
    return true;
  } catch {
    return false;
  }
}

if (!hasCommand("cmake")) {
  console.error("bldr-saucer: cmake is required but not found");
  process.exit(1);
}

if (!hasCommand("ninja")) {
  console.error("bldr-saucer: ninja is required but not found");
  process.exit(1);
}

const srcDir = __dirname;
const buildDir = path.join(srcDir, "build");

// Create build directory
if (!fs.existsSync(buildDir)) {
  fs.mkdirSync(buildDir, { recursive: true });
}

// Run CMake configure
console.log("bldr-saucer: Configuring...");
try {
  execFileSync("cmake", ["-G", "Ninja", "-B", "build"], {
    cwd: srcDir,
    stdio: "inherit",
  });
} catch (e) {
  console.error("bldr-saucer: CMake configure failed");
  process.exit(1);
}

// Run CMake build
console.log("bldr-saucer: Building...");
try {
  execFileSync("cmake", ["--build", "build"], {
    cwd: srcDir,
    stdio: "inherit",
  });
} catch (e) {
  console.error("bldr-saucer: CMake build failed");
  process.exit(1);
}

// Verify binary exists
const platform = os.platform();
const binaryName = platform === "win32" ? "bldr-saucer.exe" : "bldr-saucer";
const binaryPath = path.join(buildDir, binaryName);

if (!fs.existsSync(binaryPath)) {
  console.error(`bldr-saucer: Binary not found at ${binaryPath}`);
  process.exit(1);
}

console.log("bldr-saucer: Build successful!");
