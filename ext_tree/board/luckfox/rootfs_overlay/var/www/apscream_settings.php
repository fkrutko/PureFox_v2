<?php
// apscream_settings.php
require_once 'config.php';

$configFile = '/usr/apscream/config.txt';

// Defaults of the APScream 2.9 config.txt
$defaults = [
    'AP_MODE'               => '1',
    'TCP_MODE'              => '0',
    'MMAP_MODE'             => '1',
    'ALSA_PERIOD_FRAMES'    => '1024',
    'ALSA_BUFFER_FRAMES'    => '32768',
    'ALSA_PERIOD_TIME'      => '-1',
    'ALSA_BUFFER_TIME'      => '-1',
    'PRELOAD_BUFFER_FRAMES' => '100000',
    'SCREAM_LATENCY'        => '200',
    'PORT'                  => '4011',
    'LOCK_CORE'             => '0',
    'PRIORITY'              => '0',
    'NICE'                  => '-11',
];

// [min, max] of every value
$ranges = [
    'AP_MODE'               => [0, 1],
    'TCP_MODE'              => [0, 1],
    'MMAP_MODE'             => [0, 1],
    'ALSA_PERIOD_FRAMES'    => [-1, 1048576],
    'ALSA_BUFFER_FRAMES'    => [-1, 1048576],
    'ALSA_PERIOD_TIME'      => [-1, 10000000],
    'ALSA_BUFFER_TIME'      => [-1, 10000000],
    'PRELOAD_BUFFER_FRAMES' => [0, 490000],
    'SCREAM_LATENCY'        => [0, 10000],
    'PORT'                  => [1, 65535],
    'LOCK_CORE'             => [0, 1],
    'PRIORITY'              => [0, 99],
    'NICE'                  => [-20, 19],
];

function readApscreamConfig($configFile, $defaults) {
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
                'settings' => readApscreamConfig($configFile, $defaults)
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

            $current = readApscreamConfig($configFile, $defaults);
            $configLines = [];
            foreach ($defaults as $key => $default) {
                $value = isset($settings[$key]) ? trim((string)$settings[$key]) : $current[$key];
                list($min, $max) = $ranges[$key];
                if (!preg_match('/^-?[0-9]{1,8}$/', $value) || (int)$value < $min || (int)$value > $max) {
                    throw new Exception("Invalid value for $key");
                }
                $configLines[] = "$key=" . (int)$value;
            }

            if (file_put_contents($configFile, implode("\n", $configLines) . "\n") === false) {
                throw new Exception('Cannot write to config file');
            }

            // Apply right away when APScream is the active player
            if (trim((string)shell_exec('/bin/pidof apscream 2>/dev/null')) !== '') {
                shell_exec('/usr/bin/sudo /etc/rc.pure/S95apscream restart >/dev/null 2>&1');
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
  <title>APScream Settings</title>
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
      <h1>APScream Settings</h1>
    </div>

    <form id="apscream-settings-form">
      <div class="form-grid">
        <div class="form-group">
          <label for="ap-mode">AP mode:</label>
          <select id="ap-mode" name="AP_MODE">
            <option value="0">Off</option>
            <option value="1">On</option>
          </select>
          <div class="help-text">Album Player mode: native DSD, stereo only. Off = standard receiver</div>
        </div>

        <div class="form-group">
          <label for="tcp-mode">TCP mode:</label>
          <select id="tcp-mode" name="TCP_MODE">
            <option value="0">Off</option>
            <option value="1">On</option>
          </select>
          <div class="help-text">AP mode only; turn on TCP_MODE in the asioscream driver too</div>
        </div>

        <div class="form-group">
          <label for="mmap-mode">MMAP mode:</label>
          <select id="mmap-mode" name="MMAP_MODE">
            <option value="0">Off</option>
            <option value="1">On</option>
          </select>
          <div class="help-text">AP mode only. Off = R/W access</div>
        </div>

        <div class="form-group">
          <label for="port">Port:</label>
          <input type="number" id="port" name="PORT" min="1" max="65535">
          <div class="help-text">Unicast port (default 4011)</div>
        </div>

        <div class="form-group">
          <label for="alsa-period-frames">ALSA period, frames:</label>
          <input type="number" id="alsa-period-frames" name="ALSA_PERIOD_FRAMES" min="-1" max="1048576">
          <div class="help-text">-1 = use the period time</div>
        </div>

        <div class="form-group">
          <label for="alsa-buffer-frames">ALSA buffer, frames:</label>
          <input type="number" id="alsa-buffer-frames" name="ALSA_BUFFER_FRAMES" min="-1" max="1048576">
          <div class="help-text">-1 = use the buffer time</div>
        </div>

        <div class="form-group">
          <label for="alsa-period-time">ALSA period, &micro;s:</label>
          <input type="number" id="alsa-period-time" name="ALSA_PERIOD_TIME" min="-1" max="10000000">
          <div class="help-text">-1 = use the period in frames</div>
        </div>

        <div class="form-group">
          <label for="alsa-buffer-time">ALSA buffer, &micro;s:</label>
          <input type="number" id="alsa-buffer-time" name="ALSA_BUFFER_TIME" min="-1" max="10000000">
          <div class="help-text">-1 = use the buffer in frames</div>
        </div>

        <div class="form-group">
          <label for="preload-buffer-frames">Preload, frames:</label>
          <input type="number" id="preload-buffer-frames" name="PRELOAD_BUFFER_FRAMES" min="0" max="490000">
          <div class="help-text">Playback starts after this many frames (0-490000, 0 = full buffer)</div>
        </div>

        <div class="form-group">
          <label for="scream-latency">Latency, ms:</label>
          <input type="number" id="scream-latency" name="SCREAM_LATENCY" min="0" max="10000">
          <div class="help-text">Standard receiver mode only (AP mode off)</div>
        </div>

        <div class="form-group">
          <label for="lock-core">Lock core:</label>
          <select id="lock-core" name="LOCK_CORE">
            <option value="0">Off</option>
            <option value="1">On</option>
          </select>
          <div class="help-text">Use the last CPU core exclusively</div>
        </div>

        <div class="form-group">
          <label for="priority">Priority:</label>
          <input type="number" id="priority" name="PRIORITY" min="0" max="99">
          <div class="help-text">0 = standard scheduling, 1-99 = real-time</div>
        </div>

        <div class="form-group">
          <label for="nice">Nice:</label>
          <input type="number" id="nice" name="NICE" min="-20" max="19">
          <div class="help-text">-20 (highest) to 19 (lowest)</div>
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

      document.getElementById('apscream-settings-form').addEventListener('submit', function(e) {
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
              // every field is named after its config.txt key
              const form = document.getElementById('apscream-settings-form');
              Object.entries(data.settings).forEach(([key, value]) => {
                if (form.elements[key]) form.elements[key].value = value;
              });
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
      const formData = new FormData(document.getElementById('apscream-settings-form'));
      const settings = {};
      formData.forEach((value, key) => {
        settings[key] = value;
      });

      fetch('apscream_settings.php', {
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