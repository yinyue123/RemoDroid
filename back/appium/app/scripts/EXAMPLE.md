# Appium Script Examples

This document contains all example scripts for reference.

---

## tap_center.js

```javascript
const { Actions, Helpers } = require('../src/shortcuts');

/**
 * Tap at center of screen
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  const center = await Helpers.getCenterCoordinates(appium);
  const tapAction = Actions.tap(center.x, center.y);

  await appium.performActions([tapAction]);
  await appium.releaseActions();

  return {
    success: true,
    message: `Tapped at center (${center.x}, ${center.y})`
  };
}

module.exports = {
  name: 'Tap Center',
  config: {},
  execute
};
```

---

## swipe_up.js

```javascript
const { Actions, Helpers, SWIPE } = require('../src/shortcuts');

/**
 * Swipe up from bottom to top
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.UP, 0.6);
  const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY);

  await appium.performActions([swipeAction]);
  await appium.releaseActions();

  return {
    success: true,
    message: `Swiped up from (${coords.startX}, ${coords.startY}) to (${coords.endX}, ${coords.endY})`
  };
}

module.exports = {
  name: 'Swipe Up',
  config: {
    distance: 0.6 // Swipe distance as percentage (0-1)
  },
  execute
};
```

---

## swipe_left.js

```javascript
const { Actions, Helpers, SWIPE } = require('../src/shortcuts');

/**
 * Swipe left (next page/screen)
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.LEFT, 0.6);
  const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY);

  await appium.performActions([swipeAction]);
  await appium.releaseActions();

  return {
    success: true,
    message: `Swiped left from (${coords.startX}, ${coords.startY}) to (${coords.endX}, ${coords.endY})`
  };
}

module.exports = {
  name: 'Swipe Left',
  config: {
    distance: 0.6 // Swipe distance as percentage (0-1)
  },
  execute
};
```

---

## swipe_right.js

```javascript
const { Actions, Helpers, SWIPE } = require('../src/shortcuts');

/**
 * Swipe right (previous page/screen)
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.RIGHT, 0.6);
  const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY);

  await appium.performActions([swipeAction]);
  await appium.releaseActions();

  return {
    success: true,
    message: `Swiped right from (${coords.startX}, ${coords.startY}) to (${coords.endX}, ${coords.endY})`
  };
}

module.exports = {
  name: 'Swipe Right',
  config: {
    distance: 0.6 // Swipe distance as percentage (0-1)
  },
  execute
};
```

---

## lock_device.js

```javascript
/**
 * Lock the device
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  try {
    await appium.lock();

    return {
      success: true,
      message: 'Device locked successfully'
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to lock device: ${error.message}`
    };
  }
}

module.exports = {
  name: 'Lock Device',
  config: {},
  execute
};
```

---

## unlock_device.js

```javascript
const { KEYCODE, Actions, Helpers, SWIPE } = require('../src/shortcuts');

/**
 * Unlock device with PIN
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  // Configuration - can be customized
  const config = {
    unlockType: 'pin', // 'pin', 'password', 'pattern', 'fingerprint'
    unlockKey: '1234', // Your PIN/password
  };

  try {
    // Check if already unlocked
    const locked = await appium.isLocked();
    if (!locked) {
      return {
        success: true,
        message: 'Device is already unlocked'
      };
    }

    // Wake up the device (press POWER key)
    await appium.execute('mobile: pressKey', { keycode: KEYCODE.POWER });
    await appium.pause(500);

    // Swipe up to show lock screen
    const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.UP, 0.6);
    const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY);
    await appium.performActions([swipeAction]);
    await appium.releaseActions();
    await appium.pause(500);

    // Enter unlock credentials based on type
    switch (config.unlockType) {
      case 'pin':
      case 'password':
        await enterPassword(appium, config.unlockKey);
        break;
      case 'pattern':
        await drawPattern(appium, config.unlockKey);
        break;
      case 'fingerprint':
        await simulateFingerprint(appium);
        break;
      default:
        throw new Error(`Unsupported unlock type: ${config.unlockType}`);
    }

    return {
      success: true,
      message: 'Device unlocked successfully'
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to unlock device: ${error.message}`
    };
  }
}

/**
 * Enter PIN or password
 */
async function enterPassword(appium, password) {
  // For PIN, tap each digit
  if (/^\d+$/.test(password)) {
    for (const digit of password) {
      // Find and click the digit button
      const digitSelector = `android=new UiSelector().text("${digit}")`;
      const digitElement = await appium.$(digitSelector);
      await digitElement.waitForExist({ timeout: 3000 });
      await digitElement.click();
      await appium.pause(200);
    }

    // Look for and click Enter/OK button
    try {
      const enterButton = await appium.$('android=new UiSelector().text("Enter")');
      if (await enterButton.isExisting()) {
        await enterButton.click();
      } else {
        // Try pressing Enter key
        await appium.pressKeyCode(KEYCODE.ENTER);
      }
    } catch (e) {
      // Try pressing Enter key as fallback
      await appium.pressKeyCode(KEYCODE.ENTER);
    }
  } else {
    // For text password, find input field and enter text
    const passwordField = await appium.$('android.widget.EditText');
    await passwordField.waitForExist({ timeout: 3000 });
    await passwordField.setValue(password);
    await appium.pressKeyCode(KEYCODE.ENTER);
  }

  await appium.pause(1000);
}

/**
 * Draw unlock pattern
 */
async function drawPattern(appium, pattern) {
  const center = await Helpers.getCenterCoordinates(appium);

  // Example: Simple L-shaped pattern (customize based on your pattern)
  await appium.performActions([{
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: center.x - 100, y: center.y - 100 },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 100 },
      { type: 'pointerMove', duration: 100, x: center.x, y: center.y - 100 },
      { type: 'pause', duration: 100 },
      { type: 'pointerMove', duration: 100, x: center.x + 100, y: center.y - 100 },
      { type: 'pointerUp', button: 0 }
    ]
  }]);
  await appium.releaseActions();
  await appium.pause(1000);
}

/**
 * Simulate fingerprint authentication
 */
async function simulateFingerprint(appium, fingerprintId = 1) {
  // This requires the device to be an emulator with fingerprint configured
  await appium.execute('mobile: fingerprint', { fingerprintId });
  await appium.pause(1000);
}

module.exports = {
  name: 'Unlock Device',
  config: {
    unlockType: 'pin', // 'pin', 'password', 'pattern', 'fingerprint'
    unlockKey: '1234', // Your PIN/password
  },
  execute
};
```

---

## take_screenshot.js

```javascript
/**
 * Take a screenshot
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object with screenshot data
 */
async function execute(appium) {
  const screenshot = await appium.takeScreenshot()

  return {
    success: true,
    message: 'Screenshot captured successfully',
    data: screenshot
  }
}

module.exports = {
  name: 'Take Screenshot',
  config: {},
  execute
}
```

---

## double_tap_center.js

```javascript
const { Actions, Helpers } = require('../src/shortcuts');

/**
 * Double tap at center of screen
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  const center = await Helpers.getCenterCoordinates(appium);
  const doubleTapAction = Actions.doubleTap(center.x, center.y);

  await appium.performActions([doubleTapAction]);
  await appium.releaseActions();

  return {
    success: true,
    message: `Double tapped at center (${center.x}, ${center.y})`
  };
}

module.exports = {
  name: 'Double Tap Center',
  config: {},
  execute
};
```

---

## zoom_in.js

```javascript
const { Actions, Helpers } = require('../src/shortcuts');

/**
 * Zoom in (spread gesture) at center of screen
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  const center = await Helpers.getCenterCoordinates(appium);
  const zoomActions = Actions.zoom(center.x, center.y, 2.0);

  await appium.performActions(zoomActions);
  await appium.releaseActions();

  return {
    success: true,
    message: `Zoomed in at (${center.x}, ${center.y}) with scale 2.0`
  };
}

module.exports = {
  name: 'Zoom In',
  config: {
    scale: 2.0 // Scale factor (1.1 to 3.0)
  },
  execute
};
```

---

## zoom_out.js

```javascript
const { Actions, Helpers } = require('../src/shortcuts');

/**
 * Zoom out (pinch gesture) at center of screen
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  const center = await Helpers.getCenterCoordinates(appium);
  const pinchActions = Actions.pinch(center.x, center.y, 0.3);

  await appium.performActions(pinchActions);
  await appium.releaseActions();

  return {
    success: true,
    message: `Zoomed out at (${center.x}, ${center.y}) with scale 0.3`
  };
}

module.exports = {
  name: 'Zoom Out',
  config: {
    scale: 0.3 // Scale factor (0.1 to 0.9, smaller = more pinch)
  },
  execute
};
```

---

## press_back.js

```javascript
const { KEYCODE } = require('../src/shortcuts');

/**
 * Press the Back button
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  await appium.pressKeyCode(KEYCODE.BACK);

  return {
    success: true,
    message: 'Pressed Back button'
  };
}

module.exports = {
  name: 'Press Back',
  config: {},
  execute
};
```

---

## press_home.js

```javascript
const { KEYCODE } = require('../src/shortcuts');

/**
 * Press the Home button
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  await appium.pressKeyCode(KEYCODE.HOME);

  return {
    success: true,
    message: 'Pressed Home button'
  };
}

module.exports = {
  name: 'Press Home',
  config: {},
  execute
};
```

---

## open_app_switcher.js

```javascript
const { KEYCODE } = require('../src/shortcuts');

/**
 * Open app switcher (recent apps)
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  await appium.pressKeyCode(KEYCODE.APP_SWITCH);

  return {
    success: true,
    message: 'Opened app switcher'
  };
}

module.exports = {
  name: 'Open App Switcher',
  config: {},
  execute
};
```

---

## screen_off.js

```javascript
const { KEYCODE } = require('../src/shortcuts');

/**
 * Turn off the screen
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  try {
    // Press power button to turn off screen
    await appium.pressKeyCode(KEYCODE.POWER);
    await appium.pause(500);

    return {
      success: true,
      message: 'Screen turned off'
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to turn off screen: ${error.message}`
    };
  }
}

module.exports = {
  name: 'Screen Off',
  config: {},
  execute
};
```

---

## screen_on.js

```javascript
const { KEYCODE } = require('../src/shortcuts');

/**
 * Turn on the screen (wake up device)
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  try {
    // Check if screen is already on
    const locked = await appium.isLocked();

    if (!locked) {
      return {
        success: true,
        message: 'Screen is already on'
      };
    }

    // Press power button to wake up
    await appium.pressKeyCode(KEYCODE.POWER);
    await appium.pause(500);

    return {
      success: true,
      message: 'Screen turned on'
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to turn on screen: ${error.message}`
    };
  }
}

module.exports = {
  name: 'Screen On',
  config: {},
  execute
};
```
