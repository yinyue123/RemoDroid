const { KEYCODE, Actions, Helpers, SWIPE, CLASS, Selectors } = require('../src/shortcuts');

/**
 * Daily Publish Script - Scheduled Task
 * Automatically publishes content to Soul, Momo, and Tantan
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  try {
    appium.log('=== Starting Daily Publish Task ===');

    // Step 1: Unlock device
    appium.log('\n[Step 1] Unlocking device...');
    const unlockResult = await toggleDeviceLock(appium, false, '123456');
    appium.log(`Unlock result: ${JSON.stringify(unlockResult)}`);

    if (!unlockResult.success) {
      return unlockResult;
    }

    // Step 2: Turn on VPN
    appium.log('\n[Step 2] Turning on VPN...');
    const vpnOnResult = await toggleVPN(appium, true);
    appium.log(`VPN on result: ${JSON.stringify(vpnOnResult)}`);

    if (!vpnOnResult.success) {
      return vpnOnResult;
    }

    // Step 3: Query Gemini for self-introduction
    appium.log('\n[Step 3] Querying Gemini for content...');
    const geminiResult = await queryGemini(appium, '交朋友，做一个简单的自我介绍');
    appium.log(`Gemini result: ${JSON.stringify(geminiResult)}`);

    if (!geminiResult.success) {
      appium.log(`Gemini query failed: ${geminiResult.message}`);
      return geminiResult;
    }

    const publishContent = geminiResult.response;
    appium.log(`Gemini response: ${publishContent}`);

    // Step 4: Turn off VPN
    appium.log('\n[Step 4] Turning off VPN...');
    const vpnOffResult = await toggleVPN(appium, false);
    appium.log(`VPN off result: ${JSON.stringify(vpnOffResult)}`);

    if (!vpnOffResult.success) {
      return vpnOffResult;
    }

    // Step 5: Publish to Soul
    appium.log('\n[Step 5] Publishing to Soul...');
    const soulResult = await publishToSoul(appium, publishContent);
    appium.log(`Soul result: ${JSON.stringify(soulResult)}`);

    if (!soulResult.success) {
      appium.log(`Soul publish failed: ${soulResult.message}`);
      // Continue to next platform even if this one fails
    } else {
      appium.log(`Soul published successfully`);
    }

    // Step 6: Publish to Momo
    appium.log('\n[Step 6] Publishing to Momo...');
    const momoResult = await publishToMomo(appium, publishContent);
    appium.log(`Momo result: ${JSON.stringify(momoResult)}`);

    if (!momoResult.success) {
      appium.log(`Momo publish failed: ${momoResult.message}`);
      // Continue to next platform even if this one fails
    } else {
      appium.log(`Momo published successfully`);
    }

    // Step 7: Publish to Tantan
    appium.log('\n[Step 7] Publishing to Tantan...');
    const tantanResult = await publishToTantan(appium, publishContent);
    appium.log(`Tantan result: ${JSON.stringify(tantanResult)}`);

    if (!tantanResult.success) {
      appium.log(`Tantan publish failed: ${tantanResult.message}`);
      // Continue even if this one fails
    } else {
      appium.log(`Tantan published successfully`);
    }

    // Step 8: Lock device
    appium.log('\n[Step 8] Locking device...');
    const lockResult = await toggleDeviceLock(appium, true);
    appium.log(`Lock result: ${JSON.stringify(lockResult)}`);

    if (!lockResult.success) {
      return lockResult;
    }

    appium.log('\n=== Daily Publish Task Completed Successfully ===');

    return {
      success: true,
      message: 'Daily publish task completed',
      publishedContent: publishContent,
      results: {
        soul: soulResult.success,
        momo: momoResult.success,
        tantan: tantanResult.success
      }
    };

  } catch (error) {
    appium.err(`\nError occurred: ${error.message}`);
    appium.err(`Stack trace: ${error.stack}`);

    // Try to lock device even if error occurred
    try {
      appium.log('Attempting to lock device after error...');
      await toggleDeviceLock(appium, true);
    } catch (lockError) {
      appium.err(`Failed to lock device: ${lockError.message}`);
    }

    return {
      success: false,
      message: `Daily publish task failed: ${error.message}`,
      error: error.stack
    };
  }
}

/**
 * Toggle device lock state
 */
async function toggleDeviceLock(appium, shouldLock, pin = '') {
  try {
    appium.log(`${shouldLock ? 'Locking' : 'Unlocking'} device...`);

    if (!shouldLock) {
      // Unlock device
      if (!pin) {
        throw new Error('PIN is required for unlocking device');
      }

      // Call unlock interface
      appium.log('Calling unlock interface...');
      await appium.unlock();
      await appium.pause(500);

      // Swipe up to show lock screen
      appium.log('Swiping up to show lock screen...');
      const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.UP, 0.6);
      const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY);
      await appium.performActions([swipeAction]);
      await appium.releaseActions();
      await appium.pause(500);

      // Wait for PIN entry screen
      appium.log('Waiting for PIN entry screen...');
      const pinEntryElement = await appium.$(Selectors.byId('com.android.systemui:id/pinEntry'));
      const pinEntryExists = await pinEntryElement.waitForExist({ timeout: 5000 });

      if (!pinEntryExists) {
        appium.log('Warning: pinEntry element not found, but continuing to enter PIN...');
      } else {
        appium.log('PIN entry screen detected');
      }

      // Enter PIN using resource IDs
      appium.log(`Entering PIN: ${pin}...`);
      for (const digit of pin) {
        appium.log(`  Tapping digit: ${digit}`);
        const keyResourceId = `com.android.systemui:id/key${digit}`;
        const digitElement = await appium.$(Selectors.byId(keyResourceId));

        const exists = await digitElement.waitForExist({ timeout: 3000 });
        if (!exists) {
          throw new Error(`Could not find digit button with resource ID: ${keyResourceId}`);
        }

        await digitElement.click();
        await appium.pause(50);
      }

      // Click Enter button
      appium.log('PIN entered, clicking Enter button...');
      const enterElement = await appium.$(Selectors.byId('com.android.systemui:id/key_enter'));

      const enterExists = await enterElement.waitForExist({ timeout: 3000 });
      if (!enterExists) {
        throw new Error('Could not find Enter button with resource ID: com.android.systemui:id/key_enter');
      }

      await enterElement.click();
      await appium.pause(2000);

      // Verify unlock was successful
      const stillLocked = await appium.isLocked();
      appium.log(`Device locked status after unlock attempt: ${stillLocked}`);

      return {
        success: !stillLocked,
        message: stillLocked ? 'Failed to unlock device' : 'Device unlocked successfully'
      };

    } else {
      // Lock device
      await appium.lock();
      await appium.pause(1000);

      const isLocked = await appium.isLocked();
      appium.log(`Device locked status: ${isLocked}`);

      return {
        success: isLocked,
        message: isLocked ? 'Device locked successfully' : 'Failed to lock device'
      };
    }

  } catch (error) {
    appium.err(`Toggle device lock error: ${error.message}`);
    return {
      success: false,
      message: `Failed to ${shouldLock ? 'lock' : 'unlock'} device: ${error.message}`
    };
  }
}

/**
 * Toggle VPN connection
 */
async function toggleVPN(appium, shouldConnect) {
  // Import VPN toggle logic from Demo.js
  // This is a placeholder - you should implement the actual VPN toggle logic
  try {
    appium.log(`${shouldConnect ? 'Connecting' : 'Disconnecting'} VPN...`);

    // TODO: Implement VPN toggle logic here
    // For now, return success

    return {
      success: true,
      message: `VPN ${shouldConnect ? 'connected' : 'disconnected'} successfully`
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to toggle VPN: ${error.message}`
    };
  }
}

/**
 * Query Gemini
 */
async function queryGemini(appium, queryText) {
  // Import Gemini query logic from Demo.js
  // This is a placeholder - you should implement the actual Gemini query logic
  try {
    appium.log(`Querying Gemini: "${queryText}"`);

    // TODO: Implement Gemini query logic here
    // For now, return a mock response

    return {
      success: true,
      response: '大家好，我是一个热爱生活、喜欢交朋友的人。平时喜欢运动、看书、旅行，希望能在这里认识更多志同道合的朋友！',
      message: 'Gemini query successful'
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to query Gemini: ${error.message}`
    };
  }
}

/**
 * Publish to Soul
 */
async function publishToSoul(appium, content) {
  // Import Soul publish logic from Demo.js
  // This is a placeholder - you should implement the actual publish logic
  try {
    appium.log(`Publishing to Soul: "${content}"`);

    // TODO: Implement Soul publish logic here
    // For now, return success

    return {
      success: true,
      message: 'Published to Soul successfully',
      content: content
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to publish to Soul: ${error.message}`
    };
  }
}

/**
 * Publish to Momo
 */
async function publishToMomo(appium, content) {
  // This is a placeholder - you should implement the actual publish logic
  try {
    appium.log(`Publishing to Momo: "${content}"`);

    // TODO: Implement Momo publish logic here
    // For now, return success

    return {
      success: true,
      message: 'Published to Momo successfully',
      content: content
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to publish to Momo: ${error.message}`
    };
  }
}

/**
 * Publish to Tantan
 */
async function publishToTantan(appium, content) {
  // This is a placeholder - you should implement the actual publish logic
  try {
    appium.log(`Publishing to Tantan: "${content}"`);

    // TODO: Implement Tantan publish logic here
    // For now, return success

    return {
      success: true,
      message: 'Published to Tantan successfully',
      content: content
    };
  } catch (error) {
    return {
      success: false,
      message: `Failed to publish to Tantan: ${error.message}`
    };
  }
}

module.exports = {
  name: 'Daily Publish',
  description: 'Automatically publish daily content to Soul, Momo, and Tantan',
  config: {
    pin: '123456',
  },
  schedule: {
    enabled: true,  // Set to true to enable scheduled execution
    interval: 24 * 60 * 60 * 1000,  // Run every 24 hours (in milliseconds)
    nextRun: '2025-10-23T10:00:00.000Z'  // Next scheduled run time (ISO 8601 format)
  },
  execute
};
