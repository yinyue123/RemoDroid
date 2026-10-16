const { remote } = require('webdriverio');
const { getConfig } = require('./config');

let driver = null;
let wdOpts = null;
let logs = []; // Global logs array

/**
 * Initialize WebDriver options from config
 */
function initializeOptions() {
  if (wdOpts) return wdOpts;

  const config = getConfig();
  const capabilities = {};

  // Prepare capabilities with appium: prefix
  for (const [key, value] of Object.entries(config.capabilities || {})) {
    capabilities[key === 'platformName' ? key : `appium:${key}`] = value;
  }

  wdOpts = {
    ...config.webdriverio,
    path: config.webdriverio?.path || '/',
    connectionRetryTimeout: config.webdriverio?.connectionRetryTimeout || 120000,
    connectionRetryCount: config.webdriverio?.connectionRetryCount || 3,
    capabilities,
  };

  return wdOpts;
}

/**
 * Start a new Appium session
 */
async function startAppiumSession() {
  if (driver) {
    console.log('Session already active, checking health...');

    // Check if existing session is healthy
    try {
      await driver.getWindowSize(); // Quick health check
      console.log('Existing session is healthy');
      return driver;
    } catch (error) {
      console.log('Existing session is unhealthy, recreating...');
      driver = null;
    }
  }

  const opts = initializeOptions();
  const config = getConfig();
  const maxRetries = 3;
  let lastError = null;

  console.log('Connecting to Appium server...');
  console.log(`Target: ${opts.hostname}:${opts.port}`);

  // Retry connection up to 3 times
  for (let attempt = 1; attempt <= maxRetries; attempt++) {
    try {
      console.log(`Connection attempt ${attempt}/${maxRetries}...`);

      driver = await remote(opts);

      if (config.timeouts?.implicit) {
        await driver.setTimeout({ implicit: config.timeouts.implicit });
      }

      console.log('Appium session started successfully');
      return driver;
    } catch (error) {
      lastError = error;
      driver = null;
      console.error(`Attempt ${attempt}/${maxRetries} failed:`, error.message);

      // If not the last attempt, wait before retrying
      if (attempt < maxRetries) {
        const waitTime = 2000; // 2 seconds between retries
        console.log(`Waiting ${waitTime/1000} seconds before retry...`);
        await new Promise(resolve => setTimeout(resolve, waitTime));
      }
    }
  }

  // All retries failed
  console.error(`Failed to start Appium session after ${maxRetries} attempts`);
  throw lastError;
}

/**
 * Stop the current Appium session
 */
async function stopAppiumSession() {
  if (!driver) {
    console.log('No active session to stop');
    return;
  }

  console.log('Stopping Appium session...');

  try {
    // Try to delete the session gracefully
    await driver.deleteSession();
    console.log('Appium session stopped successfully');
  } catch (error) {
    // Session might already be closed or invalid
    console.log('Session already closed or invalid:', error.message);
  } finally {
    // Always set driver to null to clean up local state
    driver = null;
    console.log('Driver reference cleared');
  }
}

/**
 * Get the current driver instance
 * @returns {object|null}
 */
function getDriver() {
  return driver;
}

/**
 * Check if session is healthy
 * @returns {Promise<boolean>}
 */
async function isSessionHealthy() {
  if (!driver) return false;

  try {
    await driver.getWindowSize();
    return true;
  } catch (error) {
    console.error('Session health check failed:', error.message);
    driver = null;
    return false;
  }
}

/**
 * Create Appium wrapper object for scripts
 * This wrapper provides ALL basic Appium methods
 * Scripts should NOT access driver directly
 * @returns {Object} Wrapper object with all available methods
 */
function createAppiumWrapper() {
  const checkDriver = () => {
    if (!driver) throw new Error('No active Appium session. Please start a session first.');
  };

  return {
    // ============================================================================
    // Touch Actions (W3C Actions API)
    // ============================================================================

    /**
     * Perform W3C Actions - Execute touch/pointer actions on the device
     * @param {Array<Object>} actions - Array of action sequences
     * @param {string} actions[].type - Action type: 'pointer', 'key', or 'none'
     * @param {string} actions[].id - Unique identifier for this action sequence
     * @param {Object} [actions[].parameters] - Optional parameters
     * @param {string} [actions[].parameters.pointerType] - For pointer: 'touch', 'mouse', or 'pen'
     * @param {Array<Object>} actions[].actions - Array of action steps
     * @param {string} actions[].actions[].type - Step type: 'pointerMove', 'pointerDown', 'pointerUp', 'pause'
     * @param {number} [actions[].actions[].duration] - Duration in milliseconds
     * @param {number} [actions[].actions[].x] - X coordinate (for pointerMove)
     * @param {number} [actions[].actions[].y] - Y coordinate (for pointerMove)
     * @param {number} [actions[].actions[].button] - Button number (0=left, 1=middle, 2=right)
     * @returns {Promise<void>}
     * @example
     * // Tap at coordinates (100, 200)
     * await appium.performActions([{
     *   type: 'pointer',
     *   id: 'finger1',
     *   parameters: { pointerType: 'touch' },
     *   actions: [
     *     { type: 'pointerMove', duration: 0, x: 100, y: 200 },
     *     { type: 'pointerDown', button: 0 },
     *     { type: 'pause', duration: 100 },
     *     { type: 'pointerUp', button: 0 }
     *   ]
     * }])
     */
    performActions: async (actions) => {
      checkDriver();
      return await driver.performActions(actions);
    },

    /**
     * Release all active actions - Clears all active pointer/key states
     * @returns {Promise<void>}
     * @example
     * await appium.releaseActions()
     */
    releaseActions: async () => {
      checkDriver();
      return await driver.releaseActions();
    },

    // ============================================================================
    // Element Finding
    // ============================================================================

    /**
     * Find single element by selector
     * @param {string} selector - Element selector (supports multiple strategies)
     * @returns {Promise<Element>} Element object with methods like click(), setText(), etc.
     * @example
     * // Android UiSelector
     * const btn = await appium.$('android=new UiSelector().text("Login")')
     *
     * // XPath
     * const input = await appium.$('//android.widget.EditText[@resource-id="username"]')
     *
     * // Accessibility ID
     * const element = await appium.$('~loginButton')
     *
     * // Class name
     * const button = await appium.$('android.widget.Button')
     */
    $: async (selector) => {
      checkDriver();
      return await driver.$(selector);
    },

    /**
     * Find multiple elements by selector
     * @param {string} selector - Element selector
     * @returns {Promise<Array<Element>>} Array of element objects
     * @example
     * // Find all buttons
     * const buttons = await appium.$$('//android.widget.Button')
     * for (const btn of buttons) {
     *   const text = await btn.getText()
     *   console.log(text)
     * }
     */
    $$: async (selector) => {
      checkDriver();
      return await driver.$$(selector);
    },

    // ============================================================================
    // Screen Information
    // ============================================================================

    /**
     * Get device screen dimensions
     * @returns {Promise<Object>} Object with width and height properties
     * @returns {number} return.width - Screen width in pixels
     * @returns {number} return.height - Screen height in pixels
     * @example
     * const { width, height } = await appium.getWindowSize()
     * console.log(`Screen: ${width}x${height}`)
     */
    getWindowSize: async () => {
      checkDriver();
      return await driver.getWindowSize();
    },

    /**
     * Take screenshot of current screen
     * @returns {Promise<string>} Base64 encoded PNG image
     * @example
     * const screenshot = await appium.takeScreenshot()
     * // Returns: 'iVBORw0KGgoAAAANSUhEUgA...'
     */
    takeScreenshot: async () => {
      checkDriver();
      return await driver.takeScreenshot();
    },

    /**
     * Get XML representation of current screen hierarchy
     * @returns {Promise<string>} XML string of the UI hierarchy
     * @example
     * const xml = await appium.getPageSource()
     * // Parse or search the XML for elements
     */
    getPageSource: async () => {
      checkDriver();
      return await driver.getPageSource();
    },

    // ============================================================================
    // Key Events
    // ============================================================================

    /**
     * Press a key by Android keycode
     * @param {number} keycode - Android keycode (e.g., 66 for ENTER)
     * @returns {Promise<void>}
     * @example
     * await appium.pressKeyCode(66) // ENTER
     * await appium.pressKeyCode(4)  // BACK
     * await appium.pressKeyCode(3)  // HOME
     * await appium.pressKeyCode(26) // POWER
     *
     * // Common keycodes:
     * // 3 = HOME, 4 = BACK, 26 = POWER, 66 = ENTER, 82 = MENU
     * // 24 = VOLUME_UP, 25 = VOLUME_DOWN, 27 = CAMERA
     */
    pressKeyCode: async (keycode) => {
      checkDriver();
      return await driver.pressKeyCode(keycode);
    },

    /**
     * Long press a key by Android keycode
     * @param {number} keycode - Android keycode
     * @returns {Promise<void>}
     * @example
     * await appium.longPressKeyCode(26) // Long press POWER to show power menu
     */
    longPressKeyCode: async (keycode) => {
      checkDriver();
      return await driver.longPressKeyCode(keycode);
    },

    /**
     * Press the back button
     * @returns {Promise<void>}
     * @example
     * await appium.back()
     */
    back: async () => {
      checkDriver();
      return await driver.back();
    },

    // ============================================================================
    // Device Operations
    // ============================================================================

    /**
     * Check if device screen is locked
     * @returns {Promise<boolean>} true if locked, false if unlocked
     * @example
     * const locked = await appium.isLocked()
     * if (locked) {
     *   console.log('Device is locked')
     * }
     */
    isLocked: async () => {
      checkDriver();
      return await driver.isLocked();
    },

    /**
     * Lock the device screen
     * @returns {Promise<void>}
     * @example
     * await appium.lock()
     */
    lock: async () => {
      checkDriver();
      return await driver.lock();
    },

    /**
     * Unlock device (basic unlock without PIN/password)
     * Note: For PIN/password unlock, use execute() with custom logic
     * @returns {Promise<void>}
     * @example
     * await appium.unlock() // Only works if no PIN/password is set
     */
    unlock: async () => {
      checkDriver();
      return await driver.unlock();
    },

    /**
     * Hide the on-screen keyboard
     * @returns {Promise<void|null>} null if keyboard not shown
     * @example
     * await appium.hideKeyboard()
     */
    hideKeyboard: async () => {
      checkDriver();
      try {
        return await driver.hideKeyboard();
      } catch (e) {
        return null;
      }
    },

    /**
     * Check if keyboard is currently shown
     * @returns {Promise<boolean>} true if keyboard is visible
     * @example
     * const shown = await appium.isKeyboardShown()
     * if (shown) {
     *   await appium.hideKeyboard()
     * }
     */
    isKeyboardShown: async () => {
      checkDriver();
      return await driver.isKeyboardShown();
    },

    // ============================================================================
    // App Operations
    // ============================================================================

    /**
     * Get current foreground activity name
     * @returns {Promise<string>} Activity name (e.g., '.MainActivity')
     * @example
     * const activity = await appium.getCurrentActivity()
     * console.log(activity) // '.MainActivity'
     */
    getCurrentActivity: async () => {
      checkDriver();
      return await driver.getCurrentActivity();
    },

    /**
     * Get current foreground app package name
     * @returns {Promise<string>} Package name (e.g., 'com.example.app')
     * @example
     * const pkg = await appium.getCurrentPackage()
     * console.log(pkg) // 'com.android.settings'
     */
    getCurrentPackage: async () => {
      checkDriver();
      return await driver.getCurrentPackage();
    },

    /**
     * Start an activity by package and activity name
     * @param {string} appPackage - App package name (e.g., 'com.android.settings')
     * @param {string} appActivity - Activity name (e.g., '.Settings')
     * @returns {Promise<void>}
     * @example
     * await appium.startActivity('com.android.settings', '.Settings')
     * await appium.startActivity('com.example.app', '.MainActivity')
     */
    startActivity: async (appPackage, appActivity) => {
      checkDriver();
      return await driver.startActivity(appPackage, appActivity);
    },

    /**
     * Terminate (force stop) an app
     * @param {string} appId - App package name
     * @returns {Promise<void>}
     * @example
     * await appium.terminateApp('com.example.app')
     */
    terminateApp: async (appId) => {
      checkDriver();
      return await driver.terminateApp(appId);
    },

    /**
     * Activate (bring to foreground) an app
     * @param {string} appId - App package name
     * @returns {Promise<void>}
     * @example
     * await appium.activateApp('com.android.chrome')
     */
    activateApp: async (appId) => {
      checkDriver();
      return await driver.activateApp(appId);
    },

    /**
     * Query app installation/running state
     * @param {string} appId - App package name
     * @returns {Promise<number>} State code: 0=not installed, 1=not running, 3=running in background, 4=running in foreground
     * @example
     * const state = await appium.queryAppState('com.example.app')
     * // 0 = not installed
     * // 1 = not running
     * // 3 = running in background
     * // 4 = running in foreground
     */
    queryAppState: async (appId) => {
      checkDriver();
      return await driver.queryAppState(appId);
    },

    // ============================================================================
    // Context & Orientation
    // ============================================================================

    /**
     * Get current device orientation
     * @returns {Promise<string>} 'PORTRAIT' or 'LANDSCAPE'
     * @example
     * const orientation = await appium.getOrientation()
     * console.log(orientation) // 'PORTRAIT'
     */
    getOrientation: async () => {
      checkDriver();
      return await driver.getOrientation();
    },

    /**
     * Set device orientation
     * @param {string} orientation - 'PORTRAIT' or 'LANDSCAPE'
     * @returns {Promise<void>}
     * @example
     * await appium.setOrientation('LANDSCAPE')
     * await appium.pause(1000)
     * await appium.setOrientation('PORTRAIT')
     */
    setOrientation: async (orientation) => {
      checkDriver();
      return await driver.setOrientation(orientation);
    },

    // ============================================================================
    // Mobile Commands (execute)
    // ============================================================================

    /**
     * Execute mobile-specific commands
     * @param {string} command - Command name (e.g., 'mobile: shell', 'mobile: pressKey')
     * @param {Object} params - Command parameters
     * @returns {Promise<any>} Command result (varies by command)
     * @example
     * // Execute shell command
     * const output = await appium.execute('mobile: shell', {
     *   command: 'pm',
     *   args: ['list', 'packages']
     * })
     *
     * // Press power key
     * await appium.execute('mobile: pressKey', { keycode: 26 })
     *
     * // Simulate fingerprint (emulator only)
     * await appium.execute('mobile: fingerprint', { fingerprintId: 1 })
     *
     * // Swipe gesture
     * await appium.execute('mobile: swipeGesture', {
     *   left: 100, top: 500, width: 200, height: 200,
     *   direction: 'up', percent: 0.75
     * })
     */
    execute: async (command, params) => {
      checkDriver();
      return await driver.execute(command, params);
    },

    /**
     * Execute async mobile command
     * @param {string} command - Command name
     * @param {Object} params - Command parameters
     * @returns {Promise<any>} Command result
     * @example
     * const result = await appium.executeAsync('mobile: longRunningCommand', { params })
     */
    executeAsync: async (command, params) => {
      checkDriver();
      return await driver.executeAsync(command, params);
    },

    // ============================================================================
    // Timeouts & Waits
    // ============================================================================

    /**
     * Pause script execution
     * @param {number} ms - Milliseconds to pause
     * @returns {Promise<void>}
     * @example
     * await appium.pause(1000) // Wait 1 second
     * await appium.pause(500)  // Wait 0.5 seconds
     */
    pause: async (ms) => {
      checkDriver();
      return await driver.pause(ms);
    },

    /**
     * Configure Appium timeouts
     * @param {Object} timeouts - Timeout configuration object
     * @param {number} [timeouts.implicit] - Implicit wait timeout for element finding (ms)
     * @param {number} [timeouts.pageLoad] - Page load timeout (ms)
     * @param {number} [timeouts.script] - Script execution timeout (ms)
     * @returns {Promise<void>}
     * @example
     * await appium.setTimeout({ implicit: 5000 }) // Wait up to 5s for elements
     * await appium.setTimeout({ script: 30000 })  // 30s script timeout
     */
    setTimeout: async (timeouts) => {
      checkDriver();
      return await driver.setTimeout(timeouts);
    },

    // ============================================================================
    // Settings
    // ============================================================================

    /**
     * Get all Appium settings
     * @returns {Promise<Object>} Settings object
     * @example
     * const settings = await appium.getSettings()
     * console.log(settings.ignoreUnimportantViews)
     */
    getSettings: async () => {
      checkDriver();
      return await driver.getSettings();
    },

    /**
     * Update Appium settings
     * @param {Object} settings - Settings to update
     * @param {boolean} [settings.ignoreUnimportantViews] - Ignore accessibility elements
     * @param {boolean} [settings.allowInvisibleElements] - Allow finding invisible elements
     * @param {boolean} [settings.enableNotificationListener] - Enable notification listener
     * @param {number} [settings.actionAcknowledgmentTimeout] - Action timeout (ms)
     * @returns {Promise<void>}
     * @example
     * await appium.updateSettings({
     *   ignoreUnimportantViews: true,
     *   allowInvisibleElements: false
     * })
     */
    updateSettings: async (settings) => {
      checkDriver();
      return await driver.updateSettings(settings);
    },

    // ============================================================================
    // System Bars
    // ============================================================================

    /**
     * Get system bars information (status bar, navigation bar)
     * @returns {Promise<Object>} System bars info with statusBar and navigationBar properties
     * @example
     * const bars = await appium.getSystemBars()
     * console.log(bars.statusBar.height)
     * console.log(bars.navigationBar.height)
     */
    getSystemBars: async () => {
      checkDriver();
      return await driver.getSystemBars();
    },

    // ============================================================================
    // Clipboard
    // ============================================================================

    /**
     * Set device clipboard content
     * @param {string} content - Content to set
     * @param {string} [contentType='plaintext'] - Content type: 'plaintext', 'image', or 'url'
     * @returns {Promise<void>}
     * @example
     * await appium.setClipboard('Hello World', 'plaintext')
     * await appium.setClipboard('https://example.com', 'url')
     */
    setClipboard: async (content, contentType = 'plaintext') => {
      checkDriver();
      return await driver.setClipboard(content, contentType);
    },

    /**
     * Get device clipboard content
     * @param {string} [contentType='plaintext'] - Content type to retrieve
     * @returns {Promise<string>} Clipboard content
     * @example
     * const text = await appium.getClipboard('plaintext')
     * console.log(text)
     */
    getClipboard: async (contentType = 'plaintext') => {
      checkDriver();
      return await driver.getClipboard(contentType);
    },

    // ============================================================================
    // Network
    // ============================================================================

    /**
     * Toggle WiFi on/off
     * @returns {Promise<void>}
     * @example
     * await appium.toggleWiFi() // Turn off if on, turn on if off
     */
    toggleWiFi: async () => {
      checkDriver();
      return await driver.toggleWiFi();
    },

    /**
     * Toggle mobile data on/off
     * @returns {Promise<void>}
     * @example
     * await appium.toggleData()
     */
    toggleData: async () => {
      checkDriver();
      return await driver.toggleData();
    },

    /**
     * Toggle airplane mode on/off
     * @returns {Promise<void>}
     * @example
     * await appium.toggleAirplaneMode()
     */
    toggleAirplaneMode: async () => {
      checkDriver();
      return await driver.toggleAirplaneMode();
    },

    // ============================================================================
    // Geolocation
    // ============================================================================

    /**
     * Set device geolocation
     * @param {Object} location - Location object
     * @param {number} location.latitude - Latitude (-90 to 90)
     * @param {number} location.longitude - Longitude (-180 to 180)
     * @param {number} [location.altitude=0] - Altitude in meters
     * @returns {Promise<void>}
     * @example
     * await appium.setGeoLocation({
     *   latitude: 37.7749,   // San Francisco
     *   longitude: -122.4194,
     *   altitude: 10
     * })
     */
    setGeoLocation: async (location) => {
      checkDriver();
      return await driver.setGeoLocation(location);
    },

    /**
     * Get current device geolocation
     * @returns {Promise<Object>} Location object with latitude, longitude, altitude
     * @example
     * const location = await appium.getGeoLocation()
     * console.log(`${location.latitude}, ${location.longitude}`)
     */
    getGeoLocation: async () => {
      checkDriver();
      return await driver.getGeoLocation();
    },

    // ============================================================================
    // Logging
    // ============================================================================

    /**
     * Log a message and store it in the global logs array
     * @param {string} message - The log message
     * @returns {void}
     * @example
     * appium.log('Starting script execution')
     * appium.log('Button clicked successfully')
     */
    log: (message) => {
      const logMessage = String(message);
      let processedMessage = logMessage;

      // If message length exceeds 500, truncate and add ellipsis with original length
      if (logMessage.length > 500) {
        processedMessage = logMessage.substring(0, 500) + `... (original length: ${logMessage.length})`;
      }

      // Store in logs array
      logs.push(processedMessage);

      // If array exceeds 1000 entries, clear it
      if (logs.length > 1000) {
        logs = [];
      }

      // Also log to console
      console.log(processedMessage);
    },

    /**
     * Log an error message and store it in the global logs array
     * @param {string} message - The error message
     * @returns {void}
     * @example
     * appium.err('Failed to find element')
     * appium.err('Script execution failed: ' + error.message)
     */
    err: (message) => {
      const errorMessage = String(message);
      let processedMessage = `ERROR: ${errorMessage}`;

      // If message length exceeds 500, truncate and add ellipsis with original length
      if (processedMessage.length > 500) {
        processedMessage = processedMessage.substring(0, 500) + `... (original length: ${processedMessage.length})`;
      }

      // Store in logs array
      logs.push(processedMessage);

      // If array exceeds 1000 entries, clear it
      if (logs.length > 1000) {
        logs = [];
      }

      // Also log to console error
      console.error(processedMessage);
    },
  };
}

/**
 * Get and clear logs from the global logs array
 * @returns {Array<string>} Array of log messages
 */
function getLogs() {
  const currentLogs = [...logs]; // Copy the logs
  logs = []; // Clear the logs array
  return currentLogs;
}

module.exports = {
  startAppiumSession,
  stopAppiumSession,
  getDriver,
  isSessionHealthy,
  createAppiumWrapper,
  getLogs,
};
