'use client'

import { useState, useRef, useEffect } from 'react'
import Link from 'next/link'

// Client-side Utility Functions
function getHighlightStyle(selectedNode, imgElement) {
  if (!selectedNode || !selectedNode.boundsObj || !imgElement) {
    return {}
  }

  const bounds = selectedNode.boundsObj

  // Calculate scale factors
  const scaleX = imgElement.width / imgElement.naturalWidth
  const scaleY = imgElement.height / imgElement.naturalHeight

  return {
    position: 'absolute',
    left: bounds.left * scaleX + 'px',
    top: bounds.top * scaleY + 'px',
    width: bounds.width * scaleX + 'px',
    height: bounds.height * scaleY + 'px',
    border: '2px solid #00ff00',
    background: 'rgba(0, 255, 0, 0.1)',
    pointerEvents: 'none',
    boxShadow: '0 0 10px rgba(0, 255, 0, 0.5)',
  }
}

function formatNodeLabel(node) {
  const className = node.class ? node.class.split('.').pop() : 'Unknown'
  const text = node.text || ''
  const resId = node['resource-id'] ? node['resource-id'].split('/').pop() : ''

  let label = className
  if (resId) label += ` #${resId}`
  if (text) label += ` "${text.substring(0, 30)}${text.length > 30 ? '...' : ''}"`

  return label
}

function calculateClickCoordinates(event, imgElement) {
  const rect = imgElement.getBoundingClientRect()
  const x = Math.round(
    (event.clientX - rect.left) * (imgElement.naturalWidth / imgElement.width)
  )
  const y = Math.round(
    (event.clientY - rect.top) * (imgElement.naturalHeight / imgElement.height)
  )
  return { x, y }
}

function getNodeDetails(selectedNode) {
  if (!selectedNode) return []

  return [
    ['Class', selectedNode.class],
    ['Resource ID', selectedNode['resource-id']],
    ['Text', selectedNode.text],
    ['Content Desc', selectedNode['content-desc']],
    ['Package', selectedNode.package],
    ['Bounds', selectedNode.bounds],
    ['Clickable', selectedNode.clickable],
    ['Enabled', selectedNode.enabled],
    ['Scrollable', selectedNode.scrollable],
  ].filter(([, value]) => value)
}

export default function DebugPage() {
  const [nodes, setNodes] = useState([])
  const [selectedNode, setSelectedNode] = useState(null)
  const [screenshot, setScreenshot] = useState('')
  const [loading, setLoading] = useState(false)
  const [status, setStatus] = useState('')
  const [clickMode, setClickMode] = useState(false)
  const [currentActivity, setCurrentActivity] = useState('')
  const [sessionConnected, setSessionConnected] = useState(false)

  const imgRef = useRef(null)

  const showStatus = (msg) => {
    setStatus(msg)
    setTimeout(() => setStatus(''), 3000)
  }

  const handleConnect = async () => {
    try {
      setLoading(true)
      showStatus('Connecting to device...')

      const res = await fetch('/api/debug?action=connect', {
        method: 'POST'
      })
      const data = await res.json()

      if (data.success) {
        setSessionConnected(true)
        if (data.reused) {
          showStatus('Already connected, reusing existing session')
        } else {
          showStatus('Session connected successfully')
        }
      } else {
        throw new Error(data.error || 'Connection failed')
      }
    } catch (error) {
      console.error('Connection error:', error)
      showStatus('Connection failed: ' + error.message)
      setSessionConnected(false)
    } finally {
      setLoading(false)
    }
  }

  const handleDisconnect = async () => {
    try {
      setLoading(true)
      showStatus('Disconnecting...')

      const res = await fetch('/api/debug?action=disconnect', {
        method: 'POST'
      })
      const data = await res.json()

      if (data.success) {
        setSessionConnected(false)
        setScreenshot('')
        setNodes([])
        setSelectedNode(null)
        setCurrentActivity('')
        showStatus('Session disconnected')
      } else {
        throw new Error(data.error || 'Disconnect failed')
      }
    } catch (error) {
      console.error('Disconnect error:', error)
      showStatus('Disconnect failed: ' + error.message)
    } finally {
      setLoading(false)
    }
  }

  const refresh = async () => {
    if (!sessionConnected) {
      showStatus('Please connect first')
      return
    }

    try {
      setLoading(true)
      showStatus('Refreshing UI information...')

      // Fetch all data in parallel
      const [screenshotRes, dumpRes, activityRes] = await Promise.all([
        fetch('/api/debug?action=screenshot'),
        fetch('/api/debug?action=dump'),
        fetch('/api/debug?action=activity')
      ])

      const screenshotData = await screenshotRes.json()
      console.log('Screenshot response:', screenshotData)
      if (screenshotData.success) {
        setScreenshot('data:image/png;base64,' + screenshotData.image)
      } else {
        showStatus('Screenshot failed: ' + (screenshotData.error || 'Unknown error'))
        setLoading(false)
        return
      }

      const dumpData = await dumpRes.json()
      console.log('Dump response: success =', dumpData.success, ', nodes =', dumpData.data ? dumpData.data.length : 0)
      if (dumpData.xml) {
        const xmlLen = dumpData.xml.length
        console.log('Dump XML preview:', dumpData.xml.substring(0, 200) + `... (total ${xmlLen} chars)`)
      }

      if (dumpData.success) {
        if (dumpData.data && dumpData.data.length > 0) {
          setNodes(dumpData.data)
          console.log('Nodes updated:', dumpData.data.length, 'nodes')
        } else {
          console.warn('Dump returned empty data array')
          showStatus('Warning: UI dump returned no nodes')
          setNodes([])
        }
      } else {
        showStatus('Dump failed: ' + (dumpData.error || 'Unknown error'))
      }

      const activityData = await activityRes.json()
      console.log('Activity response:', activityData)
      if (activityData.success) {
        setCurrentActivity(activityData.fullActivity)
      }

      showStatus('UI information updated')
    } catch (error) {
      console.error('Refresh error:', error)
      showStatus('Error: ' + error.message)
    } finally {
      setLoading(false)
    }
  }

  const handleImageClick = async (e) => {
    if (!clickMode || !imgRef.current || !sessionConnected) return

    const { x, y } = calculateClickCoordinates(e, imgRef.current)

    try {
      setLoading(true)
      const res = await fetch('/api/debug?action=click', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ x, y })
      })
      if (res.ok) {
        showStatus(`Clicked at (${x}, ${y})`)
        setTimeout(refresh, 500)
      }
    } catch (error) {
      showStatus('Click failed: ' + error.message)
    } finally {
      setLoading(false)
    }
  }


  return (
    <div className="flex h-screen bg-gray-900 text-gray-200">
      {/* Left Panel - Screenshot Only */}
      <div className="w-1/2 flex flex-col border-r border-gray-700 overflow-hidden">
        <div className="flex-1 relative overflow-auto bg-gray-800 flex items-center justify-center">
          {screenshot ? (
            <div className="relative">
              <img
                ref={imgRef}
                src={screenshot}
                alt="Screenshot"
                className={`h-screen w-auto shadow-2xl ${clickMode ? 'cursor-crosshair' : ''}`}
                onClick={handleImageClick}
              />
              {selectedNode && (
                <div style={getHighlightStyle(selectedNode, imgRef.current)} />
              )}
            </div>
          ) : (
            <div className="text-gray-500 text-center absolute top-1/2 left-1/2 transform -translate-x-1/2 -translate-y-1/2">
              <div className="text-6xl mb-4">📱</div>
              <div>Click "Refresh" to get screenshot</div>
            </div>
          )}
        </div>
      </div>

      {/* Right Panel */}
      <div className="w-1/2 flex flex-col">
        {/* Toolbar */}
        <div className="bg-gray-800 p-4 border-b border-gray-700 flex gap-2 flex-wrap items-center">
          <Link
            href="/"
            className="px-4 py-2 bg-gray-700 hover:bg-gray-600 rounded-lg transition-colors"
          >
            ← Back
          </Link>
          {!sessionConnected ? (
            <button
              onClick={handleConnect}
              className="px-4 py-2 bg-green-600 hover:bg-green-700 rounded-lg transition-colors"
              disabled={loading}
            >
              🔌 Connect
            </button>
          ) : (
            <button
              onClick={handleDisconnect}
              className="px-4 py-2 bg-red-600 hover:bg-red-700 rounded-lg transition-colors"
              disabled={loading}
            >
              🔌 Disconnect
            </button>
          )}
          <button
            onClick={refresh}
            className="px-4 py-2 bg-blue-600 hover:bg-blue-700 rounded-lg transition-colors"
            disabled={loading || !sessionConnected}
          >
            🔄 Refresh
          </button>
          <button
            onClick={() => setClickMode(!clickMode)}
            disabled={!sessionConnected}
            className={`px-4 py-2 rounded-lg transition-colors ${
              clickMode ? 'bg-green-500 text-black' : 'bg-gray-700 hover:bg-gray-600'
            } ${!sessionConnected ? 'opacity-50 cursor-not-allowed' : ''}`}
          >
            👆 Click Mode
          </button>
        </div>

        {/* Current Activity */}
        {currentActivity && (
          <div className="bg-gray-800 px-4 py-3 border-b border-gray-700">
            <div className="text-sm text-gray-400">Current Activity</div>
            <div className="text-blue-300 font-mono text-sm mt-1">{currentActivity}</div>
          </div>
        )}
        {/* UI Hierarchy Tree */}
        <div className="flex-1 flex flex-col overflow-hidden border-b border-gray-700">
          <h3 className="text-teal-400 font-semibold p-4 border-b-2 border-gray-700 flex-shrink-0">
            UI Hierarchy Tree
          </h3>
          <div className="flex-1 overflow-y-auto overflow-x-hidden p-4 bg-gray-900">
            {nodes.length === 0 ? (
              <div className="text-gray-500 text-center py-12">
                Click "Refresh" to get UI information
              </div>
            ) : (
              nodes.map((node, index) => (
                <div
                  key={index}
                  className={`px-3 py-1 my-0.5 cursor-pointer rounded font-mono text-sm ${
                    node.clickable === 'true' ? 'text-teal-400' : ''
                  } ${
                    selectedNode === node ? 'bg-blue-600 text-white' : 'hover:bg-gray-800'
                  }`}
                  onClick={() => setSelectedNode(node)}
                >
                  {formatNodeLabel(node)}
                </div>
              ))
            )}
          </div>
        </div>

        {/* Element Details */}
        <div className="flex-1 flex flex-col overflow-hidden">
          <h3 className="text-teal-400 font-semibold p-4 border-b-2 border-gray-700 flex-shrink-0">
            Element Details
          </h3>
          <div className="flex-1 overflow-y-auto overflow-x-hidden p-4 bg-gray-800">
            {!selectedNode ? (
              <div className="text-gray-500 text-center py-8">Select an element</div>
            ) : (
              getNodeDetails(selectedNode).map(([label, value]) => (
                <div key={label} className="flex py-2 border-b border-gray-700">
                  <div className="w-36 text-blue-300 font-semibold flex-shrink-0">{label}</div>
                  <div className="flex-1 text-orange-300 break-all">{value}</div>
                </div>
              ))
            )}
          </div>
        </div>
      </div>

      {/* Loading Overlay */}
      {loading && (
        <div className="fixed top-1/2 left-1/2 transform -translate-x-1/2 -translate-y-1/2 bg-black bg-opacity-80 px-10 py-5 rounded-lg z-50">
          ⏳ Loading...
        </div>
      )}

      {/* Status Bar */}
      {status && (
        <div className="fixed bottom-0 left-0 right-0 p-3 text-center bg-blue-600 text-white">
          {status}
        </div>
      )}
    </div>
  )
}
