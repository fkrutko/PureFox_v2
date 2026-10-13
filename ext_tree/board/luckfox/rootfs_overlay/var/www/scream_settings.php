<?php
// scream_settings.php
require_once 'config.php';

$configFile = '/etc/scream_receiver.conf';

$defaults = [
    'MODE'      => 'multicast',
    'GROUP'     => '239.255.77.77',
    'PORT'      => '4010',
    'INTERFACE' => 'eth0',
    'LATENCY'   => '100',
    'DEVICE'    => '',
];

// The config is sourced by the init script as root, so every value is
// checked against a strict pattern before it is written.
$patterns = [
    'MODE'      => '/^(multicast|unicast)$/',
    'GROUP'     => '/^(22[4-9]|23[0-9])(\.(25[0-5]|2[0-4][0-9]|1?[0-9]?[0-9])){3}$/',
    'PORT'      => '/^[0-9]{1,5}$/',
    'INTERFACE' => '/^[A-Za-z0-9._-]{0,32}$/',
    'LATENCY'   => '/^[0-9]{1,4}$/',
    'DEVICE'    => '/^[A-Za-z0-9:,_-]{0,64}$/',
];

function readScreamConfig($configFile, $defaults) {
    $settings = $defaults;
    if (!file_exists($configFile)) {
        return $settings;
    }
    $content = file_get_contents($configFile);
    if ($content === false) {
        throw new Exception('Cannot read config file');
    }
    foreach (explode("\n", $content) as $line) {
        $line = trim($line);
        if ($line === '' || $line[0] === '#' || strpos($line, '=') === false) {
            continue;
        }
        list($key, $value) = explode('=', $line, 2);
        $key = trim($key);
        if (array_key_exists($key, $defaults)) {
            $settings[$key] = trim($value);
        }
    }
    return $settings;
}

// Handle AJAX requests
if ($_SERVER['REQUEST_METHOD'] === 'POST' || (isset($_GET['action']) && $_GET['action'] === 'get')) {
    header('Content-Type: application/json');

    // GET settings
    if (isset($_GET['action']) && $_GET['action'] === 'get') {
        try {
            echo json_encode([
                'success' => true,
                'settings' => readScreamConfig($configFile, $defaults)
            ]);
        } catch (Exception $e) {
            echo json_encode([
                'success' => false,
                'message' => $e->getMessage()
            ]);
        }
        exit;
    }

    // SAVE settings
    if ($_SERVER['REQUEST_METHOD'] === 'POST') {
        try {
            $input = file_get_contents('php://input');
            $settings = json_decode($input, true);

            if (!is_array($settings)) {
                throw new Exception('Invalid JSON data');
            }

            $current = readScreamConfig($configFile, $defaults);
            $values = [];
            $configLines = [];
            foreach ($defaults as $key => $default) {
                $value = isset($settings[$key]) ? trim((string)$settings[$key]) : $current[$key];
                if (!preg_match($patterns[$key], $value)) {
                    throw new Exception("Invalid value for $key");
                }
                $values[$key] = $value;
                $configLines[] = "$key=$value";
            }

            $port = (int)$values['PORT'];
            if ($port < 1 || $port > 65535) {
                throw new Exception('Invalid value for PORT');
            }

            if (file_put_contents($configFile, implode("\n", $configLines) . "\n") === false) {
                throw new Exception('Cannot write to config file');
            }

            // Apply right away when Scream is the active player
            if (trim((string)shell_exec('/bin/pidof scream 2>/dev/null')) !== '') {
                shell_exec('/usr/bin/sudo /etc/rc.pure/S95scream restart >/dev/null 2>&1');
            }

            echo json_encode([
                'success' => true,
                'message' => 'Settings saved successfully'
            ]);

        } catch (Exception $e) {
            echo json_encode([
                'success' => false,
                'message' => $e->getMessage()
            ]);
        }
        exit;
    }
}

// Display HTML page
header('Content-Type: text/html; charset=UTF-8');
?>
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Scream Settings</title>
  <link rel="stylesheet" href="assets/css/style.css?v=<?php echo VERSION; ?>">
  <style>
    .container {
        max-width: 530px;
        width: calc(100% - 40px);
        margin: auto;
        padding: 25px;
        background-color: #2a2a2a;
        border-radius: 12px;
        box-shadow: 
            0 30px 60px rgba(0, 0, 0, 0.9),
            0 20px 40px rgba(0, 0, 0, 0.8),
            0 10px 20px rgba(0, 0, 0, 0.7),
            0 0 0 1px rgba(255, 255, 255, 0.15);
        position: relative;
        top: 50%;
        transform: translateY(-50%);
    }
    
    .header {
        display: flex;
        align-items: center;
        justify-content: center;
        position: relative;
        margin-bottom: 25px;
    }
    
    .form-grid {
        display: grid;
        grid-template-columns: 1fr 1fr;
        gap: 20px;
        margin-bottom: 20px;
    }

    @media (max-width: 500px) {
        .form-grid {
            grid-template-columns: 1fr;
            gap: 15px;
        }
    }
    
    .form-group {
        margin-bottom: 15px;
    }
    
    .form-group label {
        display: block;
        font-weight: bold;
        margin-bottom: 5px;
        color: #e0e0e0;
        font-size: 14px;
    }
    
    .form-group input,
    .form-group select {
        width: 100%;
        padding: 8px;
        border: 1px solid #555;
        border-radius: 4px;
        background: #2a2a2a;
        color: #e0e0e0;
        box-sizing: border-box;
    }
    
    .help-text {
        font-size: 11px;
        color: #aaa;
        margin-top: 3px;
        line-height: 1.3;
    }
    
    .buttons {
        grid-column: 1 / -1;
        display: flex;
        align-items: center;
        justify-content: space-between;
        margin-top: 20px;
        position: relative;
    }
    
    .save-btn {
        background-color: #5d5d5d !important;
        color: white !important;
        border: none !important;
        padding: 10px 20px !important;
        border-radius: 4px !important;
        cursor: pointer !important;
        font-weight: bold !important;
    }
    
    .save-btn:hover {
        background-color: #6d6d6d !important;
    }
    
    .home-button {
        display: flex;
        align-items: center;
        justify-content: center;
        text-decoration: none;
    }

    .status-message {
        display: none;
        padding: 10px;
        margin-top: 15px;
        border-radius: 4px;
        text-align: center;
        font-weight: bold;
    }

    .status-message.success {
        background-color: #2d5a2d;
        color: #90ee90;
        border: 1px solid #4a8a4a;
    }

    .status-message.error {
        background-color: #5a2d2d;
        color: #ff9090;
        border: 1px solid #8a4a4a;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>Scream Settings</h1>
    </div>

    <form id="scream-settings-form">
      <div class="form-grid">
        <div class="form-group">
          <label for="mode">Mode:</label>
          <select id="mode" name="MODE">
            <option value="multicast">Multicast</option>
            <option value="unicast">Unicast</option>
          </select>
          <div class="help-text">Must match the sender (Windows driver)</div>
        </div>

        <div class="form-group">
          <label for="group">Multicast group:</label>
          <input type="text" id="group" name="GROUP" placeholder="239.255.77.77">
          <div class="help-text">Multicast mode only</div>
        </div>

        <div class="form-group">
          <label for="port">Port:</label>
          <input type="number" id="port" name="PORT" min="1" max="65535">
          <div class="help-text">UDP port (default 4010)</div>
        </div>

        <div class="form-group">
          <label for="interface">Interface:</label>
          <input type="text" id="interface" name="INTERFACE" placeholder="eth0">
          <div class="help-text">Network interface or IP, empty = any</div>
        </div>

        <div class="form-group">
          <label for="latency">Latency:</label>
          <input type="number" id="latency" name="LATENCY" min="10" max="2000" step="10">
          <div class="help-text">Target ALSA latency in ms (10-2000)</div>
        </div>

        <div class="form-group">
          <label for="device">ALSA device:</label>
          <input type="text" id="device" name="DEVICE" placeholder="auto">
          <div class="help-text">Empty = selected output (plughw)</div>
        </div>

        <div class="buttons">
          <a href="index.php" class="home-button">
            <img src="assets/img/home.svg" class="settings-icon" alt="">
          </a>
          <div style="flex: 1;"></div>
          <button type="submit" class="save-btn">Save</button>
        </div>
      </div>
      <div id="status-message" class="status-message"></div>
    </form>
  </div>

  <script>
    document.addEventListener('DOMContentLoaded', function() {
      loadSettings();

      document.getElementById('scream-settings-form').addEventListener('submit', function(e) {
        e.preventDefault();
        saveSettings();
      });
    });

    function loadSettings() {
      fetch('?action=get')
        .then(response => response.text())
        .then(text => {
          try {
            const data = JSON.parse(text);
            if (data.success) {
              const st = data.settings;
              document.getElementById('mode').value = st.MODE || 'multicast';
              document.getElementById('group').value = st.GROUP || '';
              document.getElementById('port').value = st.PORT || '4010';
              document.getElementById('interface').value = st.INTERFACE || '';
              document.getElementById('latency').value = st.LATENCY || '100';
              document.getElementById('device').value = st.DEVICE || '';
            } else {
              showMessage('Error loading settings: ' + data.message, 'error');
            }
          } catch (e) {
            console.error('Server response:', text);
            showMessage('Error: Server returned invalid response. Check console for details.', 'error');
          }
        })
        .catch(error => {
          showMessage('Error loading settings: ' + error.message, 'error');
        });
    }

    function saveSettings() {
      const formData = new FormData(document.getElementById('scream-settings-form'));
      const settings = {};
      formData.forEach((value, key) => {
        settings[key] = value;
      });

      fetch('scream_settings.php', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        },
        body: JSON.stringify(settings)
      })
      .then(response => response.text())
      .then(text => {
        try {
          const data = JSON.parse(text);
          if (data.success) {
            showMessage('Settings saved successfully', 'success');
            setTimeout(() => {
              window.location.href = 'index.php';
            }, 2000);
          } else {
            showMessage('Error saving settings: ' + data.message, 'error');
          }
        } catch (e) {
          console.error('Server response:', text);
          showMessage('Error: Server returned invalid response. Check console (F12) for details.', 'error');
        }
      })
      .catch(error => {
        showMessage('Error saving settings: ' + error.message, 'error');
      });
    }

    function showMessage(message, type) {
      const statusMessage = document.getElementById('status-message');
      statusMessage.textContent = message;
      statusMessage.className = 'status-message ' + type;
      statusMessage.style.display = 'block';
      setTimeout(() => {
        statusMessage.style.display = 'none';
      }, 5000);
    }
  </script>
</body>
</html>