/**
 * Task Scheduler API Route
 * Endpoint to check and execute scheduled tasks
 *
 * Usage:
 * GET /api/scheduler - Trigger task check and execution
 * POST /api/scheduler - Get scheduler status
 */

import { checkAndExecuteTasks, getStatus } from '../../src/scheduler.js';

// GET - Trigger task check and execution
export async function GET(request) {
  try {
    console.log('[Scheduler API] Task check triggered via GET request');

    // Execute tasks in background (non-blocking)
    checkAndExecuteTasks().catch(error => {
      console.error('[Scheduler API] Background task error:', error);
    });

    // Return 204 No Content immediately
    return new Response(null, { status: 204 });
  } catch (error) {
    console.error('[Scheduler API] Error:', error);
    return Response.json({
      success: false,
      message: 'Failed to trigger task check',
      error: error.message
    }, { status: 500 });
  }
}

// POST - Get scheduler status
export async function POST(request) {
  try {
    const status = getStatus();

    return Response.json({
      success: true,
      ...status
    });
  } catch (error) {
    console.error('[Scheduler API] Error getting status:', error);
    return Response.json({
      success: false,
      message: 'Failed to get scheduler status',
      error: error.message
    }, { status: 500 });
  }
}
