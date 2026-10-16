# Appium Scripts Documentation

This directory contains Appium automation scripts that can be executed from the main interface.

## Table of Contents

- [Script Format](#script-format)
- [Core Principles](#core-principles)
- [Available Appium Methods](#available-appium-methods)
- [Comprehensive Examples](#comprehensive-examples)
- [Element Operations](#element-operations)
- [Advanced Touch Gestures](#advanced-touch-gestures)
- [App Management](#app-management)
- [Device Control](#device-control)
- [Network Operations](#network-operations)
- [Best Practices](#best-practices)
- [Common Patterns](#common-patterns)
- [Debugging Tips](#debugging-tips)

## Script Format

Every script must be a `.js` file that exports three properties: `name`, `config`, and `execute` function:

```javascript
/**
 * Script description
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  // Your script logic here
  // Access config values if needed from module.exports.config

  return {
    success: true,
    message: 'Operation completed',
    data: null // Optional: set to base64 screenshot to display on left panel
  }
}

module.exports = {
  name: 'My Script Name',  // Display name shown in dropdown menu
  config: {                // Configuration object for future use
    // Add any configuration parameters here
    param1: 'value1',
    param2: 100
  },
  execute                  // The main script function
}
```

### Export Properties

- **name** (required): Display name shown in the script selection dropdown
- **config** (required): Object containing configuration parameters (can be empty `{}`)
- **execute** (required): Async function that performs the script operations

## Core Principles

### ⚠️ IMPORTANT RULES

1. **NO direct driver access** - Scripts can ONLY call methods through the `appium` object
2. **Use basic APIs only** - The `appium` object provides only Appium's native methods
3. **Scripts compose actions** - All complex operations must be composed from basic APIs in scripts
4. **No redundant code** - `appium.js` contains NO composite operations, keeping it lean

### Return Value Structure

```javascript
{
  success: boolean,    // Required: operation success status
  message: string,     // Required: human-readable message
  data: any           // Optional: screenshot base64 or other data
}
```

## Available Appium Methods

### 🖐️ Touch Actions (W3C Actions API)

#### performActions(actions)
Execute touch/pointer actions on the device.

```javascript
// Single tap
await appium.performActions([{
  type: 'pointer',
  id: 'finger1',
  parameters: { pointerType: 'touch' },
  actions: [
    { type: 'pointerMove', duration: 0, x: 100, y: 200 },
    { type: 'pointerDown', button: 0 },
    { type: 'pause', duration: 100 },
    { type: 'pointerUp', button: 0 }
  ]
}])

// Swipe gesture
await appium.performActions([{
  type: 'pointer',
  id: 'finger1',
  parameters: { pointerType: 'touch' },
  actions: [
    { type: 'pointerMove', duration: 0, x: 200, y: 800 },
    { type: 'pointerDown', button: 0 },
    { type: 'pause', duration: 100 },
    { type: 'pointerMove', duration: 500, x: 200, y: 200 },
    { type: 'pointerUp', button: 0 }
  ]
}])

// Multi-finger gesture (pinch/zoom)
await appium.performActions([
  {
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: 300, y: 500 },
      { type: 'pointerDown', button: 0 },
      { type: 'pointerMove', duration: 500, x: 200, y: 400 },
      { type: 'pointerUp', button: 0 }
    ]
  },
  {
    type: 'pointer',
    id: 'finger2',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: 400, y: 500 },
      { type: 'pointerDown', button: 0 },
      { type: 'pointerMove', duration: 500, x: 500, y: 600 },
      { type: 'pointerUp', button: 0 }
    ]
  }
])
```

#### releaseActions()
Clear all active pointer/key states.

```javascript
await appium.releaseActions()
```

### 🔍 Element Finding

#### $(selector)
Find a single element.

```javascript
// Android UiSelector
const loginBtn = await appium.$('android=new UiSelector().text("Login")')
await loginBtn.click()

// XPath
const usernameInput = await appium.$('//android.widget.EditText[@resource-id="username"]')
await usernameInput.setValue('myuser')

// Accessibility ID
const submitBtn = await appium.$('~submitButton')

// Class name
const button = await appium.$('android.widget.Button')

// Check if element exists
const element = await appium.$('~loginButton')
const exists = await element.isExisting()
if (exists) {
  await element.click()
}

// Wait for element
await element.waitForExist({ timeout: 5000 })

// Get element text
const text = await element.getText()

// Get element attribute
const enabled = await element.getAttribute('enabled')
```

#### $$(selector)
Find multiple elements.

```javascript
// Find all buttons
const buttons = await appium.$$('//android.widget.Button')

for (const btn of buttons) {
  const text = await btn.getText()
  console.log(`Button text: ${text}`)
  
  if (text === 'Submit') {
    await btn.click()
    break
  }
}

// Find all text fields and fill them
const inputs = await appium.$$('//android.widget.EditText')
const values = ['John', 'Doe', 'john@example.com']

for (let i = 0; i < inputs.length; i++) {
  await inputs[i].setValue(values[i])
}
```

### 📱 Screen Information

#### getWindowSize()
Get device screen dimensions.

```javascript
const { width, height } = await appium.getWindowSize()
console.log(`Screen: ${width}x${height}`)

// Calculate center
const centerX = Math.floor(width / 2)
const centerY = Math.floor(height / 2)
```

#### takeScreenshot()
Capture screen as base64 PNG.

```javascript
const screenshot = await appium.takeScreenshot()
return {
  success: true,
  message: 'Screenshot captured',
  data: screenshot  // Will be displayed in left panel
}
```

#### getPageSource()
Get XML hierarchy of current screen.

```javascript
const xml = await appium.getPageSource()

// Check if element exists in XML
if (xml.includes('Login')) {
  console.log('Login button found in hierarchy')
}

// Parse and analyze
// Can use XML parsing libraries if needed
```

### ⌨️ Key Events

#### pressKeyCode(keycode)
Press a key by Android keycode.

```javascript
await appium.pressKeyCode(66)  // ENTER
await appium.pressKeyCode(4)   // BACK
await appium.pressKeyCode(3)   // HOME
await appium.pressKeyCode(26)  // POWER
await appium.pressKeyCode(82)  // MENU
await appium.pressKeyCode(24)  // VOLUME_UP
await appium.pressKeyCode(25)  // VOLUME_DOWN
await appium.pressKeyCode(27)  // CAMERA
```

#### longPressKeyCode(keycode)
Long press a key.

```javascript
// Long press power to show power menu
await appium.longPressKeyCode(26)
await appium.pause(1000)
```

#### back()
Press back button.

```javascript
await appium.back()
await appium.pause(500)
```

### 📲 Device Operations

#### isLocked()
Check if device is locked.

```javascript
const locked = await appium.isLocked()
if (locked) {
  console.log('Device is locked, unlocking...')
  // Custom unlock logic here
}
```

#### lock() / unlock()
Lock or unlock device.

```javascript
await appium.lock()
await appium.pause(1000)
await appium.unlock()  // Only works without PIN/password
```

#### hideKeyboard() / isKeyboardShown()
Control keyboard.

```javascript
if (await appium.isKeyboardShown()) {
  await appium.hideKeyboard()
}
```

### 📦 App Operations

#### getCurrentActivity() / getCurrentPackage()
Get current app info.

```javascript
const pkg = await appium.getCurrentPackage()
const activity = await appium.getCurrentActivity()
console.log(`Current: ${pkg}/${activity}`)
```

#### startActivity(package, activity)
Launch an activity.

```javascript
await appium.startActivity('com.android.settings', '.Settings')
await appium.pause(2000)
```

#### terminateApp(appId) / activateApp(appId)
Stop or activate an app.

```javascript
await appium.terminateApp('com.example.app')
await appium.pause(1000)
await appium.activateApp('com.android.chrome')
```

#### queryAppState(appId)
Check app state.

```javascript
const state = await appium.queryAppState('com.example.app')
// 0 = not installed
// 1 = not running
// 3 = running in background
// 4 = running in foreground
```

### 🔄 Orientation

#### getOrientation() / setOrientation(orientation)
Control device orientation.

```javascript
const current = await appium.getOrientation()
console.log(current)  // 'PORTRAIT' or 'LANDSCAPE'

await appium.setOrientation('LANDSCAPE')
await appium.pause(1000)
await appium.setOrientation('PORTRAIT')
```

### 🛠️ Mobile Commands

#### execute(command, params)
Execute mobile-specific commands.

```javascript
// Shell commands
const packages = await appium.execute('mobile: shell', {
  command: 'pm',
  args: ['list', 'packages']
})

// Press power key
await appium.execute('mobile: pressKey', { keycode: 26 })

// Fingerprint (emulator only)
await appium.execute('mobile: fingerprint', { fingerprintId: 1 })

// Swipe gesture
await appium.execute('mobile: swipeGesture', {
  left: 100,
  top: 500,
  width: 200,
  height: 200,
  direction: 'up',
  percent: 0.75
})

// Scroll to element
await appium.execute('mobile: scrollGesture', {
  left: 100,
  top: 100,
  width: 500,
  height: 1000,
  direction: 'down',
  percent: 3.0
})

// Drag and drop
await appium.execute('mobile: dragGesture', {
  startX: 100,
  startY: 200,
  endX: 300,
  endY: 400
})

// Long click
await appium.execute('mobile: longClickGesture', {
  x: 200,
  y: 300,
  duration: 1000
})
```

### ⏱️ Timeouts & Waits

#### pause(ms)
Pause execution.

```javascript
await appium.pause(1000)  // 1 second
await appium.pause(500)   // 0.5 seconds
```

#### setTimeout(timeouts)
Configure timeouts.

```javascript
await appium.setTimeout({ implicit: 5000 })  // 5s for element finding
await appium.setTimeout({ script: 30000 })   // 30s for scripts
```

### ⚙️ Settings

#### getSettings() / updateSettings(settings)
Manage Appium settings.

```javascript
const settings = await appium.getSettings()
console.log(settings)

await appium.updateSettings({
  ignoreUnimportantViews: true,
  allowInvisibleElements: false
})
```

### 📋 Clipboard

#### setClipboard(content, type) / getClipboard(type)
Manage clipboard.

```javascript
await appium.setClipboard('Hello World', 'plaintext')
await appium.setClipboard('https://example.com', 'url')

const text = await appium.getClipboard('plaintext')
console.log(text)
```

### 🌐 Network

#### Toggle network states

```javascript
await appium.toggleWiFi()
await appium.toggleData()
await appium.toggleAirplaneMode()
```

### 📍 Geolocation

#### setGeoLocation(location) / getGeoLocation()
Manage GPS location.

```javascript
await appium.setGeoLocation({
  latitude: 37.7749,    // San Francisco
  longitude: -122.4194,
  altitude: 10
})

const loc = await appium.getGeoLocation()
console.log(`Location: ${loc.latitude}, ${loc.longitude}`)
```

## Comprehensive Examples

### Example 1: Simple Tap

```javascript
/**
 * Tap at specific coordinates
 */
async function execute(appium) {
  const x = 200
  const y = 400

  await appium.performActions([{
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x, y },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 100 },
      { type: 'pointerUp', button: 0 }
    ]
  }])
  await appium.releaseActions()

  return {
    success: true,
    message: `Tapped at (${x}, ${y})`
  }
}

module.exports = {
  name: 'Tap at Coordinates',
  config: {
    x: 200,
    y: 400
  },
  execute
}
```

### Example 2: Login Flow

```javascript
/**
 * Complete login flow
 */
async function execute(appium) {
  try {
    // Find username field
    const username = await appium.$('//android.widget.EditText[@resource-id="username"]')
    await username.waitForExist({ timeout: 5000 })
    await username.setValue('testuser')
    
    // Find password field
    const password = await appium.$('//android.widget.EditText[@resource-id="password"]')
    await password.setValue('password123')
    
    // Hide keyboard
    await appium.hideKeyboard()
    
    // Find and click login button
    const loginBtn = await appium.$('android=new UiSelector().text("Login")')
    await loginBtn.click()
    
    // Wait for dashboard
    await appium.pause(2000)
    
    // Verify login success
    const dashboard = await appium.$('android=new UiSelector().text("Dashboard")')
    const exists = await dashboard.isExisting()
    
    if (exists) {
      return {
        success: true,
        message: 'Login successful'
      }
    } else {
      throw new Error('Dashboard not found after login')
    }
  } catch (error) {
    return {
      success: false,
      message: `Login failed: ${error.message}`
    }
  }
}

module.exports = { execute }
```

### Example 3: Swipe Gestures

```javascript
/**
 * Swipe in different directions
 */
async function execute(appium) {
  const { width, height } = await appium.getWindowSize()
  
  // Swipe up (bottom to top)
  await appium.performActions([{
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: width / 2, y: height * 0.8 },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 100 },
      { type: 'pointerMove', duration: 500, x: width / 2, y: height * 0.2 },
      { type: 'pointerUp', button: 0 }
    ]
  }])
  await appium.releaseActions()
  await appium.pause(1000)
  
  // Swipe down (top to bottom)
  await appium.performActions([{
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: width / 2, y: height * 0.2 },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 100 },
      { type: 'pointerMove', duration: 500, x: width / 2, y: height * 0.8 },
      { type: 'pointerUp', button: 0 }
    ]
  }])
  await appium.releaseActions()
  await appium.pause(1000)
  
  // Swipe left (right to left)
  await appium.performActions([{
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: width * 0.8, y: height / 2 },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 100 },
      { type: 'pointerMove', duration: 500, x: width * 0.2, y: height / 2 },
      { type: 'pointerUp', button: 0 }
    ]
  }])
  await appium.releaseActions()
  await appium.pause(1000)
  
  // Swipe right (left to right)
  await appium.performActions([{
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x: width * 0.2, y: height / 2 },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 100 },
      { type: 'pointerMove', duration: 500, x: width * 0.8, y: height / 2 },
      { type: 'pointerUp', button: 0 }
    ]
  }])
  await appium.releaseActions()
  
  return {
    success: true,
    message: 'Completed all swipe gestures'
  }
}

module.exports = { execute }
```

### Example 4: Unlock Device with PIN

```javascript
/**
 * Unlock device with PIN code
 */
async function execute(appium) {
  // Access config from module.exports
  const { pin } = module.exports.config

  try {
    // Check if already unlocked
    const locked = await appium.isLocked()
    if (!locked) {
      return {
        success: true,
        message: 'Device is already unlocked'
      }
    }

    // Wake up device (press POWER)
    await appium.execute('mobile: pressKey', { keycode: 26 })
    await appium.pause(500)

    // Swipe up to show lock screen
    const { width, height } = await appium.getWindowSize()
    await appium.performActions([{
      type: 'pointer',
      id: 'finger1',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x: width / 2, y: height * 0.8 },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 100 },
        { type: 'pointerMove', duration: 500, x: width / 2, y: height * 0.2 },
        { type: 'pointerUp', button: 0 }
      ]
    }])
    await appium.releaseActions()
    await appium.pause(500)

    // Enter PIN
    for (const digit of pin) {
      const digitBtn = await appium.$(`android=new UiSelector().text("${digit}")`)
      await digitBtn.waitForExist({ timeout: 3000 })
      await digitBtn.click()
      await appium.pause(200)
    }

    // Press ENTER
    await appium.pressKeyCode(66)
    await appium.pause(1000)

    // Verify unlock
    const stillLocked = await appium.isLocked()
    if (!stillLocked) {
      return {
        success: true,
        message: 'Device unlocked successfully'
      }
    } else {
      throw new Error('Device still locked after PIN entry')
    }
  } catch (error) {
    return {
      success: false,
      message: `Unlock failed: ${error.message}`
    }
  }
}

module.exports = {
  name: 'Unlock with PIN',
  config: {
    pin: '1234'  // Configure your PIN here
  },
  execute
}
```

### Example 5: Scroll to Find Element

```javascript
/**
 * Scroll down to find a specific element
 */
async function execute(appium) {
  const targetText = 'Settings'
  const maxScrolls = 10
  
  for (let i = 0; i < maxScrolls; i++) {
    // Try to find element
    const element = await appium.$(`android=new UiSelector().text("${targetText}")`)
    const exists = await element.isExisting()
    
    if (exists) {
      await element.click()
      return {
        success: true,
        message: `Found and clicked "${targetText}" after ${i} scrolls`
      }
    }
    
    // Scroll down
    const { width, height } = await appium.getWindowSize()
    await appium.performActions([{
      type: 'pointer',
      id: 'finger1',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x: width / 2, y: height * 0.7 },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 100 },
        { type: 'pointerMove', duration: 500, x: width / 2, y: height * 0.3 },
        { type: 'pointerUp', button: 0 }
      ]
    }])
    await appium.releaseActions()
    await appium.pause(1000)
  }
  
  return {
    success: false,
    message: `Could not find "${targetText}" after ${maxScrolls} scrolls`
  }
}

module.exports = { execute }
```

### Example 6: Take Multiple Screenshots

```javascript
/**
 * Take screenshots of multiple screens
 */
async function execute(appium) {
  const screenshots = []
  const { width, height } = await appium.getWindowSize()
  
  // Take initial screenshot
  screenshots.push(await appium.takeScreenshot())
  
  // Swipe and take screenshot 3 times
  for (let i = 0; i < 3; i++) {
    await appium.performActions([{
      type: 'pointer',
      id: 'finger1',
      parameters: { pointerType: 'touch' },
      actions: [
        { type: 'pointerMove', duration: 0, x: width * 0.8, y: height / 2 },
        { type: 'pointerDown', button: 0 },
        { type: 'pause', duration: 100 },
        { type: 'pointerMove', duration: 500, x: width * 0.2, y: height / 2 },
        { type: 'pointerUp', button: 0 }
      ]
    }])
    await appium.releaseActions()
    await appium.pause(1000)
    
    screenshots.push(await appium.takeScreenshot())
  }
  
  return {
    success: true,
    message: `Captured ${screenshots.length} screenshots`,
    data: screenshots[screenshots.length - 1]  // Show last screenshot
  }
}

module.exports = { execute }
```

### Example 7: Form Filling

```javascript
/**
 * Fill out a multi-field form
 */
async function execute(appium) {
  const formData = {
    firstName: 'John',
    lastName: 'Doe',
    email: 'john.doe@example.com',
    phone: '1234567890'
  }
  
  try {
    // Find all EditText fields
    const fields = await appium.$$('//android.widget.EditText')
    
    if (fields.length < 4) {
      throw new Error(`Expected 4 fields, found ${fields.length}`)
    }
    
    // Fill each field
    await fields[0].setValue(formData.firstName)
    await appium.pause(300)
    
    await fields[1].setValue(formData.lastName)
    await appium.pause(300)
    
    await fields[2].setValue(formData.email)
    await appium.pause(300)
    
    await fields[3].setValue(formData.phone)
    await appium.pause(300)
    
    // Hide keyboard
    await appium.hideKeyboard()
    
    // Find and click submit button
    const submitBtn = await appium.$('android=new UiSelector().text("Submit")')
    await submitBtn.click()
    
    await appium.pause(2000)
    
    return {
      success: true,
      message: 'Form filled and submitted'
    }
  } catch (error) {
    return {
      success: false,
      message: `Form filling failed: ${error.message}`
    }
  }
}

module.exports = { execute }
```

### Example 8: App Switcher

```javascript
/**
 * Switch between multiple apps
 */
async function execute(appium) {
  const apps = [
    { package: 'com.android.settings', activity: '.Settings' },
    { package: 'com.android.chrome', activity: 'com.google.android.apps.chrome.Main' },
    { package: 'com.android.calculator2', activity: '.Calculator' }
  ]
  
  for (const app of apps) {
    try {
      await appium.startActivity(app.package, app.activity)
      await appium.pause(2000)
      
      const pkg = await appium.getCurrentPackage()
      console.log(`Switched to: ${pkg}`)
    } catch (error) {
      console.log(`Failed to start ${app.package}: ${error.message}`)
    }
  }
  
  return {
    success: true,
    message: `Cycled through ${apps.length} apps`
  }
}

module.exports = { execute }
```

### Example 9: Network Toggle Test

```javascript
/**
 * Test network connectivity toggles
 */
async function execute(appium) {
  const actions = []
  
  // Toggle WiFi
  await appium.toggleWiFi()
  actions.push('WiFi toggled')
  await appium.pause(2000)
  
  // Toggle back
  await appium.toggleWiFi()
  actions.push('WiFi restored')
  await appium.pause(2000)
  
  // Toggle airplane mode
  await appium.toggleAirplaneMode()
  actions.push('Airplane mode toggled')
  await appium.pause(2000)
  
  // Toggle back
  await appium.toggleAirplaneMode()
  actions.push('Airplane mode restored')
  
  return {
    success: true,
    message: actions.join(', ')
  }
}

module.exports = { execute }
```

### Example 10: Long Press and Context Menu

```javascript
/**
 * Perform long press to show context menu
 */
async function execute(appium) {
  const { width, height } = await appium.getWindowSize()
  const x = width / 2
  const y = height / 2
  
  // Long press at center
  await appium.performActions([{
    type: 'pointer',
    id: 'finger1',
    parameters: { pointerType: 'touch' },
    actions: [
      { type: 'pointerMove', duration: 0, x, y },
      { type: 'pointerDown', button: 0 },
      { type: 'pause', duration: 1000 },  // Hold for 1 second
      { type: 'pointerUp', button: 0 }
    ]
  }])
  await appium.releaseActions()
  
  await appium.pause(500)
  
  // Try to find context menu
  const menu = await appium.$('android=new UiSelector().textContains("Copy")')
  const exists = await menu.isExisting()
  
  if (exists) {
    return {
      success: true,
      message: 'Context menu appeared'
    }
  } else {
    return {
      success: false,
      message: 'Context menu did not appear'
    }
  }
}

module.exports = { execute }
```

## Best Practices

### 1. Always Check Element Existence

```javascript
const element = await appium.$('~myButton')
if (await element.isExisting()) {
  await element.click()
} else {
  console.log('Element not found')
}
```

### 2. Use Waits Appropriately

```javascript
// Wait for element to appear
await element.waitForExist({ timeout: 5000 })

// Or use implicit timeout
await appium.setTimeout({ implicit: 5000 })
```

### 3. Handle Errors Gracefully

```javascript
try {
  await someOperation()
  return { success: true, message: 'Done' }
} catch (error) {
  console.error('Error:', error)
  return { success: false, message: error.message }
}
```

### 4. Use Pauses for Animations

```javascript
await element.click()
await appium.pause(1000)  // Wait for animation
```

### 5. Clean Up Actions

```javascript
await appium.performActions([...])
await appium.releaseActions()  // Always release!
```

## Common Patterns

### Pattern: Retry Logic

```javascript
async function retryOperation(operation, maxRetries = 3) {
  for (let i = 0; i < maxRetries; i++) {
    try {
      return await operation()
    } catch (error) {
      if (i === maxRetries - 1) throw error
      await appium.pause(1000)
    }
  }
}

// Usage
await retryOperation(async () => {
  const btn = await appium.$('~submitButton')
  await btn.click()
})
```

### Pattern: Wait for Condition

```javascript
async function waitForCondition(condition, timeout = 10000) {
  const startTime = Date.now()
  while (Date.now() - startTime < timeout) {
    if (await condition()) {
      return true
    }
    await appium.pause(500)
  }
  return false
}

// Usage
const found = await waitForCondition(async () => {
  const element = await appium.$('~dashboard')
  return await element.isExisting()
})
```

### Pattern: Screenshot on Error

```javascript
try {
  // Your operations
} catch (error) {
  const screenshot = await appium.takeScreenshot()
  return {
    success: false,
    message: error.message,
    data: screenshot  // Show error state
  }
}
```

## Debugging Tips

1. **Use console.log()** - Output shows in server logs
2. **Take screenshots** - Capture state at various points
3. **Check element hierarchy** - Use `getPageSource()`
4. **Verify coordinates** - Log calculated touch positions
5. **Add generous pauses** - Help identify timing issues
6. **Test in isolation** - Break complex scripts into smaller ones

## Android Keycodes Reference

```
Common Keycodes:
  3  - HOME
  4  - BACK
  26 - POWER
  66 - ENTER
  82 - MENU
  24 - VOLUME_UP
  25 - VOLUME_DOWN
  27 - CAMERA
  84 - SEARCH
  85 - PLAY_PAUSE
  86 - STOP
  87 - NEXT
  88 - PREVIOUS

Full list: https://developer.android.com/reference/android/view/KeyEvent
```

## Hot Reload

- Scripts are reloaded on every execution
- Modify scripts without restarting the server
- Click "Reload Scripts" to refresh the dropdown list

## Additional Resources

- [WebdriverIO Documentation](https://webdriver.io/)
- [Appium Documentation](http://appium.io/docs/en/latest/)
- [Android UiSelector](https://developer.android.com/reference/androidx/test/uiautomator/UiSelector)
- [W3C WebDriver Spec](https://www.w3.org/TR/webdriver/)
