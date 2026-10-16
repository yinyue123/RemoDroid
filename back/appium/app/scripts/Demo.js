const { KEYCODE, Actions, Helpers, SWIPE, CLASS, Selectors } = require('../src/shortcuts');

/**
 * Demo Script
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function execute(appium) {
  try {
    appium.log('=== Starting Demo Script ===');

    // Step 1: Unlock device
    appium.log('\n[Step 1] Unlocking device...');
    const unlockResult = await toggleDeviceLock(appium, false, '123456');
    appium.log(`Unlock result: ${JSON.stringify(unlockResult)}`);

    if (!unlockResult.success) {
      return unlockResult;
    }

    // // Step 2: Turn on VPN
    // appium.log('\n[Step 2] Turning on VPN...');
    // const vpnOnResult = await toggleVPN(appium, true);
    // appium.log(`VPN on result: ${JSON.stringify(vpnOnResult)}`);

    // if (!vpnOnResult.success) {
    //   return vpnOnResult;
    // }

    let publishContent = '大家好';
    // // Step 3: Query Gemini about making friends
    // appium.log('\n[Step 3] Querying Gemini about making friends...');
    // const geminiResult = await queryGemini(appium, '交朋友，做一个简单的自我介绍');
    // appium.log(`Gemini result: ${JSON.stringify(geminiResult)}`);

    // if (!geminiResult.success) {
    //   appium.log(`Gemini query failed: ${geminiResult.message}`);
    //   return geminiResult;
    // }

    // publishContent = geminiResult.response;
    // appium.log(`Gemini response: ${publishContent}`);

    // // Step 4: Turn off VPN
    // appium.log('\n[Step 4] Turning off VPN...');
    // const vpnOffResult = await toggleVPN(appium, false);
    // appium.log(`VPN off result: ${JSON.stringify(vpnOffResult)}`);

    // if (!vpnOffResult.success) {
    //   return vpnOffResult;
    // }

    // Step 5: Publish Gemini's response to Soul
    appium.log('\n[Step 5] Publishing Gemini response to Soul...');
    const soulResult = await publishToSoul(appium, publishContent);
    appium.log(`Soul result: ${JSON.stringify(soulResult)}`);

    if (!soulResult.success) {
      appium.log(`Soul publish failed: ${soulResult.message}`);
      return soulResult;
    }

    appium.log(`Soul published successfully`);

    // Step 6: Publish Gemini's response to Momo
    appium.log('\n[Step 6] Publishing Gemini response to Momo...');
    const momoResult = await publishToMomo(appium, publishContent);
    appium.log(`Momo result: ${JSON.stringify(momoResult)}`);

    if (!momoResult.success) {
      appium.log(`Momo publish failed: ${momoResult.message}`);
      return momoResult;
    }

    appium.log(`Momo published successfully`);

    // Step 7: Publish Gemini's response to Tantan
    appium.log('\n[Step 7] Publishing Gemini response to Tantan...');
    const tantanResult = await publishToTantan(appium, publishContent);
    appium.log(`Tantan result: ${JSON.stringify(tantanResult)}`);

    if (!tantanResult.success) {
      appium.log(`Tantan publish failed: ${tantanResult.message}`);
      return tantanResult;
    }

    appium.log(`Tantan published successfully`);

    // Step 8: Lock device
    appium.log('\n[Step 8] Locking device...');
    const lockResult = await toggleDeviceLock(appium, true);
    appium.log(`Lock result: ${JSON.stringify(lockResult)}`);

    if (!lockResult.success) {
      return lockResult;
    }

    appium.log('\n=== Script Completed Successfully ===');

    return {
      success: true,
      message: 'Demo completed successfully',
      geminiResponse: publishContent || 'No response',
      soulPublished: true,
      momoPublished: true,
      tantanPublished: true
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
      message: `Script failed: ${error.message}`,
      error: error.stack
    };
  }
}

/**
 * Toggle device lock state (lock or unlock)
 */
async function toggleDeviceLock(appium, shouldLock, pin = null) {
  try {
    const locked = await appium.isLocked();
    appium.log(`Device locked status: ${locked}`);

    // If locking the device
    if (shouldLock) {
      if (locked) {
        appium.log('Device is already locked');
        return { success: true, message: 'Device is already locked' };
      }

      appium.log('Locking device...');
      await appium.lock();
      await appium.pause(500);

      const nowLocked = await appium.isLocked();
      if (!nowLocked) {
        return { success: false, message: 'Failed to lock device' };
      }

      return { success: true, message: 'Device locked successfully' };
    }

    // If unlocking the device
    if (!locked) {
      appium.log('Device is already unlocked');
      return { success: true, message: 'Device is already unlocked' };
    }

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

    if (stillLocked) {
      return { success: false, message: 'Failed to unlock device - device is still locked' };
    }

    return { success: true, message: 'Device unlocked successfully' };

  } catch (error) {
    appium.err(`Toggle lock error: ${error.message}`);
    return { success: false, message: `Failed to toggle device lock: ${error.message}` };
  }
}

/**
 * Toggle VPN connection (connect or disconnect)
 */
async function toggleVPN(appium, shouldConnect) {
  // Internal helper functions
  const clickFabButton = async () => {
    const fabElement = await appium.$(Selectors.byId('com.v2ray.ang:id/fab'));
    const fabExists = await fabElement.waitForExist({ timeout: 3000 });
    if (!fabExists) throw new Error('Could not find FAB button');
    await fabElement.click();
  };

  const waitForStatus = async (expectedStatus, timeout) => {
    const startTime = Date.now();

    while (Date.now() - startTime < timeout) {
      try {
        const statusElement = await appium.$(Selectors.byId('com.v2ray.ang:id/tv_test_state'));
        const statusExists = await statusElement.waitForExist({ timeout: 1000 });
        if (statusExists) {
          const currentStatus = await statusElement.getText();
          appium.log(`  Current status: ${currentStatus}`);
          if (currentStatus === expectedStatus) return true;
        }
      } catch (e) {
        // Continue waiting
      }
      await appium.pause(500);
    }
    return false;
  };

  const waitForStatusContains = async (expectedText, timeout) => {
    const startTime = Date.now();

    while (Date.now() - startTime < timeout) {
      try {
        const statusElement = await appium.$(Selectors.byId('com.v2ray.ang:id/tv_test_state'));
        const statusExists = await statusElement.waitForExist({ timeout: 1000 });
        if (statusExists) {
          const currentStatus = await statusElement.getText();
          appium.log(`  Current status: ${currentStatus}`);
          if (currentStatus.includes(expectedText)) return true;
        }
      } catch (e) {
        // Continue waiting
      }
      await appium.pause(500);
    }
    return false;
  };

  const testVPNConnection = async () => {
    try {
      appium.log('Testing VPN connection...');
      const statusElement = await appium.$(Selectors.byId('com.v2ray.ang:id/tv_test_state'));

      const statusExists = await statusElement.waitForExist({ timeout: 3000 });
      if (!statusExists) throw new Error('Could not find status element for testing');

      await statusElement.click();
      appium.log('Clicked test button, waiting for test result (timeout: 20 seconds)...');

      const startTime = Date.now();
      const testTimeout = 20000;

      while (Date.now() - startTime < testTimeout) {
        await appium.pause(1000);
        const currentStatus = await statusElement.getText();
        appium.log(`  Test status: ${currentStatus}`);

        if (currentStatus.includes('连接成功')) {
          appium.log('Connection test successful!');
          return { success: true, message: 'VPN connection test passed' };
        }

        if (currentStatus.includes('失败') || currentStatus.includes('错误')) {
          return { success: false, message: `VPN connection test failed: ${currentStatus}` };
        }
      }

      return { success: false, message: 'VPN connection test timeout' };
    } catch (error) {
      appium.err(`Test VPN error: ${error.message}`);
      return { success: false, message: `Failed to test VPN connection: ${error.message}` };
    }
  };

  // Main VPN toggle logic
  try {
    appium.log(`${shouldConnect ? 'Connecting' : 'Disconnecting'} VPN...`);

    // Open V2RayNG app
    appium.log('Opening V2RayNG app...');
    await appium.startActivity('com.v2ray.ang', '.ui.MainActivity');
    await appium.pause(1000);

    // Wait for the connection status element to appear
    appium.log('Waiting for connection status element...');
    const statusElement = await appium.$(Selectors.byId('com.v2ray.ang:id/tv_test_state'));
    const statusExists = await statusElement.waitForExist({ timeout: 5000 });

    if (!statusExists) {
      throw new Error('Could not find connection status element (tv_test_state)');
    }

    // Get current connection status
    const currentStatus = await statusElement.getText();
    appium.log(`Current VPN status: ${currentStatus}`);

    if (shouldConnect) {
      // Connect VPN
      if (currentStatus !== '未连接') {
        appium.log('VPN is already connected, verifying connection...');
        const testResult = await testVPNConnection();
        if (testResult.success) {
          return { success: true, message: 'VPN is already connected and verified' };
        } else {
          appium.log('VPN shows connected but test failed, reconnecting...');
          await clickFabButton();
          await waitForStatus('未连接', 5000);
          await appium.pause(500);
        }
      }

      // Click FAB button to connect
      appium.log('Clicking FAB button to connect...');
      await appium.pause(500);
      await clickFabButton();

      // Wait for connection to establish
      appium.log('Waiting for VPN to connect (timeout: 20 seconds)...');
      const connected = await waitForStatusContains('已连接', 20000);

      if (!connected) {
        return { success: false, message: 'VPN connection timeout - status did not show "已连接"' };
      }

      appium.log('VPN connected, testing connection...');
      const testResult = await testVPNConnection();

      if (!testResult.success) {
        return { success: false, message: 'VPN connected but connection test failed' };
      }

      appium.log('Pressing HOME button to return to home screen...');
      await appium.pressKeyCode(KEYCODE.HOME);
      await appium.pause(1000);

      return { success: true, message: 'VPN connected and verified successfully' };

    } else {
      // Disconnect VPN
      if (currentStatus === '未连接') {
        appium.log('VPN is already disconnected');
        appium.log('Pressing HOME button to return to home screen...');
        await appium.pressKeyCode(KEYCODE.HOME);
        await appium.pause(1000);
        return { success: true, message: 'VPN is already disconnected' };
      }

      // Click FAB button to disconnect
      appium.log('Clicking FAB button to disconnect...');
      await clickFabButton();

      // Wait for disconnection
      appium.log('Waiting for VPN to disconnect...');
      const disconnected = await waitForStatus('未连接', 10000);

      if (!disconnected) {
        return { success: false, message: 'VPN disconnection timeout - status did not show "未连接"' };
      }

      appium.log('Pressing HOME button to return to home screen...');
      await appium.pressKeyCode(KEYCODE.HOME);
      await appium.pause(1000);

      return { success: true, message: 'VPN disconnected successfully' };
    }

  } catch (error) {
    appium.err(`Toggle VPN error: ${error.message}`);
    return { success: false, message: `Failed to toggle VPN: ${error.message}` };
  }
}

/**
 * Query Gemini with optional image upload
 */
async function queryGemini(appium, queryText, albumName = '', imageIndex = -1) {
  // Internal helper functions
  const findAndClickAlbum = async (albumName) => {
    let previousAlbumNames = [];
    let unchangedCount = 0;
    const maxUnchangedAttempts = 3;

    while (unchangedCount < maxUnchangedAttempts) {
      const albumElements = await appium.$$(Selectors.byId('com.google.android.providers.media.module:id/album_name'));

      if (albumElements.length === 0) {
        appium.log('No albums found');
        return false;
      }

      const currentAlbumNames = [];
      for (const element of albumElements) {
        const text = await element.getText();
        currentAlbumNames.push(text);
        appium.log(`  Found album: "${text}"`);

        if (text === albumName) {
          appium.log(`Found target album: "${albumName}"`);
          await element.click();
          return true;
        }
      }

      const namesChanged = JSON.stringify(currentAlbumNames) !== JSON.stringify(previousAlbumNames);

      if (!namesChanged && previousAlbumNames.length > 0) {
        unchangedCount++;
        appium.log(`Album list unchanged (${unchangedCount}/${maxUnchangedAttempts})`);
      } else {
        unchangedCount = 0;
      }

      if (unchangedCount >= maxUnchangedAttempts) {
        appium.log('Reached bottom of album list, album not found');
        return false;
      }

      previousAlbumNames = currentAlbumNames;

      appium.log('Scrolling down to find more albums...');
      const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.UP, 0.5);
      const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY, 300);
      await appium.performActions([swipeAction]);
      await appium.releaseActions();
      await appium.pause(1000);
    }

    return false;
  };

  const selectImageByIndex = async (imageIndex) => {
    try {
      const checkboxElements = await appium.$$(Selectors.byId('com.google.android.providers.media.module:id/icon_check'));

      appium.log(`Found ${checkboxElements.length} images`);

      if (imageIndex >= checkboxElements.length) {
        appium.err(`Image index ${imageIndex} out of range (found ${checkboxElements.length} images)`);
        return false;
      }

      await checkboxElements[imageIndex].click();
      appium.log(`Selected image at index ${imageIndex}`);
      await appium.pause(500);

      return true;
    } catch (error) {
      appium.err(`Error selecting image: ${error.message}`);
      return false;
    }
  };

  const getGeminiResponse = async () => {
    try {
      const chatHistory = await appium.$(Selectors.byId('com.google.android.googlequicksearchbox:id/assistant_robin_chat_history_list'));

      const chatHistoryExists = await chatHistory.waitForExist({ timeout: 5000 });
      if (!chatHistoryExists) {
        throw new Error('Could not find chat history');
      }

      const responseElements = await appium.$$(Selectors.byId('com.google.android.googlequicksearchbox:id/assistant_robin_text'));

      if (responseElements.length === 0) {
        throw new Error('No response elements found');
      }

      const lastResponseElement = responseElements[responseElements.length - 1];
      const responseText = await lastResponseElement.getText();

      return responseText;
    } catch (error) {
      appium.err(`Error getting response: ${error.message}`);
      return null;
    }
  };

  // Main Gemini query logic
  try {
    appium.log(`Querying Gemini: "${queryText}"`);
    if (albumName && imageIndex >= 0) {
      appium.log(`With image from album: "${albumName}", index: ${imageIndex}`);
    }

    // Open Gemini app using search
    appium.log('Opening Gemini app...');
    const openResult = await openAppBySearch(appium, 'Gemini');
    if (!openResult.success) {
      throw new Error(`Failed to open Gemini: ${openResult.message}`);
    }
    await appium.pause(2000);

    // Click on input field
    appium.log('Clicking on input field...');
    const inputElement = await appium.$(Selectors.byId('com.google.android.googlequicksearchbox:id/assistant_robin_input_collapsed_text_half_sheet'));
    const inputExists = await inputElement.waitForExist({ timeout: 5000 });

    if (!inputExists) {
      throw new Error('Could not find input field');
    }

    await inputElement.click();
    await appium.pause(500);

    // Enter query text
    appium.log('Entering query text...');
    await inputElement.setValue(queryText);
    await appium.pause(1000);

    // Upload image if required
    if (albumName && imageIndex >= 0) {
      appium.log('Uploading image...');

      // Click attachment button
      appium.log('Clicking attachment button...');
      const attachmentBtn = await appium.$(Selectors.byId('com.google.android.googlequicksearchbox:id/assistant_robin_chat_input_attachment_btn'));
      const attachmentExists = await attachmentBtn.waitForExist({ timeout: 5000 });

      if (!attachmentExists) {
        throw new Error('Could not find attachment button');
      }

      await attachmentBtn.click();
      await appium.pause(1500);

      // Click "图库" (Gallery)
      appium.log('Looking for "图库" (Gallery) option...');
      const galleryElement = await appium.$(Selectors.byText('图库'));
      const galleryExists = await galleryElement.waitForExist({ timeout: 5000 });

      if (!galleryExists) {
        throw new Error('Could not find "图库" (Gallery) option');
      }

      await galleryElement.click();
      await appium.pause(2000);

      // Click "相册" (Albums)
      appium.log('Looking for "相册" (Albums) option...');
      const albumsElement = await appium.$(Selectors.byText('相册'));
      const albumsExists = await albumsElement.waitForExist({ timeout: 5000 });

      if (!albumsExists) {
        throw new Error('Could not find "相册" (Albums) option');
      }

      await albumsElement.click();
      await appium.pause(2000);

      // Find target album
      appium.log(`Looking for album: "${albumName}"...`);
      const albumFound = await findAndClickAlbum(albumName);

      if (!albumFound) {
        throw new Error(`Could not find album: "${albumName}"`);
      }

      await appium.pause(1500);

      // Select image by index
      appium.log(`Selecting image at index: ${imageIndex}...`);
      const imageSelected = await selectImageByIndex(imageIndex);

      if (!imageSelected) {
        throw new Error(`Could not select image at index: ${imageIndex}`);
      }

      // Click Add button
      appium.log('Clicking Add button...');
      const addButton = await appium.$(Selectors.byId('com.google.android.providers.media.module:id/button_add'));
      const addButtonExists = await addButton.waitForExist({ timeout: 5000 });

      if (!addButtonExists) {
        throw new Error('Could not find Add button');
      }

      await addButton.click();
      appium.log('Waiting for image to upload (10 seconds)...');
      await appium.pause(10000);
    }

    // Click send button
    appium.log('Clicking send button...');
    const sendButton = await appium.$(Selectors.byId('com.google.android.googlequicksearchbox:id/assistant_robin_send_button'));
    const sendButtonExists = await sendButton.waitForExist({ timeout: 5000 });

    if (!sendButtonExists) {
      throw new Error('Could not find send button');
    }

    await sendButton.click();
    await appium.pause(2000);

    // Handle review popup if it appears
    appium.log('Checking for review popup...');
    try {
      const reviewPopup = await appium.$(`android=new UiSelector().resourceId("com.google.android.googlequicksearchbox:id/0_resource_name_obfuscated").textContains("评价会公开显示")`);
      const reviewPopupExists = await reviewPopup.waitForExist({ timeout: 3000 });

      if (reviewPopupExists) {
        appium.log('Review popup detected, clicking "以后再说"...');
        const laterButton = await appium.$(Selectors.byText('以后再说'));
        const laterButtonExists = await laterButton.waitForExist({ timeout: 3000 });

        if (laterButtonExists) {
          await laterButton.click();
          await appium.pause(1000);
        }
      }
    } catch (e) {
      appium.log('No review popup appeared');
    }

    // Wait for response
    appium.log('Waiting for Gemini response (60 seconds)...');
    await appium.pause(60000);

    // Get response
    appium.log('Retrieving response...');
    const response = await getGeminiResponse();

    if (!response) {
      throw new Error('Could not retrieve Gemini response');
    }

    appium.log(`Gemini response: ${response}`);

    // Press HOME button to return to home screen
    appium.log('Pressing HOME button to return to home screen...');
    await appium.pressKeyCode(KEYCODE.HOME);
    await appium.pause(1000);

    return {
      success: true,
      message: 'Query completed successfully',
      response: response
    };

  } catch (error) {
    appium.err(`Query Gemini error: ${error.message}`);
    return {
      success: false,
      message: `Failed to query Gemini: ${error.message}`
    };
  }
}

/**
 * Check and handle popups
 * @param {Object} appium - Appium wrapper object
 * @returns {Promise<Object>} Result object
 */
async function handlePopups(appium) {
  try {
    appium.log('Checking for popups...');

    // Wait 2 seconds before checking
    await appium.pause(2000);

    let popupHandled = false;

    // Check for skip button (ad popup)
    appium.log('Checking for skip button...');
    try {
      const skipSelector = Selectors.byClassAndText(CLASS.TEXTVIEW, '跳过');
      const skipButton = await appium.$(skipSelector);
      const skipExists = await skipButton.isExisting();

      if (skipExists) {
        appium.log('Skip button found, clicking...');
        await skipButton.click();
        await appium.pause(1000);
        appium.log('Skip button clicked successfully');
        popupHandled = true;
      } else {
        appium.log('No skip button found');
      }
    } catch (e) {
      appium.log('No skip button found');
    }

    // Check for permission popup
    appium.log('Checking for permission popup...');
    try {
      const permissionSelector = Selectors.byClassAndTextContains(CLASS.TEXTVIEW, '权限');
      const permissionElement = await appium.$(permissionSelector);
      const permissionExists = await permissionElement.isExisting();

      if (permissionExists) {
        appium.log('Permission popup detected, looking for "暂不开启" button...');
        const notNowSelector = Selectors.byClassAndText(CLASS.TEXTVIEW, '暂不开启');
        const notNowButton = await appium.$(notNowSelector);
        const notNowExists = await notNowButton.isExisting();

        if (notNowExists) {
          appium.log('"暂不开启" button found, clicking...');
          await notNowButton.click();
          await appium.pause(1000);
          appium.log('"暂不开启" button clicked successfully');
          popupHandled = true;
        } else {
          appium.log('"暂不开启" button not found');
        }
      } else {
        appium.log('No permission popup found');
      }
    } catch (e) {
      appium.log('No permission popup found');
    }

    // Define popup keywords to check
    const popupKeywords = ['好评', '评价', '新功能'];
    let popupFound = false;

    // Check if any popup appears
    for (const keyword of popupKeywords) {
      const popupSelector = Selectors.byClassAndTextContains(CLASS.TEXTVIEW, keyword);
      const popupElement = await appium.$(popupSelector);
      const popupExists = await popupElement.isExisting();

      if (popupExists) {
        appium.log(`Popup detected with keyword: "${keyword}"`);
        popupFound = true;
        break;
      }
    }

    // If popup found, check for cancel button
    if (popupFound) {
      appium.log('Looking for cancel button...');
      const cancelSelector = Selectors.byClassAndText(CLASS.TEXTVIEW, '取消');
      const cancelButton = await appium.$(cancelSelector);
      const cancelExists = await cancelButton.isExisting();

      if (cancelExists) {
        appium.log('Cancel button found, clicking...');
        await cancelButton.click();
        await appium.pause(1000);
        appium.log('Cancel button clicked successfully');
        popupHandled = true;
      } else {
        appium.log('Cancel button not found');
      }
    }

    if (popupHandled) {
      return { success: true, message: 'Popup dismissed', popupHandled: true };
    } else {
      appium.log('No popup detected');
      return { success: true, message: 'No popup detected', popupHandled: false };
    }

  } catch (error) {
    appium.err(`Handle popups error: ${error.message}`);
    return { success: false, message: `Failed to handle popups: ${error.message}` };
  }
}

/**
 * Publish content to Soul
 * @param {Object} appium - Appium wrapper object
 * @param {string} content - The content to publish
 * @param {string} albumName - Optional album name to select image from
 * @param {number} imageIndex - Optional image index to select
 * @returns {Promise<Object>} Result object
 */
async function publishToSoul(appium, content, albumName = '', imageIndex = -1) {
  try {
    appium.log(`Publishing to Soul: "${content}"`);
    if (albumName && imageIndex >= 0) {
      appium.log(`With image from album: "${albumName}", index: ${imageIndex}`);
    }

    // Open Soul app using search
    appium.log('Opening Soul app...');
    const openResult = await openAppBySearch(appium, 'Soul');
    if (!openResult.success) {
      throw new Error(`Failed to open Soul: ${openResult.message}`);
    }
    await appium.pause(2000);

    // Check for popups after opening app
    await handlePopups(appium);

    // Click on the center publish button
    appium.log('Clicking on publish button...');
    const publishButton = await appium.$(Selectors.byId('cn.soulapp.android:id/main_tab_center_img'));
    const publishButtonExists = await publishButton.waitForExist({ timeout: 5000 });

    if (!publishButtonExists) {
      throw new Error('Could not find publish button (main_tab_center_img)');
    }

    await publishButton.click();
    await appium.pause(2000);

    // Enter content text
    appium.log('Entering content text...');
    const contentInput = await appium.$(Selectors.byId('cn.soulapp.android:id/textContent'));
    const contentInputExists = await contentInput.waitForExist({ timeout: 5000 });

    if (!contentInputExists) {
      throw new Error('Could not find content input field (textContent)');
    }

    await contentInput.click();
    await appium.pause(500);
    await contentInput.setValue(content);
    await appium.pause(1000);

    appium.log('Content text entered successfully');

    // Click on camera button to add image
    if (albumName && imageIndex >= 0) {
      appium.log('Clicking on camera button...');
      const cameraButton = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, '相机'));
      const cameraButtonExists = await cameraButton.waitForExist({ timeout: 5000 });

      if (!cameraButtonExists) {
        throw new Error('Could not find camera button (TextView with text "相机")');
      }

      await cameraButton.click();
      await appium.pause(2000);

      appium.log('Camera button clicked successfully');

      // Select first image in RecyclerView
      appium.log('Selecting first image...');
      const imageSelect = await appium.$(Selectors.byRecyclerViewChild('cn.soulapp.android:id/tv_select_mark', 0));
      const imageSelectExists = await imageSelect.waitForExist({ timeout: 5000 });

      if (!imageSelectExists) {
        throw new Error('Could not find image select element (tv_select_mark)');
      }

      await imageSelect.click();
      await appium.pause(1000);

      appium.log('First image selected successfully');

      // Click next button
      appium.log('Clicking next button...');
      const nextButton = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, '下一步'));
      const nextButtonExists = await nextButton.waitForExist({ timeout: 5000 });

      if (!nextButtonExists) {
        throw new Error('Could not find next button (TextView with text "下一步")');
      }

      await nextButton.click();
      await appium.pause(2000);

      appium.log('Next button clicked successfully');

      // Check if close button appears and click it
      appium.log('Checking for close button (ivClose)...');
      try {
        const closeButton = await appium.$(Selectors.byId('cn.soulapp.android:id/ivClose'));
        const closeButtonExists = await closeButton.waitForExist({ timeout: 3000 });

        if (closeButtonExists) {
          appium.log('Close button found, clicking it...');
          await closeButton.click();
          await appium.pause(1000);
          appium.log('Close button clicked successfully');
        } else {
          appium.log('Close button not found, continuing...');
        }
      } catch (e) {
        appium.log('No close button appeared');
      }

      // Click next button again
      appium.log('Clicking next button again...');
      const nextButton2 = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, '下一步'));
      const nextButton2Exists = await nextButton2.waitForExist({ timeout: 5000 });

      if (!nextButton2Exists) {
        throw new Error('Could not find second next button (TextView with text "下一步")');
      }

      await nextButton2.click();
      await appium.pause(2000);

      appium.log('Second next button clicked successfully');
    } else {
      appium.log('No image required, skipping camera button');
      // TODO: Submit the post without image
    }

    await appium.pause(3000);

    // Click publish button to submit the post
    appium.log('Clicking publish button...');
    const publishSubmit = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, '发布'));
    const publishSubmitExists = await publishSubmit.waitForExist({ timeout: 5000 });

    if (!publishSubmitExists) {
      throw new Error('Could not find publish button (TextView with text "发布")');
    }

    await publishSubmit.click();
    await appium.pause(3000);

    appium.log('Publish button clicked successfully');

    // Press HOME button to return to home screen
    appium.log('Pressing HOME button to return to home screen...');
    await appium.pressKeyCode(KEYCODE.HOME);
    await appium.pause(1000);

    return {
      success: true,
      message: 'Soul app opened successfully (publishing logic pending)',
      content: content
    };

  } catch (error) {
    appium.err(`Publish to Soul error: ${error.message}`);
    return {
      success: false,
      message: `Failed to publish to Soul: ${error.message}`
    };
  }
}

/**
 * Publish content to Momo
 * @param {Object} appium - Appium wrapper object
 * @param {string} content - The content to publish
 * @param {string} albumName - Optional album name to select image from
 * @param {number} imageIndex - Optional image index to select
 * @returns {Promise<Object>} Result object
 */
async function publishToMomo(appium, content, albumName = '', imageIndex = -1) {
  try {
    appium.log(`Publishing to Momo: "${content}"`);
    if (albumName && imageIndex >= 0) {
      appium.log(`With image from album: "${albumName}", index: ${imageIndex}`);
    }

    // Open Momo app using search
    appium.log('Opening Momo app...');
    const openResult = await openAppBySearch(appium, 'Momo');
    if (!openResult.success) {
      throw new Error(`Failed to open Momo: ${openResult.message}`);
    }
    await appium.pause(2000);

    // Check for popups after opening app
    await handlePopups(appium);

    // TODO: Click on the publish button (modify resource ID)
    appium.log('Clicking on publish button...');
    const publishButton = await appium.$(Selectors.byId('MOMO_PUBLISH_BUTTON_RESOURCE_ID'));
    const publishButtonExists = await publishButton.waitForExist({ timeout: 5000 });

    if (!publishButtonExists) {
      throw new Error('Could not find publish button');
    }

    await publishButton.click();
    await appium.pause(2000);

    // TODO: Enter content text (modify resource ID)
    appium.log('Entering content text...');
    const contentInput = await appium.$(Selectors.byId('MOMO_CONTENT_INPUT_RESOURCE_ID'));
    const contentInputExists = await contentInput.waitForExist({ timeout: 5000 });

    if (!contentInputExists) {
      throw new Error('Could not find content input field');
    }

    await contentInput.click();
    await appium.pause(500);
    await contentInput.setValue(content);
    await appium.pause(1000);

    appium.log('Content text entered successfully');

    // TODO: Click on camera/image button to add image (if needed)
    if (albumName && imageIndex >= 0) {
      appium.log('Clicking on camera button...');
      const cameraButton = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'MOMO_CAMERA_BUTTON_TEXT'));
      const cameraButtonExists = await cameraButton.waitForExist({ timeout: 5000 });

      if (!cameraButtonExists) {
        throw new Error('Could not find camera button');
      }

      await cameraButton.click();
      await appium.pause(2000);

      appium.log('Camera button clicked successfully');

      // TODO: Select first image in RecyclerView (modify resource ID)
      appium.log('Selecting first image...');
      const imageSelect = await appium.$(Selectors.byRecyclerViewChild('MOMO_IMAGE_SELECT_RESOURCE_ID', 0));
      const imageSelectExists = await imageSelect.waitForExist({ timeout: 5000 });

      if (!imageSelectExists) {
        throw new Error('Could not find image select element');
      }

      await imageSelect.click();
      await appium.pause(1000);

      appium.log('First image selected successfully');

      // TODO: Click next button (modify button text)
      appium.log('Clicking next button...');
      const nextButton = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'MOMO_NEXT_BUTTON_TEXT'));
      const nextButtonExists = await nextButton.waitForExist({ timeout: 5000 });

      if (!nextButtonExists) {
        throw new Error('Could not find next button');
      }

      await nextButton.click();
      await appium.pause(2000);

      appium.log('Next button clicked successfully');

      // TODO: Check if close button appears and click it (modify resource ID if needed)
      appium.log('Checking for close button...');
      try {
        const closeButton = await appium.$(Selectors.byId('MOMO_CLOSE_BUTTON_RESOURCE_ID'));
        const closeButtonExists = await closeButton.waitForExist({ timeout: 3000 });

        if (closeButtonExists) {
          appium.log('Close button found, clicking it...');
          await closeButton.click();
          await appium.pause(1000);
          appium.log('Close button clicked successfully');
        } else {
          appium.log('Close button not found, continuing...');
        }
      } catch (e) {
        appium.log('No close button appeared');
      }

      // TODO: Click next button again (if needed, modify button text)
      appium.log('Clicking next button again...');
      const nextButton2 = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'MOMO_NEXT_BUTTON_TEXT'));
      const nextButton2Exists = await nextButton2.waitForExist({ timeout: 5000 });

      if (!nextButton2Exists) {
        throw new Error('Could not find second next button');
      }

      await nextButton2.click();
      await appium.pause(2000);

      appium.log('Second next button clicked successfully');
    } else {
      appium.log('No image required, skipping camera button');
    }

    await appium.pause(3000);

    // TODO: Click publish/submit button to finalize (modify button text)
    appium.log('Clicking publish button...');
    const publishSubmit = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'MOMO_PUBLISH_BUTTON_TEXT'));
    const publishSubmitExists = await publishSubmit.waitForExist({ timeout: 5000 });

    if (!publishSubmitExists) {
      throw new Error('Could not find publish submit button');
    }

    await publishSubmit.click();
    await appium.pause(3000);

    appium.log('Publish button clicked successfully');

    // Press HOME button to return to home screen
    appium.log('Pressing HOME button to return to home screen...');
    await appium.pressKeyCode(KEYCODE.HOME);
    await appium.pause(1000);

    return {
      success: true,
      message: 'Momo content published successfully',
      content: content
    };

  } catch (error) {
    appium.err(`Publish to Momo error: ${error.message}`);
    return {
      success: false,
      message: `Failed to publish to Momo: ${error.message}`
    };
  }
}

/**
 * Publish content to Tantan
 * @param {Object} appium - Appium wrapper object
 * @param {string} content - The content to publish
 * @param {string} albumName - Optional album name to select image from
 * @param {number} imageIndex - Optional image index to select
 * @returns {Promise<Object>} Result object
 */
async function publishToTantan(appium, content, albumName = '', imageIndex = -1) {
  try {
    appium.log(`Publishing to Tantan: "${content}"`);
    if (albumName && imageIndex >= 0) {
      appium.log(`With image from album: "${albumName}", index: ${imageIndex}`);
    }

    // Open Tantan app using search
    appium.log('Opening Tantan app...');
    const openResult = await openAppBySearch(appium, 'Tantan');
    if (!openResult.success) {
      throw new Error(`Failed to open Tantan: ${openResult.message}`);
    }
    await appium.pause(2000);

    // Check for popups after opening app
    await handlePopups(appium);

    // TODO: Click on the publish button (modify resource ID)
    appium.log('Clicking on publish button...');
    const publishButton = await appium.$(Selectors.byId('TANTAN_PUBLISH_BUTTON_RESOURCE_ID'));
    const publishButtonExists = await publishButton.waitForExist({ timeout: 5000 });

    if (!publishButtonExists) {
      throw new Error('Could not find publish button');
    }

    await publishButton.click();
    await appium.pause(2000);

    // TODO: Enter content text (modify resource ID)
    appium.log('Entering content text...');
    const contentInput = await appium.$(Selectors.byId('TANTAN_CONTENT_INPUT_RESOURCE_ID'));
    const contentInputExists = await contentInput.waitForExist({ timeout: 5000 });

    if (!contentInputExists) {
      throw new Error('Could not find content input field');
    }

    await contentInput.click();
    await appium.pause(500);
    await contentInput.setValue(content);
    await appium.pause(1000);

    appium.log('Content text entered successfully');

    // TODO: Click on camera/image button to add image (if needed)
    if (albumName && imageIndex >= 0) {
      appium.log('Clicking on camera button...');
      const cameraButton = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'TANTAN_CAMERA_BUTTON_TEXT'));
      const cameraButtonExists = await cameraButton.waitForExist({ timeout: 5000 });

      if (!cameraButtonExists) {
        throw new Error('Could not find camera button');
      }

      await cameraButton.click();
      await appium.pause(2000);

      appium.log('Camera button clicked successfully');

      // TODO: Select first image in RecyclerView (modify resource ID)
      appium.log('Selecting first image...');
      const imageSelect = await appium.$(Selectors.byRecyclerViewChild('TANTAN_IMAGE_SELECT_RESOURCE_ID', 0));
      const imageSelectExists = await imageSelect.waitForExist({ timeout: 5000 });

      if (!imageSelectExists) {
        throw new Error('Could not find image select element');
      }

      await imageSelect.click();
      await appium.pause(1000);

      appium.log('First image selected successfully');

      // TODO: Click next button (modify button text)
      appium.log('Clicking next button...');
      const nextButton = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'TANTAN_NEXT_BUTTON_TEXT'));
      const nextButtonExists = await nextButton.waitForExist({ timeout: 5000 });

      if (!nextButtonExists) {
        throw new Error('Could not find next button');
      }

      await nextButton.click();
      await appium.pause(2000);

      appium.log('Next button clicked successfully');

      // TODO: Check if close button appears and click it (modify resource ID if needed)
      appium.log('Checking for close button...');
      try {
        const closeButton = await appium.$(Selectors.byId('TANTAN_CLOSE_BUTTON_RESOURCE_ID'));
        const closeButtonExists = await closeButton.waitForExist({ timeout: 3000 });

        if (closeButtonExists) {
          appium.log('Close button found, clicking it...');
          await closeButton.click();
          await appium.pause(1000);
          appium.log('Close button clicked successfully');
        } else {
          appium.log('Close button not found, continuing...');
        }
      } catch (e) {
        appium.log('No close button appeared');
      }

      // TODO: Click next button again (if needed, modify button text)
      appium.log('Clicking next button again...');
      const nextButton2 = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'TANTAN_NEXT_BUTTON_TEXT'));
      const nextButton2Exists = await nextButton2.waitForExist({ timeout: 5000 });

      if (!nextButton2Exists) {
        throw new Error('Could not find second next button');
      }

      await nextButton2.click();
      await appium.pause(2000);

      appium.log('Second next button clicked successfully');
    } else {
      appium.log('No image required, skipping camera button');
    }

    await appium.pause(3000);

    // TODO: Click publish/submit button to finalize (modify button text)
    appium.log('Clicking publish button...');
    const publishSubmit = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, 'TANTAN_PUBLISH_BUTTON_TEXT'));
    const publishSubmitExists = await publishSubmit.waitForExist({ timeout: 5000 });

    if (!publishSubmitExists) {
      throw new Error('Could not find publish submit button');
    }

    await publishSubmit.click();
    await appium.pause(3000);

    appium.log('Publish button clicked successfully');

    // Press HOME button to return to home screen
    appium.log('Pressing HOME button to return to home screen...');
    await appium.pressKeyCode(KEYCODE.HOME);
    await appium.pause(1000);

    return {
      success: true,
      message: 'Tantan content published successfully',
      content: content
    };

  } catch (error) {
    appium.err(`Publish to Tantan error: ${error.message}`);
    return {
      success: false,
      message: `Failed to publish to Tantan: ${error.message}`
    };
  }
}

/**
 * Open app by searching in launcher
 * @param {Object} appium - Appium wrapper object
 * @param {string} appName - The name of the app to search and open
 * @returns {Promise<Object>} Result object
 */
async function openAppBySearch(appium, appName) {
  try {
    appium.log(`Opening app by search: "${appName}"`);

    // Press HOME button to go to home screen
    appium.log('Pressing HOME button to go to home screen...');
    await appium.pressKeyCode(KEYCODE.HOME);
    await appium.pause(1000);

    // Wait for home screen to appear - check for "设定" TextView
    appium.log('Waiting for home screen (looking for "设定" TextView)...');
    const settingsElement = await appium.$(Selectors.byClassAndText(CLASS.TEXTVIEW, '设定'));
    const settingsExists = await settingsElement.waitForExist({ timeout: 5000 });

    if (!settingsExists) {
      throw new Error('Could not confirm home screen - "设定" TextView not found');
    }
    appium.log('Home screen confirmed - found "设定" TextView');

    // Swipe up to show app drawer
    appium.log('Swiping up to show app drawer...');
    const coords = await Helpers.getSwipeCoordinates(appium, SWIPE.UP, 0.6);
    const swipeAction = Actions.swipe(coords.startX, coords.startY, coords.endX, coords.endY);
    await appium.performActions([swipeAction]);
    await appium.releaseActions();
    await appium.pause(1000);

    // Click on search view and enter app name
    appium.log('Clicking on search view...');
    const searchView = await appium.$(Selectors.byId('com.android.launcher3:id/fallback_search_view'));
    const searchViewExists = await searchView.waitForExist({ timeout: 5000 });

    if (!searchViewExists) {
      throw new Error('Could not find search view (fallback_search_view)');
    }

    await searchView.click();
    await appium.pause(1000);

    // Enter app name directly into the search view
    appium.log(`Entering app name: "${appName}"...`);
    await searchView.setValue(appName);
    await appium.pause(1500);

    // Find the first TextView in RecyclerView
    appium.log('Looking for app in search results...');
    const recyclerView = await appium.$(Selectors.byClass(CLASS.RECYCLERVIEW));
    const recyclerViewExists = await recyclerView.waitForExist({ timeout: 5000 });

    if (!recyclerViewExists) {
      throw new Error('Could not find RecyclerView with search results');
    }

    // Find the first TextView inside RecyclerView
    const firstTextView = await appium.$(`android=new UiSelector().className("${CLASS.RECYCLERVIEW}").childSelector(new UiSelector().className("${CLASS.TEXTVIEW}").instance(0))`);
    const firstTextViewExists = await firstTextView.waitForExist({ timeout: 5000 });

    if (!firstTextViewExists) {
      throw new Error('Could not find app result in search results');
    }

    // Get the text to confirm
    const resultText = await firstTextView.getText();
    appium.log(`Found app result: "${resultText}"`);

    // Click on the first result
    appium.log('Clicking on first search result...');
    await firstTextView.click();
    await appium.pause(2000);

    appium.log(`Successfully opened app: "${appName}"`);

    return {
      success: true,
      message: `App "${appName}" opened successfully`,
      appResult: resultText
    };

  } catch (error) {
    appium.err(`Open app by search error: ${error.message}`);
    return {
      success: false,
      message: `Failed to open app "${appName}": ${error.message}`
    };
  }
}

module.exports = {
  name: 'Demo',
  config: {
    pin: '123456',
  },
  execute
};
