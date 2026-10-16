# Next.js Learning Guide - Your Project Structure Explained

## 🎯 Core Concepts

### 1. **File-Based Routing** (Most Important!)
Next.js automatically creates routes based on your file structure:

```
app/page.jsx              → URL: /
app/debug/page.jsx        → URL: /debug
app/about/contact/page.jsx → URL: /about/contact
```

**Rule**: Folder names = URL paths, but only `page.jsx` files become actual pages.

---

### 2. **Two Types of Components**

#### **Server Components** (default)
- Run on the SERVER only
- Can access databases, file system directly
- Cannot use `useState`, `onClick`, or browser APIs
- Example: `app/layout.jsx`

```jsx
// This runs on the server
export default function Layout({ children }) {
  return <html><body>{children}</body></html>
}
```

#### **Client Components** (add `'use client'`)
- Run in the BROWSER
- Can use `useState`, event handlers, browser APIs
- Example: `app/page.jsx`

```jsx
'use client'  // ← This makes it a Client Component

import { useState } from 'react'

export default function Page() {
  const [count, setCount] = useState(0)  // ✅ Works!
  return <button onClick={() => setCount(count + 1)}>Click</button>
}
```

---

### 3. **Special Files in Next.js**

| File | Purpose | Route |
|------|---------|-------|
| `page.jsx` | A page component | Creates a route |
| `layout.jsx` | Wraps pages (like a template) | No route, just wraps children |
| `route.js` | API endpoint | Creates API route |
| `loading.jsx` | Loading UI | Shows while page loads |
| `error.jsx` | Error boundary | Shows when page errors |

---

## 📂 Your Project Structure Explained

```
app/
├── layout.jsx              # Root layout - wraps ALL pages
├── page.jsx                # Home page (/)
├── globals.css             # Global styles
├── debug/
│   └── page.jsx            # Debug page (/debug)
└── api/                    # API routes (server-side endpoints)
    ├── debug/
    │   └── route.js        # API: /api/debug
    └── appium/
        ├── session/
        │   └── route.js    # API: /api/appium/session
        ├── command/
        │   └── route.js    # API: /api/appium/command
        └── unlock/
            └── route.js    # API: /api/appium/unlock
```

---

## 🔄 How Routing Works

### Page Routes
```
File: app/page.jsx          → URL: http://localhost:3000/
File: app/debug/page.jsx    → URL: http://localhost:3000/debug
```

### API Routes
```
File: app/api/debug/route.js → URL: http://localhost:3000/api/debug
```

**API routes are SERVER-ONLY.** They never run in the browser.

---

## 🧩 API Routes Explained

### File Structure
```jsx
// app/api/debug/route.js

import { NextResponse } from 'next/server'

// Export named function for each HTTP method
export async function GET(request) {
  return NextResponse.json({ message: 'Hello GET' })
}

export async function POST(request) {
  const body = await request.json()  // Parse request body
  return NextResponse.json({ received: body })
}
```

### Calling from Client
```jsx
// In app/page.jsx (Client Component)

const response = await fetch('/api/debug', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ data: 'test' })
})
const result = await response.json()
```

---

## 🔗 Navigation

### Using Link Component (Recommended)
```jsx
import Link from 'next/link'

<Link href="/debug">Go to Debug</Link>
```

**Why use Link?**
- Client-side navigation (no page reload)
- Faster than `<a>` tags
- Prefetches pages for better performance

---

## 🎨 Layout System

### How Layouts Work
```
app/
├── layout.jsx          ← Wraps everything
└── debug/
    ├── layout.jsx      ← Wraps only /debug pages
    └── page.jsx        ← Actual page content
```

**Rendering flow for `/debug`:**
```
app/layout.jsx
  └── app/debug/layout.jsx (if exists)
      └── app/debug/page.jsx
```

Layouts are **persistent** - they don't re-render when navigating between pages.

---

## 📝 Key Differences from Traditional React

| Traditional React | Next.js App Router |
|-------------------|-------------------|
| Use React Router for routing | File-based routing (automatic) |
| All components are client-side | Server Components by default |
| Create API in separate backend | API routes in `app/api/` |
| Manual code splitting | Automatic code splitting |
| No built-in SSR | SSR by default |

---

## ⚡ Quick Reference

### Create a New Page
1. Create folder: `app/newpage/`
2. Add file: `app/newpage/page.jsx`
3. Export default component
4. Done! Route `/newpage` now exists

### Create a New API Route
1. Create folder: `app/api/newapi/`
2. Add file: `app/api/newapi/route.js`
3. Export `GET`, `POST`, etc.
4. Done! API `/api/newapi` now exists

### When to Use 'use client'?
Use it when you need:
- ✅ `useState`, `useEffect`
- ✅ Event handlers (`onClick`, `onChange`)
- ✅ Browser APIs (`window`, `document`)
- ✅ React hooks

Don't use it when:
- ❌ Just displaying static content
- ❌ Fetching data on server
- ❌ Building layouts

---

## 🎓 Learning Path

1. **Start here**: Understand file-based routing
2. **Then**: Learn Server vs Client Components
3. **Next**: Master API routes
4. **Finally**: Explore advanced features (middleware, data fetching, etc.)

---

## 🔍 Your Project's Flow

```
User visits "/"
    ↓
app/layout.jsx renders (server)
    ↓
app/page.jsx renders (client)
    ↓
User clicks "Debug" link
    ↓
Client-side navigation (no reload!)
    ↓
app/debug/page.jsx renders (client)
    ↓
User clicks "Refresh" button
    ↓
fetch('/api/debug?action=screenshot')
    ↓
app/api/debug/route.js GET() executes (server)
    ↓
Returns JSON response
    ↓
app/debug/page.jsx receives data and updates UI
```

---

## 💡 Common Patterns in Your Code

### Pattern 1: Fetch API Route
```jsx
const res = await fetch('/api/appium/session', {
  method: 'POST',
  body: JSON.stringify({ action: 'start' })
})
const data = await res.json()
```

### Pattern 2: Navigation
```jsx
<Link href="/debug">Debug Page</Link>
```

### Pattern 3: State Management
```jsx
const [status, setStatus] = useState('idle')
// Update state triggers re-render
setStatus('loading')
```

---

## 📚 Further Reading

- **Routing**: https://nextjs.org/docs/app/building-your-application/routing
- **Server vs Client**: https://nextjs.org/docs/app/building-your-application/rendering/server-components
- **API Routes**: https://nextjs.org/docs/app/building-your-application/routing/route-handlers

---

**Remember**: Next.js is just React with superpowers (routing, SSR, API routes built-in).
If you know React, you already know 80% of Next.js!
