// 'use client': This directive tells Next.js this is a CLIENT COMPONENT
// - Runs in the browser (can use useState, onClick, browser APIs)
// - Without this, components are SERVER COMPONENTS by default (run on server only)
// - Use 'use client' when you need: state, effects, event handlers, browser APIs
'use client'

// Import React hooks - only works in Client Components
import { useState, useEffect } from 'react'

// Link: Next.js component for navigation between pages
// - Uses client-side navigation (no full page reload)
// - Prefetches linked pages for faster navigation
// - Example: <Link href="/debug">Go to Debug</Link>
import Link from 'next/link'

// HomePage: This is a PAGE component
// - File location: app/page.jsx → Route: "/"
// - This is the home page of your app
// - Default export is required for page components
export default function HomePage() {
  // useState: React hook for managing component state
  // - Only works in Client Components (needs 'use client')
  // - Re-renders component when state changes
  const [sessionActive, setSessionActive] = useState(false)
  const [status, setStatus] = useState('Disconnected')
  const [logs, setLogs] = useState([])
  const [results, setResults] = useState([])
  const [selectedScript, setSelectedScript] = useState('')
  const [scripts, setScripts] = useState([])
  const [screenshot, setScreenshot] = useState('')
  const [isExecutingScript, setIsExecutingScript] = useState(false)

  const addLog = (message) => {
    setLogs(prev => [...prev, `[${new Date().toLocaleTimeString()}] ${message}`])
  }

  const addResult = (command, status, data = null, error = null) => {
    const result = {
      timestamp: new Date().toLocaleTimeString(),
      command,
      status,
      data,
      error,
      type: status === 'error' ? 'error' : 'success',
    }
    setResults(prev => [result, ...prev])
  }

  // Helper function to add timeout to fetch requests
  const fetchWithTimeout = async (url, options = {}, timeout = 30000) => {
    const controller = new AbortController()
    const id = setTimeout(() => controller.abort(), timeout)

    try {
      const response = await fetch(url, {
        ...options,
        signal: controller.signal
      })
      clearTimeout(id)
      return response
    } catch (error) {
      clearTimeout(id)
      if (error.name === 'AbortError') {
        throw new Error('Request timeout - server may be unreachable')
      }
      throw error
    }
  }

  // handleStartSession: Event handler for button click
  // - async: allows using 'await' for asynchronous operations
  const handleStartSession = async () => {
    try {
      setStatus('Connecting...')
      addLog('Starting Appium session...')
      addResult('Start Session', 'pending')

      const res = await fetchWithTimeout('/api/session', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'start' })
      }, 60000) // 60 second timeout for session start

      const data = await res.json()

      if (data.success) {
        setSessionActive(true)
        setStatus('Connected')
        addLog('Session started successfully')
        addResult('Start Session', 'success', 'Appium session started')
      } else {
        throw new Error(data.error || 'Failed to start session')
      }
    } catch (error) {
      setStatus('Error')
      addLog(`Error: ${error.message}`)
      addResult('Start Session', 'error', null, error.message)
    }
  }

  const handleStopSession = async () => {
    try {
      setStatus('Disconnecting...')
      addLog('Stopping Appium session...')
      addResult('Stop Session', 'pending')

      const res = await fetch('/api/session', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'stop' })
      })
      const data = await res.json()

      if (data.success) {
        setSessionActive(false)
        setStatus('Disconnected')
        addLog('Session stopped successfully')
        addResult('Stop Session', 'success', 'Appium session stopped')
      } else {
        throw new Error(data.error || 'Failed to stop session')
      }
    } catch (error) {
      addLog(`Error: ${error.message}`)
      addResult('Stop Session', 'error', null, error.message)
    }
  }

  const handleExecuteScript = async () => {
    if (!selectedScript) return

    try {
      setIsExecutingScript(true)
      addLog(`Executing script: ${selectedScript}`)
      addResult(selectedScript, 'pending')

      const res = await fetchWithTimeout('/api/scripts', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ scriptName: selectedScript })
      }, 1800000) // 30 minute timeout for script execution

      const data = await res.json()

      if (data.success) {
        addLog(`Script completed: ${selectedScript}`)
        addResult(selectedScript, 'success', data.result)
        // If script returned screenshot data, update the screenshot
        if (data.result?.data) {
          setScreenshot('data:image/png;base64,' + data.result.data)
        }
      } else {
        throw new Error(data.error || 'Script execution failed')
      }
    } catch (error) {
      addLog(`Error: ${error.message}`)
      addResult(selectedScript, 'error', null, error.message)
    } finally {
      setIsExecutingScript(false)
      // Fetch final logs after script completion
      await fetchLogs()
    }
  }

  const fetchLogs = async () => {
    try {
      const res = await fetch('/api/scripts', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' }
      })
      const data = await res.json()

      if (data.success && data.logs && data.logs.length > 0) {
        // Append new logs to the existing logs
        setLogs(prev => [...prev, ...data.logs.map(log => `[${new Date().toLocaleTimeString()}] ${log}`)])
      }
    } catch (error) {
      console.error('Failed to fetch logs:', error)
    }
  }

  const loadScripts = async () => {
    try {
      const res = await fetch('/api/scripts')
      const data = await res.json()

      if (data.success) {
        setScripts(data.scripts)
        addLog(`Loaded ${data.scripts.length} scripts`)
      }
    } catch (error) {
      console.error('Failed to load scripts:', error)
      addLog(`Failed to load scripts: ${error.message}`)
    }
  }


  // Update screenshot and check session status
  const updateScreenshot = async () => {
    try {
      // Use timestamp to prevent caching
      const timestamp = Date.now()
      const response = await fetch(`/api/screenshot?t=${timestamp}`)

      if (response.status === 200) {
        // 200: Success - get image and update
        const blob = await response.blob()
        const imageUrl = URL.createObjectURL(blob)

        // Revoke old URL to prevent memory leaks
        if (screenshot && screenshot.startsWith('blob:')) {
          URL.revokeObjectURL(screenshot)
        }

        setScreenshot(imageUrl)

        // Update status if needed
        if (status !== 'Connected') {
          setStatus('Connected')
        }
      } else if (response.status === 204) {
        // 204: Session connected but screenshot failed
        if (status !== 'Connected') {
          setStatus('Connected')
        }
        console.warn('Screenshot capture failed but session is connected')
      } else if (response.status === 503) {
        // 503: Session disconnected
        if (sessionActive) {
          setSessionActive(false)
          setStatus('Disconnected')
          addLog('Session disconnected (detected by screenshot API)')

          // Clear screenshot and revoke URL
          if (screenshot && screenshot.startsWith('blob:')) {
            URL.revokeObjectURL(screenshot)
          }
          setScreenshot('')
        }
      }
    } catch (error) {
      // Network error or other issues
      console.error('Screenshot update failed:', error)
    }
  }

  // Set up periodic screenshot updates (also checks session status)
  useEffect(() => {
    if (sessionActive) {
      // Take initial screenshot immediately
      updateScreenshot()

      // Update screenshot every 10 seconds
      const intervalId = setInterval(updateScreenshot, 10000)

      // Cleanup interval and revoke blob URL on unmount
      return () => {
        clearInterval(intervalId)
        if (screenshot && screenshot.startsWith('blob:')) {
          URL.revokeObjectURL(screenshot)
        }
      }
    } else {
      // Clear screenshot and revoke URL when disconnected
      if (screenshot && screenshot.startsWith('blob:')) {
        URL.revokeObjectURL(screenshot)
      }
      setScreenshot('')
    }
  }, [sessionActive])

  // Load scripts on component mount
  useEffect(() => {
    loadScripts()
  }, [])

  // Poll logs every second when executing script
  useEffect(() => {
    if (isExecutingScript) {
      // Fetch logs immediately
      fetchLogs()

      // Set up interval to fetch logs every second
      const logInterval = setInterval(fetchLogs, 1000)

      // Cleanup interval when script execution stops
      return () => {
        clearInterval(logInterval)
      }
    }
  }, [isExecutingScript])

  return (
    <div className="flex h-screen bg-gray-900 text-white">
      {/* Left Panel - Screenshot Display */}
      <div className="w-1/2 flex flex-col border-r border-gray-700 overflow-hidden">
        <div className="flex-1 relative overflow-auto bg-gray-800 flex items-center justify-center">
          {screenshot ? (
            <div className="relative">
              <img
                src={screenshot}
                alt="Device Screenshot"
                className="h-screen w-auto shadow-2xl"
              />
            </div>
          ) : (
            <div className="text-gray-500 text-center">
              <div className="text-6xl mb-4">📱</div>
              <div className="text-lg mb-2">No Screenshot</div>
              <div className="text-sm">Take a screenshot to display device screen</div>
            </div>
          )}
        </div>
      </div>

      {/* Right Panel - Control Interface */}
      <div className="w-1/2 flex flex-col overflow-hidden">
        {/* Header Bar */}
        <div className="bg-gray-800 p-4 border-b border-gray-700 flex-shrink-0">
          <div className="flex justify-between items-center">
            <div>
              <h1 className="text-2xl font-bold bg-gradient-to-r from-blue-400 to-purple-500 bg-clip-text text-transparent">
                Appium Control
              </h1>
            </div>
            <div className="flex items-center space-x-3">
              <div className="flex items-center space-x-2">
                <div className={`w-3 h-3 rounded-full ${
                  status === 'Connected' ? 'bg-green-500 animate-pulse' :
                  status === 'Disconnected' ? 'bg-red-500' :
                  'bg-yellow-500 animate-pulse'
                }`}></div>
                <span className="text-sm font-medium">{status}</span>
              </div>
              <Link
                href="/debug"
                className="px-3 py-2 bg-purple-600 hover:bg-purple-700 rounded-lg transition-colors text-sm"
              >
                🐛 Debug
              </Link>
            </div>
          </div>
        </div>

        {/* Scrollable Control Panels */}
        <div className="flex-1 overflow-y-auto p-4 space-y-4">
          {/* Session Controls */}
          <div className="bg-gray-800 rounded-lg shadow-xl p-5 border border-gray-700">
            <h2 className="text-lg font-semibold mb-3 flex items-center space-x-2">
              <span className="text-xl">🎮</span>
              <span>Session Control</span>
            </h2>
            <div className="flex space-x-3">
              <button
                onClick={handleStartSession}
                disabled={sessionActive}
                className={`flex-1 py-2.5 rounded-lg font-medium transition-all ${
                  sessionActive
                    ? 'bg-gray-700 text-gray-500 cursor-not-allowed'
                    : 'bg-green-600 hover:bg-green-700'
                }`}
              >
                Start Session
              </button>
              <button
                onClick={handleStopSession}
                disabled={!sessionActive}
                className={`flex-1 py-2.5 rounded-lg font-medium transition-all ${
                  !sessionActive
                    ? 'bg-gray-700 text-gray-500 cursor-not-allowed'
                    : 'bg-red-600 hover:bg-red-700'
                }`}
              >
                Stop Session
              </button>
            </div>
          </div>


          {/* Script Section */}
          <div className="bg-gray-800 rounded-lg shadow-xl p-5 border border-gray-700">
            <h2 className="text-lg font-semibold mb-3 flex items-center space-x-2">
              <span className="text-xl">⚡</span>
              <span>Execute Script</span>
            </h2>
            <div className="space-y-3">
              <select
                value={selectedScript}
                onChange={(e) => setSelectedScript(e.target.value)}
                onClick={loadScripts}
                disabled={!sessionActive}
                className="w-full px-4 py-2.5 bg-gray-700 border border-gray-600 rounded-lg focus:outline-none focus:ring-2 focus:ring-blue-500 disabled:opacity-50"
              >
                <option value="">Select a script...</option>
                {scripts.map((script) => (
                  <option key={script.id} value={script.id}>
                    {script.name}
                  </option>
                ))}
              </select>
              <button
                onClick={handleExecuteScript}
                disabled={!sessionActive || !selectedScript}
                className="w-full py-2.5 bg-blue-600 hover:bg-blue-700 rounded-lg font-medium transition-all disabled:opacity-50 disabled:cursor-not-allowed"
              >
                Execute Script
              </button>
            </div>
          </div>


          {/* Results Panel */}
          <div className="bg-gray-800 rounded-lg shadow-xl p-5 border border-gray-700">
            <h2 className="text-lg font-semibold mb-3 flex items-center space-x-2">
              <span className="text-xl">📊</span>
              <span>Results</span>
            </h2>
            <div className="space-y-2 max-h-60 overflow-y-auto">
              {results.length === 0 ? (
                <p className="text-gray-500 text-center py-6 text-sm">No results yet</p>
              ) : (
                results.map((result, index) => (
                  <div
                    key={index}
                    className={`p-3 rounded-lg border text-sm ${
                      result.type === 'error'
                        ? 'bg-red-900/30 border-red-700'
                        : result.status === 'pending'
                        ? 'bg-yellow-900/30 border-yellow-700'
                        : 'bg-green-900/30 border-green-700'
                    }`}
                  >
                    <div className="flex justify-between items-start mb-1">
                      <span className="font-medium">{result.command}</span>
                      <span className="text-xs text-gray-400">{result.timestamp}</span>
                    </div>
                    {result.data && (
                      <div className="text-xs text-gray-300 mt-1 font-mono break-words whitespace-pre-wrap">
                        {typeof result.data === 'string' ? result.data : JSON.stringify(result.data, null, 2)}
                      </div>
                    )}
                    {result.error && (
                      <div className="text-xs text-red-400 mt-1 break-words whitespace-pre-wrap">{result.error}</div>
                    )}
                  </div>
                ))
              )}
            </div>
          </div>

          {/* Logs Panel */}
          <div className="bg-gray-800 rounded-lg shadow-xl p-5 border border-gray-700">
            <h2 className="text-lg font-semibold mb-3 flex items-center space-x-2">
              <span className="text-xl">📝</span>
              <span>Logs</span>
            </h2>
            <div className="bg-gray-900 rounded-lg p-3 max-h-60 overflow-y-auto font-mono text-xs">
              {logs.length === 0 ? (
                <p className="text-gray-500 text-center py-6">No logs yet</p>
              ) : (
                logs.map((log, index) => (
                  <div key={index} className="text-gray-300 py-0.5">
                    {log}
                  </div>
                ))
              )}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
