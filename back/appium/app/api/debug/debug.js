const { remote } = require('webdriverio');
const { getConfig } = require('../../src/config');

// ============================================================================
// Server-side API Functions (Node.js)
// ============================================================================

// Debug module maintains its own driver instance
let debugDriver = null;

/**
 * Initialize and get debug driver
 * Creates a new session if one doesn't exist
 */
async function getDebugDriver() {
  if (debugDriver) {
    // Verify the session is still active
    try {
      await debugDriver.status();
      return debugDriver;
    } catch (error) {
      console.log('Debug session is no longer active, creating a new one...');
      // Try to close the old session gracefully
      try {
        await debugDriver.deleteSession();
      } catch (e) {
        // Ignore cleanup errors
      }
      debugDriver = null;
    }
  }

  // Create new session
  const config = getConfig();
  const capabilities = {};

  // Prepare capabilities with appium: prefix
  for (const [key, value] of Object.entries(config.capabilities || {})) {
    capabilities[key === 'platformName' ? key : `appium:${key}`] = value;
  }

  const wdOpts = {
    ...config.webdriverio,
    path: config.webdriverio?.path || '/',
    connectionRetryTimeout: config.webdriverio?.connectionRetryTimeout || 120000,
    connectionRetryCount: config.webdriverio?.connectionRetryCount || 3,
    capabilities,
  };

  console.log('Creating new debug session...');
  debugDriver = await remote(wdOpts);

  if (config.timeouts?.implicit) {
    await debugDriver.setTimeout({ implicit: config.timeouts.implicit });
  }

  console.log('Debug session created successfully');
  return debugDriver;
}

/**
 * Check if session is connected and active
 */
async function checkConnection() {
  if (!debugDriver) {
    return { connected: false, message: 'No session exists' };
  }

  try {
    await debugDriver.status();
    console.log('Debug session is active');
    return { connected: true, message: 'Session is active' };
  } catch (error) {
    console.log('Debug session is no longer active');
    debugDriver = null;
    return { connected: false, message: 'Session is not active' };
  }
}

/**
 * Explicitly connect to debug session
 */
async function connectSession() {
  // First check if already connected
  const status = await checkConnection();
  if (status.connected) {
    console.log('Already connected to debug session, reusing existing connection');
    return { success: true, message: 'Already connected', reused: true };
  }

  // Create new connection
  try {
    await getDebugDriver();
    return { success: true, message: 'Connected successfully', reused: false };
  } catch (error) {
    console.error('Failed to connect:', error);
    return { success: false, error: error.message };
  }
}

/**
 * Close debug session
 */
async function closeDebugSession() {
  if (debugDriver) {
    try {
      await debugDriver.deleteSession();
      console.log('Debug session closed successfully');
    } catch (error) {
      console.error('Error closing debug session:', error);
    } finally {
      debugDriver = null;
    }
  }
}

/**
 * Unified error handler wrapper with retry logic
 */
async function handleRequest(operation, handler, retries = 1) {
  let lastError;

  for (let attempt = 0; attempt <= retries; attempt++) {
    try {
      const driver = await getDebugDriver();
      return await handler(driver);
    } catch (error) {
      lastError = error;
      const errorMessage = error.message || String(error);

      // Check if it's a session crash error
      const isSessionCrash = errorMessage.includes('instrumentation process is not running') ||
                            errorMessage.includes('session') ||
                            errorMessage.includes('proxy');

      if (isSessionCrash && attempt < retries) {
        console.log(`Session crashed during ${operation}, forcing session recreation (attempt ${attempt + 1}/${retries + 1})...`);
        // Force close the current session
        debugDriver = null;
        // Wait a bit before retrying
        await new Promise(resolve => setTimeout(resolve, 1000));
        continue;
      }

      console.error(`Failed to ${operation} (attempt ${attempt + 1}/${retries + 1}):`, error);

      if (attempt === retries) {
        throw error;
      }
    }
  }

  throw lastError;
}

/**
 * Get UI hierarchy dump from device
 */
async function getUIDump() {
  return handleRequest('get UI dump', async (driver) => {
    console.log('Getting UI dump...');
    const xmlContent = await driver.getPageSource();
    const parsed = parseXmlToJson(xmlContent);
    console.log(`UI dump retrieved successfully: ${parsed.length} nodes, XML length: ${xmlContent.length} chars`);
    return { success: true, data: parsed, xml: xmlContent };
  });
}

/**
 * Take screenshot from device
 */
async function takeScreenshot() {
  return handleRequest('take screenshot', async (driver) => {
    console.log('Taking screenshot...');
    const screenshot = await driver.takeScreenshot();
    console.log('Screenshot captured successfully');
    return { success: true, image: screenshot };
  });
}

/**
 * Click at specific coordinates
 */
async function clickAtCoordinates(x, y) {
  return handleRequest('click coordinates', async (driver) => {
    console.log(`Clicking at coordinates: (${x}, ${y})`);
    // Use W3C Actions API instead of deprecated touchAction
    await driver.performActions([{
      type: 'pointer',
      id: 'finger1',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x, y },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 100 },
        { type: 'pointerUp', button: 0 }
      ]
    }]);
    await driver.releaseActions();
    console.log('Click executed successfully');
    return { success: true };
  });
}

/**
 * Get current activity information
 */
async function getCurrentActivity() {
  return handleRequest('get current activity', async (driver) => {
    console.log('Getting current activity...');
    const activity = await driver.getCurrentActivity();
    const packageName = await driver.getCurrentPackage();
    console.log(`Current activity: ${packageName}/${activity}`);
    return {
      success: true,
      activity,
      package: packageName,
      fullActivity: `${packageName}/${activity}`
    };
  });
}

/**
 * Get all installed packages
 */
async function getAllPackages() {
  return handleRequest('get packages', async (driver) => {
    console.log('Getting all installed packages...');
    const result = await driver.execute('mobile: shell', {
      command: 'pm',
      args: ['list', 'packages']
    });
    const packages = result
      .split('\n')
      .filter(line => line.startsWith('package:'))
      .map(line => line.replace('package:', '').trim())
      .filter(pkg => pkg.length > 0)
      .sort();
    console.log(`Found ${packages.length} packages`);
    return { success: true, packages, count: packages.length };
  });
}

/**
 * Get activities for a specific package
 */
async function getPackageActivities(packageName) {
  return handleRequest('get package activities', async (driver) => {
    console.log(`Getting activities for package: ${packageName}`);
    const result = await driver.execute('mobile: shell', {
      command: 'dumpsys',
      args: ['package', packageName]
    });
    const activities = [];
    const lines = result.split('\n');
    for (const line of lines) {
      const trimmed = line.trim();
      if (trimmed.includes('Activity') && trimmed.includes(packageName)) {
        const match = trimmed.match(/([a-zA-Z0-9_.]+\/[a-zA-Z0-9_.]+)/);
        if (match && !activities.includes(match[1])) {
          activities.push(match[1]);
        }
      }
    }
    console.log(`Found ${activities.length} activities`);
    return { success: true, package: packageName, activities, count: activities.length };
  });
}

/**
 * Launch an app by package and activity
 */
async function launchApp(packageName, activityName = null) {
  return handleRequest('launch app', async (driver) => {
    console.log(`Launching app: ${packageName}${activityName ? '/' + activityName : ''}`);
    if (activityName) {
      await driver.execute('mobile: shell', {
        command: 'am',
        args: ['start', '-n', `${packageName}/${activityName}`]
      });
    } else {
      await driver.execute('mobile: shell', {
        command: 'monkey',
        args: ['-p', packageName, '-c', 'android.intent.category.LAUNCHER', '1']
      });
    }
    console.log('App launched successfully');
    return { success: true };
  });
}

/**
 * Parse XML to JSON structure
 * @param {string} xml - XML content
 * @returns {Array} Parsed nodes
 */
function parseXmlToJson(xml) {
  // Match any XML tag (not just <node>), including android.widget.*, android.view.*, etc.
  const regex = /<([\w.]+)([^>]*?)(?:\/>|>)/g;
  const nodes = [];
  let match;

  while ((match = regex.exec(xml)) !== null) {
    const tagName = match[1];
    const attrs = match[2];

    // Skip hierarchy and other non-UI elements
    if (tagName === 'hierarchy' || tagName === '?xml') {
      continue;
    }

    const node = {
      class: tagName
    };

    // Parse attributes
    const attrRegex = /(\w+(?:-\w+)*)="([^"]*)"/g;
    let attrMatch;

    while ((attrMatch = attrRegex.exec(attrs)) !== null) {
      node[attrMatch[1]] = attrMatch[2];
    }

    // Parse bounds
    if (node.bounds) {
      const boundsMatch = node.bounds.match(/\[(\d+),(\d+)\]\[(\d+),(\d+)\]/);
      if (boundsMatch) {
        node.boundsObj = {
          left: parseInt(boundsMatch[1]),
          top: parseInt(boundsMatch[2]),
          right: parseInt(boundsMatch[3]),
          bottom: parseInt(boundsMatch[4]),
          width: parseInt(boundsMatch[3]) - parseInt(boundsMatch[1]),
          height: parseInt(boundsMatch[4]) - parseInt(boundsMatch[2]),
          centerX: (parseInt(boundsMatch[1]) + parseInt(boundsMatch[3])) / 2,
          centerY: (parseInt(boundsMatch[2]) + parseInt(boundsMatch[4])) / 2
        };
      }
    }

    nodes.push(node);
  }

  return nodes;
}

/**
 * Handle debug API requests
 */
async function handleDebugAPI(method, action, query, body) {
  try {
    if (method === 'GET') {
      switch (action) {
        case 'dump':
          return await getUIDump();
        case 'screenshot':
          return await takeScreenshot();
        case 'activity':
          return await getCurrentActivity();
        case 'packages':
          return await getAllPackages();
        case 'activities':
          if (!query.package) {
            return { success: false, error: 'Package name required' };
          }
          return await getPackageActivities(query.package);
        case 'status':
          return await checkConnection();
        default:
          return { success: false, error: 'Unknown action' };
      }
    } else if (method === 'POST') {
      switch (action) {
        case 'click':
          return await clickAtCoordinates(body.x, body.y);
        case 'launch':
          return await launchApp(body.package, body.activity);
        case 'connect':
          return await connectSession();
        case 'disconnect':
          await closeDebugSession();
          return { success: true, message: 'Session disconnected' };
        default:
          return { success: false, error: 'Unknown action' };
      }
    }
    return { success: false, error: 'Invalid method' };
  } catch (error) {
    return { success: false, error: error.message };
  }
}

// ============================================================================
// Exports
// ============================================================================

module.exports = {
  getUIDump,
  takeScreenshot,
  clickAtCoordinates,
  getCurrentActivity,
  getAllPackages,
  getPackageActivities,
  launchApp,
  parseXmlToJson,
  handleDebugAPI,
  getDebugDriver,
  closeDebugSession,
  connectSession,
  checkConnection,
};
