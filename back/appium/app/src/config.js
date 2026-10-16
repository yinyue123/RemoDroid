const fs = require('fs');
const path = require('path');

let config = null;

/**
 * Load configuration from config.json
 * Loads once and caches the result
 */
function loadConfig() {
  if (config) return config;

  const configPath = path.join(process.cwd(), 'config.json');
  const configFile = fs.readFileSync(configPath, 'utf8');
  config = JSON.parse(configFile);

  console.log('Configuration loaded successfully');
  return config;
}

/**
 * Get the cached configuration
 */
function getConfig() {
  if (!config) {
    return loadConfig();
  }
  return config;
}

module.exports = {
  loadConfig,
  getConfig
};
