"""Exercise the GUI/engine boundary without capture, playback, or user settings."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

engine = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="capture-view-gui-") as directory:
    config = Path(directory) / 'settings with "quotes".toml'
    source = 'capture "audio" \\ source'
    command = [str(engine), "--no-load-config", "--config", str(config),
               "--video", "/dev/nonexistent-capture-view-test", "--test-pattern",
               "--audio-input", source, "--audio-output", "default",
               "--render-backend", "vulkan", "--size", "1920x1080", "--fps", "60",
               "--no-audio-monitor", "--virtual-devices", "2", "--save-only"]
    saved = subprocess.run(command, capture_output=True, text=True, timeout=5)
    assert saved.returncode == 0, saved.stderr
    assert config.is_file(), "Save-only did not create the selected config file"
    result = subprocess.run([str(engine), "--config", str(config), "--gui-info"],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    data = json.loads(result.stdout)
    assert data["config_path"] == str(config)
    assert data["settings"]["audio_input"] == source, "Quoted source name was not preserved"
    assert data["settings"]["audio_output"] == "default"
    assert data["settings"]["renderer"] == "vulkan"
    assert data["settings"]["virtual_devices"] == 2
    assert data["settings"]["width"] == 1920 and data["settings"]["fps"] == 60
    assert isinstance(data["video_devices"], list) and isinstance(data["audio_devices"], list)
    assert "--no-load-config" in data["arguments"]
print("GUI bridge: isolated save, JSON escaping, and settings round-trip passed")
