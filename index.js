const os = require("os");
const path = require("path");
const fs = require("fs");

/**
 * Get the path to the bldr-saucer binary.
 *
 * Tries platform-specific npm package first, then falls back to
 * source-built binary if BLDR_SAUCER_FROM_SOURCE is set or binary
 * doesn't exist.
 *
 * @returns {string} Path to the bldr-saucer binary
 * @throws {Error} If no binary is found
 */
function getBinaryPath() {
  // Check for forced source build
  if (process.env.BLDR_SAUCER_FROM_SOURCE === "true") {
    return getSourceBinaryPath();
  }

  // Try platform-specific package
  const platformBinary = getPlatformBinaryPath();
  if (platformBinary && fs.existsSync(platformBinary)) {
    return platformBinary;
  }

  // Fallback to source-built binary
  const sourceBinary = getSourceBinaryPath();
  if (sourceBinary && fs.existsSync(sourceBinary)) {
    return sourceBinary;
  }

  throw new Error(
    `bldr-saucer binary not found for platform ${os.platform()}-${os.arch()}. ` +
    `Try running with BLDR_SAUCER_FROM_SOURCE=true to build from source.`
  );
}

/**
 * Get the platform-specific package binary path.
 * @returns {string|null}
 */
function getPlatformBinaryPath() {
  const platform = os.platform();
  const arch = os.arch();

  let packageName;
  switch (`${platform}-${arch}`) {
    case "darwin-arm64":
      packageName = "@aptre/bldr-saucer-darwin-arm64";
      break;
    case "darwin-x64":
      packageName = "@aptre/bldr-saucer-darwin-x64";
      break;
    case "linux-x64":
      packageName = "@aptre/bldr-saucer-linux-x64";
      break;
    case "linux-arm64":
      packageName = "@aptre/bldr-saucer-linux-arm64";
      break;
    case "win32-x64":
      packageName = "@aptre/bldr-saucer-win32-x64";
      break;
    default:
      return null;
  }

  try {
    const packagePath = require.resolve(`${packageName}/package.json`);
    const binDir = path.join(path.dirname(packagePath), "bin");
    const binaryName = platform === "win32" ? "bldr-saucer.exe" : "bldr-saucer";
    return path.join(binDir, binaryName);
  } catch {
    return null;
  }
}

/**
 * Get the source-built binary path.
 * @returns {string}
 */
function getSourceBinaryPath() {
  const platform = os.platform();
  const binaryName = platform === "win32" ? "bldr-saucer.exe" : "bldr-saucer";
  return path.join(__dirname, "build", binaryName);
}

/**
 * Check if the binary exists.
 * @returns {boolean}
 */
function hasBinary() {
  try {
    getBinaryPath();
    return true;
  } catch {
    return false;
  }
}

module.exports = {
  getBinaryPath,
  getPlatformBinaryPath,
  getSourceBinaryPath,
  hasBinary,
};
