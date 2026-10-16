import { NextResponse } from 'next/server'
import { getDriver } from '../../src/appium'

// API Route: /api/screenshot
// Returns screenshot as PNG image (not base64)
// Status codes:
// - 200: Success, returns PNG image
// - 204: Session connected but screenshot failed
// - 503: Session disconnected

export async function GET(request) {
  try {
    const driver = getDriver()

    // 503: Session disconnected
    if (!driver) {
      return new NextResponse(null, {
        status: 503,
        statusText: 'Service Unavailable - No active session'
      })
    }

    // Try to take screenshot
    try {
      const screenshotBase64 = await driver.takeScreenshot()
      const imageBuffer = Buffer.from(screenshotBase64, 'base64')

      // 200: Success, return image
      return new NextResponse(imageBuffer, {
        status: 200,
        headers: {
          'Content-Type': 'image/png',
          'Cache-Control': 'no-store, no-cache, must-revalidate',
        },
      })
    } catch (screenshotError) {
      // 204: Session exists but screenshot failed
      console.error('Screenshot capture failed:', screenshotError.message)
      return new NextResponse(null, {
        status: 204,
        statusText: 'No Content - Screenshot failed'
      })
    }
  } catch (error) {
    // 503: Unexpected error
    console.error('Screenshot API error:', error)
    return new NextResponse(null, {
      status: 503,
      statusText: 'Service Unavailable'
    })
  }
}
