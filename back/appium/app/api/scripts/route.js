import { NextResponse } from 'next/server'
import fs from 'fs'
import path from 'path'
import { createAppiumWrapper, getLogs } from '../../src/appium'

const SCRIPTS_DIR = path.join(process.cwd(), 'app', 'scripts')

// Helper function to load CommonJS module from file
function loadCommonJSModule(filePath) {
  const code = fs.readFileSync(filePath, 'utf-8')
  const module = { exports: {} }
  const exports = module.exports

  // Create a function wrapper to execute the module code
  const wrappedCode = `(function(module, exports, require) {
    ${code}
    return module.exports;
  })`

  // Create a custom require function for the module
  const customRequire = (modulePath) => {
    if (modulePath.startsWith('.')) {
      // Resolve relative paths
      const resolvedPath = path.resolve(path.dirname(filePath), modulePath)
      if (fs.existsSync(resolvedPath + '.js')) {
        return loadCommonJSModule(resolvedPath + '.js')
      } else if (fs.existsSync(resolvedPath)) {
        return loadCommonJSModule(resolvedPath)
      }
    }
    // For node_modules, use dynamic import or throw
    throw new Error(`Cannot require '${modulePath}' from scripts`)
  }

  // Execute the wrapped code
  const fn = eval(wrappedCode)
  return fn(module, exports, customRequire)
}

/**
 * GET - List all available scripts
 */
export async function GET(request) {
  try {
    // Read all .js files from scripts directory
    const files = fs.readdirSync(SCRIPTS_DIR)

    const scripts = files
      .filter(file => file.endsWith('.js') && file !== 'README.md')
      .map(file => {
        const filePath = path.join(SCRIPTS_DIR, file)

        try {
          // Load CommonJS module
          const script = loadCommonJSModule(filePath)

          return {
            id: file.replace('.js', ''), // File name without extension
            name: script.name || file.replace('.js', ''), // Display name from export
            file,
            config: script.config || {} // Config object from export
          }
        } catch (error) {
          console.error(`Error loading script ${file}:`, error.message)
          // Return basic info if script fails to load
          return {
            id: file.replace('.js', ''),
            name: file.replace('.js', ''),
            file,
            config: {},
            error: error.message
          }
        }
      })

    return NextResponse.json({
      success: true,
      scripts
    })
  } catch (error) {
    console.error('GET /api/scripts error:', error)
    return NextResponse.json(
      { success: false, error: error.message },
      { status: 500 }
    )
  }
}

/**
 * POST - Get logs
 */
export async function POST(request) {
  try {
    const logs = getLogs()

    return NextResponse.json({
      success: true,
      logs
    })
  } catch (error) {
    console.error('Get logs error:', error)
    return NextResponse.json(
      { success: false, error: error.message },
      { status: 500 }
    )
  }
}

/**
 * PUT - Execute a script with hot reload
 */
export async function PUT(request) {
  try {
    const { scriptName } = await request.json()

    if (!scriptName) {
      return NextResponse.json(
        { success: false, error: 'Script name is required' },
        { status: 400 }
      )
    }

    const scriptPath = path.join(SCRIPTS_DIR, `${scriptName}.js`)

    // Check if script exists
    if (!fs.existsSync(scriptPath)) {
      return NextResponse.json(
        { success: false, error: `Script '${scriptName}' not found` },
        { status: 404 }
      )
    }

    // Load CommonJS module (hot reload is automatic since we read from disk each time)
    const script = loadCommonJSModule(scriptPath)

    if (typeof script.execute !== 'function') {
      return NextResponse.json(
        { success: false, error: 'Script must export an execute function' },
        { status: 400 }
      )
    }

    // Create Appium wrapper for the script
    const appiumWrapper = createAppiumWrapper()

    // Execute the script with the wrapper
    const result = await script.execute(appiumWrapper)

    return NextResponse.json({
      success: true,
      scriptName,
      result
    })
  } catch (error) {
    console.error('Script execution error:', error)
    return NextResponse.json(
      { success: false, error: error.message || 'Script execution failed' },
      { status: 500 }
    )
  }
}
