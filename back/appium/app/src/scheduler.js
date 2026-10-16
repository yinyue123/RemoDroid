/**
 * Task Scheduler with Min Heap
 *
 * Uses a min heap to efficiently manage scheduled tasks based on:
 * - period: Period in seconds, e.g., 24*60*60 for one day
 * - position: Position in seconds within period, e.g., 12*60*60 for 12:00 in a day
 * - offset: Random offset in seconds, e.g., 3600 for random execution within ±1 hour
 */

import fs from 'fs';
import path from 'path';
import { startAppiumSession, stopAppiumSession, createAppiumWrapper } from './appium.js';

// Scripts directory (reference from app/api/scripts/route.js)
const SCRIPTS_DIR = path.join(process.cwd(), 'app', 'scripts');

// Global variables for singleton pattern (like appium.js)
let heap = null;
let initialized = false;

/**
 * Helper function to load CommonJS module from file
 * (Reference from app/api/scripts/route.js)
 */
function loadCommonJSModule(filePath) {
  const code = fs.readFileSync(filePath, 'utf-8');
  const module = { exports: {} };
  const exports = module.exports;

  // Create a function wrapper to execute the module code
  const wrappedCode = `(function(module, exports, require) {
    ${code}
    return module.exports;
  })`;

  // Create a custom require function for the module
  const customRequire = (modulePath) => {
    if (modulePath.startsWith('.')) {
      // Resolve relative paths
      const resolvedPath = path.resolve(path.dirname(filePath), modulePath);
      if (fs.existsSync(resolvedPath + '.js')) {
        return loadCommonJSModule(resolvedPath + '.js');
      } else if (fs.existsSync(resolvedPath)) {
        return loadCommonJSModule(resolvedPath);
      }
    }
    // For node_modules, use dynamic import or throw
    throw new Error(`Cannot require '${modulePath}' from scripts`);
  };

  // Execute the wrapped code
  const fn = eval(wrappedCode);
  return fn(module, exports, customRequire);
}

/**
 * Min Heap for Task Scheduling
 */
class MinHeap {
  constructor() {
    this.heap = [];
  }

  parent(i) {
    return Math.floor((i - 1) / 2);
  }

  left(i) {
    return 2 * i + 1;
  }

  right(i) {
    return 2 * i + 2;
  }

  swap(i, j) {
    [this.heap[i], this.heap[j]] = [this.heap[j], this.heap[i]];
  }

  push(task) {
    this.heap.push(task);
    this.heapifyUp(this.heap.length - 1);
  }

  pop() {
    if (this.heap.length === 0) return null;
    if (this.heap.length === 1) return this.heap.pop();

    const min = this.heap[0];
    this.heap[0] = this.heap.pop();
    this.heapifyDown(0);
    return min;
  }

  peek() {
    return this.heap.length > 0 ? this.heap[0] : null;
  }

  size() {
    return this.heap.length;
  }

  heapifyUp(i) {
    while (i > 0 && this.heap[this.parent(i)].nextRun > this.heap[i].nextRun) {
      this.swap(i, this.parent(i));
      i = this.parent(i);
    }
  }

  heapifyDown(i) {
    let minIndex = i;
    const l = this.left(i);
    const r = this.right(i);

    if (l < this.heap.length && this.heap[l].nextRun < this.heap[minIndex].nextRun) {
      minIndex = l;
    }

    if (r < this.heap.length && this.heap[r].nextRun < this.heap[minIndex].nextRun) {
      minIndex = r;
    }

    if (i !== minIndex) {
      this.swap(i, minIndex);
      this.heapifyDown(minIndex);
    }
  }

  getAll() {
    return [...this.heap];
  }
}

/**
 * Calculate next run time based on period, position, and offset
 * @param {number} period - Period in seconds
 * @param {number} position - Position in seconds within period
 * @param {number} offset - Random offset in seconds
 * @returns {number} Next run timestamp (milliseconds)
 */
function calculateNextRun(period, position, offset) {
  const now = Date.now();
  const nowSeconds = Math.floor(now / 1000);

  // Calculate the next period start (current time + period)
  const nextPeriodStart = nowSeconds + period;

  // Calculate which period the next run should be in
  const targetPeriodStart = Math.floor(nextPeriodStart / period) * period;

  // Calculate the target time in that period
  let targetTime = targetPeriodStart + position;

  // Add random offset within the range
  const randomOffset = Math.floor(Math.random() * (offset * 2 + 1)) - offset;
  targetTime += randomOffset;

  return targetTime * 1000; // Convert to milliseconds
}

/**
 * Initialize the heap with all scheduled tasks (Lazy Loading)
 * Only loads scripts on first call, then cached
 */
async function initialize() {
  if (initialized) return;

  // Initialize heap if not already done
  if (!heap) {
    heap = new MinHeap();
  }

  try {
    console.log('[Scheduler] Initializing task heap (lazy loading)...');

    // Read all .js files from scripts directory (synchronous)
    const files = fs.readdirSync(SCRIPTS_DIR);
    const scriptFiles = files.filter(file => file.endsWith('.js'));

    for (const file of scriptFiles) {
      const scriptPath = path.join(SCRIPTS_DIR, file);

      try {
        // Load CommonJS module (reference from scripts/route.js GET method)
        const script = loadCommonJSModule(scriptPath);

        // Check if script has schedule configuration and is enabled
        if (!script.schedule || !script.schedule.enabled) {
          console.log(`[Scheduler] Skipping "${script.name || file}" - schedule not enabled`);
          continue;
        }

        const { period, position, offset } = script.schedule;

        // Validate schedule configuration
        if (!period || position === undefined) {
          console.warn(`[Scheduler] Invalid schedule config for "${script.name || file}" - missing period or position`);
          continue;
        }

        // Calculate next run time
        const nextRun = calculateNextRun(period, position, offset || 0);

        // Add to heap
        heap.push({
          name: script.name || file,
          scriptPath,
          script,
          schedule: script.schedule,
          nextRun
        });

        const nextRunDate = new Date(nextRun);
        console.log(`[Scheduler] Added task "${script.name || file}" - next run: ${nextRunDate.toLocaleString()}`);

      } catch (error) {
        console.error(`[Scheduler] Error loading script ${file}:`, error.message);
      }
    }

    initialized = true;
    console.log(`[Scheduler] Heap initialized with ${heap.size()} task(s) - scripts cached`);

  } catch (error) {
    console.error('[Scheduler] Error initializing heap:', error);
  }
}

/**
 * Check and execute due tasks from the heap
 */
async function checkAndExecuteTasks() {
  // Initialize heap if not already done (lazy loading)
  if (!initialized) {
    await initialize();
  }

  const now = Date.now();
  const executedTasks = [];

  console.log(`[Scheduler] Checking tasks... (${heap.size()} in queue)`);

  // Pop all due tasks from the heap
  while (heap.peek() && heap.peek().nextRun <= now) {
    const task = heap.pop();
    executedTasks.push(task);

    console.log(`[Scheduler] Task "${task.name}" is due, executing...`);

    // Execute task in background
    executeTask(task).catch(error => {
      console.error(`[Scheduler] Error executing task "${task.name}":`, error);
    });
  }

  if (executedTasks.length === 0) {
    const nextTask = heap.peek();
    if (nextTask) {
      const timeUntil = Math.floor((nextTask.nextRun - now) / 1000 / 60);
      console.log(`[Scheduler] No due tasks. Next task "${nextTask.name}" in ${timeUntil} minutes`);
    } else {
      console.log('[Scheduler] No tasks in queue');
    }
  } else {
    console.log(`[Scheduler] Executed ${executedTasks.length} task(s)`);
  }
}

/**
 * Execute a task and reschedule it
 * (Reference from app/api/scripts/route.js PUT method and session/route.js)
 */
async function executeTask(task) {
  try {
    console.log(`[Scheduler] Executing "${task.name}"...`);

    // Start Appium session (reference from session/route.js)
    console.log(`[Scheduler] Starting Appium session for "${task.name}"...`);
    await startAppiumSession();
    console.log(`[Scheduler] Appium session started for "${task.name}"`);

    // Create Appium wrapper for the script (reference from scripts/route.js)
    const appiumWrapper = createAppiumWrapper();

    // Execute the script with the wrapper
    const result = await task.script.execute(appiumWrapper);

    // Stop Appium session
    await stopAppiumSession();
    console.log(`[Scheduler] Appium session stopped for "${task.name}"`);

    console.log(`[Scheduler] Task "${task.name}" completed:`, result.success ? 'SUCCESS' : 'FAILED');
    if (!result.success) {
      console.error(`[Scheduler] Error message: ${result.message}`);
    }

    // Calculate next run time
    const { period, position, offset } = task.schedule;
    const nextRun = calculateNextRun(period, position, offset || 0);

    // Add back to heap with new time
    heap.push({
      ...task,
      nextRun
    });

    const nextRunDate = new Date(nextRun);
    console.log(`[Scheduler] Rescheduled "${task.name}" for ${nextRunDate.toLocaleString()}`);

  } catch (error) {
    console.error(`[Scheduler] Error executing task "${task.name}":`, error);

    // Try to stop session if it was started
    try {
      await stopAppiumSession();
      console.log(`[Scheduler] Appium session cleaned up after error`);
    } catch (cleanupError) {
      console.error(`[Scheduler] Error during cleanup:`, cleanupError.message);
    }

    // Still reschedule even if failed
    const { period, position, offset } = task.schedule;
    const nextRun = calculateNextRun(period, position, offset || 0);
    heap.push({
      ...task,
      nextRun
    });
  }
}

/**
 * Get current scheduler status
 */
function getStatus() {
  return {
    initialized,
    queueSize: heap ? heap.size() : 0,
    tasks: heap ? heap.getAll().map(task => ({
      name: task.name,
      nextRun: new Date(task.nextRun).toISOString(),
      schedule: task.schedule
    })) : []
  };
}

// Export functions
export {
  initialize,
  checkAndExecuteTasks,
  getStatus
};
