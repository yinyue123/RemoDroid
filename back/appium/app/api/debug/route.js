// NextResponse: Next.js utility for creating HTTP responses
import { NextResponse } from 'next/server'
import { handleDebugAPI } from './debug'

// API ROUTE HANDLER for GET requests
// - File location: app/api/debug/route.js → Route: /api/debug
// - This file must be named "route.js" (required by Next.js)
// - ROUTING RULE for API routes:
//   - app/api/debug/route.js → /api/debug
//   - app/api/appium/session/route.js → /api/appium/session
// - Export named functions: GET, POST, PUT, DELETE, PATCH
// - These run on the SERVER only (never sent to browser)
export async function GET(request) {
  try {
    // Extract query parameters from URL
    // Example: /api/debug?action=screenshot
    // searchParams.get('action') returns "screenshot"
    const { searchParams } = new URL(request.url)
    const action = searchParams.get('action')
    const query = Object.fromEntries(searchParams.entries())

    const result = await handleDebugAPI('GET', action, query, {})

    // NextResponse.json(): Return JSON response
    // - Automatically sets Content-Type: application/json
    // - Can set status code with { status: 400 }
    if (result.success) {
      return NextResponse.json(result)
    } else {
      return NextResponse.json(result, { status: 400 })
    }
  } catch (error) {
    return NextResponse.json(
      { success: false, error: error.message || 'Internal server error' },
      { status: 500 }
    )
  }
}

// API ROUTE HANDLER for POST requests
// - Same route (/api/debug) but handles POST method
export async function POST(request) {
  try {
    const { searchParams } = new URL(request.url)
    const action = searchParams.get('action')
    const query = Object.fromEntries(searchParams.entries())

    // request.json(): Parse JSON body from POST request
    // Example: fetch('/api/debug', { method: 'POST', body: JSON.stringify({x: 10}) })
    // Handle empty body gracefully
    let body = {}
    try {
      const text = await request.text()
      if (text) {
        body = JSON.parse(text)
      }
    } catch (e) {
      // Body is empty or not JSON, use empty object
      body = {}
    }

    const result = await handleDebugAPI('POST', action, query, body)

    if (result.success) {
      return NextResponse.json(result)
    } else {
      return NextResponse.json(result, { status: 400 })
    }
  } catch (error) {
    return NextResponse.json(
      { success: false, error: error.message || 'Internal server error' },
      { status: 500 }
    )
  }
}
