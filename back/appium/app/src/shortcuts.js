/**
 * Shortcuts Module
 *
 * Provides common action combinations and constants for Appium automation.
 * Use these shortcuts in your scripts for better code readability and maintainability.
 *
 * @module shortcuts
 */

// ============================================================================
// ANDROID KEYCODES
// ============================================================================

/**
 * Android KeyCode Constants
 * Reference: https://developer.android.com/reference/android/view/KeyEvent
 */
const KEYCODE = {
  // Navigation Keys
  HOME: 3,
  BACK: 4,
  MENU: 82,
  APP_SWITCH: 187,

  // Power & Volume
  POWER: 26,
  VOLUME_UP: 24,
  VOLUME_DOWN: 25,
  VOLUME_MUTE: 164,

  // Basic Input
  ENTER: 66,
  DELETE: 67,
  DEL: 67,
  BACKSPACE: 67,
  SPACE: 62,
  TAB: 61,

  // Directional Pad
  DPAD_UP: 19,
  DPAD_DOWN: 20,
  DPAD_LEFT: 21,
  DPAD_RIGHT: 22,
  DPAD_CENTER: 23,

  // Media Controls
  MEDIA_PLAY: 126,
  MEDIA_PAUSE: 127,
  MEDIA_PLAY_PAUSE: 85,
  MEDIA_STOP: 86,
  MEDIA_NEXT: 87,
  MEDIA_PREVIOUS: 88,
  MEDIA_REWIND: 89,
  MEDIA_FAST_FORWARD: 90,

  // Phone
  CALL: 5,
  ENDCALL: 6,
  CAMERA: 27,

  // Numbers
  NUM_0: 7,
  NUM_1: 8,
  NUM_2: 9,
  NUM_3: 10,
  NUM_4: 11,
  NUM_5: 12,
  NUM_6: 13,
  NUM_7: 14,
  NUM_8: 15,
  NUM_9: 16,

  // Letters (A-Z)
  A: 29, B: 30, C: 31, D: 32, E: 33, F: 34, G: 35, H: 36,
  I: 37, J: 38, K: 39, L: 40, M: 41, N: 42, O: 43, P: 44,
  Q: 45, R: 46, S: 47, T: 48, U: 49, V: 50, W: 51, X: 52,
  Y: 53, Z: 54,

  // Symbols
  COMMA: 55,
  PERIOD: 56,
  SLASH: 76,
  BACKSLASH: 73,
  APOSTROPHE: 75,
  AT: 77,
  PLUS: 81,
  MINUS: 69,
  EQUALS: 70,
  LEFT_BRACKET: 71,
  RIGHT_BRACKET: 72,
  SEMICOLON: 74,
  STAR: 17,
  POUND: 18,

  // Function Keys
  F1: 131, F2: 132, F3: 133, F4: 134, F5: 135, F6: 136,
  F7: 137, F8: 138, F9: 139, F10: 140, F11: 141, F12: 142,

  // System
  NOTIFICATION: 83,
  SEARCH: 84,
  SETTINGS: 176,
  BRIGHTNESS_UP: 221,
  BRIGHTNESS_DOWN: 220,

  // Special Android Keys
  SOFT_LEFT: 1,
  SOFT_RIGHT: 2,
  FOCUS: 80,
  HEADSETHOOK: 79,
};

// ============================================================================
// ANDROID CLASS NAMES
// ============================================================================

/**
 * Common Android UI Widget Class Names
 * Use these constants for element selection to avoid typos
 */
const CLASS = {
  // Common Widgets
  VIEW: 'android.view.View',
  VIEWGROUP: 'android.view.ViewGroup',
  TEXTVIEW: 'android.widget.TextView',
  EDITTEXT: 'android.widget.EditText',
  BUTTON: 'android.widget.Button',
  IMAGEVIEW: 'android.widget.ImageView',
  IMAGEBUTTON: 'android.widget.ImageButton',
  CHECKBOX: 'android.widget.CheckBox',
  RADIOBUTTON: 'android.widget.RadioButton',
  SWITCH: 'android.widget.Switch',
  TOGGLEBUTTON: 'android.widget.ToggleButton',
  SEEKBAR: 'android.widget.SeekBar',
  PROGRESSBAR: 'android.widget.ProgressBar',
  SPINNER: 'android.widget.Spinner',

  // Layouts
  LINEARLAYOUT: 'android.widget.LinearLayout',
  RELATIVELAYOUT: 'android.widget.RelativeLayout',
  FRAMELAYOUT: 'android.widget.FrameLayout',
  CONSTRAINTLAYOUT: 'androidx.constraintlayout.widget.ConstraintLayout',
  COORDINATORLAYOUT: 'androidx.coordinatorlayout.widget.CoordinatorLayout',

  // Lists & Scrolling
  LISTVIEW: 'android.widget.ListView',
  GRIDVIEW: 'android.widget.GridView',
  SCROLLVIEW: 'android.widget.ScrollView',
  HORIZONTALSCROLLVIEW: 'android.widget.HorizontalScrollView',
  RECYCLERVIEW: 'androidx.recyclerview.widget.RecyclerView',

  // Web
  WEBVIEW: 'android.webkit.WebView',

  // Other
  TOOLBAR: 'android.widget.Toolbar',
  TABWIDGET: 'android.widget.TabWidget',
  DATEPICKER: 'android.widget.DatePicker',
  TIMEPICKER: 'android.widget.TimePicker',
};

// ============================================================================
// SWIPE DIRECTIONS
// ============================================================================

/**
 * Swipe Direction Constants
 */
const SWIPE = {
  UP: 'up',
  DOWN: 'down',
  LEFT: 'left',
  RIGHT: 'right',
};

// ============================================================================
// ACTION BUILDERS
// ============================================================================

/**
 * Create a tap action at specific coordinates
 *
 * @param {number} x - X coordinate
 * @param {number} y - Y coordinate
 * @param {number} duration - Press duration in milliseconds (default: 100)
 * @returns {Object} W3C Actions object
 *
 * @example
 * const tapAction = Actions.tap(100, 200);
 * await appium.performActions([tapAction]);
 * await appium.releaseActions();
 */
function tap(x, y, duration = 100) {
  return {
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x, y },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration },
      { type: 'pointerUp', button: 0 }
    ]
  };
}

/**
 * Create a double tap action at specific coordinates
 *
 * @param {number} x - X coordinate
 * @param {number} y - Y coordinate
 * @param {number} tapDuration - Duration of each tap (default: 100ms)
 * @param {number} interval - Time between taps (default: 100ms)
 * @returns {Object} W3C Actions object
 *
 * @example
 * const doubleTapAction = Actions.doubleTap(200, 300);
 * await appium.performActions([doubleTapAction]);
 * await appium.releaseActions();
 */
function doubleTap(x, y, tapDuration = 100, interval = 100) {
  return {
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      // First tap
      { type: 'pointerMove', duration: 0, x, y },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: tapDuration },
      { type: 'pointerUp', button: 0 },
      // Wait between taps
      { type: 'pause', duration: interval },
      // Second tap
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: tapDuration },
      { type: 'pointerUp', button: 0 }
    ]
  };
}

/**
 * Create a long press action at specific coordinates
 *
 * @param {number} x - X coordinate
 * @param {number} y - Y coordinate
 * @param {number} duration - Press duration in milliseconds (default: 1000)
 * @returns {Object} W3C Actions object
 *
 * @example
 * const longPressAction = Actions.longPress(300, 400, 2000);
 * await appium.performActions([longPressAction]);
 * await appium.releaseActions();
 */
function longPress(x, y, duration = 1000) {
  return {
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x, y },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration },
      { type: 'pointerUp', button: 0 }
    ]
  };
}

/**
 * Create a swipe action
 *
 * @param {number} startX - Starting X coordinate
 * @param {number} startY - Starting Y coordinate
 * @param {number} endX - Ending X coordinate
 * @param {number} endY - Ending Y coordinate
 * @param {number} duration - Swipe duration in milliseconds (default: 500)
 * @returns {Object} W3C Actions object
 *
 * @example
 * const swipeAction = Actions.swipe(100, 500, 100, 100);
 * await appium.performActions([swipeAction]);
 * await appium.releaseActions();
 */
function swipe(startX, startY, endX, endY, duration = 500) {
  return {
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: startX, y: startY },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 100 },
      { type: 'pointerMove', duration, x: endX, y: endY },
      { type: 'pointerUp', button: 0 }
    ]
  };
}

/**
 * Create a pinch (zoom out) action
 *
 * @param {number} centerX - Center X coordinate
 * @param {number} centerY - Center Y coordinate
 * @param {number} scale - Scale factor (0.1 to 0.9, where smaller = more pinch)
 * @param {number} duration - Pinch duration in milliseconds (default: 500)
 * @returns {Array<Object>} Array of W3C Actions objects (2 fingers)
 *
 * @example
 * const pinchActions = Actions.pinch(400, 600, 0.3);
 * await appium.performActions(pinchActions);
 * await appium.releaseActions();
 */
function pinch(centerX, centerY, scale = 0.5, duration = 500) {
  const offset = 100;
  const scaledOffset = offset * scale;

  return [
    // Finger 1 - move from top to center
    {
      type: 'pointer',
      id: 'finger1',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x: centerX, y: centerY - offset },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 50 },
        { type: 'pointerMove', duration, x: centerX, y: centerY - scaledOffset },
        { type: 'pointerUp', button: 0 }
      ]
    },
    // Finger 2 - move from bottom to center
    {
      type: 'pointer',
      id: 'finger2',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x: centerX, y: centerY + offset },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 50 },
        { type: 'pointerMove', duration, x: centerX, y: centerY + scaledOffset },
        { type: 'pointerUp', button: 0 }
      ]
    }
  ];
}

/**
 * Create a zoom (spread) action
 *
 * @param {number} centerX - Center X coordinate
 * @param {number} centerY - Center Y coordinate
 * @param {number} scale - Scale factor (1.1 to 3.0, where larger = more zoom)
 * @param {number} duration - Zoom duration in milliseconds (default: 500)
 * @returns {Array<Object>} Array of W3C Actions objects (2 fingers)
 *
 * @example
 * const zoomActions = Actions.zoom(400, 600, 2.0);
 * await appium.performActions(zoomActions);
 * await appium.releaseActions();
 */
function zoom(centerX, centerY, scale = 2.0, duration = 500) {
  const offset = 50;
  const scaledOffset = offset * scale;

  return [
    // Finger 1 - move from center to top
    {
      type: 'pointer',
      id: 'finger1',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x: centerX, y: centerY - offset },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 50 },
        { type: 'pointerMove', duration, x: centerX, y: centerY - scaledOffset },
        { type: 'pointerUp', button: 0 }
      ]
    },
    // Finger 2 - move from center to bottom
    {
      type: 'pointer',
      id: 'finger2',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x: centerX, y: centerY + offset },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 50 },
        { type: 'pointerMove', duration, x: centerX, y: centerY + scaledOffset },
        { type: 'pointerUp', button: 0 }
      ]
    }
  ];
}

/**
 * Actions object containing all action builder functions
 */
const Actions = {
  tap,
  doubleTap,
  longPress,
  swipe,
  pinch,
  zoom,
};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Calculate coordinates for directional swipes
 *
 * @param {Object} appium - Appium wrapper object
 * @param {string} direction - Swipe direction (up, down, left, right)
 * @param {number} distance - Swipe distance as percentage (0-1, default: 0.6)
 * @returns {Promise<Object>} Object with startX, startY, endX, endY
 *
 * @example
 * const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.UP);
 * const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY);
 * await appium.performActions([swipeAction]);
 * await appium.releaseActions();
 */
async function getSwipeCoordinates(appium, direction, distance = 0.6) {
  const { width, height } = await appium.getWindowSize();
  const centerX = Math.floor(width / 2);
  const centerY = Math.floor(height / 2);

  const coords = {
    startX: centerX,
    startY: centerY,
    endX: centerX,
    endY: centerY,
  };

  switch (direction) {
    case SWIPE.UP:
      coords.startY = Math.floor(height * (0.5 + distance / 2));
      coords.endY = Math.floor(height * (0.5 - distance / 2));
      break;
    case SWIPE.DOWN:
      coords.startY = Math.floor(height * (0.5 - distance / 2));
      coords.endY = Math.floor(height * (0.5 + distance / 2));
      break;
    case SWIPE.LEFT:
      coords.startX = Math.floor(width * (0.5 + distance / 2));
      coords.endX = Math.floor(width * (0.5 - distance / 2));
      break;
    case SWIPE.RIGHT:
      coords.startX = Math.floor(width * (0.5 - distance / 2));
      coords.endX = Math.floor(width * (0.5 + distance / 2));
      break;
    default:
      throw new Error(`Invalid swipe direction: ${direction}`);
  }

  return coords;
}

/**
 * Get coordinates for screen center
 *
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Object with x and y coordinates
 *
 * @example
 * const center = await Helpers.getCenterCoordinates(appium);
 * const tapAction = Actions.tap(center.x, center.y);
 * await appium.performActions([tapAction]);
 * await appium.releaseActions();
 */
async function getCenterCoordinates(appium) {
  const { width, height } = await appium.getWindowSize();
  return {
    x: Math.floor(width / 2),
    y: Math.floor(height / 2),
  };
}

/**
 * Get coordinates as percentage of screen
 *
 * @param {Object} appium - Appium wrapper object
 * @param {number} xPercent - X position as percentage (0-1)
 * @param {number} yPercent - Y position as percentage (0-1)
 * @returns {Promise<Object>} Object with x and y coordinates
 *
 * @example
 * // Get coordinates at 25% from left, 75% from top
 * const coords = await Helpers.getPercentageCoordinates(appium, 0.25, 0.75);
 * const tapAction = Actions.tap(coords.x, coords.y);
 * await appium.performActions([tapAction]);
 * await appium.releaseActions();
 */
async function getPercentageCoordinates(appium, xPercent, yPercent) {
  const { width, height } = await appium.getWindowSize();
  return {
    x: Math.floor(width * xPercent),
    y: Math.floor(height * yPercent),
  };
}

/**
 * Helpers object containing utility functions
 */
const Helpers = {
  getSwipeCoordinates,
  getCenterCoordinates,
  getPercentageCoordinates,
};

// ============================================================================
// ELEMENT SELECTORS
// ============================================================================

/**
 * Build selector string by Resource ID
 *
 * @param {string} resourceId - The resource ID (e.g., "com.example:id/button")
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byId('cn.soulapp.android:id/textContent');
 * const element = await appium.$(selector);
 */
function byId(resourceId) {
  return `android=new UiSelector().resourceId("${resourceId}")`;
}

/**
 * Build selector string by class name and text
 *
 * @param {string} className - The class name (use CLASS constants)
 * @param {string} text - The exact text to match
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byClassAndText(CLASS.TEXTVIEW, '下一步');
 * const element = await appium.$(selector);
 */
function byClassAndText(className, text) {
  return `android=new UiSelector().className("${className}").text("${text}")`;
}

/**
 * Build selector string by class name and text contains
 *
 * @param {string} className - The class name (use CLASS constants)
 * @param {string} text - The text to search for (partial match)
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byClassAndTextContains(CLASS.TEXTVIEW, '权限');
 * const element = await appium.$(selector);
 */
function byClassAndTextContains(className, text) {
  return `android=new UiSelector().className("${className}").textContains("${text}")`;
}

/**
 * Build selector string by text only
 *
 * @param {string} text - The exact text to match
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byText('登录');
 * const element = await appium.$(selector);
 */
function byText(text) {
  return `android=new UiSelector().text("${text}")`;
}

/**
 * Build selector string by text contains
 *
 * @param {string} text - The text to search for (partial match)
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byTextContains('确认');
 * const element = await appium.$(selector);
 */
function byTextContains(text) {
  return `android=new UiSelector().textContains("${text}")`;
}

/**
 * Build selector string for child element in RecyclerView
 *
 * @param {string} childResourceId - The child element resource ID
 * @param {number} instance - The instance index (default: 0)
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byRecyclerViewChild('cn.soulapp.android:id/tv_select_mark', 0);
 * const element = await appium.$(selector);
 */
function byRecyclerViewChild(childResourceId, instance = 0) {
  return `android=new UiSelector().className("${CLASS.RECYCLERVIEW}").childSelector(new UiSelector().resourceId("${childResourceId}").instance(${instance}))`;
}

/**
 * Build selector string by content description
 *
 * @param {string} description - The content description to match
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byDescription('Navigate up');
 * const element = await appium.$(selector);
 */
function byDescription(description) {
  return `android=new UiSelector().description("${description}")`;
}

/**
 * Build selector string by class name only
 *
 * @param {string} className - The class name (use CLASS constants)
 * @param {number} instance - The instance index (default: 0)
 * @returns {string} UiSelector string
 *
 * @example
 * const selector = Selectors.byClass(CLASS.EDITTEXT, 0);
 * const element = await appium.$(selector);
 */
function byClass(className, instance = 0) {
  return `android=new UiSelector().className("${className}").instance(${instance})`;
}

/**
 * Selectors object containing all selector builder functions
 */
const Selectors = {
  byId,
  byClassAndText,
  byClassAndTextContains,
  byText,
  byTextContains,
  byRecyclerViewChild,
  byDescription,
  byClass,
};

// ============================================================================
// EXPORTS
// ============================================================================

module.exports = {
  // Constants
  KEYCODE,
  SWIPE,
  CLASS,

  // Action Builders
  Actions,

  // Helper Functions
  Helpers,

  // Element Selectors
  Selectors,
};
