# SH1106 Display Integration Guide

## Overview
The SH1106 128x64 OLED display has been successfully integrated into your snapclient firmware. This display can show:
- Song metadata (title, artist, album, genre)
- Audio visualization with EQ bars
- Volume levels
- Connection status
- WiFi signal strength
- Device name

## Hardware Setup

### Wiring
Connect the SH1106 display to your ESP32 using I2C:

| SH1106 Pin | ESP32 Pin (Default) | Description |
|------------|---------------------|-------------|
| VCC        | 3.3V               | Power supply |
| GND        | GND                | Ground |
| SCL        | GPIO 22            | I2C Clock |
| SDA        | GPIO 21            | I2C Data |

**Note:** The default pins are GPIO 21 (SDA) and GPIO 22 (SCL), but these can be configured in menuconfig.

## Software Configuration

### Step 1: Enable the Display in Menuconfig

Run the configuration menu:
```bash
idf.py menuconfig
```

Navigate to:
```
Snapclient Configuration → SH1106 Display Configuration
```

### Step 2: Configure Display Settings

Enable and configure the following options:

1. **Enable SH1106 OLED Display**: Set to `Yes` (Y)

2. **I2C SDA GPIO**: Default is `21`
   - Change this if your wiring is different
   
3. **I2C SCL GPIO**: Default is `22`
   - Change this if your wiring is different

4. **I2C Frequency (Hz)**: Default is `400000` (400 kHz)
   - This is the standard I2C speed for most displays
   - Can be lowered to 100000 (100 kHz) if you experience issues

5. **I2C Address**: Default is `0x3C`
   - Most SH1106 displays use 0x3C
   - Some use 0x3D - check your display's documentation

### Step 3: Build and Flash

After configuring, build and flash your firmware:
```bash
idf.py build
idf.py flash
idf.py monitor
```

## Display Features

### What's Displayed

The display is divided into zones:

1. **Top Section (32 pixels)**:
   - Device name (scrolls if long)
   - Song title (scrolls if long)
   - Artist name (scrolls if long)
   - WiFi signal strength indicator (bars)
   - Connection status indicator

2. **Bottom Section (32 pixels)**:
   - Real-time audio equalizer (16 bars)
   - Visual representation of audio frequencies

### Display States

- **Connected**: Shows "CONNECTED" with song metadata
- **Disconnected**: Shows "DISCONNECTED" 
- **Resyncing**: Shows "RESYNCING HARD" during synchronization

## API Integration

The display component provides these functions to update content:

```c
// Set song metadata
display_set_song_metadata(title, artist, album, genre);

// Set audio levels (for EQ visualization)
display_set_audio_levels(eq_levels, volume, is_playing);

// Set connection status
display_set_connection_status(connected);

// Set resyncing state
display_set_resyncing(resyncing);

// Set device name
display_set_device_name(device_name);

// Set WiFi signal
display_set_wifi_signal(rssi, wifi_connected);
```

## Troubleshooting

### Display Not Working

1. **Check Wiring**:
   - Verify VCC is connected to 3.3V (NOT 5V)
   - Verify GND is connected to ground
   - Verify SDA and SCL are connected correctly

2. **Check I2C Address**:
   - Some displays use 0x3D instead of 0x3C
   - Run an I2C scanner to detect the correct address

3. **Check GPIO Pins**:
   - Ensure the GPIO pins in menuconfig match your wiring
   - Some ESP32 boards have restrictions on which pins can be used

4. **Check Logs**:
   - Monitor the serial output: `idf.py monitor`
   - Look for "SH1106 display initialized successfully" or error messages

### Display Shows Garbled Output

1. **Lower I2C Frequency**:
   - Try setting I2C frequency to 100000 Hz in menuconfig
   - Some displays or long wires need slower speeds

2. **Check Power Supply**:
   - Ensure 3.3V supply can provide enough current
   - Try adding a decoupling capacitor (0.1µF) near the display

### Display Updates Slowly

- The display updates in a background task
- Text scrolling is intentionally slowed for readability
- Audio EQ updates in real-time with audio playback

## Advanced Configuration

### Customizing Display Layout

Edit `components/display_sh1106/display_sh1106.c` to modify:
- Font sizes
- Layout zones
- Scroll speed
- EQ bar count and appearance

### Customizing Pins at Runtime

While menuconfig sets the default pins, you can also modify them directly in:
`components/display_sh1106/include/display_sh1106.h`

## Disabling the Display

To disable the display:
1. Run `idf.py menuconfig`
2. Navigate to: `Snapclient Configuration → SH1106 Display Configuration`
3. Set "Enable SH1106 OLED Display" to `No` (N)
4. Rebuild: `idf.py build`

The display code will be completely excluded from the build when disabled.

## Next Steps

To fully integrate the display with your audio pipeline, you'll need to:
1. Add calls to `display_set_audio_levels()` in your audio processing code
2. Add calls to `display_set_song_metadata()` when song info is received
3. Add calls to `display_set_connection_status()` when connection changes
4. Add calls to `display_set_wifi_signal()` periodically to show WiFi strength

These integration points depend on your specific snapcast protocol implementation.

## Support

For issues or questions:
- Check the serial monitor output for error messages
- Verify all wiring connections
- Ensure menuconfig settings match your hardware
- Test with the I2C scanner if display is not detected
