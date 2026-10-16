# Appium Control Interface


## Project Structure

```
appium/
├── app/                          # Next.js App Router
│   ├── layout.jsx               # Root layout
│   ├── page.jsx                 # Main control interface (/)
│   ├── globals.css              # Global styles with Tailwind
│   ├── debug/
│   │   └── page.jsx             # Debug page (/debug)
│   └── api/                     # API routes
│       ├── debug/
│       │   └── route.js         # Debug API endpoints
│       └── appium/
│           ├── session/
│           │   └── route.js     # Session management[NEXTJS_LEARNING_GUIDE.md](NEXTJS_LEARNING_GUIDE.md)
│           ├── command/
│           │   └── route.js     # Command execution
│           └── unlock/
│               └── route.js     # Device lock/unlock
├── src/
│   ├── lib/                     # Core library functions
│   │   ├── config.js           # Configuration loader
│   │   ├── appium.js           # Appium operations
│   │   ├── debug.js            # Debug operations
│   │   └── unlock.js           # Device lock/unlock
│   └── components/              # Legacy components (can be removed)
├── public/                      # Static assets
├── config.json                  # Application configuration
├── next.config.js              # Next.js configuration
├── tailwind.config.js          # Tailwind CSS configuration
├── postcss.config.js           # PostCSS configuration
└── package.json                # Dependencies

```

## Features

- 🎮 **Session Management** - Start/stop Appium sessions
- ⚡ **Command Execution** - Execute Appium commands (tap, swipe, screenshot, etc.)
- 🔐 **Device Control** - Lock/unlock device with PIN/password/pattern/fingerprint
- 🐛 **Debug Interface** - Visual UI hierarchy inspection and interaction
- 📦 **Package Manager** - View and launch installed applications
- 📱 **Activity Monitor** - View current activity and package information
- 🎨 **Modern UI** - Built with Tailwind CSS for a beautiful, responsive interface

## Technology Stack

- **Next.js 14** - App Router with server components
- **React 18** - UI components with hooks
- **Tailwind CSS** - Utility-first CSS framework
- **WebDriverIO** - Appium client for Node.js
- **Appium 2.0** - Mobile automation framework

## Getting Started

### Prerequisites

- Node.js 18+ installed
- Appium server running (default: localhost:4723)
- Android device connected via ADB

### Installation

1. Install dependencies:
```bash
npm install
```

2. Configure your device in `config.json`:
```json
{
  "webdriverio": {
    "hostname": "localhost",
    "port": 4723
  },
  "capabilities": {
    "platformName": "Android",
    "deviceName": "Your Device Name"
  }
}
```

3. Start the development server:
```bash
npm run dev
```

4. Open your browser and navigate to:
   - Main interface: http://localhost:3000
   - Debug interface: http://localhost:3000/debug

## Usage

### Main Control Interface

1. Click **Start Session** to connect to the Appium server
2. Select a command from the dropdown menu
3. Click **Execute** to run the command
4. View results in the Results panel and logs in the Logs panel

### Debug Interface

1. Click the **🐛 Debug** button from the main interface
2. Click **🔄 Refresh** to capture screenshot and UI hierarchy
3. Enable **👆 Click Mode** to interact with the screenshot
4. Click on elements in the tree to view details
5. Use **📦 Packages** to browse and launch apps

## API Routes

### Session Management
- `POST /api/appium/session` - Start/stop Appium session
  ```json
  { "action": "start" | "stop" }
  ```

### Command Execution
- `POST /api/appium/command` - Execute Appium command
  ```json
  { "command": "tap" | "swipe" | "getSource" | "screenshot" }
  ```

### Device Lock/Unlock
- `POST /api/appium/unlock` - Lock or unlock device
  ```json
  { "action": "lock" | "unlock" }
  ```

### Debug Operations
- `GET /api/debug?action=screenshot` - Capture screenshot
- `GET /api/debug?action=dump` - Get UI hierarchy
- `GET /api/debug?action=activity` - Get current activity
- `GET /api/debug?action=packages` - List all packages
- `GET /api/debug?action=activities&package=<name>` - Get package activities
- `POST /api/debug?action=click` - Click at coordinates
  ```json
  { "x": 100, "y": 200 }
  ```
- `POST /api/debug?action=launch` - Launch app
  ```json
  { "package": "com.example.app", "activity": ".MainActivity" }
  ```

## Configuration

Edit `config.json` to customize:

- **WebDriverIO settings** - Appium server connection
- **Capabilities** - Device and automation settings
- **Timeouts** - Wait times for operations
- **Device unlock** - PIN/password/pattern settings

## Building for Production

```bash
npm run build
npm start
```

## Troubleshooting

- **Connection failed**: Make sure Appium server is running
- **Device not found**: Check ADB connection with `adb devices`
- **Session timeout**: Increase timeout values in `config.json`
- **Build errors**: Delete `.next` folder and `node_modules`, then reinstall

## License

ISC
