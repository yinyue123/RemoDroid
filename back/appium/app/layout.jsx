// Import global CSS styles - this will apply to ALL pages in your app
import './globals.css'

// metadata: Next.js uses this to set <title> and <meta> tags in the HTML <head>
// This is a Server Component feature - only works in layout.js, not in 'use client' components
export const metadata = {
  title: 'Appium Control Interface',
  description: 'Appium device control and debugging interface',
}

// RootLayout: This is the ROOT LAYOUT for your entire app
// - It wraps ALL pages (the {children} prop contains your page content)
// - Must include <html> and <body> tags (required by Next.js)
// - This component runs on the SERVER by default (Server Component)
// - Gets rendered once and reused for all routes
export default function RootLayout({ children }) {
  return (
    <html lang="en">
      <body className="antialiased">
        {/* {children} is where your page content gets injected */}
        {/* For route "/", children = content from app/page.jsx */}
        {/* For route "/debug", children = content from app/debug/page.jsx */}
        {children}
      </body>
    </html>
  )
}
